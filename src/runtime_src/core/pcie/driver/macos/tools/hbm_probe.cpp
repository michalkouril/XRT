// SPDX-License-Identifier: Apache-2.0
// Why does mem-bw return zeros on a U50 when verify and dma both pass?
//
// verify proves a kernel writing a buffer plus C2H DMA works; dma proves H2C
// and C2H are correct in both directions. The combination neither covers is a
// kernel READING a buffer that DMA wrote, which is what the bandwidth kernel
// does. This splits that into its two halves and reports which one fails:
//
//   1. write a pattern, sync to device, sync straight back, compare
//      -> a mismatch means DMA is not landing where it claims
//   2. run the kernel, read the output, compare
//      -> a mismatch after 1 passes means the kernel is not reading what DMA
//         wrote, i.e. the address the kernel is given is not the address DMA
//         used
//
// usage: hbm_probe <bandwidth.xclbin> [bytes] [reps]
//
// With no bytes given it sweeps 4K..16M at reps=10000, which is what mem-bw
// does, so a failure that only appears at some size or repetition count shows
// up here rather than only inside the test.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <chrono>
#include "xrt/xrt_device.h"
#include "xrt/xrt_kernel.h"
#include "xrt/xrt_bo.h"

int
main(int argc, char** argv)
{
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <bandwidth.xclbin> [bytes]\n", argv[0]);
    return 2;
  }
  unsigned bytes = (argc > 2) ? std::strtoul(argv[2], nullptr, 0) : 0;
  unsigned reps = (argc > 3) ? std::strtoul(argv[3], nullptr, 0) : 10000;

  // Unbuffered: this probe is used to find hangs, and a buffered stdout
  // redirected to a file loses everything printed before the hang.
  setvbuf(stdout, nullptr, _IONBF, 0);

  try {
    std::printf("opening device\n");
    auto device = xrt::device(0);
    std::printf("loading %s\n", argv[1]);
    auto uuid = device.load_xclbin(argv[1]);
    std::printf("loaded; making a context\n");
    xrt::hw_context ctx(device, uuid);
    auto krnl = xrt::kernel(ctx, "bandwidth:{bandwidth_1}");
    std::printf("kernel ready\n");

    auto g0 = krnl.group_id(0), g1 = krnl.group_id(1);
    std::printf("group_id(0) = %d, group_id(1) = %d, reps %u\n", g0, g1, reps);

    // Mode 2: one pair of buffers, the kernel run several times. If this
    // passes while the reallocating sweep fails, the fault is in allocation or
    // in the addresses handed out; if it fails too, it is the CU handshake
    // between consecutive runs.
    if (getenv("HBM_PROBE_REUSE")) {
      unsigned n = bytes ? bytes : 4096;
      // HBM_PROBE_BANKS=a,b puts the two buffers in specific memory indices
      // rather than letting the merged group place them both at its bottom,
      // which is where a first-fit allocator puts them - the same 256 MiB
      // pseudo-channel, read contending with write.
      int ba = g0, bb = g1;
      if (const char* e = getenv("HBM_PROBE_BANKS"))
        std::sscanf(e, "%d,%d", &ba, &bb);
      auto in2 = xrt::bo(ctx, n, ba);
      auto out2 = xrt::bo(ctx, n, bb);
      std::printf("banks %d and %d\n", ba, bb);
      std::printf("reuse mode: in 0x%llx out 0x%llx, %u bytes\n",
                  (unsigned long long)in2.address(), (unsigned long long)out2.address(), n);
      std::vector<unsigned char> pat(n), got(n);
      for (unsigned i = 0; i < n; ++i)
        pat[i] = static_cast<unsigned char>(i % 256);
      in2.write(pat.data());
      in2.sync(XCL_BO_SYNC_BO_TO_DEVICE);
      int bad_runs = 0;
      for (int r = 0; r < 4; ++r) {
        std::vector<unsigned char> zeros(n, 0);
        out2.write(zeros.data());
        out2.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        auto t0 = std::chrono::high_resolution_clock::now();
        auto run2 = krnl(in2, out2, n, reps);
        run2.wait();
        auto t1 = std::chrono::high_resolution_clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count() / (reps ? reps : 1);
        // Same arithmetic as the mem-bw test: bytes moved counted both ways.
        double mbps = (2.0 * n / secs) / (1024.0 * 1024.0);
        out2.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
        out2.read(got.data());
        unsigned wrong = 0;
        for (unsigned i = 0; i < n; ++i)
          wrong += (got[i] != pat[i]);
        std::printf("  run %d: state %d, %u/%u wrong, %.1f MB/s\n", r,
                    static_cast<int>(run2.state()), wrong, n, mbps);
        bad_runs += (wrong != 0);
      }
      return bad_runs ? 1 : 0;
    }

    std::vector<unsigned> sizes;
    if (bytes) {
      sizes.push_back(bytes);
      sizes.push_back(bytes);       // the same size twice: is it size, or the
      sizes.push_back(bytes);       // second and third run in one process?
    }
    else
      for (unsigned sz = 4096; sz <= 16u * 1024 * 1024; sz *= 2)
        sizes.push_back(sz);

    int failures = 0;
    for (unsigned sz : sizes) {
    bytes = sz;
    std::printf("\n--- %u bytes ---\n", bytes);
    auto in = xrt::bo(ctx, bytes, g0);
    auto out = xrt::bo(ctx, bytes, g1);
    std::printf("input  device address 0x%llx\n", (unsigned long long)in.address());
    std::printf("output device address 0x%llx\n", (unsigned long long)out.address());

    std::vector<unsigned char> pattern(bytes);
    for (unsigned i = 0; i < bytes; ++i)
      pattern[i] = static_cast<unsigned char>(i % 256);

    // --- 1. DMA round trip at the address the kernel will be given
    in.write(pattern.data());
    in.sync(XCL_BO_SYNC_BO_TO_DEVICE);

    std::vector<unsigned char> back(bytes, 0xAA);
    in.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
    in.read(back.data());
    unsigned bad = 0, first = 0;
    for (unsigned i = 0; i < bytes; ++i)
      if (back[i] != pattern[i]) { if (!bad) first = i; ++bad; }
    if (bad)
      std::printf("1. DMA round trip: %u of %u bytes wrong, first at %u"
                  " (wrote 0x%02x, read 0x%02x)\n", bad, bytes, first,
                  pattern[first], back[first]);
    else
      std::printf("1. DMA round trip: all %u bytes match\n", bytes);

    // --- 2. the kernel reading what DMA wrote
    std::vector<unsigned char> zero(bytes, 0);
    out.write(zero.data());
    out.sync(XCL_BO_SYNC_BO_TO_DEVICE);

    auto run = krnl(in, out, bytes, reps);
    run.wait();
    std::printf("   kernel state after wait: %d\n", static_cast<int>(run.state()));

    out.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
    std::vector<unsigned char> got(bytes, 0xAA);
    out.read(got.data());
    bad = 0; first = 0;
    for (unsigned i = 0; i < bytes; ++i)
      if (got[i] != pattern[i]) { if (!bad) first = i; ++bad; }
    if (bad) {
      std::printf("2. kernel copy: %u of %u bytes wrong, first at %u"
                  " (expected 0x%02x, got 0x%02x)\n", bad, bytes, first,
                  pattern[first], got[first]);
      unsigned zeros = 0;
      for (unsigned i = 0; i < bytes; ++i)
        zeros += (got[i] == 0);
      std::printf("   output is %u/%u zero bytes%s\n", zeros, bytes,
                  zeros == bytes ? " (the kernel wrote nothing, or read zeros)" : "");
    }
    else
      std::printf("2. kernel copy: all %u bytes match\n", bytes);
    failures += (bad != 0);
    if (bad)
      break;                       // first failing size is the interesting one
    }
    return failures ? 1 : 0;
  }
  catch (const std::exception& e) {
    std::fprintf(stderr, "failed: %s\n", e.what());
    return 1;
  }
}
