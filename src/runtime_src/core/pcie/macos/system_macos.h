// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
#ifndef XRT_CORE_PCIE_MACOS_SYSTEM_H
#define XRT_CORE_PCIE_MACOS_SYSTEM_H

#include "pci_enum.h"
#include "core/pcie/common/system_pcie.h"

namespace xrt_core {

class system_macos : public system_pcie
{
public:
  void
  get_driver_info(boost::property_tree::ptree& pt) override;

  std::pair<device::id_type, device::id_type>
  get_total_devices(bool is_user) const override;

  // Accepts a device index or a BDF ("dddd:bb:dd.f" or "bb:dd.f"), like Linux.
  device::id_type
  get_device_id(const std::string& str) const override;

  std::tuple<uint16_t, uint16_t, uint16_t, uint16_t>
  get_bdf_info(device::id_type id, bool is_user) const override;

  std::shared_ptr<device>
  get_userpf_device(device::id_type id) const override;

  std::shared_ptr<device>
  get_userpf_device(device::handle_type device_handle, device::id_type id) const override;

  std::shared_ptr<device>
  get_mgmtpf_device(device::id_type id) const override;

  // Functions of one kind (user or mgmt), in stable bus order.
  static std::vector<macos::pci_function>
  functions(bool is_user);
};

} // xrt_core

#endif
