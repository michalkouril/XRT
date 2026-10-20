// Check that XRT_DEXT_SET_PLATFORM refuses a map that tries to place the flash
// block. That refusal is what stops XRT_DEXT_SPI_REG from becoming an arbitrary
// write anywhere in the management BAR: the selector bounds an offset against the
// flash block's base, so whoever chooses that base chooses the reach.
//
// This only exercises the REFUSAL, which installs nothing and leaves the driver's
// block table exactly as it was. It deliberately does not test the accept path,
// because that would install a map on a card whose shell we do not own.
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include "xrt_dext_abi.h"

static io_connect_t
open_func(uint8_t want)
{
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it))
    return 0;
  io_connect_t found = 0;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_connect_t conn = 0;
    if (!IOServiceOpen(svc, mach_task_self(), 0, &conn)) {
      xrt_dext_device_info info{}; size_t sz = sizeof(info);
      if (!IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &sz)
          && info.func == want) { found = conn; IOObjectRelease(svc); break; }
      IOServiceClose(conn);
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  return found;
}

int
main()
{
  io_connect_t conn = open_func(0);
  if (!conn) { puts("no management function with an attached dext"); return 2; }
  int fail = 0;

  // A map that is otherwise well formed but places the flash block over the ICAP.
  // If this were accepted, SPI_REG writes would land on the ICAP.
  xrt_dext_platform p{};
  p.abi = XRT_DEXT_ABI_VERSION;
  p.count = XRT_BLK_MAX;
  p.blk[XRT_BLK_FLASH] = { 0x20000, 0x1000, 0, 0, {} };
  kern_return_t kr = IOConnectCallStructMethod(conn, XRT_DEXT_SET_PLATFORM, &p, sizeof(p), nullptr, nullptr);
  std::printf("map placing the flash block over the ICAP: %s (0x%x)\n",
              kr ? "refused" : "ACCEPTED - BUG", kr);
  if (!kr) fail++;

  // And one placing it somewhere harmless-looking, to show the refusal is about
  // the block and not about the address.
  xrt_dext_platform q{};
  q.abi = XRT_DEXT_ABI_VERSION;
  q.count = XRT_BLK_MAX;
  q.blk[XRT_BLK_FLASH] = { 0x40000, 0x1000, 0, 0, {} };
  kr = IOConnectCallStructMethod(conn, XRT_DEXT_SET_PLATFORM, &q, sizeof(q), nullptr, nullptr);
  std::printf("map placing it at the real candidate 0x40000:  %s (0x%x)\n",
              kr ? "refused" : "ACCEPTED - BUG", kr);
  if (!kr) fail++;

  // The block table must be untouched: a refusal installs nothing.
  xrt_dext_platform got{};
  size_t sz = sizeof(got);
  if (IOConnectCallStructMethod(conn, XRT_DEXT_GET_PLATFORM, nullptr, 0, &got, &sz)) {
    puts("GET_PLATFORM failed"); fail++;
  } else {
    bool dflt = (got.flags & XRT_PLATFORM_DEFAULT) != 0;
    std::printf("block table after the refusals: %s\n",
                dflt ? "still the compiled-in defaults (nothing installed)"
                     : "A MAP IS INSTALLED - BUG");
    if (!dflt) fail++;
  }

  IOServiceClose(conn);
  std::printf("\n%d check(s) failed\n", fail);
  return fail ? 1 : 0;
}
