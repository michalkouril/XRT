// Read the hello_1 CU registers (user BAR0 + 0x1800000) through the dext's
// read-only mapping of the user function.
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
static int g_argc; static char** g_argv;
#include "xrt_dext_abi.h"
int main(int argc, char** argv) {
  g_argc = argc; g_argv = argv;
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
    if (IOConnectMapMemory64(conn, 0, mach_task_self(), &a, &l, kIOMapAnywhere | kIOMapInhibitCache)) { puts("map failed"); return 1; }
    // Usage: cu_peek [hex-offset ...]  (default: 0x1800000, hello_1 in verify.xclbin)
    std::vector<uint32_t> offs;
    for (int i = 1; i < g_argc; ++i) offs.push_back(std::strtoul(g_argv[i], nullptr, 16));
    if (offs.empty()) offs.push_back(0x1800000);
    for (auto off : offs) {
      auto cu = reinterpret_cast<volatile uint32_t*>(a + off);
      uint32_t c = cu[0];
      printf("CU @ user BAR0+0x%x  ap_ctrl 0x%08x : start=%u done=%u idle=%u ready=%u  GIE 0x%x IER 0x%x ISR 0x%x\n",
             off, c, c & 1, (c >> 1) & 1, (c >> 2) & 1, (c >> 3) & 1, cu[1], cu[2], cu[3]);
    }
    IOConnectUnmapMemory64(conn, 0, mach_task_self(), a); IOServiceClose(conn);
  }
}
