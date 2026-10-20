// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
#define XCL_DRIVER_DLL_EXPORT
#include "bo_macos.h"
#include "dext_client.h"
#include "xrt_dext_abi.h"

#include "core/common/device.h"
#include "core/common/error.h"
#include "xrt/detail/xclbin.h"
#include "xrt/detail/xrt_mem.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr size_t host_alignment = 16384;   // Apple silicon DART page

// A DDR bank of the loaded xclbin. Allocation is done by the dext, which
// owns device memory for all processes on the card (Phase 7).
class bank_allocator
{
  uint16_t m_bus, m_dev;
  uint64_t m_base;
  uint64_t m_size;
public:
  bank_allocator(uint16_t bus, uint16_t dev, uint64_t base, uint64_t size)
    : m_bus(bus), m_dev(dev), m_base(base), m_size(size) {}

  uint64_t
  alloc(uint64_t size)
  {
    return xrt_core::macos::mem_alloc(m_bus, m_dev, m_base, m_size, size);
  }

  void
  free(uint64_t addr)
  {
    xrt_core::macos::mem_free(m_bus, m_dev, addr);
  }
};

std::mutex s_mutex;
std::map<const void*, std::pair<uint16_t, uint16_t>> s_bus_dev;   // device -> bus/dev
std::map<std::pair<const void*, uint16_t>, std::shared_ptr<bank_allocator>> s_banks;

std::shared_ptr<bank_allocator>
bank_for(const xrt_core::device* device, uint16_t bank)
{
  std::lock_guard lk(s_mutex);
  auto key = std::make_pair(static_cast<const void*>(device), bank);
  if (auto it = s_banks.find(key); it != s_banks.end())
    return it->second;
  // The bank index here is what xrt::kernel::group_id() returned, and that
  // indexes the GROUP topology: an xclbin whose argument is backed by several
  // banks appends one merged entry per group, so the group index can be one
  // past the end of the plain memory topology. A U50 bandwidth.xclbin has 37
  // memory entries and asks for index 37, MBG[0:31]. Where an xclbin groups
  // nothing the two sections are the same, which is why this went unnoticed.
  //
  // This process's registered xclbin first, else what the dext kept for the
  // xclbin another process loaded.
  auto topo = device->get_axlf_section<const mem_topology*>(ASK_GROUP_TOPOLOGY);
  if (!topo)
    topo = device->get_axlf_section<const mem_topology*>(MEM_TOPOLOGY);
  static std::map<const void*, std::vector<char>> kept;
  if (!topo) {
    auto& raw = kept[device];
    const auto& fn_bus_dev = s_bus_dev[device];
    for (uint32_t slot : { XRT_SECTION_GROUP_TOPOLOGY, XRT_SECTION_MEM_TOPOLOGY }) {
      raw = xrt_core::macos::loaded_section(fn_bus_dev.first, fn_bus_dev.second, slot);
      if (raw.size() >= sizeof(mem_topology)) {
        topo = reinterpret_cast<const mem_topology*>(raw.data());
        break;
      }
    }
  }
  if (!topo)
    throw xrt_core::error(std::errc::no_such_device, "no xclbin loaded: memory topology unknown");
  if (bank >= topo->m_count)
    throw xrt_core::error(std::errc::invalid_argument, "memory bank " + std::to_string(bank) + " out of range (topology has "
                          + std::to_string(topo->m_count) + " entries)");
  const auto& m = topo->m_mem_data[bank];
  // Device memory the XDMA engine can move data into: the DDR variants of the
  // XDMA 5.x cards and the HBM of a U50, whose verify.xclbin has exactly one
  // used bank and it is MEM_HBM. Streaming, host and on-chip (BRAM/URAM) banks
  // are not DMA targets and stay rejected.
  const bool dma_target = m.m_type == MEM_DDR3 || m.m_type == MEM_DDR4
                       || m.m_type == MEM_DRAM || m.m_type == MEM_HBM;
  if (!m.m_used || !dma_target)
    throw xrt_core::error(std::errc::not_supported,
                          "memory bank " + std::to_string(bank)
                          + " is not a used device memory bank the driver can DMA into");
  const auto& fn_bus_dev2 = s_bus_dev[device];
  auto alloc = std::make_shared<bank_allocator>(fn_bus_dev2.first, fn_bus_dev2.second,
                                                m.m_base_address, m.m_size * 1024);   // m_size is KB
  s_banks[key] = alloc;
  return alloc;
}

class bo_macos : public xrt_core::buffer_handle
{
  uint16_t m_bus, m_dev;
  void* m_host = nullptr;
  bool m_own_host = false;
  size_t m_size;
  uint64_t m_flags;
  uint64_t m_card = 0;
  std::shared_ptr<bank_allocator> m_bank;

public:
  bo_macos(const xrt_core::device* device, uint16_t bus, uint16_t dev,
           void* userptr, size_t size, uint64_t flags)
    : m_bus(bus), m_dev(dev), m_size(size), m_flags(flags)
  {
    xcl_bo_flags xflags{flags};
    if (xflags.flags & XCL_BO_FLAGS_EXECBUF) {
      // Command buffer: host memory only, read by the shim's scheduler.
      size_t rounded = (size + host_alignment - 1) & ~(host_alignment - 1);
      if (posix_memalign(&m_host, host_alignment, rounded) != 0)
        throw xrt_core::error(std::errc::not_enough_memory, "command buffer allocation failed");
      std::memset(m_host, 0, rounded);
      m_own_host = true;
      return;
    }
    if (xflags.flags & (XCL_BO_FLAGS_HOST_ONLY | XCL_BO_FLAGS_P2P | XCL_BO_FLAGS_SVM))
      throw xrt_core::error(std::errc::not_supported, "host-only, P2P and SVM buffers are not supported on macOS");
    if (size == 0)
      throw xrt_core::error(std::errc::invalid_argument, "zero-size buffer");
    m_bank = bank_for(device, xflags.bank);
    m_card = m_bank->alloc(size);
    if (!(xflags.flags & XCL_BO_FLAGS_DEV_ONLY)) {
      if (userptr) {
        m_host = userptr;
      }
      else {
        size_t rounded = (size + host_alignment - 1) & ~(host_alignment - 1);
        if (posix_memalign(&m_host, host_alignment, rounded) != 0) {
          m_bank->free(m_card);
          throw xrt_core::error(std::errc::not_enough_memory, "host buffer allocation failed");
        }
        std::memset(m_host, 0, rounded);
        m_own_host = true;
      }
    }
  }

  ~bo_macos() override
  {
    if (m_own_host)
      std::free(m_host);
    if (m_bank)
      m_bank->free(m_card);
  }

  std::unique_ptr<xrt_core::shared_handle>
  share() const override
  {
    throw xrt_core::error(std::errc::not_supported, "buffer export is not supported on macOS");
  }

  void*
  map(map_type) override
  {
    if (!m_host)
      throw xrt_core::error(std::errc::invalid_argument, "device-only buffer cannot be mapped");
    return m_host;
  }

  void
  unmap(void*) override
  {}

  void
  sync(direction dir, size_t size, size_t offset) override
  {
    if (!m_host)
      throw xrt_core::error(std::errc::invalid_argument, "device-only buffer cannot be synced");
    if (!m_bank)
      return;                                    // command buffer: nothing on the card
    if (offset + size > m_size)
      throw xrt_core::error(std::errc::invalid_argument, "sync range outside buffer");
    if (size == 0)
      return;
    xrt_core::macos::dma_sync(m_bus, m_dev, m_card + offset, static_cast<char*>(m_host) + offset, size,
                              dir == direction::host2device);
  }

  void
  copy(const buffer_handle*, size_t, size_t, size_t) override
  {
    throw xrt_core::error(std::errc::not_supported, "device-to-device copy (M2M) is not supported on macOS");
  }

  properties
  get_properties() const override
  {
    return {m_flags, m_size, m_card, 0};
  }
};

} // namespace

namespace xrt_core::macos {

std::unique_ptr<xrt_core::buffer_handle>
alloc_bo(const xrt_core::device* device, uint16_t bus, uint16_t dev,
         void* userptr, size_t size, uint64_t flags)
{
  {
    std::lock_guard lk(s_mutex);
    s_bus_dev[device] = {bus, dev};
  }
  return std::make_unique<bo_macos>(device, bus, dev, userptr, size, flags);
}

} // xrt_core::macos
