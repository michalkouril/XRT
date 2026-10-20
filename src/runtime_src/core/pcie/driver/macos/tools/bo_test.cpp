// Phase 4 exit test via XRT's public API: xrt::bo alloc/map/sync round trips.
#include "xrt/xrt_device.h"
#include "xrt/xrt_bo.h"
#include "xrt/experimental/xrt_xclbin.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

static int
roundtrip(xrt::bo& bo, size_t size, uint32_t seed, bool report)
{
  auto p = bo.map<uint32_t*>();
  std::cout << "  mapped at " << static_cast<void*>(p) << "\n";
  std::mt19937 gen(seed);
  size_t n = size / 4;
  std::vector<uint32_t> ref(n);
  for (size_t i = 0; i < n; ++i) ref[i] = p[i] = gen();
  auto t0 = std::chrono::steady_clock::now();
  bo.sync(XCL_BO_SYNC_BO_TO_DEVICE, size, 0);
  auto t1 = std::chrono::steady_clock::now();
  std::memset(p, 0, size);
  bo.sync(XCL_BO_SYNC_BO_FROM_DEVICE, size, 0);
  auto t2 = std::chrono::steady_clock::now();
  size_t bad = 0;
  for (size_t i = 0; i < n; ++i) bad += (p[i] != ref[i]);
  if (report) {
    auto mbps = [&](auto a, auto b) { return size / std::chrono::duration<double>(b - a).count() / 1e6; };
    std::cout << "  " << size / 1024 << " KiB @0x" << std::hex << bo.address() << std::dec
              << "  to device " << (int)mbps(t0, t1) << " MB/s, from device " << (int)mbps(t1, t2)
              << " MB/s, mismatched words " << bad << "\n";
  }
  return bad ? 1 : 0;
}

int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  std::string path = argc > 1 ? argv[1] : "verify.xclbin";
  xrt::device device{0};
  xrt::xclbin xclbin{path};
  auto uuid = device.load_xclbin(xclbin);
  std::cout << "loaded " << uuid.to_string() << "\n";
  int bank = -1;
  for (auto& m : xclbin.get_mems())
    if (m.get_used()) { bank = m.get_index(); std::cout << "using bank " << m.get_tag() << " (index " << bank << ")\n"; break; }
  if (bank < 0) { std::cout << "no used memory bank\n"; return 1; }
  int fails = 0;
  // Sizes: tiny (bounce path), page, odd, large (zero-copy path).
  for (size_t sz : {64ul, 4096ul, 16384ul, 1000004ul, 16ul << 20, 256ul << 20}) {
    std::cout << "alloc " << sz << " bytes in bank " << bank << "\n";
    xrt::bo bo{device, sz, XRT_BO_FLAGS_NONE, static_cast<xrt::memory_group>(bank)};
    std::cout << "  bo at 0x" << std::hex << bo.address() << std::dec << ", mapping\n";
    fails += roundtrip(bo, sz & ~size_t(3), static_cast<uint32_t>(sz), true);
  }
  // User pointer buffer.
  size_t usz = 8 << 20;
  void* up = nullptr;
  posix_memalign(&up, 16384, usz);
  {
    xrt::bo ubo{device, up, usz, XRT_BO_FLAGS_NONE, static_cast<xrt::memory_group>(bank)};
    std::cout << "user pointer buffer:\n";
    fails += roundtrip(ubo, usz, 42, true);
  }
  std::free(up);
  // Partial sync at an offset.
  {
    xrt::bo bo{device, 1 << 20, XRT_BO_FLAGS_NONE, static_cast<xrt::memory_group>(bank)};
    auto p = bo.map<uint8_t*>();
    for (int i = 0; i < (1 << 20); ++i) p[i] = static_cast<uint8_t>(i * 7);
    bo.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    std::memset(p, 0, 1 << 20);
    bo.sync(XCL_BO_SYNC_BO_FROM_DEVICE, 4096, 12288);   // only [12288, 16384)
    int bad = 0;
    for (int i = 0; i < (1 << 20); ++i) {
      uint8_t want = (i >= 12288 && i < 16384) ? static_cast<uint8_t>(i * 7) : 0;
      bad += (p[i] != want);
    }
    std::cout << "partial sync of [12288,16384): " << bad << " wrong bytes\n";
    fails += bad ? 1 : 0;
  }
  std::cout << (fails ? "FAIL" : "PASS") << "\n";
  return fails;
}
