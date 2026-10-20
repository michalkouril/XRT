// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
#define XCL_DRIVER_DLL_EXPORT
#include "system_macos.h"
#include "device_macos.h"
#include "dext_client.h"

#include "core/common/error.h"

#include <boost/property_tree/ptree.hpp>
#include <cstdio>
#include <string>

// Constructing the singleton registers it with xrt_core::system.
static xrt_core::system_macos singleton;

namespace xrt_core {

std::vector<macos::pci_function>
system_macos::
functions(bool is_user)
{
  std::vector<macos::pci_function> result;
  for (const auto& card : macos::enumerate_cards()) {
    if (is_user && card.has_user)
      result.push_back(card.user);
    else if (!is_user && card.has_mgmt)
      result.push_back(card.mgmt);
  }
  return result;
}

void
system_macos::
get_driver_info(boost::property_tree::ptree& pt)
{
  // This is what `xrt-smi examine` prints for the driver, so a hard-coded
  // "not installed" told the user the extension they had just installed was
  // absent. Report what is actually there: the dext's name, and the ABI it
  // reports, which is the only version number it has.
  boost::property_tree::ptree driver;
  bool attached = false;
  uint32_t abi = 0;
  for (const auto& card : macos::enumerate_cards()) {
    if (!card.has_user || !macos::dext_attached(card.user.bus, card.user.dev))
      continue;
    attached = true;
    // The dext reports no version of its own beyond the ABI it speaks, which is
    // the number that matters when a tool and the extension disagree.
    if (auto plat = macos::get_platform(card.user.bus, card.user.dev, 1))
      abi = plat->abi;
    break;
  }
  driver.put("name", attached ? "xrt_user" : "none");
  driver.put("version", attached
             ? "DriverKit extension (org.xrtmac.XrtUser), ABI " + std::to_string(abi)
             : "no xrt_user extension attached to any card");
  pt.push_back({"", driver});
}

std::pair<device::id_type, device::id_type>
system_macos::
get_total_devices(bool is_user) const
{
  auto count = static_cast<device::id_type>(functions(is_user).size());
  return {count, count};
}

device::id_type
system_macos::
get_device_id(const std::string& bdf) const
{
  // Treat non-BDF strings as a device index.
  if (bdf.find_first_not_of("0123456789") == std::string::npos)
    return system::get_device_id(bdf);

  auto fns = functions(true);
  for (device::id_type i = 0; i < fns.size(); ++i) {
    const auto& fn = fns[i];
    char full[16], shrt[16];
    std::snprintf(full, sizeof(full), "%04x:%02x:%02x.%01x", fn.domain, fn.bus, fn.dev, fn.func);
    std::snprintf(shrt, sizeof(shrt), "%02x:%02x.%01x", fn.bus, fn.dev, fn.func);
    if (bdf == full || (fn.domain == 0 && bdf == shrt))
      return i;
  }
  throw xrt_core::system_error(EINVAL, "No such device '" + bdf + "'");
}

std::tuple<uint16_t, uint16_t, uint16_t, uint16_t>
system_macos::
get_bdf_info(device::id_type id, bool is_user) const
{
  auto fns = functions(is_user);
  if (id >= fns.size())
    throw xrt_core::error(std::errc::no_such_device, "no device at index " + std::to_string(id));
  const auto& fn = fns[id];
  return {fn.domain, fn.bus, fn.dev, fn.func};
}

std::shared_ptr<device>
system_macos::
get_userpf_device(device::id_type id) const
{
  // xclOpen constructs the shim, which registers the core device through
  // the (handle, id) overload below.
  return xrt_core::get_userpf_device(xclOpen(id, nullptr, XCL_QUIET));
}

std::shared_ptr<device>
system_macos::
get_userpf_device(device::handle_type handle, device::id_type id) const
{
  auto fns = functions(true);
  if (id >= fns.size())
    throw xrt_core::error(std::errc::no_such_device, "no user function at index " + std::to_string(id));
  return std::shared_ptr<device_macos>(new device_macos(handle, id, true, fns[id]));
}

std::shared_ptr<device>
system_macos::
get_mgmtpf_device(device::id_type id) const
{
  auto fns = functions(false);
  if (id >= fns.size())
    throw xrt_core::error(std::errc::no_such_device, "no management function at index " + std::to_string(id));
  return std::shared_ptr<device_macos>(new device_macos(XRT_NULL_HANDLE, id, false, fns[id]));
}

} // xrt_core
