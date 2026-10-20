// AXI firewall status on the management BAR.
// Tripped = any bit besides READ/WRITE_RESPONSE_BUSY (bits 0 and 16).
//
// The offsets come from the dext's platform table, not from the XDMA 5.x
// constants: a U50 puts its firewalls at 0x1f02000, 0x44000 and elsewhere, and
// reading the legacy offsets there returns filler - 0xdec0de1c - which looks
// exactly like four tripped firewalls. That false alarm is why this asks.
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include "xrt_dext_abi.h"
int main() {
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it))
    return 1;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_connect_t conn; if (IOServiceOpen(svc, mach_task_self(), 0, &conn)) continue;
    xrt_dext_device_info info{}; size_t sz = sizeof(info);
    // A discarded status leaves info zeroed, which would read as func 0.
    if (IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &sz)
        || info.func != 0) { IOServiceClose(conn); continue; }
    mach_vm_address_t a = 0; mach_vm_size_t l = 0;
    if (IOConnectMapMemory64(conn, 0, mach_task_self(), &a, &l, kIOMapAnywhere | kIOMapInhibitCache)) { puts("map failed"); return 1; }
    auto bar = reinterpret_cast<volatile uint32_t*>(a);
    // Where this card's firewalls actually are.
    uint32_t fw[XRT_MGMT_FIREWALLS] = {};
    uint32_t nfw = 0;
    xrt_dext_platform plat{}; size_t psz = sizeof(plat);
    if (IOConnectCallStructMethod(conn, XRT_DEXT_GET_PLATFORM, nullptr, 0, &plat, &psz) == KERN_SUCCESS) {
      for (uint32_t i = 0; i < XRT_MGMT_FIREWALLS; ++i) {
        const auto& blk = plat.blk[XRT_BLK_FIREWALL_0 + i];
        if (blk.size)
          fw[nfw++] = static_cast<uint32_t>(blk.offset);
      }
    }
    if (!nfw) {
      const uint32_t legacy[] = {0xD0000, 0xE0000, 0xE1000, 0xF0000};   // XOCL_RES_AF_DSA52
      for (auto b : legacy) fw[nfw++] = b;
      puts("  (no platform table; falling back to the XDMA 5.x offsets)");
    }
    // The filler a read of an unmapped window returns. All have bit 0 set, so a
    // plain "any bit set" test mistakes them for a fault.
    auto is_filler = [](uint32_t v) {
      return v == 0xffffffffu || v == 0xdeadfa11u || v == 0xdec0de1cu;
    };
    int tripped = 0, absent = 0;
    for (uint32_t i = 0; i < nfw; ++i) {
      const uint32_t b = fw[i];
      uint32_t s0 = bar[b / 4], s1 = bar[(b + 0x100) / 4];
      if (is_filler(s0)) {
        ++absent;
        printf("  firewall @0x%07x  reads filler 0x%08x - not mapped here\n", b, s0);
        continue;
      }
      bool t = ((s0 & ~0x10001u) != 0) || ((s1 & ~0x10001u) != 0 && !is_filler(s1));
      tripped += t;
      printf("  firewall @0x%07x  FAULT_STATUS 0x%08x  SI_FAULT_STATUS 0x%08x  %s\n", b, s0, s1, t ? "TRIPPED?" : "ok");
    }
    printf("%d firewall(s) flagged", tripped);
    if (absent)
      printf(", %d not mapped", absent);
    printf("\n");
    IOConnectUnmapMemory64(conn, 0, mach_task_self(), a); IOServiceClose(conn);
  }
}
