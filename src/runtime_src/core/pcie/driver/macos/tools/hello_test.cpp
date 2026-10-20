// Phase 5 exit test: run the hello kernel from verify.xclbin through the
// public XRT API, as XRT's own TestVerify does.
#include "xrt/xrt_bo.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_kernel.h"
#include "xrt/experimental/xrt_xclbin.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  std::string path = argc > 1 ? argv[1] : "verify.xclbin";
  int iterations = argc > 2 ? std::stoi(argv[2]) : 1;
  xrt::device device{0};
  xrt::xclbin xclbin{path};
  auto uuid = device.load_xclbin(xclbin);
  std::cout << "loaded " << uuid.to_string() << "\n";
  xrt::hw_context ctx{device, uuid};
  xrt::kernel krnl{ctx, "hello"};
  std::cout << "kernel hello, arg0 group " << krnl.group_id(0) << "\n";
  constexpr size_t buffer_size = 128;
  xrt::bo out(ctx, buffer_size, krnl.group_id(0));
  std::cout << "output bo @0x" << std::hex << out.address() << std::dec << "\n";
  int fails = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) {
    char zero[buffer_size] = {};
    out.write(zero);
    out.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    auto run = krnl(out);
    auto state = run.wait(std::chrono::milliseconds(5000));
    out.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
    char got[buffer_size] = {};
    out.read(got);
    bool ok = state == ERT_CMD_STATE_COMPLETED && std::memcmp(got, "Hello World\n", 12) == 0;
    if (!ok) {
      ++fails;
      std::cout << "iteration " << i << ": state " << state << ", got \"" << std::string(got, strnlen(got, 32)) << "\"\n";
    }
    if (i == 0)
      std::cout << "run 0: state " << state << ", output \"" << std::string(got, strnlen(got, 32)) << "\"";
  }
  auto s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::cout << "\n" << iterations << " run(s), " << fails << " failure(s), " << (s / iterations * 1e6)
            << " us per write+run+read\n" << (fails ? "FAIL" : "PASS") << "\n";
  return fails ? 1 : 0;
}
