// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
#define XCL_DRIVER_DLL_EXPORT
#include "device_macos.h"
#include "dext_client.h"
#include "bo_macos.h"
#include "xrt_dext_abi.h"
#include "xrt/detail/xclbin.h"

#include "core/common/query_requests.h"
#include "core/common/query_reset.h"
#include "core/common/smi/smi.h"
#include "core/common/smi/smi_alveo.h"

#include <map>
#include <optional>
#include <memory>

namespace {

namespace query = xrt_core::query;
using key_type = query::key_type;

const xrt_core::macos::pci_function&
fn_of(const xrt_core::device* device)
{
  return static_cast<const xrt_core::device_macos*>(device)->get_pci_function();
}

// Answer PCI identity queries straight from the IOKit registry snapshot.
template <typename QueryRequestType>
struct pci_getter : QueryRequestType
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    using result = typename QueryRequestType::result_type;
    switch (QueryRequestType::key) {
    case key_type::pcie_vendor:                 return result(fn.vendor);
    case key_type::pcie_device:                 return result(fn.device);
    case key_type::pcie_subsystem_vendor:       return result(fn.subsystem_vendor);
    case key_type::pcie_subsystem_id:           return result(fn.subsystem_id);
    case key_type::pcie_link_speed_max:         return result(fn.max_speed());
    case key_type::pcie_express_lane_width_max: return result(fn.max_width());
    case key_type::pcie_link_speed:
      if (!fn.link_status_valid())
        throw query::no_such_key(QueryRequestType::key, "PCIe link status not readable");
      return result(fn.cur_speed());
    case key_type::pcie_express_lane_width:
      if (!fn.link_status_valid())
        throw query::no_such_key(QueryRequestType::key, "PCIe link status not readable");
      return result(fn.cur_width());
    default:
      throw query::no_such_key(QueryRequestType::key);
    }
  }
};

// The shell's logic uuid, for platforms that carry their own description.
// XRT uses it two ways: to find the platform package on the host
// (<firmware dir>/<logic uuid>/partition.xsabin, and the test xclbins beside
// it), and to tell one dynamic-IP shell from another. XDMA 5.x shells have no
// UUID ROM, and the query is then unavailable rather than empty, which is what
// sends XRT to the VBNV-based lookup those cards do have.
struct logic_uuids_getter : query::logic_uuids
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    auto uuid = xrt_core::macos::logic_uuid(fn.bus, fn.dev);
    if (uuid.empty())
      throw query::no_such_key(key_type::logic_uuids,
                               "this shell has no UUID ROM: it is identified by its feature ROM VBNV");
    return query::logic_uuids::result_type{uuid};
  }
};

struct bdf_getter : query::pcie_bdf
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    return query::pcie_bdf::result_type{fn.domain, fn.bus, fn.dev, fn.func};
  }
};

struct device_class_getter : query::device_class
{
  std::any
  get(const xrt_core::device*) const override
  {
    return query::device_class::type::alveo;
  }
};

// Feature ROM fields, read through the xrt_user dext. Without the dext the
// shell name is reported as unknown and the other fields are unavailable.
std::string
fixed_string(const unsigned char* s, size_t n)
{
  size_t len = 0;
  while (len < n && s[len])
    ++len;
  return std::string(reinterpret_cast<const char*>(s), len);
}

template <typename QueryRequestType>
struct rom_getter : QueryRequestType
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    auto rom = xrt_core::macos::read_feature_rom(fn.bus, fn.dev);
    using result = typename QueryRequestType::result_type;
    if (!rom) {
      if constexpr (QueryRequestType::key == key_type::rom_vbnv) {
        // Say which of the two reasons applies. A dynamic-IP shell such as the
        // U50 has a working dext and simply keeps no feature ROM here, and it
        // is identified by its logic uuid instead - so name that when we have
        // it, because it is what the platform package is looked up by.
        // A platform with no feature ROM still has a name; it is in the package
        // its logic uuid names, which is where Linux gets it for such a shell
        // too. Only say "unknown" when there is genuinely nothing to go on.
        if (auto vbnv = xrt_core::macos::shell_vbnv(fn.bus, fn.dev); !vbnv.empty())
          return result(vbnv);

        char buf[192];
        if (!xrt_core::macos::dext_attached(fn.bus, fn.dev))
          std::snprintf(buf, sizeof(buf), "unknown (xrt_user dext not attached; %04x:%04x subsys %04x)",
                        fn.vendor, fn.device, fn.subsystem_id);
        else if (auto uuid = xrt_core::macos::logic_uuid(fn.bus, fn.dev); !uuid.empty())
          std::snprintf(buf, sizeof(buf),
                        "unknown (no feature ROM, and no platform package for logic uuid %s)",
                        uuid.c_str());
        else
          std::snprintf(buf, sizeof(buf), "unknown (no feature ROM; %04x:%04x subsys %04x)",
                        fn.vendor, fn.device, fn.subsystem_id);
        return result(buf);
      }
      throw query::no_such_key(QueryRequestType::key, "feature ROM needs the xrt_user dext");
    }
    if constexpr (QueryRequestType::key == key_type::rom_vbnv)
      return result(fixed_string(rom->VBNVName, sizeof(rom->VBNVName)));
    else if constexpr (QueryRequestType::key == key_type::rom_fpga_name)
      return result(fixed_string(rom->FPGAPartName, sizeof(rom->FPGAPartName)));
    else if constexpr (QueryRequestType::key == key_type::rom_ddr_bank_size_gb)
      return result(rom->DDRChannelSize);
    else if constexpr (QueryRequestType::key == key_type::rom_ddr_bank_count_max)
      return result(rom->DDRChannelCount);
    else if constexpr (QueryRequestType::key == key_type::rom_time_since_epoch)
      return result(rom->TimeSinceEpoch);
    else if constexpr (QueryRequestType::key == key_type::rom_uuid) {
      char buf[33] = {0};
      for (int i = 0; i < 16; ++i)
        std::snprintf(buf + 2 * i, 3, "%02x", rom->uuid[i]);
      return result(buf);
    }
    else if constexpr (QueryRequestType::key == key_type::is_mfg) {
      // Factory (golden) images carry "GOLDEN" in their VBNV.
      return result(fixed_string(rom->VBNVName, sizeof(rom->VBNVName)).find("GOLDEN") != std::string::npos);
    }
    else
      throw query::no_such_key(QueryRequestType::key);
  }
};

// xrt-smi asks every shim which subcommands and options it supports.
struct smi_config_getter : query::xrt_smi_config
{
  std::any
  get(const xrt_core::device*, const std::any& req) const override
  {
    if (std::any_cast<query::xrt_smi_config::type>(req) != query::xrt_smi_config::type::options_config)
      throw query::no_such_key(key, "Not implemented");
    return xrt_core::smi::alveo::get_smi_config();
  }
};

// SYSMON on the management function's BAR0 (XRT devices.h XOCL_RES_SYSMON).
// Card-level, so user and management devices both read it through the
// management function (func 0) of the same card. Conversions follow xocl
// subdev/sysmon.c for UltraScale+ SYSMON4. Shells with an XMC report these
// through the XMC instead; that is not implemented yet.
// A read that never reached a register. On Apple silicon an unclaimed read
// returns filler rather than faulting, and this card has produced 0xdeadfa11
// and 0xdec0de1c as well as all-ones. Filler that reaches a sensor conversion
// comes out as a plausible-looking number: before this was checked, a U50
// reported an FPGA temperature of 29,043,283 C and Vcc Auxillary of 171,074 V.
static bool
is_filler(uint32_t v)
{
  return v == 0xffffffff || v == 0xdeadfa11 || v == 0xdec0de1c;
}

// Where a management block lives on this card, or nothing if the platform has
// no such block. The dext holds the map; the compiled-in XDMA 5.x offset is
// only a default, so a getter that uses it directly reads whatever happens to
// sit there on another platform.
static std::optional<xrt_dext_block>
mgmt_block(const xrt_core::macos::pci_function& fn, uint32_t id)
{
  auto plat = xrt_core::macos::get_platform(fn.bus, fn.dev, 0);
  if (!plat || !plat->blk[id].size)
    return std::nullopt;
  return plat->blk[id];
}

// No sysmon_base here on purpose: where SYSMON lives is a property of the
// platform, so it comes from the map. See mgmt_block().
constexpr uint32_t sysmon_temp    = 0x400;
constexpr uint32_t sysmon_vccint  = 0x404;
constexpr uint32_t sysmon_vccaux  = 0x408;

// Sensors from the board controller. Its register map is a RAM the controller
// fills once running; every register reads 0 before that, so the magic word is
// what says the values mean anything. Each rail and temperature is a triple of
// max/average/instant and this reads the first of each, as xocl's sysfs does.
//
// This is the only source of board-level sensors on a platform with no SYSMON,
// and the only source of rail voltages and currents on any of them.
template <typename QueryRequestType, uint32_t reg>
struct cmc_getter : QueryRequestType
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    auto blk = mgmt_block(fn, XRT_BLK_CMC_REGMAP);
    if (!blk)
      throw query::no_such_key(QueryRequestType::key, "this platform has no board controller");
    uint64_t size = 0;
    auto bar = xrt_core::macos::map_bar0(fn.bus, fn.dev, 0, &size);
    if (!bar || blk->offset + blk->size > size)
      throw query::no_such_key(QueryRequestType::key,
                               "the board controller needs the dext on the management function");
    auto r = bar + blk->offset / 4;
    uint32_t magic = r[XRT_CMC_MAGIC_REG / 4];
    if (magic != XRT_CMC_MAGIC)
      throw query::no_such_key(QueryRequestType::key,
                               magic == 0 ? "the board controller is not running"
                                          : "the board controller is not responding");
    uint32_t v = r[reg / 4];
    if (is_filler(v))
      throw query::no_such_key(QueryRequestType::key, "board controller register is not readable");
    // A rail or current the board does not monitor reads 0; reporting that as a
    // measurement would be a lie, so it is reported as unavailable. A
    // temperature of 0 C is likewise not a real reading from a powered card.
    if (v == 0)
      throw query::no_such_key(QueryRequestType::key, "this board does not monitor that sensor");
    return typename QueryRequestType::result_type(v);
  }
};

template <typename QueryRequestType>
struct sysmon_getter : QueryRequestType
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    // A platform without SYSMON has no FPGA temperature or rail voltages to
    // report here at all - a U50 keeps its sensors behind the CMC instead - and
    // saying so is the only honest answer. Reading the XDMA 5.x offset anyway
    // returns filler that converts to a believable-looking reading.
    auto blk = mgmt_block(fn, XRT_BLK_SYSMON);
    if (!blk)
      throw query::no_such_key(QueryRequestType::key,
                               "this platform has no SYSMON; its sensors are behind a"
                               " board controller, which is not implemented yet");
    uint64_t size = 0;
    auto bar = xrt_core::macos::map_bar0(fn.bus, fn.dev, 0, &size);
    if (!bar || size < blk->offset + blk->size)
      throw query::no_such_key(QueryRequestType::key, "SYSMON needs the dext on the management function");
    const uint32_t base = static_cast<uint32_t>(blk->offset);
    auto rd = [bar, base](uint32_t off) { return bar[(base + off) / 4]; };
    using result = typename QueryRequestType::result_type;
    uint32_t raw = rd(QueryRequestType::key == key_type::temp_fpga ? sysmon_temp
                    : QueryRequestType::key == key_type::int_vcc_millivolts ? sysmon_vccint
                    : sysmon_vccaux);
    if (is_filler(raw))
      throw query::no_such_key(QueryRequestType::key, "SYSMON is not responding");
    if constexpr (QueryRequestType::key == key_type::temp_fpga) {
      int64_t mdeg = ((static_cast<int64_t>(raw) * 509314) >> 16) - 280230;
      return result(mdeg < 0 ? 0 : mdeg / 1000);           // degrees C, like Linux
    }
    else if constexpr (QueryRequestType::key == key_type::int_vcc_millivolts
                       || QueryRequestType::key == key_type::vcc_aux_millivolts)
      return result(static_cast<uint64_t>(raw) * 1000 * 3 >> 16);
    else
      throw query::no_such_key(QueryRequestType::key);
  }
};

// Legacy clock wizards on the management BAR (XRT devices.h __RES_CLOCK_K1
// and __RES_CLOCK_K2). The frequency is computed from the wizard's
// configuration registers with the UltraScale formula from xocl
// subdev/clock_wiz.c: read-only, unlike the frequency counter which needs a
// write to start a measurement.
unsigned int
clkwiz_mhz(volatile uint32_t* bar, uint32_t base)
{
  auto rd = [bar, base](uint32_t off) { return bar[(base + off) / 4]; };
  if ((rd(0x4) & 1) == 0)
    return 0;                                  // wizard not locked
  uint32_t v = rd(0x200);                      // CONFIG(0)
  uint64_t div0 = v & 0xff, mul0 = (v >> 8) & 0xff, mul_frac0 = 0;
  if (v & (1u << 26))
    mul_frac0 = (v >> 16) & 0x3ff;
  mul0 = mul0 * 1000 + mul_frac0;
  div0 *= 1000;
  v = rd(0x208);                               // CONFIG(2)
  uint64_t div1 = v & 0xff, div_frac1 = 0;
  if (v & (1u << 18))
    div_frac1 = (v >> 8) & 0x3ff;
  div1 = div1 * 1000 + div_frac1;
  div0 *= div1;
  mul0 *= 1000;
  constexpr uint64_t input_mhz = 100;
  return div0 ? static_cast<unsigned int>((input_mhz * mul0) / div0) : 0;
}

struct clock_freqs_getter : query::clock_freqs_mhz
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    // The kernel clocks of a dynamic-IP platform are in the partition the
    // xclbin brings, not in the base platform, so there is nothing to read at a
    // fixed management offset. Reporting what is there anyway gives filler.
    auto k1 = mgmt_block(fn, XRT_BLK_CLKWIZ_K1);
    auto k2 = mgmt_block(fn, XRT_BLK_CLKWIZ_K2);
    if (!k1 || !k2) {
      // The clocks are in the partition the xclbin brought. Their counters came
      // with them, so the frequency can be measured - which is better than the
      // wizard reading anyway, being what the hardware runs at rather than what
      // it was asked for.
      query::clock_freqs_mhz::result_type measured;
      for (uint32_t i = 0; i < 3; ++i)
        if (uint32_t mhz = xrt_core::macos::clock_measure(fn.bus, fn.dev, i))
          measured.push_back(std::to_string(mhz));
      if (measured.empty())
        throw query::no_such_key(key, "this platform's kernel clocks are in the user"
                                      " partition and no frequency counter answered");
      return measured;
    }
    uint64_t size = 0;
    auto bar = xrt_core::macos::map_bar0(fn.bus, fn.dev, 0, &size);
    if (!bar || size < k2->offset + k2->size)
      throw query::no_such_key(key, "clocks need the dext on the management function");
    return query::clock_freqs_mhz::result_type{
      std::to_string(clkwiz_mhz(bar, static_cast<uint32_t>(k1->offset))),
      std::to_string(clkwiz_mhz(bar, static_cast<uint32_t>(k2->offset)))
    };
  }
};

// Linux reports flash_type and board_name straight from the board table in
// xocl devices.h, keyed by management device ID and subsystem ID. This is
// that table for the functions the dext matches. board_name is unset for all
// of them, so Linux reports it empty too.
std::string
board_flash_type(const xrt_core::macos::pci_function& fn)
{
  // Management IDs; user-function IDs map to their management sibling.
  uint16_t mgmt_id = fn.device;
  switch (fn.device) {
  case 0x6a90: mgmt_id = 0x6a8f; break;
  case 0x5001: mgmt_id = 0x5000; break;
  case 0x5005: mgmt_id = 0x5004; break;
  case 0x5021: mgmt_id = 0x5020; break;
  default: break;
  }
  switch (mgmt_id) {
  case 0x5000:                    // MGMT_XBB_DSA52_U200
  case 0x5004:                    // MGMT_XBB_DSA52
  case 0x5020:                    // MGMT_U50
    return "spi";
  case 0x6a8f:                    // MGMT_6A8F (4351) sets spi; DSA50/DSA52 do not
    return fn.subsystem_id == 0x4351 ? "spi" : "";
  default:
    return "";
  }
}

struct flash_type_getter : query::flash_type
{
  std::any
  get(const xrt_core::device* device) const override
  {
    return board_flash_type(fn_of(device));
  }
};

struct board_name_getter : query::board_name
{
  std::any
  get(const xrt_core::device*) const override
  {
    return std::string{};
  }
};

// Recovery mode exists only on Versal shells; XDMA shells never report it.
struct is_recovery_getter : query::is_recovery
{
  std::any
  get(const xrt_core::device*) const override
  {
    return false;
  }
};

struct smi_lists_getter : query::xrt_smi_lists
{
  std::any
  get(const xrt_core::device*, const std::any& req) const override
  {
    switch (std::any_cast<query::xrt_smi_lists::type>(req)) {
    case query::xrt_smi_lists::type::validate_tests:
      return xrt_core::smi::get_list("validate", "run");
    case query::xrt_smi_lists::type::examine_reports:
      return xrt_core::smi::get_list("examine", "report");
    case query::xrt_smi_lists::type::configure_option_options:
      return xrt_core::smi::get_option_options("configure");
    case query::xrt_smi_lists::type::subcommands:
      return xrt_core::smi::alveo::get_subcommands_list();
    default:
      throw query::no_such_key(key, "Not implemented");
    }
  }
};

// UUID recorded by the dext after a successful load. Throws when nothing is
// recorded so XRT treats the device as having no xclbin.
struct xclbin_uuid_getter : query::xclbin_uuid
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    auto uuid = xrt_core::macos::loaded_xclbin_uuid(fn.bus, fn.dev);
    if (uuid.empty())
      throw query::no_such_key(key, "no xclbin loaded by the xrt_user dext");
    return uuid;
  }
};

// One entry per XDMA channel pair, "<h2c bytes> <c2h bytes>" like xocl's
// channel_stat_raw. The dext does not count bytes yet, so both read 0.
struct dma_threads_getter : query::dma_threads_raw
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    uint64_t size = 0;
    // The XDMA engine is on BAR1 of the XDMA 5.x shells and BAR2 of a U50; the
    // dext knows which from the platform map it was given.
    uint32_t xdma_bar = XRT_XDMA_BAR;
    if (auto plat = xrt_core::macos::get_platform(fn.bus, fn.dev, 1);
        plat && plat->blk[XRT_BLK_XDMA].size)
      xdma_bar = plat->blk[XRT_BLK_XDMA].bar;
    auto bar = xrt_core::macos::map_bar(fn.bus, fn.dev, 1, xdma_bar, &size);
    if (!bar || size < 0x2000)
      throw query::no_such_key(key, "XDMA BAR not available through the xrt_user dext");
    query::dma_threads_raw::result_type channels;
    for (uint32_t ch = 0; ch < 4; ++ch) {
      bool h2c = (bar[XRT_XDMA_H2C(ch) / 4] >> 16) == 0x1fc0;
      bool c2h = (bar[XRT_XDMA_C2H(ch) / 4] >> 16) == 0x1fc1;
      if (h2c && c2h)
        channels.emplace_back("0 0");
    }
    return channels;
  }
};

struct pcie_id_getter : query::pcie_id
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    query::pcie_id::result_type id{};
    id.device_id = fn.device;
    id.revision_id = fn.revision;
    return id;
  }
};

// MIG calibration status: bit0 of the memcalib register on the management
// BAR (XRT devices.h __RES_PRP_IORES_MGMT), as xocl icap reads it.
struct mig_calibrated_getter : query::status_mig_calibrated
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    auto blk = mgmt_block(fn, XRT_BLK_MEMCALIB);
    if (!blk)
      throw query::no_such_key(key, "this platform has no memory calibration register");
    uint64_t size = 0;
    auto bar = xrt_core::macos::map_bar0(fn.bus, fn.dev, 0, &size);
    if (!bar || size <= blk->offset)
      throw query::no_such_key(key, "memcalib register needs the dext on the management function");
    uint32_t v = bar[blk->offset / 4];
    if (is_filler(v))
      throw query::no_such_key(key, "memory calibration register is not responding");
    return static_cast<bool>(v & 1);
  }
};

// Per-bank usage, "<bytes used> <buffer count>" per mem_topology entry like
// xocl's memstat_raw, from the dext's allocator (all processes).
struct memstat_getter : query::memstat_raw
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    auto raw = xrt_core::macos::loaded_section(fn.bus, fn.dev, XRT_SECTION_MEM_TOPOLOGY);
    if (raw.size() < sizeof(mem_topology))
      throw query::no_such_key(key, "no xclbin metadata kept by the xrt_user dext");
    auto topo = reinterpret_cast<const mem_topology*>(raw.data());
    query::memstat_raw::result_type stats;
    for (int32_t i = 0; i < topo->m_count; ++i) {
      const auto& m = topo->m_mem_data[i];
      auto [used, count] = xrt_core::macos::mem_stat(fn.bus, fn.dev, m.m_base_address, m.m_size * 1024);
      stats.push_back(std::to_string(used) + " " + std::to_string(count));
    }
    return stats;
  }
};

// Raw sections of the loaded xclbin, kept by the dext across processes. A
// second slot is a fallback for sections an xclbin need not carry: the group
// topology is only there when something is grouped, and the plain memory
// topology answers for the rest.
template <typename QueryRequestType, uint32_t slot, uint32_t fallback_slot = slot>
struct section_getter : QueryRequestType
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    auto data = xrt_core::macos::loaded_section(fn.bus, fn.dev, slot);
    if (data.empty() && fallback_slot != slot)
      data = xrt_core::macos::loaded_section(fn.bus, fn.dev, fallback_slot);
    if (data.empty())
      throw query::no_such_key(QueryRequestType::key, "no xclbin metadata kept by the xrt_user dext");
    return data;
  }
};

// The FPGA die temperature comes from SYSMON on a platform that has one and
// from the board controller otherwise. Both can be absent, in which case there
// is no reading and saying so is the answer - a card that reports 0 C or a
// converted filler value is worse than one that reports nothing.
struct fpga_temp_getter : query::temp_fpga
{
  std::any
  get(const xrt_core::device* device) const override
  {
    try {
      return sysmon_getter<query::temp_fpga>{}.get(device);
    }
    catch (const query::no_such_key&) {
      return cmc_getter<query::temp_fpga, XRT_CMC_FPGA_TEMP>{}.get(device);
    }
  }
};

// The board's own name and power rating, from the controller's mailbox. These
// are what tells `validate` whether this board has an aux power connector and
// whether it is plugged in - it matches the name against a list and treats a
// zero power level as "not connected".
struct xmc_board_name_getter : query::xmc_board_name
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    auto v = xrt_core::macos::cmc_board_info(fn.bus, fn.dev, XRT_BDINFO_NAME);
    if (v.empty())
      throw query::no_such_key(key, "the board controller did not report a board name");
    // NUL-terminated in the blob; trim rather than trusting the length.
    std::string s(reinterpret_cast<const char*>(v.data()), v.size());
    if (auto z = s.find('\0'); z != std::string::npos)
      s.resize(z);
    return s;
  }
};

// The satellite controller's firmware version, which the controller reports
// alongside the board info. Visible in the platform report and used by
// `validate`'s sc-version test.
struct sc_version_getter : query::xmc_sc_version
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    auto v = xrt_core::macos::cmc_board_info(fn.bus, fn.dev, XRT_BDINFO_BMC_VER);
    if (v.empty())
      throw query::no_such_key(key, "the board controller did not report an SC version");
    std::string s(reinterpret_cast<const char*>(v.data()), v.size());
    if (auto z = s.find('\0'); z != std::string::npos)
      s.resize(z);
    if (s.empty())
      throw query::no_such_key(key, "the board controller reported an empty SC version");
    return s;
  }
};

struct max_power_getter : query::max_power_level
{
  std::any
  get(const xrt_core::device* device) const override
  {
    const auto& fn = fn_of(device);
    auto v = xrt_core::macos::cmc_board_info(fn.bus, fn.dev, XRT_BDINFO_MAX_PWR);
    if (v.empty())
      throw query::no_such_key(key, "the board controller did not report a power rating");
    // A single byte on the boards seen so far; a longer value is decimal text.
    if (v.size() == 1)
      return static_cast<uint64_t>(v[0]);
    std::string s(reinterpret_cast<const char*>(v.data()), v.size());
    if (auto z = s.find('\0'); z != std::string::npos)
      s.resize(z);
    try { return static_cast<uint64_t>(std::stoull(s)); }
    catch (const std::exception&) {
      throw query::no_such_key(key, "the power rating was not a number: '" + s + "'");
    }
  }
};

std::map<key_type, std::unique_ptr<query::request>> query_tbl;

template <typename QueryRequestType, typename Getter>
void
emplace()
{
  query_tbl.emplace(QueryRequestType::key, std::make_unique<Getter>());
}

void
initialize_query_table()
{
  emplace<query::pcie_vendor,                 pci_getter<query::pcie_vendor>>();
  emplace<query::pcie_device,                 pci_getter<query::pcie_device>>();
  emplace<query::pcie_subsystem_vendor,       pci_getter<query::pcie_subsystem_vendor>>();
  emplace<query::pcie_subsystem_id,           pci_getter<query::pcie_subsystem_id>>();
  emplace<query::pcie_link_speed,             pci_getter<query::pcie_link_speed>>();
  emplace<query::pcie_link_speed_max,         pci_getter<query::pcie_link_speed_max>>();
  emplace<query::pcie_express_lane_width,     pci_getter<query::pcie_express_lane_width>>();
  emplace<query::pcie_express_lane_width_max, pci_getter<query::pcie_express_lane_width_max>>();
  emplace<query::pcie_bdf,                    bdf_getter>();
  emplace<query::device_class,                device_class_getter>();
  emplace<query::logic_uuids,                 logic_uuids_getter>();
  emplace<query::rom_vbnv,                    rom_getter<query::rom_vbnv>>();
  emplace<query::rom_fpga_name,               rom_getter<query::rom_fpga_name>>();
  emplace<query::rom_ddr_bank_size_gb,        rom_getter<query::rom_ddr_bank_size_gb>>();
  emplace<query::rom_ddr_bank_count_max,      rom_getter<query::rom_ddr_bank_count_max>>();
  emplace<query::rom_time_since_epoch,        rom_getter<query::rom_time_since_epoch>>();
  emplace<query::rom_uuid,                    rom_getter<query::rom_uuid>>();
  emplace<query::is_mfg,                      rom_getter<query::is_mfg>>();
  // Where there is a SYSMON it is the FPGA die sensor, as on the XDMA 5.x
  // shells; where there is not, the board controller has one.
  emplace<query::temp_fpga,                   fpga_temp_getter>();
  emplace<query::int_vcc_millivolts,          sysmon_getter<query::int_vcc_millivolts>>();
  emplace<query::vcc_aux_millivolts,          sysmon_getter<query::vcc_aux_millivolts>>();
  // Rail voltages and currents exist only on the board controller.
  emplace<query::v12v_pex_millivolts,         cmc_getter<query::v12v_pex_millivolts, 0x20>>();
  emplace<query::v12v_pex_milliamps,          cmc_getter<query::v12v_pex_milliamps, 0xc8>>();
  emplace<query::v12v_aux_millivolts,         cmc_getter<query::v12v_aux_millivolts, 0x44>>();
  emplace<query::v3v3_pex_millivolts,         cmc_getter<query::v3v3_pex_millivolts, 0x2c>>();
  emplace<query::xmc_board_name,              xmc_board_name_getter>();
  emplace<query::xmc_sc_version,              sc_version_getter>();
  emplace<query::max_power_level,             max_power_getter>();
  emplace<query::flash_type,                  flash_type_getter>();
  emplace<query::clock_freqs_mhz,             clock_freqs_getter>();
  emplace<query::xclbin_uuid,                 xclbin_uuid_getter>();
  emplace<query::dma_threads_raw,             dma_threads_getter>();
  emplace<query::mem_topology_raw,
          section_getter<query::mem_topology_raw, XRT_SECTION_MEM_TOPOLOGY>>();
  emplace<query::ip_layout_raw,
          section_getter<query::ip_layout_raw, XRT_SECTION_IP_LAYOUT>>();
  // An xclbin that groups banks behind an argument carries its own group
  // topology; where it does not, the memory topology is the answer, which is
  // the case for every XDMA 5.x test xclbin.
  emplace<query::group_topology,
          section_getter<query::group_topology, XRT_SECTION_GROUP_TOPOLOGY,
                         XRT_SECTION_MEM_TOPOLOGY>>();
  emplace<query::pcie_id,                     pcie_id_getter>();
  emplace<query::status_mig_calibrated,       mig_calibrated_getter>();
  emplace<query::memstat_raw,                 memstat_getter>();
  emplace<query::board_name,                  board_name_getter>();
  emplace<query::is_recovery,                 is_recovery_getter>();
  emplace<query::xrt_smi_lists,               smi_lists_getter>();
  emplace<query::xrt_smi_config,              smi_config_getter>();
}

struct init { init() { initialize_query_table(); } };
init s_init;

} // namespace

namespace xrt_core {

device_macos::
device_macos(handle_type device_handle, id_type device_id, bool user,
             const macos::pci_function& fn)
  : shim<device_pcie>(device_handle, device_id, user)
  , m_fn(fn)
  , m_sched(macos::create_scheduler(this, fn.bus, fn.dev))
{
  // Before anything reads a register: make sure the dexts are using this
  // card's register map rather than the XDMA 5.x offsets they assume.
  macos::install_platform(fn.bus, fn.dev);
}

std::unique_ptr<hwctx_handle>
device_macos::
create_hw_context(const xrt::uuid& xclbin_uuid, const xrt::hw_context::cfg_param_type&,
                  xrt::hw_context::access_mode mode) const
{
  return macos::create_hwctx(const_cast<device_macos*>(this), m_sched, xclbin_uuid, mode);
}

void
device_macos::
exec_buf(buffer_handle* cmd)
{
  macos::submit(m_sched.get(), cmd);
}

int
device_macos::
exec_wait(int timeout_ms) const
{
  return macos::wait(m_sched.get(), timeout_ms);
}

void
device_macos::
reset(query::reset_type& type) const
{
  if (type.get_key() != query::reset_key::hot)
    throw xrt_core::error(std::errc::not_supported, "only hot reset is supported on macOS");
  macos::hot_reset(m_fn.bus, m_fn.dev);
}

void
device_macos::
get_device_info(xclDeviceInfo2* info)
{
  // Same sources as the query table: PCI registry, feature ROM through the
  // dext, SYSMON / clock wizards / memcalib through the management BAR.
  std::memset(info, 0, sizeof(*info));
  info->mMagic = 0x586C0C6C;
  info->mHALMajorVersion = XCLHAL_MAJOR_VER;
  info->mHALMinorVersion = XCLHAL_MINOR_VER;
  info->mVendorId = m_fn.vendor;
  info->mDeviceId = m_fn.device;
  info->mSubsystemId = m_fn.subsystem_id;
  info->mSubsystemVendorId = m_fn.subsystem_vendor;
  info->mDeviceVersion = m_fn.subsystem_id & 0xff;
  info->mDataAlignment = 16384;          // Apple silicon page size (Linux: getpagesize())
  info->mMinTransferSize = 64;           // DDR_BUFFER_ALIGNMENT in the Linux shim
  info->mDMAThreads = 2;
  info->mPciSlot = (m_fn.domain << 16) + (m_fn.bus << 8) + (m_fn.dev << 3) + m_fn.func;
  info->mPCIeLinkWidth = static_cast<unsigned short>(m_fn.cur_width());
  info->mPCIeLinkSpeed = static_cast<unsigned short>(m_fn.cur_speed());
  info->mPCIeLinkWidthMax = static_cast<unsigned short>(m_fn.max_width());
  info->mPCIeLinkSpeedMax = static_cast<unsigned short>(m_fn.max_speed());

  if (auto rom = macos::read_feature_rom(m_fn.bus, m_fn.dev)) {
    std::snprintf(info->mName, sizeof(info->mName), "%s", fixed_string(rom->VBNVName, sizeof(rom->VBNVName)).c_str());
    std::snprintf(info->mFpga, sizeof(info->mFpga), "%s", fixed_string(rom->FPGAPartName, sizeof(rom->FPGAPartName)).c_str());
    info->mTimeStamp = rom->TimeSinceEpoch;
    info->mDDRBankCount = rom->DDRChannelCount;
    info->mDDRSize = static_cast<size_t>(rom->DDRChannelSize) * rom->DDRChannelCount * (1ULL << 30);
  }

  // The rest of this legacy structure is read straight off the management BAR,
  // so every block has to come from the platform map: on a card that is not
  // XDMA 5.x these offsets are somebody else's registers, or nothing at all,
  // and filler converts into believable-looking clocks and temperatures.
  uint64_t size = 0;
  auto bar = macos::map_bar0(m_fn.bus, m_fn.dev, 0, &size);
  if (!bar)
    return;
  auto blk = [&](uint32_t id) -> std::optional<xrt_dext_block> { return mgmt_block(m_fn, id); };
  auto in_bar = [&](const std::optional<xrt_dext_block>& b) {
    return b && b->offset + b->size <= size;
  };

  auto k1 = blk(XRT_BLK_CLKWIZ_K1), k2 = blk(XRT_BLK_CLKWIZ_K2);
  if (in_bar(k1) && in_bar(k2)) {
    info->mOCLFrequency[0] = static_cast<unsigned short>(clkwiz_mhz(bar, static_cast<uint32_t>(k1->offset)));
    info->mOCLFrequency[1] = static_cast<unsigned short>(clkwiz_mhz(bar, static_cast<uint32_t>(k2->offset)));
    info->mNumClocks = 2;
  }
  if (auto mc = blk(XRT_BLK_MEMCALIB); in_bar(mc)) {
    uint32_t v = bar[mc->offset / 4];
    if (!is_filler(v))
      info->mMigCalib = v & 1;
  }
  if (auto sm = blk(XRT_BLK_SYSMON); in_bar(sm)) {
    const uint32_t sbase = static_cast<uint32_t>(sm->offset);
    auto rd = [bar, sbase](uint32_t off) { return bar[(sbase + off) / 4]; };
    auto mv = [](uint32_t raw) {
      return static_cast<unsigned short>(static_cast<uint64_t>(raw) * 3000 >> 16);
    };
    if (uint32_t t = rd(sysmon_temp); !is_filler(t)) {
      int64_t mdeg = ((static_cast<int64_t>(t) * 509314) >> 16) - 280230;
      info->mOnChipTemp = static_cast<unsigned short>(mdeg > 0 ? mdeg / 1000 : 0);
    }
    if (uint32_t v = rd(sysmon_vccint); !is_filler(v)) info->mVInt = mv(v);
    if (uint32_t v = rd(sysmon_vccaux); !is_filler(v)) info->mVAux = mv(v);
    if (uint32_t v = rd(0x418);         !is_filler(v)) info->mVBram = mv(v);
  }
}

void
device_macos::
set_cu_read_range(cuidx_type cuidx, uint32_t start, uint32_t size)
{
  m_read_ranges[cuidx.index] = {start, size};
}

void
device_macos::
reg_read(uint32_t ipidx, uint32_t offset, uint32_t* data) const
{
  const auto& cus = get_cus();
  if (ipidx >= cus.size())
    throw xrt_core::error(std::errc::invalid_argument, "CU index out of range");
  auto it = m_read_ranges.find(ipidx);
  if (it != m_read_ranges.end()
      && (offset < it->second.first || offset + 4 > it->second.first + it->second.second))
    throw xrt_core::error(std::errc::permission_denied, "register outside the granted read range");
  uint64_t size = 0;
  auto bar = macos::map_bar0(m_fn.bus, m_fn.dev, 1, &size);
  uint64_t addr = cus[ipidx] + offset;
  if (!bar || addr + 4 > size)
    throw xrt_core::error(std::errc::io_error, "cannot map user BAR0 through the xrt_user dext");
  *data = bar[addr / 4];
}

void
device_macos::
reg_write(uint32_t ipidx, uint32_t offset, uint32_t data)
{
  const auto& cus = get_cus();
  if (ipidx >= cus.size())
    throw xrt_core::error(std::errc::invalid_argument, "CU index out of range");
  macos::cu_write(m_fn.bus, m_fn.dev, cus[ipidx] + offset, &data, 1);
}

void
device_macos::
unmgd_pread(void* buffer, size_t size, uint64_t offset)
{
  if (size)
    macos::dma_sync(m_fn.bus, m_fn.dev, offset, buffer, size, false);
}

void
device_macos::
unmgd_pwrite(const void* buffer, size_t size, uint64_t offset)
{
  // Host-to-card only reads the buffer.
  if (size)
    macos::dma_sync(m_fn.bus, m_fn.dev, offset, const_cast<void*>(buffer), size, true);
}

std::unique_ptr<buffer_handle>
device_macos::
alloc_bo(size_t size, uint64_t flags)
{
  return xrt_core::macos::alloc_bo(this, m_fn.bus, m_fn.dev, nullptr, size, flags);
}

std::unique_ptr<buffer_handle>
device_macos::
alloc_bo(void* userptr, size_t size, uint64_t flags)
{
  return xrt_core::macos::alloc_bo(this, m_fn.bus, m_fn.dev, userptr, size, flags);
}

void
device_macos::
xclmgmt_load_xclbin(span<char> buffer) const
{
  xrt_core::macos::load_xclbin(m_fn.bus, m_fn.dev, reinterpret_cast<const axlf*>(buffer.data()));
}

const query::request&
device_macos::
lookup_query(query::key_type query_key) const
{
  auto it = query_tbl.find(query_key);
  if (it == query_tbl.end())
    throw query::no_such_key(query_key);
  return *(it->second);
}

} // xrt_core
