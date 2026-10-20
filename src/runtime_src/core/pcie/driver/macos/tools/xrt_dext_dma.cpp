// SPDX-License-Identifier: Apache-2.0
// Phase 4 test: XDMA H2C + C2H round trip through the xrt_user dext.
// Usage: xrt_dext_dma <card address hex> <bytes> [seed]
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include <cstdlib>
#include "../XrtUser/xrt_dext_abi.h"
int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: %s <card-addr-hex> <bytes> [seed]\n", argv[0]); return 2; }
  uint64_t args[3] = { std::strtoull(argv[1], nullptr, 16), std::strtoull(argv[2], nullptr, 0),
                       argc > 3 ? std::strtoull(argv[3], nullptr, 0) : 1 };
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it))
    return 1;
  int rc = 1;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_connect_t conn; if (IOServiceOpen(svc, mach_task_self(), 0, &conn)) continue;
    xrt_dext_device_info info{}; size_t sz = sizeof(info);
    // A discarded status leaves info zeroed, which would read as func 0.
    if (IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &sz)
        || info.func != 1) { IOServiceClose(conn); continue; }
    xrt_dext_dma_result r{}; size_t rsz = sizeof(r);
    kern_return_t kr = IOConnectCallMethod(conn, XRT_DEXT_DMA_SELFTEST, args, 3, nullptr, 0, nullptr, nullptr, &r, &rsz);
    IOServiceClose(conn);
    if (kr) { std::printf("DMA_SELFTEST call failed 0x%x\n", kr); break; }
    static const char* steps[] = {"setup", "H2C", "C2H", "compare", "complete"};
    std::printf("status 0x%x, reached '%s'; card 0x%llx, %u bytes, %u host segment(s), host IOVA 0x%llx, desc IOVA 0x%llx, PCI cmd 0x%04x\n",
                r.status, r.step <= 4 ? steps[r.step] : "?", (unsigned long long)r.card_addr, r.length, r.segments,
                (unsigned long long)r.host_iova, (unsigned long long)r.desc_iova, r.pci_command);
    std::printf("  H2C status 0x%08x, %u desc done, %u us", r.h2c_status, r.h2c_done, r.h2c_us);
    if (r.h2c_us) std::printf(" (%.1f MB/s)", r.length / (double)r.h2c_us);
    std::printf("\n  C2H status 0x%08x, %u desc done, %u us", r.c2h_status, r.c2h_done, r.c2h_us);
    if (r.c2h_us) std::printf(" (%.1f MB/s)", r.length / (double)r.c2h_us);
    std::printf("\n  compare: %u mismatched words", r.mismatches);
    if (r.mismatches) std::printf(" (first at word %u: expected 0x%08x got 0x%08x)", r.first_bad_index, r.first_bad_expected, r.first_bad_actual);
    std::printf("\n");
    rc = r.status ? 1 : 0;
  }
  IOObjectRelease(it);
  return rc;
}
