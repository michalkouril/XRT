// Read bytes from the boot flash, read-only, over the management function's SPI.
//
// 0x13 is READ with a 4-byte address, the same command the project's rawspi.py
// uses. Nothing here can modify flash: no write-enable, no erase, no program.
//
// usage: flash_read <slave> <addr-hex> [bytes]     bytes default 64, max 251
//        flash_read both <addr-hex> [bytes]        both chips, side by side
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include "xrt_dext_abi.h"

static io_connect_t
open_mgmt()
{
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it))
    return 0;
  io_connect_t found = 0;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_connect_t conn = 0;
    if (!IOServiceOpen(svc, mach_task_self(), XRT_DEXT_CLIENT_FLASH, &conn)) {
      xrt_dext_device_info info{}; size_t sz = sizeof(info);
      // A failed call leaves info zeroed, and func 0 would then "match" whatever
      // this connection actually is - so the status has to be checked.
      if (!IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &sz)
          && info.func == 0) { found = conn; IOObjectRelease(svc); break; }
      IOServiceClose(conn);
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  return found;
}

// 0x13 + 4 address bytes, then n dummy bytes whose clock cycles carry the data.
static int
flash_read(io_connect_t conn, uint32_t slave, uint64_t addr, uint32_t n, uint8_t* out)
{
  if (n > XRT_SPI_MAX_XFER - 5)
    n = XRT_SPI_MAX_XFER - 5;
  uint8_t tx[XRT_SPI_MAX_XFER] = {};
  tx[0] = 0x13;
  tx[1] = (uint8_t)(addr >> 24); tx[2] = (uint8_t)(addr >> 16);
  tx[3] = (uint8_t)(addr >> 8);  tx[4] = (uint8_t)addr;
  const uint32_t total = 5 + n;
  uint8_t rx[XRT_SPI_MAX_XFER] = {};
  uint64_t in[2] = { slave, total };
  uint64_t res[6] = {};
  uint32_t nout = 6;
  size_t rxsz = total;
  kern_return_t kr = IOConnectCallMethod(conn, XRT_DEXT_SPI_XFER, in, 2, tx, total,
                                         res, &nout, rx, &rxsz);
  if (kr != KERN_SUCCESS || res[0] != 0)
    return -1;
  // The first five bytes clocked back accompany the command and address.
  memcpy(out, rx + 5, n);
  return (int)n;
}

static void
dump(const char* label, const uint8_t* b, uint32_t n, uint64_t addr)
{
  int nonzero = 0, nonff = 0;
  for (uint32_t i = 0; i < n; ++i) { nonzero += b[i] != 0x00; nonff += b[i] != 0xff; }
  std::printf("%s @0x%08llx:", label, (unsigned long long)addr);
  for (uint32_t i = 0; i < (n < 32 ? n : 32); ++i)
    std::printf(" %02x", b[i]);
  std::printf("%s\n", n > 32 ? " ..." : "");
  std::printf("      %u bytes, %d non-zero, %d non-ff -> %s\n", n, nonzero, nonff,
              nonzero == 0 ? "all zeros: nothing is driving the line"
              : nonff == 0 ? "all ones: the line is idle high"
                           : "real data");
}

// Releasing the exclusive flash claim is ASYNCHRONOUS: IOServiceClose returns before
// the dext's Stop runs, so an open right after another flash tool exits can be refused
// with kIOReturnBusy. Retry briefly rather than fail - being refused while someone
// else genuinely holds it is correct behaviour, and a few hundred ms tells the two
// apart. Running the documented procedure as a script (flash_audit then xbflash) hits
// this, so it is not just a test concern.
static io_connect_t
open_flash_retry()
{
  for (int attempt = 0; attempt < 30; ++attempt) {
    io_connect_t c = open_mgmt();
    if (c)
      return c;
    usleep(100000);
  }
  return 0;
}

int
main(int argc, char** argv)
{
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <slave|both> <addr-hex> [bytes]\n", argv[0]);
    return 2;
  }
  io_connect_t conn = open_flash_retry();
  if (!conn) { puts("no management function with an attached dext"); return 2; }
  const uint64_t addr = strtoull(argv[2], nullptr, 0);
  uint32_t n = argc > 3 ? (uint32_t)strtoul(argv[3], nullptr, 0) : 64;
  const bool both = strcmp(argv[1], "both") == 0;

  uint8_t buf[XRT_SPI_MAX_XFER] = {};
  int rc = 0;
  for (uint32_t slave = both ? 0 : (uint32_t)strtoul(argv[1], nullptr, 0);
       slave < 2; ++slave) {
    memset(buf, 0xaa, sizeof(buf));          // so an untouched buffer is obvious
    int got = flash_read(conn, slave, addr, n, buf);
    char label[16];
    std::snprintf(label, sizeof(label), "chip %u", slave);
    if (got < 0) { std::printf("%s: transaction failed\n", label); rc = 1; }
    else dump(label, buf, (uint32_t)got, addr);
    if (!both) break;
  }
  IOServiceClose(conn);
  return rc;
}
