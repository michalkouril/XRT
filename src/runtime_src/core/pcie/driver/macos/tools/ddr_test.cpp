// SPDX-License-Identifier: Apache-2.0
// Does card memory hold data? Write a pattern through XRT's own buffer path,
// read it back and say exactly what came back if it differs.
//
//   ddr_test <xclbin> [bytes ...]        default sizes 4 KiB, 64 KiB, 1 MiB, 4 MiB
//
// Written for the Innova-2 rbdm shell, whose DDR gate (see ddr_mode) answers a
// fenced read with 0xBADCA1B0 instead of hanging the AXI master. That filler is
// the point of this tool: a fenced bank and a broken bank both "fail", and only
// the data tells them apart. So every mismatching run reports how many words
// came back as the fence filler, and the first word that was neither right nor
// filler.
//
// Each size runs in its own scope, so every buffer is freed before the next is
// allocated and each one starts at the bank base. That matters in the shell's
// emulated-DDR mode, where only the bottom 1 MB of bank 0 is real storage.
#include "xrt/xrt_device.h"
#include "xrt/xrt_bo.h"
#include "xrt/experimental/xrt_xclbin.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// What the rbdm shell's fence returns for a read it will not pass through.
static const uint32_t FENCE_FILLER = 0xBADCA1B0u;

static int
one_size(xrt::device& device, int bank, size_t size)
{
  std::printf("  %-9zu bytes: ", size);
  std::fflush(stdout);
  try {
    xrt::bo bo{device, size, XRT_BO_FLAGS_NONE, static_cast<xrt::memory_group>(bank)};
    auto p = bo.map<uint32_t*>();
    const size_t n = size / 4;
    std::vector<uint32_t> ref(n);
    // A cheap counter-plus-salt pattern: every word differs from every other,
    // so a transfer that lands at the wrong offset shows up as a mismatch and
    // not as a coincidence.
    for (size_t i = 0; i < n; ++i)
      ref[i] = p[i] = static_cast<uint32_t>(i * 0x9e3779b9u + 0x12345678u);

    auto t0 = std::chrono::steady_clock::now();
    bo.sync(XCL_BO_SYNC_BO_TO_DEVICE, size, 0);
    auto t1 = std::chrono::steady_clock::now();
    std::memset(p, 0, size);
    bo.sync(XCL_BO_SYNC_BO_FROM_DEVICE, size, 0);
    auto t2 = std::chrono::steady_clock::now();

    size_t bad = 0, filler = 0, zero = 0, first_bad = 0;
    for (size_t i = 0; i < n; ++i) {
      if (p[i] == ref[i])
        continue;
      if (!bad)
        first_bad = i;
      ++bad;
      if (p[i] == FENCE_FILLER)
        ++filler;
      else if (p[i] == 0)
        ++zero;
    }
    auto mbps = [&](auto a, auto b) {
      double s = std::chrono::duration<double>(b - a).count();
      return s > 0 ? size / s / 1e6 : 0.0;
    };
    std::printf("addr 0x%llx  to %4.0f MB/s  from %4.0f MB/s  ",
                (unsigned long long)bo.address(), mbps(t0, t1), mbps(t1, t2));
    if (!bad) {
      std::printf("OK, all %zu words\n", n);
      return 0;
    }
    std::printf("%zu/%zu words wrong\n", bad, n);
    std::printf("             first at word %zu: wrote 0x%08x read 0x%08x\n",
                first_bad, ref[first_bad], p[first_bad]);
    std::printf("             of the wrong words: %zu read 0x%08x (fenced), %zu read 0, %zu other\n",
                filler, FENCE_FILLER, zero, bad - filler - zero);
    return 1;
  }
  catch (const std::exception& e) {
    // A transfer that never completes comes back as an exception from sync(),
    // which is a different failure from data that comes back wrong - the engine
    // stalled rather than the memory answering. Keep going to the next size.
    std::printf("FAILED: %s\n", e.what());
    return 1;
  }
}

int
main(int argc, char** argv)
{
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <xclbin> [bytes ...]\n", argv[0]);
    return 2;
  }
  std::vector<size_t> sizes;
  for (int i = 2; i < argc; ++i)
    sizes.push_back(std::strtoull(argv[i], nullptr, 0) & ~size_t(3));
  if (sizes.empty())
    sizes = {4096, 65536, 1u << 20, 4u << 20};

  try {
    xrt::device device{0};
    xrt::xclbin xclbin{std::string(argv[1])};
    auto uuid = device.load_xclbin(xclbin);
    std::printf("xclbin %s loaded\n", uuid.to_string().c_str());
    int bank = -1;
    for (auto& m : xclbin.get_mems()) {
      if (!m.get_used())
        continue;
      bank = m.get_index();
      std::printf("bank %s (index %d) at 0x%llx, %llu KiB\n", m.get_tag().c_str(), bank,
                  (unsigned long long)m.get_base_address(),
                  (unsigned long long)m.get_size_kb());
      break;
    }
    if (bank < 0) {
      std::printf("no used memory bank in this xclbin\n");
      return 1;
    }
    int fails = 0;
    for (size_t sz : sizes)
      fails += one_size(device, bank, sz);
    std::printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
  }
  catch (const std::exception& e) {
    std::printf("setup failed: %s\n", e.what());
    return 1;
  }
}
