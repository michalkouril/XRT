// SPDX-License-Identifier: Apache-2.0
// Prints the register map each function's dext is using (XRT_DEXT_GET_PLATFORM).
//
// Two things worth checking with it. On an XDMA 5.x card the map must equal the
// offsets that used to be compiled in, because those are still the defaults and
// nothing should have moved. On a dynamic-IP card such as the U50 the map must
// be the one the shim read out of the platform package - ICAP at 0x1e88000
// rather than 0x20000 - and the blocks that platform does not have must be
// reported absent rather than pointing at an XDMA 5.x offset.
//
// Read-only: it touches no registers, only the dext's own bookkeeping.
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstring>
#include "../XrtUser/xrt_dext_abi.h"

namespace {

const char*
blk_name(uint32_t id)
{
  switch (id) {
  case XRT_BLK_FEATURE_ROM: return "feature ROM";
  case XRT_BLK_ICAP:        return "ICAP";
  case XRT_BLK_GATE_ULP:    return "ULP gate";
  case XRT_BLK_MEMCALIB:    return "memory calibration";
  case XRT_BLK_CLKWIZ_K1:   return "clock wizard 1";
  case XRT_BLK_CLKWIZ_K2:   return "clock wizard 2";
  case XRT_BLK_SYSMON:      return "SYSMON";
  case XRT_BLK_UUID_ROM:    return "UUID ROM";
  case XRT_BLK_CMC_REGMAP:  return "CMC regmap";
  default:
    if (id >= XRT_BLK_FIREWALL_0 && id <= XRT_BLK_FIREWALL_7) {
      static char buf[32];
      std::snprintf(buf, sizeof(buf), "firewall %u", id - XRT_BLK_FIREWALL_0);
      return buf;
    }
    return "?";
  }
}

bool
parent_bdf(io_service_t svc, unsigned& bus, unsigned& dev, unsigned& func)
{
  io_registry_entry_t parent = IO_OBJECT_NULL;
  if (IORegistryEntryGetParentEntry(svc, kIOServicePlane, &parent) != KERN_SUCCESS)
    return false;
  bool ok = false;
  if (auto prop = IORegistryEntryCreateCFProperty(parent, CFSTR("pcidebug"), kCFAllocatorDefault, 0)) {
    char buf[64] = {0};
    if (CFGetTypeID(prop) == CFStringGetTypeID()
        && CFStringGetCString((CFStringRef)prop, buf, sizeof(buf), kCFStringEncodingUTF8))
      ok = std::sscanf(buf, "%u:%u:%u", &bus, &dev, &func) == 3;
    CFRelease(prop);
  }
  IOObjectRelease(parent);
  return ok;
}

} // namespace

int
main()
{
  io_iterator_t it = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it) != KERN_SUCCESS) {
    std::printf("no %s service\n", XRT_DEXT_USER_CLASS);
    return 1;
  }
  int functions = 0;
  while (io_service_t svc = IOIteratorNext(it)) {
    unsigned bus = 0, dev = 0, func = 0;
    if (parent_bdf(svc, bus, dev, func)) {
      io_connect_t conn = IO_OBJECT_NULL;
      if (IOServiceOpen(svc, mach_task_self(), 0, &conn) == KERN_SUCCESS) {
        xrt_dext_platform plat = {};
        size_t size = sizeof(plat);
        kern_return_t kr = IOConnectCallStructMethod(conn, XRT_DEXT_GET_PLATFORM,
                                                     nullptr, 0, &plat, &size);
        if (kr == KERN_SUCCESS) {
          ++functions;
          std::printf("\n%02x:%02x.%x  abi %u  %s\n", bus, dev, func, plat.abi,
                      (plat.flags & XRT_PLATFORM_DEFAULT)
                        ? "compiled-in XDMA 5.x defaults"
                        : "installed from this platform's own description");
          std::printf("  %-20s %-6s %-12s %-10s\n", "block", "BAR", "offset", "size");
          std::printf("  CU window: 0x%llx + 0x%llx%s\n",
                      plat.cu_window_offset, plat.cu_window_size,
                      (plat.cu_window_offset == XRT_CU_WINDOW_BASE
                       && plat.cu_window_size == XRT_CU_WINDOW_END - XRT_CU_WINDOW_BASE)
                        ? "  (the default)" : "  (moved for the loaded xclbin)");
          std::printf("  card memory: 0x%llx..0x%llx%s\n", plat.card_min, plat.card_max,
                      (plat.card_min == XRT_DMA_CARD_MIN && plat.card_max == XRT_DMA_CARD_MAX)
                        ? "  (the default)" : "  (from the loaded xclbin)");
          for (uint32_t i = 0; i < XRT_BLK_MAX; ++i) {
            const auto& b = plat.blk[i];
            if (!b.size)
              std::printf("  %-20s %-6s %-12s %-10s\n", blk_name(i), "-", "absent", "-");
            else
              std::printf("  %-20s %-6u 0x%-10llx 0x%-8llx (function %u)\n",
                          blk_name(i), b.bar, b.offset, b.size, b.func);
          }
        }
        else {
          std::printf("\n%02x:%02x.%x  GET_PLATFORM failed 0x%x"
                      " (a dext older than ABI %u has no register map)\n",
                      bus, dev, func, kr, XRT_DEXT_ABI_VERSION);
        }
        IOServiceClose(conn);
      }
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  if (!functions)
    std::printf("no function answered\n");
  return functions ? 0 : 1;
}
