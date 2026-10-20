// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
#ifndef XRT_CORE_PCIE_MACOS_DEVICE_H
#define XRT_CORE_PCIE_MACOS_DEVICE_H

#include "pci_enum.h"
#include "exec_macos.h"

#include <map>

#include "core/common/ishim.h"
#include "core/pcie/common/device_pcie.h"

namespace xrt_core {

// Phase 1 device: PCI identity only. Everything that needs BAR access,
// DMA or a driver throws not_supported until the DriverKit extension exists.
class device_macos : public shim<device_pcie>
{
public:
  device_macos(handle_type device_handle, id_type device_id, bool user,
               const macos::pci_function& fn);

  const macos::pci_function&
  get_pci_function() const
  {
    return m_fn;
  }

  // Phase 5: single-slot hardware context and software CU scheduler.
  std::unique_ptr<hwctx_handle>
  create_hw_context(const xrt::uuid& xclbin_uuid, const xrt::hw_context::cfg_param_type&,
                    xrt::hw_context::access_mode mode) const override;

  void
  exec_buf(buffer_handle* cmd) override;

  int
  exec_wait(int timeout_ms) const override;

  // Unmanaged DMA to/from a card address (device-only buffers, xrt::bo
  // read/write): XDMA through the dext, card DDR window only.
  void
  unmgd_pread(void* buffer, size_t size, uint64_t offset) override;

  void
  unmgd_pwrite(const void* buffer, size_t size, uint64_t offset) override;

  // CU register access (xrt::kernel read_register / write_register). Reads
  // go through the read-only BAR mapping and, for shared CUs, only inside a
  // range granted with set_cu_read_range. Writes go through the dext.
  void
  set_cu_read_range(cuidx_type cuidx, uint32_t start, uint32_t size) override;

  // xbmgmt/xrt-smi reset: "hot" resets the card through the dext.
  void
  reset(query::reset_type& type) const override;

  // Legacy device info used by the OpenCL layer (xclGetDeviceInfo2).
  void
  get_device_info(xclDeviceInfo2* info) override;

  void
  reg_read(uint32_t ipidx, uint32_t offset, uint32_t* data) const override;

  void
  reg_write(uint32_t ipidx, uint32_t offset, uint32_t data) override;

  // Phase 4: host memory + card DDR, synced by XDMA through the dext.
  std::unique_ptr<buffer_handle>
  alloc_bo(size_t size, uint64_t flags) override;

  std::unique_ptr<buffer_handle>
  alloc_bo(void* userptr, size_t size, uint64_t flags) override;

  // Program an xclbin through the management function (xbmgmt path).
  void
  xclmgmt_load_xclbin(span<char> buffer) const override;

private:
  const query::request&
  lookup_query(query::key_type query_key) const override;

  macos::pci_function m_fn;
  std::shared_ptr<macos::scheduler> m_sched;
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> m_read_ranges;   // cu -> {start, size}
};

} // xrt_core

#endif
