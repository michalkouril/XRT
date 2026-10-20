// Read-only: MicroBlaze reset GPIO and status on the management BAR.
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include "xrt_dext_abi.h"
int main() {
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it))
    return 1;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_connect_t c; if (IOServiceOpen(svc, mach_task_self(), 0, &c)) continue;
    xrt_dext_device_info info{}; size_t sz = sizeof(info);
    // A discarded status leaves info zeroed, which would read as func 0.
    if (IOConnectCallStructMethod(c, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &sz)
        || info.func != 0) { IOServiceClose(c); continue; }
    mach_vm_address_t a = 0; mach_vm_size_t l = 0;
    IOConnectMapMemory64(c, 0, mach_task_self(), &a, &l, kIOMapAnywhere | kIOMapInhibitCache);
    auto r = [&](uint32_t off) { return reinterpret_cast<volatile uint32_t*>(a)[off / 4]; };
    printf("MB reset GPIO @0x131000: 0x%x (0 = held in reset, 1 = running)\n", r(0x131000));
    printf("MB regs @0x120000: magic 0x%x id 0x%x status 0x%x err 0x%x\n", r(0x120000), r(0x120004), r(0x120008), r(0x12000c));
    printf("MB mgmt image word0 @0x140000: 0x%08x\n", r(0x140000));
    IOServiceClose(c);
  }
}
