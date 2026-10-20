// Read-only survey of the XDMA register BAR (user function BAR1).
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
        || info.func != 1) { IOServiceClose(conn); continue; }
    mach_vm_address_t a = 0; mach_vm_size_t l = 0;
    if (IOConnectMapMemory64(conn, 1, mach_task_self(), &a, &l, kIOMapAnywhere | kIOMapInhibitCache)) { puts("map BAR1 failed"); return 1; }
    auto r = [&](uint32_t off) { return reinterpret_cast<volatile uint32_t*>(a)[off / 4]; };
    printf("BAR1 %llu KiB, PCI command 0x%04x\n", (unsigned long long)l / 1024, info.command);
    for (int dir = 0; dir < 2; ++dir)
      for (int ch = 0; ch < 4; ++ch) {
        uint32_t base = dir * 0x1000 + ch * 0x100, id = r(base);
        if ((id >> 20) != 0x1fc) continue;
        printf("  %s%d @0x%04x id 0x%08x ctrl 0x%08x status 0x%08x done %u align 0x%08x | sgdma id 0x%08x\n",
               dir ? "C2H" : "H2C", ch, base, id, r(base + 0x4), r(base + 0x40), r(base + 0x48), r(base + 0x4c),
               r(base + 0x4000));
      }
    printf("  IRQ block 0x2000 id 0x%08x, config block 0x3000 id 0x%08x\n", r(0x2000), r(0x3000));
    IOConnectUnmapMemory64(conn, 1, mach_task_self(), a); IOServiceClose(conn);
  }
}
