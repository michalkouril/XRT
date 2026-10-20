// Read the boot flash identity over the management function's AXI Quad SPI.
//
// RDID (0x9f) is a read-only command: it cannot modify the flash. SPI is
// symmetric, so the byte clocked back during the command itself is whatever the
// device drives then - usually 0xff - and the identity follows. This is the gate
// the shell project's flash procedure requires: both chips must answer, because
// an image is striped across the pair as x8 and writing with only one reachable
// leaves half an image and fails the card over.
#include <IOKit/IOKitLib.h>
#include <cstdio>
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

// One transaction: tx bytes out, the same number clocked back.
static kern_return_t
xfer(io_connect_t conn, uint32_t slave, const uint8_t* tx, uint32_t n,
     uint8_t* rx, uint32_t* got, uint64_t* sr_before, uint64_t* sr_after, uint64_t* waited)
{
  uint64_t in[2] = { slave, n };
  uint64_t out[6] = {};
  uint32_t nout = 6;
  size_t rxsz = n;
  kern_return_t kr = IOConnectCallMethod(conn, XRT_DEXT_SPI_XFER, in, 2, tx, n,
                                         out, &nout, rx, &rxsz);
  if (got) *got = (uint32_t)out[3];
  if (sr_before) *sr_before = out[1];
  if (sr_after) *sr_after = out[2];
  if (waited) *waited = out[5];
  return kr ? kr : (kern_return_t)out[0];
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
  io_connect_t conn = open_flash_retry();
  if (!conn) { puts("no management function with an attached dext"); return 2; }

  // A device in deep power-down ignores every command except 0xab, Release from
  // Deep Power-Down, and a reset-enable/reset pair (0x66/0x99) brings one out of
  // a stuck quad or XIP mode. Neither alters a single flash byte. Only sent when
  // asked, so the plain RDID stays a pure observation.
  const bool wake = argc > 1 && strcmp(argv[1], "--wake") == 0;
  if (wake) {
    for (uint32_t slave = 0; slave < 2; ++slave) {
      uint8_t rx[4] = {};
      uint32_t got = 0;
      uint8_t res_cmd[1] = { 0xab };                  // release from deep power-down
      xfer(conn, slave, res_cmd, 1, rx, &got, nullptr, nullptr, nullptr);
      uint8_t rst_en[1] = { 0x66 };                   // reset enable
      xfer(conn, slave, rst_en, 1, rx, &got, nullptr, nullptr, nullptr);
      uint8_t rst[1] = { 0x99 };                      // reset device
      xfer(conn, slave, rst, 1, rx, &got, nullptr, nullptr, nullptr);
      std::printf("slave %u: sent 0xab release, then 0x66/0x99 reset\n", slave);
    }
  }

  int answered = 0;
  for (uint32_t slave = 0; slave < 2; ++slave) {
    // 0x9f then five dummies: the identity is five bytes after the command.
    uint8_t tx[6] = { 0x9f, 0, 0, 0, 0, 0 };
    uint8_t rx[6] = {};
    uint32_t got = 0;
    uint64_t sr0 = 0, sr1 = 0, us = 0;
    kern_return_t kr = xfer(conn, slave, tx, sizeof(tx), rx, &got, &sr0, &sr1, &us);
    std::printf("slave %u: ", slave);
    if (kr != kIOReturnSuccess) {
      std::printf("transaction failed 0x%x (SPISR before 0x%llx)\n", kr, (unsigned long long)sr0);
      continue;
    }
    std::printf("RDID");
    for (uint32_t i = 0; i < got; ++i)
      std::printf(" %02x", rx[i]);
    std::printf("   (%u bytes, SPISR 0x%llx -> 0x%llx, %llu us)\n",
                got, (unsigned long long)sr0, (unsigned long long)sr1, (unsigned long long)us);
    // Micron MT25QU512: manufacturer 0x20, memory type 0xbb (1.8 V), capacity 0x20.
    const uint8_t* id = rx + 1;              // skip the byte clocked out with the command
    if (got >= 4 && id[0] == 0x20 && id[1] == 0xbb) {
      std::printf("          Micron, 1.8 V, capacity code 0x%02x", id[2]);
      if (id[2] == 0x20) std::printf(" = 512 Mbit (64 MiB)");
      std::printf("\n");
      ++answered;
    }
    else if (got >= 2 && (id[0] == 0x00 || id[0] == 0xff)) {
      std::printf("          nothing is driving the bus: this chip is not answering\n");
    }
    else {
      std::printf("          unrecognised manufacturer 0x%02x\n", id[0]);
    }
  }
  std::printf("\n%d of 2 chips answered%s\n", answered,
              answered == 2 ? " - the pair is complete, writing is permitted by the project's gate"
                            : " - DO NOT WRITE: an x8 image striped across the pair needs both");
  IOServiceClose(conn);
  return answered == 2 ? 0 : 1;
}
