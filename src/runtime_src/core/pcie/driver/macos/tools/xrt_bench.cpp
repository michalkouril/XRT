// SPDX-License-Identifier: Apache-2.0
// Same-code comparison of an Alveo card on macOS and Linux:
//  1. hello kernel: synchronous run+wait latency
//  2. hello kernel: throughput with 64 commands in flight
//  3. xrt::bo sync bandwidth to and from device memory
//
// With --exclusive the hardware context takes the compute units exclusively,
// which on macOS lets the driver map each CU's register page into the process
// read-write, so a command is started with stores instead of a call into the
// extension. The default, as in XRT generally, is a shared context.
//
// usage: xrt_bench [--exclusive] <bdf> <xclbin with hello kernel>
#include <xrt/xrt_bo.h>
#include <xrt/xrt_device.h>
#include <xrt/xrt_hw_context.h>
#include <xrt/xrt_kernel.h>

#include <sys/resource.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;

static double
cpu_seconds()
{
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
}

static double
secs(clk::time_point t0)
{
  return std::chrono::duration<double>(clk::now() - t0).count();
}

int
main(int argc, char** argv)
{
  bool exclusive = argc > 1 && std::string(argv[1]) == "--exclusive";
  int a = exclusive ? 2 : 1;
  if (argc < a + 2) {
    std::fprintf(stderr, "usage: %s [--exclusive] <bdf> <xclbin>\n", argv[0]);
    return 2;
  }
  xrt::device dev{std::string(argv[a])};
  auto uuid = dev.register_xclbin(xrt::xclbin{std::string(argv[a + 1])});
  xrt::hw_context ctx{dev, uuid, exclusive ? xrt::hw_context::access_mode::exclusive
                                           : xrt::hw_context::access_mode::shared};
  std::printf("hardware context      : %s\n", exclusive ? "exclusive" : "shared");
  xrt::kernel k{ctx, "hello"};
  xrt::bo buf(ctx, 1024, k.group_id(0));

  // 1. latency
  for (int i = 0; i < 100; ++i)
    k(buf).wait();
  const int n = 20000;
  double c0 = cpu_seconds();
  auto t0 = clk::now();
  for (int i = 0; i < n; ++i)
    k(buf).wait();
  double t = secs(t0), c = cpu_seconds() - c0;
  std::printf("hello run+wait        : %8.1f us per run, CPU %3.0f%% of a core\n", t * 1e6 / n, 100 * c / t);

  // 2. throughput, 64 in flight
  const int depth = 64, total = 100000;
  std::vector<xrt::run> runs;
  for (int i = 0; i < depth; ++i)
    runs.emplace_back(k), runs.back().set_arg(0, buf);
  c0 = cpu_seconds();
  t0 = clk::now();
  int issued = 0;
  for (auto& r : runs)
    r.start(), ++issued;
  for (int done = 0; done < total;) {
    for (auto& r : runs) {
      r.wait();
      ++done;
      if (issued < total)
        r.start(), ++issued;
    }
  }
  t = secs(t0);
  c = cpu_seconds() - c0;
  std::printf("hello, 64 in flight   : %8.0f runs/s, CPU %3.0f%% of a core\n", total / t, 100 * c / t);

  // 3. DMA
  for (size_t mib : {1, 16, 256}) {
    size_t bytes = mib << 20;
    xrt::bo bo(ctx, bytes, k.group_id(0));
    std::fill_n(bo.map<char*>(), bytes, 0x5a);
    int reps = std::max<int>(4, 2048 / mib);
    bo.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    t0 = clk::now();
    for (int i = 0; i < reps; ++i)
      bo.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    double to = bytes * double(reps) / secs(t0) / 1e6;
    t0 = clk::now();
    for (int i = 0; i < reps; ++i)
      bo.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
    double from = bytes * double(reps) / secs(t0) / 1e6;
    std::printf("bo sync %4zu MiB      : %8.0f MB/s to card, %6.0f MB/s from card\n", mib, to, from);
  }
  return 0;
}
