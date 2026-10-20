// SPDX-License-Identifier: Apache-2.0
// Measures how long xrt::run::wait() takes to notice a finished CU and how
// much CPU the waiting process burns, for kernels of different lengths.
// Uses bandwidth1 from the XDMA 5.x shells' test bandwidth.xclbin,
// bandwidth1(output, input, blocks, reps): copies <blocks> 64-byte blocks
// from input to output <reps> times.
//
// usage: cu_wait_test <bandwidth.xclbin> [runs]
#include <xrt/xrt_bo.h>
#include <xrt/xrt_device.h>
#include <xrt/xrt_hw_context.h>
#include <xrt/xrt_kernel.h>

#include <sys/resource.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static double
cpu_seconds()
{
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
}

int
main(int argc, char** argv)
{
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <bandwidth.xclbin> [runs]\n", argv[0]);
    return 2;
  }
  int runs = argc > 2 ? std::atoi(argv[2]) : 20;
  xrt::device dev{0};
  auto uuid = dev.register_xclbin(xrt::xclbin{std::string(argv[1])});
  xrt::hw_context ctx{dev, uuid};
  xrt::kernel k{ctx, "bandwidth1"};
  const uint32_t bytes = 1 << 20;
  const uint32_t blocks = bytes / 64;
  xrt::bo out(ctx, bytes, k.group_id(0));
  xrt::bo in(ctx, bytes, k.group_id(1));
  auto p = in.map<uint8_t*>();
  for (uint32_t i = 0; i < bytes; ++i)
    p[i] = static_cast<uint8_t>(i * 7);
  in.sync(XCL_BO_SYNC_BO_TO_DEVICE);

  std::printf("%8s %6s %12s %12s %10s\n", "reps", "runs", "run (ms)", "cpu/wall", "cpu (ms)");
  for (uint32_t reps : {1u, 10u, 100u, 1000u}) {
    auto r = k(out, in, blocks, reps);            // warm up
    r.wait();
    double c0 = cpu_seconds();
    auto t0 = std::chrono::steady_clock::now();
    int n = reps >= 1000 ? std::max(2, runs / 10) : runs;
    for (int i = 0; i < n; ++i) {
      auto run = k(out, in, blocks, reps);
      if (run.wait() != ERT_CMD_STATE_COMPLETED) {
        std::printf("run failed\n");
        return 1;
      }
    }
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double cpu = cpu_seconds() - c0;
    std::printf("%8u %6d %12.3f %11.1f%% %10.1f\n", reps, n, wall * 1e3 / n, 100 * cpu / wall, cpu * 1e3);
  }
  out.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
  auto q = out.map<uint8_t*>();
  for (uint32_t i = 0; i < bytes; ++i)
    if (q[i] != static_cast<uint8_t>(i * 7)) {
      std::printf("mismatch at %u\n", i);
      return 1;
    }
  std::printf("PASS\n");
  return 0;
}
