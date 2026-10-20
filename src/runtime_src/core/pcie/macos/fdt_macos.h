// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// The partition metadata that dynamic-IP platforms carry describes itself as a
// flattened device tree. Two of them matter here: the base platform's, which
// ships with the platform package, and the one inside an xclbin, which
// describes the user partition that xclbin brings with it - including the
// kernel clock wizards, which on such a platform are NOT in the base platform
// at all.
//
// This is a reader for the few properties the driver needs, not a device-tree
// library: the same shape of minimal walker XRT's own tools use
// (XBUtilities::get_uuids), so nothing has to be vendored.
#ifndef XRT_CORE_PCIE_MACOS_FDT_H
#define XRT_CORE_PCIE_MACOS_FDT_H

#include <cstdint>
#include <string>
#include <vector>

namespace xrt_core::macos {

// One addressable endpoint of a partition.
struct fdt_endpoint
{
  std::string name;                 // e.g. ep_aclk_kernel_00
  std::string abstraction;          // e.g. xilinx.com:reg_abs:clkwiz:1.0
  uint64_t    offset = 0;
  uint64_t    size = 0;
  uint8_t     bar = 0;
  uint8_t     func = 0;
};

struct fdt_partition
{
  std::string interface_uuid;       // 32 lowercase hex characters, or empty
  std::string logic_uuid;
  std::vector<fdt_endpoint> endpoints;
};

// Parse a PARTITION_METADATA blob. Returns false if it is not a device tree or
// carries nothing we recognise.
bool
parse_partition_metadata(const void* blob, size_t size, fdt_partition& out);

} // xrt_core::macos

#endif
