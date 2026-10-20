// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
#define XCL_DRIVER_DLL_EXPORT
#include "exec_macos.h"
#include "dext_client.h"
#include "xrt_dext_abi.h"

#include "core/common/device.h"
#include "core/common/error.h"
#include "core/common/message.h"
#include "core/common/shim/buffer_handle.h"
#include "xrt/detail/ert.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// ap_ctrl_hs control register (xocl common/drv/cu_hls.c)
constexpr uint32_t CU_CTRL     = 0x00;
constexpr uint32_t CU_ARGS     = 0x10;
constexpr uint32_t CU_AP_START = 0x1;
constexpr uint32_t CU_AP_DONE  = 0x2;
constexpr uint32_t CU_AP_IDLE  = 0x4;
constexpr uint32_t CU_AP_CONTINUE = 0x10;

// A CU control register that reads all ones is not a CU: the reserved bits
// read 0 on ap_ctrl_hs, and ap_start, ap_done and ap_idle are never set at
// once. It means the read never reached the user partition, because the
// partition is gone or an AXI firewall has tripped and isolated it.
constexpr uint32_t CU_NO_RESPONSE = 0xffffffff;

// How a running CU is waited for. There are no CU interrupts on these
// shells without the ERT firmware, so the wait polls; see
// driver/macos/README.md.
//
// For the first spin_window of a command's run time the loop may not sleep
// at all. That is not a busy-wait at instruction granularity: an MMIO read
// of a CU register costs about 3.3 us over Thunderbolt, so the read itself
// paces the loop, and a command that finishes inside the window is noticed
// within ~3.3 us instead of the 32 us that sleep_for(20 us) really takes.
//
// Spinning only pays when the CU really does finish inside the window.
// Spinning unconditionally was measured to cost 20 points of CPU on a
// 0.17 ms kernel and buy no latency at all, because the window expires
// long before the CU is done and the completion is still found by a
// sleeping poll. So a CU is spun on only until it has shown otherwise:
// the last run time of each CU is remembered, and a CU whose last command
// took longer than the window is slept on from the start.
//
// After the window the interval grows to 1/16 of how long the oldest
// running command has been running, clamped, so a long kernel costs about
// 1,000 polls a second and is noticed at most ~6% (or 1 ms) late.
constexpr auto spin_window = std::chrono::microseconds(50);
constexpr auto poll_min = std::chrono::microseconds(20);
constexpr auto poll_max = std::chrono::microseconds(1000);
constexpr int  poll_div = 16;

} // namespace

namespace xrt_core::macos {

class scheduler
{
  xrt_core::device* m_device;
  uint16_t m_bus, m_dev;
  std::mutex m_mutex;

  struct cmd_entry {
    ert_start_kernel_cmd* pkt;
    uint32_t cu = UINT32_MAX;   // running on this CU index
    std::chrono::steady_clock::time_point started{};
  };
  std::deque<cmd_entry> m_pending;           // waiting for a free CU
  std::map<uint32_t, cmd_entry> m_running;   // cu index -> command
  // How long the last command on each CU took, to decide whether spinning
  // on it is worth the CPU. Absent means not seen yet: spin once and find out.
  std::map<uint32_t, std::chrono::microseconds> m_last_run;

  volatile uint32_t* m_bar = nullptr;        // read-only user BAR0
  bool m_reported = false;                   // an unreachable CU was logged once
  // Read-write mappings of individual CU register pages, for CUs this process
  // holds exclusively. Starting such a CU needs no call into the dext.
  std::map<uint32_t, volatile uint32_t*> m_cu_rw;

  volatile uint32_t*
  bar()
  {
    if (!m_bar) {
      uint64_t size = 0;
      m_bar = map_bar0(m_bus, m_dev, 1, &size);
      // The CU window is not at a fixed place: a U50's is at 0x1400000, below
      // the XDMA 5.x base. Require BAR0 to cover the window actually in force,
      // not the one this driver used to assume.
      uint64_t need = XRT_CU_WINDOW_END;
      if (auto plat = xrt_core::macos::get_platform(m_bus, m_dev, 1);
          plat && plat->cu_window_size)
        need = plat->cu_window_offset + plat->cu_window_size;
      if (!m_bar || size < need)
        throw xrt_core::error(std::errc::no_such_device,
                              "user BAR0 is too small for this xclbin's CU window,"
                              " or the xrt_user dext is not attached");
    }
    return m_bar;
  }

  // Which CUs use ap_ctrl_chain. Such a CU asserts ap_done and then waits to be
  // acknowledged with ap_continue; until then it takes no further command. A
  // scheduler that never acknowledges runs a chain CU exactly once, and the run
  // after that sees the stale done and reports instant completion having
  // executed nothing - which is how this was found (the U50's verify and
  // bandwidth kernels are both AP_CTRL_CHAIN).
  std::map<uint32_t, bool> m_chain;

  void
  load_cu_control()
  {
    const auto& cus = m_device->get_cus();
    for (uint32_t i = 0; i < cus.size(); ++i)
      m_chain[i] = false;                    // assume ap_ctrl_hs

    auto ips = m_device->get_axlf_section<const ::ip_layout*>(IP_LAYOUT);
    std::vector<char> kept;
    if (!ips) {
      kept = xrt_core::macos::loaded_section(m_bus, m_dev, XRT_SECTION_IP_LAYOUT);
      if (kept.size() >= sizeof(::ip_layout))
        ips = reinterpret_cast<const ::ip_layout*>(kept.data());
    }
    if (!ips)
      return;
    for (int32_t k = 0; k < ips->m_count; ++k) {
      const auto& ip = ips->m_ip_data[k];
      if (ip.m_type != IP_KERNEL)
        continue;
      for (uint32_t i = 0; i < cus.size(); ++i)
        if (cus[i] == ip.m_base_address)
          m_chain[i] = ((ip.properties & IP_CONTROL_MASK) >> IP_CONTROL_SHIFT) == AP_CTRL_CHAIN;
    }
  }

  bool
  cu_is_chain(uint32_t cuidx)
  {
    if (m_chain.empty())
      load_cu_control();
    auto it = m_chain.find(cuidx);
    return it != m_chain.end() && it->second;
  }

  uint64_t
  cu_base(uint32_t cuidx)
  {
    const auto& cus = m_device->get_cus();
    if (cuidx >= cus.size())
      throw xrt_core::error(std::errc::invalid_argument, "CU index " + std::to_string(cuidx) + " out of range");
    return cus[cuidx];
  }

  static bool
  cu_selected(const ert_start_kernel_cmd* pkt, uint32_t cuidx)
  {
    uint32_t word = cuidx / 32;
    if (word > pkt->extra_cu_masks)
      return false;
    uint32_t mask = word == 0 ? pkt->cu_mask : pkt->data[word - 1];
    return (mask >> (cuidx % 32)) & 1;
  }

  // Fail a command whose CU cannot be reached. Without this the command
  // would sit in m_pending for ever, because try_start reads ap_start set
  // in the all-ones control register and never starts it, and the caller
  // would wait for a completion that cannot come. ERT_CMD_STATE_ABORT is
  // past ERT_CMD_STATE_COMPLETED, so xrt::run::wait returns and the XRT
  // API raises an error. Recover with a hot reset and reload the xclbin.
  void
  abort_cmd(cmd_entry& e, const std::string& what)
  {
    e.pkt->state = ERT_CMD_STATE_ABORT;
    if (!m_reported) {
      m_reported = true;
      xrt_core::message::send(xrt_core::message::severity_level::error, "XRT",
                              "macOS scheduler: " + what + ". The user partition is not "
                              "responding; check the AXI firewalls, then reset the card "
                              "and load the xclbin again.");
    }
  }

  // Configure and start pkt on the first idle selected CU; false if none.
  bool
  try_start(cmd_entry& e)
  {
    auto pkt = e.pkt;
    const auto ncus = m_device->get_cus().size();
    for (uint32_t cu = 0; cu < ncus; ++cu) {
      if (!cu_selected(pkt, cu) || m_running.count(cu))
        continue;
      uint64_t base = cu_base(cu);
      // Register map: skip the 4 control words (xocl start_krnl_ecmd2xcmd).
      uint32_t num_mask = 1 + pkt->extra_cu_masks;
      uint32_t nregs = pkt->count >= num_mask + 4 ? pkt->count - num_mask - 4 : 0;
      const uint32_t* args = &pkt->data[pkt->extra_cu_masks + 4];

      // A CU this process holds exclusively has its register page mapped
      // read-write, so it can be started with stores: no call into the dext,
      // which is 14.2 us. Nobody else may touch the CU, and this scheduler
      // holds m_mutex, so the check and the start are already atomic.
      auto rw = m_cu_rw.find(cu);
      if (rw != m_cu_rw.end()) {
        volatile uint32_t* regs = rw->second;
        uint32_t ctrl = regs[0];
        if (ctrl == CU_NO_RESPONSE) {
          abort_cmd(e, "CU " + std::to_string(cu) + " does not respond");
          return true;
        }
        // A chain CU parked holding ap_done accepts nothing until it is
        // acknowledged; here that is a single store. Doing it at start rather
        // than after the previous command also recovers a CU some other
        // process left parked.
        if (cu_is_chain(cu) && (ctrl & CU_AP_DONE)) {
          regs[0] = CU_AP_CONTINUE;
          ctrl = regs[0];
        }
        if (!(ctrl & CU_AP_IDLE) || (ctrl & CU_AP_START))
          continue;
        for (uint32_t i = 0; i < nregs; ++i)
          regs[CU_ARGS / 4 + i] = args[i];
        regs[0] = CU_AP_START;
      }
      else {
        // Otherwise the dext starts it: it checks the CU is idle and writes
        // the arguments and ap_start under one lock, and hands back the
        // control register it read, so there is no read here either.
        uint32_t ctrl = 0;
        // The chain flag rides in with the base: the dext acknowledges a parked
        // CU off the read it does anyway, so this costs no extra call.
        uint64_t exec_base = base | (cu_is_chain(cu) ? XRT_CU_EXEC_CHAIN : 0ULL);
        if (!cu_exec(m_bus, m_dev, exec_base, CU_ARGS, args, nregs, &ctrl)) {
          if (ctrl == CU_NO_RESPONSE) {
            abort_cmd(e, "CU " + std::to_string(cu) + " does not respond");
            return true;
          }
          continue;    // busy: another process is running on this CU
        }
      }
      e.cu = cu;
      e.started = std::chrono::steady_clock::now();
      pkt->state = ERT_CMD_STATE_RUNNING;
      m_running[cu] = e;
      return true;
    }
    return false;
  }

  void
  start_pending()
  {
    for (auto it = m_pending.begin(); it != m_pending.end();) {
      if (try_start(*it))
        it = m_pending.erase(it);
      else
        ++it;
    }
  }

  // Check running CUs; mark finished commands completed.
  int
  poll()
  {
    int completed = 0;
    auto now = std::chrono::steady_clock::now();
    for (auto it = m_running.begin(); it != m_running.end();) {
      uint32_t ctrl = bar()[cu_base(it->first) / 4];   // ap_done clears on read
      if (ctrl == CU_NO_RESPONSE) {
        abort_cmd(it->second, "CU " + std::to_string(it->first) + " stopped responding while running");
        it = m_running.erase(it);
        ++completed;
      }
      else if ((ctrl & CU_AP_DONE)
               || (!cu_is_chain(it->first)
                   && (ctrl & CU_AP_IDLE) && !(ctrl & CU_AP_START))) {
        // A chain CU is only finished when it says ap_done: it may report idle
        // while parked waiting to be acknowledged, so the idle fallback that
        // suits ap_ctrl_hs would complete a command that never ran.
        //
        // The acknowledgement itself is not done here. It happens as part of
        // starting the next command, where the control register is read anyway:
        // doing it here would cost a whole extra call into the dext per command
        // on the shared path, and would leave a CU parked for ever if this
        // process died in between.
        it->second.pkt->state = ERT_CMD_STATE_COMPLETED;
        m_last_run[it->first] =
          std::chrono::duration_cast<std::chrono::microseconds>(now - it->second.started);
        it = m_running.erase(it);
        ++completed;
      }
      else
        ++it;
    }
    if (completed)
      start_pending();
    return completed;
  }

  // Whether to spin on this CU rather than sleep. Caller holds m_mutex.
  bool
  spin_worthwhile(uint32_t cu) const
  {
    auto it = m_last_run.find(cu);
    return it == m_last_run.end() || it->second < spin_window;
  }

  // Caller holds m_mutex.
  std::chrono::microseconds
  poll_interval(std::chrono::steady_clock::time_point now) const
  {
    // Nothing running: the command is queued behind a CU that another
    // command or another process holds. Spinning would not free it, so
    // sleep.
    if (m_running.empty())
      return poll_min;
    // The oldest running command decides the interval. `now` was taken
    // before the lock, so a command may have started after it; clamp the
    // elapsed time at zero rather than relying on the ordering.
    auto oldest = m_running.begin()->second.started;
    uint32_t oldest_cu = m_running.begin()->first;
    for (const auto& [cu, e] : m_running)
      if (e.started < oldest) {
        oldest = e.started;
        oldest_cu = cu;
      }
    auto elapsed = now > oldest
      ? std::chrono::duration_cast<std::chrono::microseconds>(now - oldest)
      : std::chrono::microseconds(0);
    if (elapsed < spin_window && spin_worthwhile(oldest_cu))
      return std::chrono::microseconds(0);      // spin; the MMIO read paces it
    auto interval = elapsed / poll_div;
    return std::clamp(interval, std::chrono::microseconds(poll_min), std::chrono::microseconds(poll_max));
  }

public:
  scheduler(xrt_core::device* device, uint16_t bus, uint16_t dev)
    : m_device(device), m_bus(bus), m_dev(dev)
  {}

  uint16_t bus() const { return m_bus; }
  uint16_t dev() const { return m_dev; }

  // Register (or forget) a read-write mapping of a CU held exclusively.
  void
  set_cu_rw(uint32_t cu, volatile uint32_t* regs)
  {
    std::lock_guard lk(m_mutex);
    if (regs)
      m_cu_rw[cu] = regs;
    else
      m_cu_rw.erase(cu);
  }

  void
  submit(xrt_core::buffer_handle* cmd)
  {
    auto pkt = static_cast<ert_start_kernel_cmd*>(cmd->map(xrt_core::buffer_handle::map_type::write));
    std::lock_guard lk(m_mutex);
    switch (pkt->opcode) {
    case ERT_START_CU:
      break;
    case ERT_CONFIGURE:
      // No embedded scheduler to configure: accept and complete.
      pkt->state = ERT_CMD_STATE_COMPLETED;
      return;
    default:
      xrt_core::message::send(xrt_core::message::severity_level::error, "XRT",
                              "macOS scheduler: unsupported ERT opcode " + std::to_string(pkt->opcode));
      pkt->state = ERT_CMD_STATE_ERROR;
      return;
    }
    pkt->state = ERT_CMD_STATE_QUEUED;
    cmd_entry e{pkt};
    if (!try_start(e))
      m_pending.push_back(e);
  }

  int
  wait(int timeout_ms)
  {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
      std::chrono::microseconds interval;
      auto now = std::chrono::steady_clock::now();
      {
        std::lock_guard lk(m_mutex);
        if (int n = poll())
          return n;
        interval = poll_interval(now);
      }
      if (now >= deadline)
        return 0;
      if (interval == std::chrono::microseconds::zero())
        continue;                                // inside the spin window
      std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(interval, deadline - now));
    }
  }
};

std::shared_ptr<scheduler>
create_scheduler(xrt_core::device* device, uint16_t bus, uint16_t dev)
{
  return std::make_shared<scheduler>(device, bus, dev);
}

void
submit(scheduler* sched, xrt_core::buffer_handle* cmd)
{
  sched->submit(cmd);
}

int
wait(scheduler* sched, int timeout_ms)
{
  return sched->wait(timeout_ms);
}

namespace {

// Single-slot hardware context in the style of the Windows shim.
class hwctx_macos : public xrt_core::hwctx_handle
{
  xrt_core::device* m_device;
  std::shared_ptr<scheduler> m_sched;
  xrt::uuid m_uuid;
  access_mode m_mode;

public:
  hwctx_macos(xrt_core::device* device, std::shared_ptr<scheduler> sched,
              const xrt::uuid& uuid, access_mode mode)
    : m_device(device), m_sched(std::move(sched)), m_uuid(uuid), m_mode(mode)
  {}

  void update_access_mode(access_mode mode) override { m_mode = mode; }
  slot_id get_slotidx() const override { return 0; }
  xrt_core::hwqueue_handle* get_hw_queue() override { return nullptr; }

  std::unique_ptr<xrt_core::buffer_handle>
  alloc_bo(void* userptr, size_t size, uint64_t flags) override
  {
    return m_device->alloc_bo(userptr, size, flags);
  }

  std::unique_ptr<xrt_core::buffer_handle>
  alloc_bo(size_t size, uint64_t flags) override
  {
    return m_device->alloc_bo(size, flags);
  }

  // The dext holds CU contexts, so they arbitrate across processes: the
  // scheduler here is per process and could not see another one's commands.
  xrt_core::cuidx_type
  open_cu_context(const std::string& cuname) override
  {
    auto cuidx = m_device->get_cuidx(0, cuname);
    const auto& cus = m_device->get_cus();
    uint64_t base = cuidx.index < cus.size() ? cus[cuidx.index] : 0;
    bool shared = m_mode == access_mode::shared;
    cu_ctx_open(m_sched->bus(), m_sched->dev(), cuidx.index, shared, base);
    // An exclusive context can have the CU's register page mapped read-write
    // and skip the call into the dext per command. The dext may decline, for
    // instance if the CU shares a page with another connection's CU; then the
    // scheduler simply keeps using the call.
    if (!shared) {
      if (auto regs = map_cu_rw(m_sched->bus(), m_sched->dev(), cuidx.index))
        m_sched->set_cu_rw(cuidx.index, regs);
    }
    return cuidx;
  }

  void
  close_cu_context(xrt_core::cuidx_type cuidx) override
  {
    // Stop using the mapping before the context goes; the mapping itself
    // cannot be revoked by the dext, so dropping it here is what makes it
    // go away (see xrt_dext_abi.h).
    m_sched->set_cu_rw(cuidx.index, nullptr);
    unmap_cu_rw(m_sched->bus(), m_sched->dev(), cuidx.index);
    cu_ctx_close(m_sched->bus(), m_sched->dev(), cuidx.index);
  }

  void
  exec_buf(xrt_core::buffer_handle* cmd) override
  {
    m_sched->submit(cmd);
  }
};

} // namespace

std::unique_ptr<xrt_core::hwctx_handle>
create_hwctx(xrt_core::device* device, std::shared_ptr<scheduler> sched,
             const xrt::uuid& xclbin_uuid, xrt::hw_context::access_mode mode)
{
  return std::make_unique<hwctx_macos>(device, std::move(sched), xclbin_uuid, mode);
}

} // xrt_core::macos
