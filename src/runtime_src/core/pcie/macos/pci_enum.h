// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Phase 1 of the XRT macOS port: discover Xilinx PCIe functions from the
// IOKit registry. This needs no driver; it only reads the properties the
// macOS PCI family publishes for every enumerated function.
#ifndef XRT_CORE_PCIE_MACOS_PCI_ENUM_H
#define XRT_CORE_PCIE_MACOS_PCI_ENUM_H

#include <cstdint>
#include <string>
#include <vector>

namespace xrt_core::macos {

struct pci_bar
{
  unsigned int index = 0;   // BAR number 0..5
  uint64_t     addr  = 0;   // host physical address assigned by macOS
  uint64_t     size  = 0;   // bytes
  bool         is64  = false;
  bool         prefetchable = false;
};

struct pci_function
{
  std::string  registry_path;   // IOService plane path, for later IOServiceOpen
  uint16_t     vendor = 0;
  uint16_t     device = 0;
  uint16_t     subsystem_vendor = 0;
  uint16_t     subsystem_id = 0;
  uint8_t      revision = 0;
  uint16_t     domain = 0;
  uint16_t     bus = 0;
  uint16_t     dev = 0;
  uint16_t     func = 0;
  uint32_t     link_caps = 0;     // PCIe Link Capabilities register
  uint32_t     link_status = 0;   // PCIe Link Status register (0xffff = unreadable)
  std::string  slot;              // e.g. "Thunderbolt@3,0,1"
  std::vector<pci_bar> bars;

  // Gen number (1..6) and lane width decoded from the PCIe registers.
  unsigned int max_speed() const { return link_caps & 0xf; }
  unsigned int max_width() const { return (link_caps >> 4) & 0x3f; }
  bool link_status_valid() const { return link_status != 0xffff && link_status != 0; }
  unsigned int cur_speed() const { return link_status_valid() ? (link_status & 0xf) : 0; }
  unsigned int cur_width() const { return link_status_valid() ? ((link_status >> 4) & 0x3f) : 0; }
};

// A card is the pair of functions on one bus/device: the management PF
// (function 0) and the user PF (function 1). Either may be absent.
struct alveo_card
{
  bool has_mgmt = false;
  bool has_user = false;
  pci_function mgmt;
  pci_function user;
};

// All Xilinx (0x10ee) PCIe functions, grouped into cards and sorted by
// domain/bus/device so device indices are stable across calls.
std::vector<alveo_card>
enumerate_cards();

} // xrt_core::macos

#endif
