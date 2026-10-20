// Fixed cost of a bo sync, as a function of size.
//
// The point is the intercept, not the slope: extrapolating the per-transfer
// time back to zero bytes gives the fixed cost per sync, which is what items
// 10, 44 and 47 are about. Small sizes carry that signal, so the sweep starts
// at 4 KiB rather than 1 MiB.
//
// usage: sync_parts <bdf> <xclbin> [kernel name]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "xrt/xrt_device.h"
#include "xrt/xrt_bo.h"
#include "xrt/xrt_kernel.h"
#include "xrt/xrt_hw_context.h"

using clk = std::chrono::steady_clock;
static double secs(clk::time_point t0)
{
  return std::chrono::duration<double>(clk::now() - t0).count();
}

int
main(int argc, char** argv)
{
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <bdf> <xclbin> [kernel]\n", argv[0]);
    return 2;
  }
  setvbuf(stdout, nullptr, _IONBF, 0);   // so a hang shows where it hung
  xrt::device dev{std::string(argv[1])};
  auto uuid = dev.register_xclbin(xrt::xclbin{std::string(argv[2])});
  xrt::hw_context ctx{dev, uuid};

  // A kernel only to borrow its memory group; any kernel in the xclbin will do.
  std::string kname = argc > 3 ? argv[3] : "";
  xrt::kernel k = kname.empty() ? xrt::kernel{ctx, xrt::xclbin{std::string(argv[2])}.get_kernels().at(0).get_name()}
                                : xrt::kernel{ctx, kname};
  int grp = k.group_id(0);

  struct size_entry { const char* name; size_t bytes; };
  size_entry sizes[] = {
    { "4 KiB",   4u << 10 }, { "16 KiB",  16u << 10 }, { "64 KiB",  64u << 10 },
    { "256 KiB", 256u << 10 }, { "1 MiB",   1u << 20 }, { "4 MiB",   4u << 20 },
    { "16 MiB",  16u << 20 },
  };
  // A single size in KiB, so a dext-side profile log line can be attributed to
  // one size instead of having to be matched up against the whole sweep.
  size_entry one[1] = {};
  char onename[32] = {};
  size_t nsizes = sizeof(sizes) / sizeof(sizes[0]);
  if (const char* kib = getenv("SYNC_PARTS_KIB")) {
    size_t k = strtoul(kib, nullptr, 10);
    std::snprintf(onename, sizeof(onename), "%zu KiB", k);
    one[0].name = onename;
    one[0].bytes = k << 10;
  }
  std::printf("%-8s %12s %12s %12s %12s\n", "size", "to us", "from us", "to MB/s", "from MB/s");
  for (size_t si = 0; si < (one[0].bytes ? 1 : nsizes); ++si) {
    auto& s = one[0].bytes ? one[0] : sizes[si];
    xrt::bo bo(ctx, s.bytes, grp);
    // Enough rounds that the timer noise is irrelevant, fewer as they get slow.
    int n = s.bytes >= (4u << 20) ? 100 : 2000;
    for (int i = 0; i < 20; ++i) { bo.sync(XCL_BO_SYNC_BO_TO_DEVICE); bo.sync(XCL_BO_SYNC_BO_FROM_DEVICE); }
    auto t0 = clk::now();
    for (int i = 0; i < n; ++i) bo.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    double to = secs(t0) * 1e6 / n;
    t0 = clk::now();
    for (int i = 0; i < n; ++i) bo.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
    double fr = secs(t0) * 1e6 / n;
    std::printf("%-8s %12.1f %12.1f %12.0f %12.0f\n", s.name, to, fr,
                s.bytes / to, s.bytes / fr);
  }
  return 0;
}
