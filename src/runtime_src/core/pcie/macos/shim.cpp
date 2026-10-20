// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Legacy xcl* C entry points for the macOS shim (libxrt_core). Phase 1
// supports probe/open/close only; every data-path call reports ENOTSUP
// until the DriverKit extensions exist.
#define XCL_DRIVER_DLL_EXPORT
#define XRT_CORE_PCIE_MACOS_SOURCE

#include "device_macos.h"
#include "system_macos.h"

#include "core/common/message.h"
#include "xrt/xrt_uuid.h"
#include "core/common/system.h"
#include "dext_client.h"

#include <cerrno>
#include <limits>
#include <string>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

// The opaque xclDeviceHandle handed back to XRT. It owns the core device.
struct macos_shim
{
  unsigned int index;
  std::shared_ptr<xrt_core::device> core_device;

  explicit macos_shim(unsigned int idx)
    : index(idx)
  {
    if (idx >= xrt_core::system_macos::functions(true).size())
      throw xrt_core::error(std::errc::no_such_device, "no Xilinx user function at index " + std::to_string(idx));
    // Registers this handle with the core and constructs device_macos.
    core_device = xrt_core::get_userpf_device(this, idx);
  }
};

macos_shim*
get_shim(xclDeviceHandle handle)
{
  if (!handle)
    throw xrt_core::error(std::errc::invalid_argument, "null device handle");
  return static_cast<macos_shim*>(handle);
}

constexpr int not_supported = -ENOTSUP;

} // namespace

unsigned int
xclProbe()
{
  return static_cast<unsigned int>(xrt_core::system_macos::functions(true).size());
}

xclDeviceHandle
xclOpen(unsigned int deviceIndex, const char*, xclVerbosityLevel)
{
  try {
    return new macos_shim(deviceIndex);
  }
  catch (const std::exception& ex) {
    xrt_core::send_exception_message(ex.what());
  }
  return nullptr;
}

void
xclClose(xclDeviceHandle handle)
{
  delete static_cast<macos_shim*>(handle);
}

int
xclGetDeviceInfo2(xclDeviceHandle handle, xclDeviceInfo2* info)
{
  try {
    auto shim = get_shim(handle);
    std::static_pointer_cast<xrt_core::device_macos>(shim->core_device)->get_device_info(info);
    return 0;
  }
  catch (const std::exception& ex) {
    xrt_core::send_exception_message(ex.what());
  }
  return -EINVAL;
}

// Everything below needs BAR access, DMA or a scheduler. All of it is
// implemented now; what remains stubbed further down is listed in the driver
// README's "Not supported" section, and each stub says why.
int
xclLoadXclBin(xclDeviceHandle handle, const axlf* buffer)
{
  try {
    auto shim = get_shim(handle);
    const auto& fn = std::static_pointer_cast<xrt_core::device_macos>(shim->core_device)->get_pci_function();
    xrt_core::macos::load_xclbin(fn.bus, fn.dev, buffer);
    // Cache memory topology, IP layout etc. in the core device, as the
    // Linux and Windows shims do after a successful download.
    shim->core_device->register_axlf(buffer);
    return 0;
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    return ex.get_code() ? -std::abs(ex.get_code()) : -EIO;
  }
  catch (const std::exception& ex) {
    xrt_core::send_exception_message(ex.what());
    return -EIO;
  }
}
// Legacy per-IP context API. XRT still uses it for the whole-xclbin lock
// (`XBUtilities::xclbin_lock`), which every tool that inspects a programmed
// card takes, so it cannot just report ENOTSUP: xrt-smi validate's m2m and
// p2p tests died on the lock instead of reaching the checks that would have
// skipped them.
//
// ip_index UINT_MAX is xocl's virtual CU: a claim on the xclbin rather than on
// a compute unit. There is nothing to arbitrate for it here - a reload is
// already kept away from processes by the partition hold - so it only checks
// that the caller is talking about the xclbin that is loaded. A real index
// maps onto the CU contexts the extension keeps, the same ones
// xrt::hw_context uses.
namespace {

constexpr unsigned int virtual_cu = std::numeric_limits<unsigned int>::max();

// The uuid the card is running, or empty if none.
std::string
loaded_uuid(const xrt_core::device_macos* dev)
{
  const auto& fn = dev->get_pci_function();
  return xrt_core::macos::loaded_xclbin_uuid(fn.bus, fn.dev);
}

} // namespace

int
xclOpenContext(xclDeviceHandle handle, const xuid_t xclbin_id, unsigned int ip_index, bool shared)
{
  try {
    auto shim = get_shim(handle);
    auto dev = std::static_pointer_cast<xrt_core::device_macos>(shim->core_device);
    auto want = xrt::uuid(xclbin_id).to_string();
    auto have = loaded_uuid(dev.get());
    if (have.empty())
      throw xrt_core::error(std::errc::no_such_device, "no xclbin is loaded on the card");
    if (have != want)
      throw xrt_core::error(std::errc::invalid_argument,
                            "xclbin " + want + " is not the one loaded (" + have + ")");
    if (ip_index == virtual_cu)
      return 0;                       // xclbin lock only
    const auto& fn = dev->get_pci_function();
    const auto& cus = dev->get_cus();
    if (ip_index >= cus.size())
      throw xrt_core::error(std::errc::invalid_argument,
                            "IP index " + std::to_string(ip_index) + " out of range");
    xrt_core::macos::cu_ctx_open(fn.bus, fn.dev, ip_index, shared, cus[ip_index]);
    return 0;
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    return ex.get_code() ? -std::abs(ex.get_code()) : -EINVAL;
  }
  catch (const std::exception& ex) {
    xrt_core::send_exception_message(ex.what());
    return -EINVAL;
  }
}

int
xclCloseContext(xclDeviceHandle handle, const xuid_t, unsigned int ip_index)
{
  try {
    auto shim = get_shim(handle);
    if (ip_index == virtual_cu)
      return 0;
    auto dev = std::static_pointer_cast<xrt_core::device_macos>(shim->core_device);
    const auto& fn = dev->get_pci_function();
    xrt_core::macos::cu_ctx_close(fn.bus, fn.dev, ip_index);
    return 0;
  }
  catch (const std::exception& ex) {
    xrt_core::send_exception_message(ex.what());
    return -EINVAL;
  }
}
// The legacy exec pair stays unsupported, and the reason is the handle type,
// not a missing scheduler: device_macos::exec_buf and exec_wait are fully
// implemented, but they take a buffer_handle*, while xclExecBuf takes an
// xclBufferHandle - the legacy integer handle. The macOS shim implements no part
// of the legacy BO API (no xclAllocBO, xclMapBO, xclFreeBO), so no integer
// handle is ever handed out and there is nothing to translate. Wiring these up
// means building the legacy BO handle table first; the native xrt::run path does
// not need it.
int xclExecBuf(xclDeviceHandle, xclBufferHandle) { return not_supported; }
int xclExecWait(xclDeviceHandle, int) { return not_supported; }
int xclRegRead(xclDeviceHandle, uint32_t, uint32_t, uint32_t*) { return not_supported; }
int xclRegWrite(xclDeviceHandle, uint32_t, uint32_t, uint32_t) { return not_supported; }
size_t xclRead(xclDeviceHandle, enum xclAddressSpace, uint64_t, void*, size_t) { return static_cast<size_t>(not_supported); }
size_t xclWrite(xclDeviceHandle, enum xclAddressSpace, uint64_t, const void*, size_t) { return static_cast<size_t>(not_supported); }
ssize_t xclUnmgdPread(xclDeviceHandle, unsigned int, void*, size_t, uint64_t) { return not_supported; }
ssize_t xclUnmgdPwrite(xclDeviceHandle, unsigned int, const void*, size_t, uint64_t) { return not_supported; }
int xclReClock2(xclDeviceHandle, unsigned short, const unsigned short*) { return not_supported; }
int xclP2pEnable(xclDeviceHandle, bool, bool) { return not_supported; }
int xclCmaEnable(xclDeviceHandle, bool, uint64_t) { return not_supported; }
int xclUpdateSchedulerStat(xclDeviceHandle) { return not_supported; }
int xclInternalResetDevice(xclDeviceHandle, xclResetKind) { return not_supported; }
