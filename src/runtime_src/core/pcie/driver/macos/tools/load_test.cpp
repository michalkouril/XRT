// Phase 3 via XRT's public API: xrt::device::load_xclbin on macOS.
#include "xrt/xrt_device.h"
#include "xrt/experimental/xrt_xclbin.h"
#include <iostream>
#include <string>
int main(int argc, char** argv) {
  std::string path = argc > 1 ? argv[1] : "verify.xclbin";
  xrt::device device{0};
  std::cout << "device 0: " << device.get_info<xrt::info::device::name>()
            << " bdf " << device.get_info<xrt::info::device::bdf>() << "\n";
  xrt::xclbin xclbin{path};
  std::cout << "loading " << path << " (uuid " << xclbin.get_uuid().to_string() << ")\n";
  auto uuid = device.load_xclbin(xclbin);
  std::cout << "load_xclbin returned " << uuid.to_string() << "\n";
  std::cout << "device reports loaded xclbin " << device.get_xclbin_uuid().to_string() << "\n";
  for (auto& k : xclbin.get_kernels()) {
    std::cout << "  kernel " << k.get_name() << ":";
    for (auto& cu : k.get_cus())
      std::cout << " " << cu.get_name() << "@0x" << std::hex << cu.get_base_address() << std::dec;
    std::cout << "\n";
  }
  for (auto& m : xclbin.get_mems())
    if (m.get_used())
      std::cout << "  memory " << m.get_tag() << " base 0x" << std::hex << m.get_base_address()
                << std::dec << " " << m.get_size_kb() / 1024 / 1024 << " GB\n";
}
