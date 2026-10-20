// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Phase 2 of the XRT macOS port: talk to the xrt_user DriverKit extension
// (org.xrtmac.XrtUser) through its IOUserClient.
#ifndef XRT_CORE_PCIE_MACOS_DEXT_CLIENT_H
#define XRT_CORE_PCIE_MACOS_DEXT_CLIENT_H

#include <cstdint>          // xclfeatures.h uses uint*_t without including it
#include <optional>
#include <string>
#include <vector>
#include <utility>
#include "xclfeatures.h"
#include "xrt_dext_abi.h"

struct axlf;   // xrt/detail/xclbin.h

namespace xrt_core::macos {

// Feature ROM of the card at bus/dev, read through the user function's
// dext. Empty if the dext is not installed or not attached to that card.
std::optional<FeatureRomHeader>
read_feature_rom(uint16_t bus, uint16_t dev);

// The register map the dext on bus/dev/func is using, or nothing if that
// function has no dext. See xrt_dext_abi.h: a block with size 0 is absent.
std::optional<xrt_dext_platform>
get_platform(uint16_t bus, uint16_t dev, uint16_t func);

// Make sure the dexts on this card are using the right register map: work out
// what this platform is and, if it is not one the dext already assumes, install
// the map. Idempotent and safe to call from every process - a map may only be
// installed once per dext lifetime, and a second attempt is not an error.
//
// Does nothing for an XDMA 5.x card: the dext's compiled-in defaults are that
// card's map already.
void
install_platform(uint16_t bus, uint16_t dev);

// Move the CU register window on the user function to cover the CUs this
// xclbin declares. A no-op when they already fit where the driver looks by
// default, which is the case for every XDMA 5.x card - so those keep exactly
// the bounds they have always had.
void
set_cu_window(uint16_t bus, uint16_t dev, const ::axlf* top);

// The shell's name, for a platform that has no feature ROM to carry it. Taken
// from the platform package the card's logic uuid names, the same way Linux
// names a 2RP shell: the VBNV out of partition.xsabin's header, with ':' and '.'
// normalised to '_'. Empty if there is no package for this card.
std::string
shell_vbnv(uint16_t bus, uint16_t dev);

// The frequency a kernel clock is actually running at, in MHz, measured with the
// counter the loaded xclbin brought with it. 0 if there is no counter for that
// clock or it did not measure.
uint32_t
clock_measure(uint16_t bus, uint16_t dev, uint32_t index);

// One entry of the board controller's board info, by key (XRT_BDINFO_*). Empty
// if the controller is not running, has no mailbox, or does not carry that key.
// The values are mostly strings; a numeric one comes back as its bytes.
std::vector<uint8_t>
cmc_board_info(uint16_t bus, uint16_t dev, uint8_t key);

// Bring the board controller up from the firmware in the platform package, so
// the card has sensors at all. Returns its version, or 0 if there is none to
// start or it could not be started - neither of which is an error. The
// controller runs from RAM, so this is needed once per power cycle.
uint32_t
load_cmc(uint16_t bus, uint16_t dev);

// Tell the dext which card addresses this xclbin's memory occupies, so DMA and
// the allocator can reach it. A no-op when the banks already fall inside the
// XDMA 5.x span the driver assumes by default.
void
set_card_range(uint16_t bus, uint16_t dev, const ::axlf* top);

// True if the xrt_user dext is attached to either function of the card at
// bus/dev. A read that comes back empty does not distinguish "no driver" from
// "driver attached, but this shell has no such block"; this does.
bool
dext_attached(uint16_t bus, uint16_t dev);

// Logic uuid of the shell in the card at bus/dev: the 32-character lowercase
// string that names the directory its description is shipped in
// (<firmware dir>/<logic uuid>/partition.xsabin). Read from the UUID ROM that
// the management function's PCIe vendor capability points at.
//
// Empty for a shell that carries no such capability, which is every XDMA 5.x
// card including the VCU1525 - those are identified by the VBNV in their fixed
// feature ROM instead.
std::string
logic_uuid(uint16_t bus, uint16_t dev);

// Read-only mapping of BAR0 of the function at bus/dev/func through its
// dext. The mapping lives for the rest of the process. nullptr if the dext
// is not attached to that function.
volatile uint32_t*
map_bar(uint16_t bus, uint16_t dev, uint16_t func, uint32_t bar, uint64_t* size = nullptr);

inline volatile uint32_t*
map_bar0(uint16_t bus, uint16_t dev, uint16_t func, uint64_t* size = nullptr)
{
  return map_bar(bus, dev, func, 0, size);
}

// Program the xclbin into the card at bus/dev through the dext on its
// management function: checks the shell, programs the xclbin's clock
// targets, streams the bitstream through ICAP. Throws xrt_core::error on
// failure, with the dext's diagnostics in the message.
void
load_xclbin(uint16_t bus, uint16_t dev, const ::axlf* top);

// UUID of the xclbin the dext loaded into the card at bus/dev, as XRT's
// lowercase dashed string. Empty if none is recorded (for example after the
// dext restarted).
std::string
loaded_xclbin_uuid(uint16_t bus, uint16_t dev);

// DMA between host memory and card memory through the dext on the user
// function (bus/dev/1). Throws xrt_core::error on failure.
void
dma_sync(uint16_t bus, uint16_t dev, uint64_t card_addr, void* host, size_t length, bool to_card);

// Write consecutive CU registers in user BAR0 through the dext (CU window only).
void
cu_write(uint16_t bus, uint16_t dev, uint64_t offset, const uint32_t* values, size_t count);

// Start one command on the CU at cu_base: check the CU is idle, write count
// argument words at cu_base + arg_offset, then ap_start at cu_base, all as one
// call and atomically with respect to other processes. False if the CU was
// busy, which means another process started on it first; throws on error.
//
// *ctrl receives the control register as the dext read it inside its lock, so
// the caller needs no MMIO read of its own; an all-ones value means the read
// never reached the user partition.
bool
cu_exec(uint16_t bus, uint16_t dev, uint64_t cu_base, uint64_t arg_offset,
        const uint32_t* values, size_t count, uint32_t* ctrl);

// CU contexts, kept by the dext so they hold across processes. An exclusive
// context excludes every other connection, a shared one excludes only
// exclusive ones. Throws if the context cannot be acquired.
void
cu_ctx_open(uint16_t bus, uint16_t dev, uint32_t cuidx, bool shared, uint64_t base);

void
cu_ctx_close(uint16_t bus, uint16_t dev, uint32_t cuidx);

// Read-write mapping of one CU's register page, for a context this process
// holds exclusively: starting that CU then costs a few stores instead of a
// call into the dext. nullptr if the dext will not grant it, which is not an
// error - the caller falls back to cu_exec. The page starts at the CU's base.
volatile uint32_t*
map_cu_rw(uint16_t bus, uint16_t dev, uint32_t cuidx);

void
unmap_cu_rw(uint16_t bus, uint16_t dev, uint32_t cuidx);

// Raw xclbin section (XRT_SECTION_* slot) of the xclbin loaded into the card
// at bus/dev, as kept by the dext. Empty if none.
std::vector<char>
loaded_section(uint16_t bus, uint16_t dev, uint32_t slot);

// Device memory, owned by the dext and shared by all processes. Buffers are
// tied to this process's connection and freed if the process exits.
uint64_t
mem_alloc(uint16_t bus, uint16_t dev, uint64_t bank_base, uint64_t bank_size, uint64_t size);

void
mem_free(uint16_t bus, uint16_t dev, uint64_t addr);

// Bytes used and number of buffers in a bank, across all processes.
std::pair<uint64_t, uint64_t>
mem_stat(uint16_t bus, uint16_t dev, uint64_t bank_base, uint64_t bank_size);

// PCIe hot reset of the card through the dext on its management function,
// with firewall recovery. The loaded xclbin is gone afterwards. Throws on
// failure with the dext's diagnostics.
void
hot_reset(uint16_t bus, uint16_t dev);

// Keeps every process off the user partition while it is reprogrammed or
// reset through the management function. Without this a DMA or CU write can
// reach the partition while its AXI gate is frozen, which trips the shell's
// firewalls and takes the card down until it is reset.
//
// The hold belongs to this process's connection to the user function, so it
// is dropped if the process dies part way through. Constructing it is a
// no-op when the user function has no dext attached: there is then nothing
// that could be reaching the partition through one.
class partition_hold
{
  uint16_t m_bus, m_dev;
  bool m_held = false;
public:
  partition_hold(uint16_t bus, uint16_t dev);
  ~partition_hold();
  partition_hold(const partition_hold&) = delete;
  partition_hold& operator=(const partition_hold&) = delete;
};

} // xrt_core::macos

#endif
