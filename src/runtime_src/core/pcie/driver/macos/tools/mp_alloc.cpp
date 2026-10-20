// Phase 7 test: allocate N device buffers in a bank, print their card
// addresses, fill and verify them, then hold them for S seconds.
// Usage: mp_alloc <tag> <count> <MiB each> <hold seconds> [bank]
#include "xrt/xrt_bo.h"
#include "xrt/xrt_device.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  std::string tag = argv[1];
  int count = std::stoi(argv[2]);
  size_t mib = std::stoul(argv[3]);
  int hold = std::stoi(argv[4]);
  int bank = argc > 5 ? std::stoi(argv[5]) : 1;
  xrt::device device{0};
  std::vector<xrt::bo> bos;
  for (int i = 0; i < count; ++i) {
    bos.emplace_back(device, mib << 20, XRT_BO_FLAGS_NONE, static_cast<xrt::memory_group>(bank));
    auto p = bos.back().map<uint32_t*>();
    for (size_t w = 0; w < (mib << 20) / 4; w += 1024) p[w] = static_cast<uint32_t>(w ^ (i << 24) ^ tag[0]);
    bos.back().sync(XCL_BO_SYNC_BO_TO_DEVICE);
    std::cout << tag << " bo " << i << " @0x" << std::hex << bos.back().address() << std::dec << "\n";
  }
  std::this_thread::sleep_for(std::chrono::seconds(hold));
  int bad = 0;
  for (int i = 0; i < count; ++i) {
    auto p = bos[i].map<uint32_t*>();
    std::memset(p, 0, mib << 20);
    bos[i].sync(XCL_BO_SYNC_BO_FROM_DEVICE);
    for (size_t w = 0; w < (mib << 20) / 4; w += 1024) bad += p[w] != static_cast<uint32_t>(w ^ (i << 24) ^ tag[0]);
  }
  std::cout << tag << " verify after hold: " << bad << " bad words -> " << (bad ? "FAIL" : "PASS") << "\n";
  return bad ? 1 : 0;
}
