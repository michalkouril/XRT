// SPDX-License-Identifier: Apache-2.0
// Is the ERT hardware there, and is its MicroBlaze running?
//
// Read-only reconnaissance before any attempt to load the scheduler firmware.
// On the XDMA 5.x shells (xocl devices.h __RES_XMC, and ert.h):
//   mgmt BAR0 + 0x131000   reset GPIO (shared with the XMC on these shells)
//   mgmt BAR0 + 0x160000   the MicroBlaze's instruction RAM, 128 KiB
//   user BAR0 + 0x180000   ERT status/CSR registers
//   user BAR0 + 0x190000   the command queue, 64 KiB
//
// An unloaded MicroBlaze holds a cold-boot self-jump at address 0, which xocl
// recognises as (instruction & 0xfc00ffff) == 0xb8000000. That is the cheapest
// way to tell "no firmware" from "firmware running".
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstring>
#include "../XrtUser/xrt_dext_abi.h"
#include "xclfeatures.h"

namespace {

constexpr uint32_t kGpio      = 0x131000;
constexpr uint32_t kImageSched = 0x160000;
constexpr uint32_t kCsr       = 0x180000;
constexpr uint32_t kCq        = 0x190000;

bool
self_jump(uint32_t ins)
{
  return (ins & 0xfc00ffff) == 0xb8000000;
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

bool
filler(uint32_t v)
{
  return v == 0xffffffff || v == 0xdeadfa11 || v == 0xdec0de1c;
}

volatile uint32_t*
map0(io_connect_t conn, uint64_t* len)
{
  mach_vm_address_t a = 0;
  mach_vm_size_t l = 0;
  if (IOConnectMapMemory64(conn, XRT_DEXT_MAP_BAR(0), mach_task_self(), &a, &l,
                           kIOMapAnywhere | kIOMapInhibitCache) != KERN_SUCCESS)
    return nullptr;
  *len = l;
  return reinterpret_cast<volatile uint32_t*>(a);
}

} // namespace

int
main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  io_iterator_t it = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it) != KERN_SUCCESS) {
    std::printf("no %s service\n", XRT_DEXT_USER_CLASS);
    return 1;
  }
  while (io_service_t svc = IOIteratorNext(it)) {
    unsigned bus = 0, dev = 0, func = 0;
    if (parent_bdf(svc, bus, dev, func)) {
      io_connect_t conn = IO_OBJECT_NULL;
      if (IOServiceOpen(svc, mach_task_self(), 0, &conn) == KERN_SUCCESS) {
        uint64_t len = 0;
        auto bar = map0(conn, &len);
        if (bar) {
          std::printf("\n%02x:%02x.%x  BAR0 %llu MiB\n", bus, dev, func, len / (1024 * 1024));
          // The CQ and CSR are listed on both functions in xocl's resource
          // tables; whether both can actually reach them decides whether ERT
          // bring-up needs one dext or two.
          if (kCsr + 0x68 <= len)
            std::printf("  CSR  +0x%06x = 0x%08x   CQ +0x%06x = 0x%08x  %s\n",
                        kCsr, bar[kCsr / 4], kCq, bar[kCq / 4],
                        filler(bar[kCsr / 4]) ? "(filler: not reachable here)" : "(reachable)");
          if (func == 0) {
            // The decisive question: does this shell declare a scheduler at all?
            // xocl gates loading the firmware on the feature ROM's MB_SCHEDULER
            // bit (feature_rom.c mb_sched_on), so a shell without it has the
            // BRAM but no MicroBlaze to execute from it.
            uint8_t rom[XRT_DEXT_ROM_SIZE] = {};
            size_t rsz = sizeof(rom);
            if (IOConnectCallStructMethod(conn, XRT_DEXT_READ_ROM, nullptr, 0, rom, &rsz)
                  == KERN_SUCCESS && rsz >= sizeof(FeatureRomHeader)) {
              FeatureRomHeader h;
              std::memcpy(&h, rom, sizeof(h));
              std::printf("  feature ROM FeatureBitMap = 0x%016llx\n",
                          (unsigned long long)h.FeatureBitMap);
              std::printf("    MB_SCHEDULER (0x8)   : %s\n",
                          (h.FeatureBitMap & 0x8) ? "SET - this shell has an ERT MicroBlaze"
                                                  : "CLEAR - this shell has NO ERT MicroBlaze");
              std::printf("    BOARD_MGMT_ENBLD(0x4): %s\n",
                          (h.FeatureBitMap & 0x4) ? "set" : "clear");
            }
            if (kGpio + 4 <= len) {
              uint32_t g = bar[kGpio / 4];
              std::printf("  reset GPIO   +0x%06x = 0x%08x %s\n", kGpio, g,
                          filler(g) ? "(filler: nothing there)"
                          : g == 0 ? "(held in reset)" : "(released)");
            }
            if (kImageSched + 32 <= len) {
              std::printf("  MicroBlaze instruction RAM +0x%06x:\n", kImageSched);
              for (int i = 0; i < 4; ++i) {
                uint32_t w = bar[kImageSched / 4 + i];
                std::printf("    +%02x = 0x%08x%s\n", i * 4, w,
                            i == 0 ? (filler(w) ? "  (filler: no RAM there)"
                                      : self_jump(w) ? "  <- cold-boot self jump: NO firmware loaded"
                                                     : "  <- not a self jump: something is loaded")
                                   : "");
              }
            }
          }
          else {
            if (kCsr + 0x68 <= len) {
              std::printf("  ERT CSR +0x%06x: status 0x%08x 0x%08x 0x%08x 0x%08x\n", kCsr,
                          bar[(kCsr + 0x00) / 4], bar[(kCsr + 0x04) / 4],
                          bar[(kCsr + 0x08) / 4], bar[(kCsr + 0x0c) / 4]);
              std::printf("           CU status 0x%08x, CQ status 0x%08x\n",
                          bar[(kCsr + 0x44) / 4], bar[(kCsr + 0x58) / 4]);
            }
            if (kCq + 32 <= len) {
              std::printf("  command queue +0x%06x: ", kCq);
              for (int i = 0; i < 4; ++i)
                std::printf("0x%08x ", bar[kCq / 4 + i]);
              std::printf("\n");
              uint32_t w0 = bar[kCq / 4];
              std::printf("           %s\n", filler(w0) ? "filler: no queue there"
                                                        : "the queue is readable");
            }
          }
          IOConnectUnmapMemory64(conn, XRT_DEXT_MAP_BAR(0), mach_task_self(),
                                 reinterpret_cast<mach_vm_address_t>(bar));
        }
        IOServiceClose(conn);
      }
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  return 0;
}
