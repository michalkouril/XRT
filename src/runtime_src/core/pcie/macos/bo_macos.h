// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Phase 4 buffer objects for the macOS shim: host memory in the process,
// device memory in a DDR bank of the loaded xclbin, XDMA sync via the dext.
#ifndef XRT_CORE_PCIE_MACOS_BO_H
#define XRT_CORE_PCIE_MACOS_BO_H

#include "core/common/shim/buffer_handle.h"
#include <cstdint>
#include <memory>

namespace xrt_core {
class device;
}

namespace xrt_core::macos {

std::unique_ptr<xrt_core::buffer_handle>
alloc_bo(const xrt_core::device* device, uint16_t bus, uint16_t dev,
         void* userptr, size_t size, uint64_t flags);

} // xrt_core::macos

#endif
