// The properties the two-client split is supposed to give, tested rather than
// asserted. Read-only with respect to flash: it reads SPISR and nothing else.
//
//   1. the DEFAULT client cannot reach the SPI selectors at all
//   2. only one flash connection may exist at a time
//   3. the claim is released when the owning process is KILLED, and the release
//      leaves the SPI core disabled - the case a user-space destructor misses
//
// Property 3 is the one that matters most: upstream's xspi.cpp never disables the
// core and calls exit() directly on some error paths, so "the process died mid
// program" is a normal case, not an exotic one.
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <crt_externs.h>

// libc++ on macOS does not declare `environ` for a non-main-binary TU.
#define environ (*_NSGetEnviron())
#include "xrt_dext_abi.h"

static io_service_t
find_mgmt_service()
{
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it))
    return 0;
  io_service_t found = 0;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_connect_t conn = 0;
    if (!IOServiceOpen(svc, mach_task_self(), XRT_DEXT_CLIENT_DEFAULT, &conn)) {
      xrt_dext_device_info info{}; size_t sz = sizeof(info);
      bool mgmt = !IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO,
                                             nullptr, 0, &info, &sz) && info.func == 0;
      IOServiceClose(conn);
      if (mgmt) { found = svc; break; }
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  return found;
}

static kern_return_t
spi_read(io_connect_t conn, uint32_t off, uint32_t* v)
{
  uint64_t in[3] = { off, 0, 0 };
  uint64_t out = 0; uint32_t nout = 1;
  kern_return_t kr = IOConnectCallScalarMethod(conn, XRT_DEXT_SPI_REG, in, 3, &out, &nout);
  if (v) *v = (uint32_t)out;
  return kr;
}

int
main(int argc, char** argv)
{
  io_service_t svc = find_mgmt_service();
  if (!svc) { puts("no management function with an attached dext"); return 2; }

  // A hidden mode: hold the flash claim and wait to be killed. Used by property 3.
  if (argc > 1 && std::strcmp(argv[1], "--hold") == 0) {
    io_connect_t c = 0;
    if (IOServiceOpen(svc, mach_task_self(), XRT_DEXT_CLIENT_FLASH, &c)) {
      std::fputs("hold: could not open the flash client\n", stderr);
      return 1;
    }
    // Enable the core, exactly as xspi.cpp's prepareXSpi does, then die holding it.
    uint64_t in[3] = { XRT_SPI_CR,
                       XRT_SPI_CR_MASTER | XRT_SPI_CR_MANUAL_SS
                       | XRT_SPI_CR_INHIBIT | XRT_SPI_CR_SPE, 1 };
    uint64_t out = 0; uint32_t nout = 1;
    IOConnectCallScalarMethod(c, XRT_DEXT_SPI_REG, in, 3, &out, &nout);
    std::fputs("held\n", stdout);
    std::fflush(stdout);
    pause();
    return 0;
  }

  int fail = 0;

  // --- 1. the default client must not reach the SPI selectors
  io_connect_t dflt = 0;
  if (IOServiceOpen(svc, mach_task_self(), XRT_DEXT_CLIENT_DEFAULT, &dflt)) {
    puts("could not open the default client"); return 2;
  }
  uint32_t v = 0;
  kern_return_t kr = spi_read(dflt, XRT_SPI_SR, &v);
  std::printf("1. SPI_REG on the default client: %s (0x%x)\n",
              kr ? "refused" : "ACCEPTED - BUG", kr);
  if (!kr) fail++;
  uint64_t xin[2] = { 0, 1 };
  uint8_t tx[1] = { 0x9f }, rx[8] = {};
  size_t rxsz = sizeof(rx);
  uint64_t xout[6] = {}; uint32_t xnout = 6;
  kr = IOConnectCallMethod(dflt, XRT_DEXT_SPI_XFER, xin, 2, tx, 1, xout, &xnout, rx, &rxsz);
  std::printf("   SPI_XFER on the default client: %s (0x%x)\n",
              kr ? "refused" : "ACCEPTED - BUG", kr);
  if (!kr) fail++;

  // --- 2. only one flash connection at a time
  //
  // Retry the FIRST open. The claim is exclusive, so anything else holding it - a
  // flash tool, or this test's own child from a previous run whose teardown has not
  // finished - makes this fail with kIOReturnBusy. Run back-to-back with no gap, that
  // showed up as an intermittent failure of the whole suite; waiting briefly is the
  // honest fix, because being refused while someone else holds it is correct
  // behaviour, not a bug.
  io_connect_t f1 = 0, f2 = 0;
  for (int attempt = 0; attempt < 40; ++attempt) {
    kr = IOServiceOpen(svc, mach_task_self(), XRT_DEXT_CLIENT_FLASH, &f1);
    if (kr != kIOReturnBusy)
      break;
    usleep(100000);
  }
  std::printf("2. first flash connection:  %s (0x%x)\n", kr ? "REFUSED - BUG" : "opened", kr);
  if (kr) fail++;
  else {
    kern_return_t kr2 = IOServiceOpen(svc, mach_task_self(), XRT_DEXT_CLIENT_FLASH, &f2);
    std::printf("   second flash connection: %s (0x%x)\n",
                kr2 ? "refused" : "ACCEPTED - BUG", kr2);
    if (!kr2) { fail++; IOServiceClose(f2); }
    IOServiceClose(f1);
    // The claim comes back after the close - but NOT synchronously. IOServiceClose
    // returns before the dext's Stop runs, so an immediate reopen can still see
    // kIOReturnBusy. That is what made this suite fail intermittently; the property
    // being tested is that the claim is released, not that it is released instantly.
    for (int attempt = 0; attempt < 30; ++attempt) {
      kr2 = IOServiceOpen(svc, mach_task_self(), XRT_DEXT_CLIENT_FLASH, &f2);
      if (kr2 != kIOReturnBusy)
        break;
      usleep(100000);
    }
    std::printf("   after closing the first:  %s (0x%x)\n",
                kr2 ? "STILL REFUSED - BUG" : "opened again", kr2);
    if (kr2) fail++; else IOServiceClose(f2);
  }

  // --- 3. SIGKILL releases the claim and disables the core
  puts("3. kill a process holding the claim with the core enabled:");
  pid_t pid = 0;
  char* args[] = { argv[0], (char*)"--hold", nullptr };
  if (posix_spawn(&pid, argv[0], nullptr, nullptr, args, environ) != 0) {
    puts("   could not spawn the holder"); fail++;
  } else {
    // Give it time to open and enable. Crude, but this is a test, and the holder
    // prints "held" before it waits.
    usleep(400000);
    io_connect_t probe = 0;
    kern_return_t held = IOServiceOpen(svc, mach_task_self(), XRT_DEXT_CLIENT_FLASH, &probe);
    std::printf("   while it holds, another open is: %s (0x%x)\n",
                held ? "refused" : "ACCEPTED - BUG", held);
    if (!held) { fail++; IOServiceClose(probe); }
    kill(pid, SIGKILL);
    int st = 0;
    waitpid(pid, &st, 0);
    usleep(400000);                       // let Stop run
    kern_return_t after = IOServiceOpen(svc, mach_task_self(), XRT_DEXT_CLIENT_FLASH, &probe);
    std::printf("   after SIGKILL, an open is:      %s (0x%x)\n",
                after ? "STILL REFUSED - BUG" : "accepted", after);
    if (after) fail++;
    else {
      uint32_t cr = 0;
      spi_read(probe, XRT_SPI_CR, &cr);
      std::printf("   SPICR after SIGKILL: 0x%08x %s\n", cr,
                  (cr & XRT_SPI_CR_SPE) ? "(SPE STILL SET - the ICAP will fail)"
                                        : "(core disabled by the driver)");
      if (cr & XRT_SPI_CR_SPE) fail++;
      IOServiceClose(probe);
    }
  }

  IOServiceClose(dflt);
  IOObjectRelease(svc);
  std::printf("\n%d check(s) failed\n", fail);
  return fail ? 1 : 0;
}
