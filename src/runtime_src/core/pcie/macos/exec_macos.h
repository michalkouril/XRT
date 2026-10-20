// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Phase 5 of the XRT macOS port: hardware context and a user-space
// command scheduler for ap_ctrl_hs compute units, modelled on xocl's
// software KDS path (ERT disabled). Commands are ERT packets in host
// memory; CU registers are written through the dext (CU_WRITE) and polled
// through the read-only BAR0 mapping.
#ifndef XRT_CORE_PCIE_MACOS_EXEC_H
#define XRT_CORE_PCIE_MACOS_EXEC_H

#include "core/common/shim/hwctx_handle.h"
#include "xrt/xrt_hw_context.h"
#include "xrt/xrt_uuid.h"

#include <memory>

namespace xrt_core {
class device;
class buffer_handle;
}

namespace xrt_core::macos {

class scheduler;

// One scheduler per device object.
std::shared_ptr<scheduler>
create_scheduler(xrt_core::device* device, uint16_t bus, uint16_t dev);

// Submit an ERT command buffer (a host-memory bo).
void
submit(scheduler* sched, xrt_core::buffer_handle* cmd);

// Poll running commands; returns number completed, 0 on timeout.
int
wait(scheduler* sched, int timeout_ms);

std::unique_ptr<xrt_core::hwctx_handle>
create_hwctx(xrt_core::device* device, std::shared_ptr<scheduler> sched,
             const xrt::uuid& xclbin_uuid, xrt::hw_context::access_mode mode);

} // xrt_core::macos

#endif
