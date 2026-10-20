#include <IOKit/IOKitLib.h>
#include <cstdio>
#include <vector>
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
    uint8_t u[16]; sz = 16; kern_return_t kr = IOConnectCallStructMethod(c, XRT_DEXT_GET_XCLBIN_UUID, nullptr, 0, u, &sz);
    printf("uuid kr 0x%x: ", kr); for (int i = 0; i < 16; ++i) printf("%02x", u[i]); printf("\n");
    for (uint64_t s = 0; s < XRT_SECTION_SLOTS; ++s) {
      std::vector<char> b(XRT_SECTION_MAX_BYTES); size_t n = b.size();
      uint64_t in[2] = {s, 0}, tot = 0; uint32_t no = 1; n = XRT_SECTION_CHUNK;
      kr = IOConnectCallMethod(c, XRT_DEXT_GET_SECTION, in, 2, nullptr, 0, &tot, &no, b.data(), &n);
      if (!kr) n = tot;
      printf("section %llu: kr 0x%x, %zu bytes\n", s, kr, kr ? 0 : n);
    }
    IOServiceClose(c);
  }
}
