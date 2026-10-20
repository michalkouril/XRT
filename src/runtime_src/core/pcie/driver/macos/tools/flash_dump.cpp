// Dump a range of the boot flash to a file, read-only, one chip at a time.
//
// Read-only by construction: the only commands issued are 0x13 (READ with a
// 4-byte address) and, once, 0xab (release from deep power-down, which alters no
// stored byte). There is no write-enable, erase or program path in this file.
//
// Also prints "slave off sha1 len" lines per block, the same shape as the shell
// project's qspi_scan.py, so a dump taken here can be compared against one taken
// there - while noting that project's standing rule: the Linux DRIVER read path
// lies, so only raw-SPI dumps are comparable.
//
// usage: flash_dump <slave> <start-hex> <len-hex> <outfile> [blocksize-hex]
#include <IOKit/IOKitLib.h>
#include <CommonCrypto/CommonDigest.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <ctime>
#include "xrt_dext_abi.h"

static const uint32_t kChunk = XRT_SPI_MAX_XFER - 5;   // 251: the FIFO minus cmd+address

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

static bool
xfer(io_connect_t conn, uint32_t slave, const uint8_t* tx, uint32_t n, uint8_t* rx)
{
  uint64_t in[2] = { slave, n };
  uint64_t out[6] = {};
  uint32_t nout = 6;
  size_t rxsz = n;
  kern_return_t kr = IOConnectCallMethod(conn, XRT_DEXT_SPI_XFER, in, 2, tx, n,
                                        out, &nout, rx, &rxsz);
  return kr == KERN_SUCCESS && out[0] == 0 && out[3] == n;
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
  if (argc < 5) {
    std::fprintf(stderr, "usage: %s <slave> <start-hex> <len-hex> <outfile> [blocksize-hex]\n",
                 argv[0]);
    return 2;
  }
  const uint32_t slave = (uint32_t)strtoul(argv[1], nullptr, 0);
  const uint64_t start = strtoull(argv[2], nullptr, 0);
  const uint64_t len = strtoull(argv[3], nullptr, 0);
  const uint64_t bs = argc > 5 ? strtoull(argv[5], nullptr, 0) : 0x10000;
  if (slave > 1) { std::fprintf(stderr, "slave must be 0 or 1\n"); return 2; }

  io_connect_t conn = open_flash_retry();
  if (!conn) { std::fprintf(stderr, "no management function with an attached dext\n"); return 2; }

  // Chip 0 has twice been found in deep power-down, where it answers nothing but
  // 0xab. Release unconditionally; it alters no stored byte.
  uint8_t wake[1] = { 0xab }, junk[1] = {};
  xfer(conn, slave, wake, 1, junk);

  FILE* f = std::fopen(argv[4], "wb");
  if (!f) { std::perror("open outfile"); return 2; }

  auto* block = (uint8_t*)malloc(bs);
  uint8_t rx[XRT_SPI_MAX_XFER] = {}, tx[XRT_SPI_MAX_XFER] = {};
  uint64_t done = 0;
  int failures = 0;
  const clock_t t0 = clock();

  while (done < len) {
    const uint64_t this_block = (len - done) < bs ? (len - done) : bs;
    uint64_t in_block = 0;
    while (in_block < this_block) {
      const uint64_t addr = start + done + in_block;
      uint32_t want = (uint32_t)((this_block - in_block) < kChunk ? (this_block - in_block) : kChunk);
      tx[0] = 0x13;
      tx[1] = (uint8_t)(addr >> 24); tx[2] = (uint8_t)(addr >> 16);
      tx[3] = (uint8_t)(addr >> 8);  tx[4] = (uint8_t)addr;
      memset(tx + 5, 0, want);
      if (!xfer(conn, slave, tx, 5 + want, rx)) {
        // A failed transaction must not silently become zeros in the image.
        std::fprintf(stderr, "read failed at 0x%08llx\n", (unsigned long long)addr);
        memset(block + in_block, 0xdd, want);
        ++failures;
      }
      else {
        memcpy(block + in_block, rx + 5, want);
      }
      in_block += want;
    }
    unsigned char sha[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1(block, (CC_LONG)this_block, sha);
    std::printf("%u 0x%08llX ", slave, (unsigned long long)(start + done));
    for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; ++i) std::printf("%02x", sha[i]);
    std::printf(" %llu\n", (unsigned long long)this_block);
    std::fflush(stdout);
    std::fwrite(block, 1, this_block, f);
    done += this_block;
  }
  std::fclose(f);
  free(block);
  const double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
  std::fprintf(stderr, "read %llu B in %.1f s (%.2f MB/s), %d failed transaction(s)\n",
               (unsigned long long)done, secs, done / 1e6 / (secs > 0 ? secs : 1), failures);
  IOServiceClose(conn);
  return failures ? 1 : 0;
}
