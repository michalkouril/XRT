// SPDX-License-Identifier: Apache-2.0
#include <os/log.h>
#include <string.h>
#include <time.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOUserClient.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSCollections.h>
#include <PCIDriverKit/PCIDriverKit.h>

#include "XrtUser.h"
#include "xrt_dext_abi.h"
#include "clkwiz_table.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "xrt_user: " fmt, ##__VA_ARGS__)

// Which of the three build configurations this binary is. build.sh defines both
// flags for every build; these defaults make a compile outside build.sh the
// generic driver rather than silently the widest one. Defined here, above every
// use, rather than beside each one: an #if on an undefined macro quietly reads
// as 0, so a fallback that sits below its first use works by accident.
//
//   XRT_MACOS_ENABLE_INNOVA2   XRT_DEXT_REG_WRITE, two FPGA shell registers
//   XRT_MACOS_ENABLE_CX5       claims the ConnectX-5, displacing Apple's driver
#ifndef XRT_MACOS_ENABLE_INNOVA2
#define XRT_MACOS_ENABLE_INNOVA2 0
#endif
#ifndef XRT_MACOS_ENABLE_CX5
#define XRT_MACOS_ENABLE_CX5 0
#endif
#if XRT_MACOS_ENABLE_CX5
#define XRT_BUILD_CONFIG "cx5 (claims the ConnectX-5)"
#elif XRT_MACOS_ENABLE_INNOVA2
#define XRT_BUILD_CONFIG "innova2"
#else
#define XRT_BUILD_CONFIG "generic"
#endif

static constexpr uint32_t kXdmaEngines = 4;       // H2C0, H2C1, C2H0, C2H1
static constexpr uint64_t kDmaDescBytes = 16384;  // one page of XDMA descriptors
// DmaTransfer stage profile: how many transfers between log lines.
static constexpr uint64_t kProfEvery = 2000;
// How long to sleep waiting for a completion interrupt before re-reading the
// status. It used to be 20 ms, which is what made an undelivered interrupt cost
// 22 ms a transfer; the loop polls on wake, so a short deadline only costs a
// few extra reads and bounds the damage.
static constexpr uint64_t kIrqSleepNs = 1000000ull;      // 1 ms
static constexpr uint32_t kIrqGiveUpAfter = 8;
static inline uint64_t prof_now() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// PCI config space offsets
static constexpr uint64_t kCfgVendor      = 0x00;
static constexpr uint64_t kCfgCommand     = 0x04;
static constexpr uint64_t kCfgRevision    = 0x08;
static constexpr uint64_t kCfgSubsystem   = 0x2c;
static constexpr uint32_t kCapPCIExpress  = 0x10;
static constexpr uint16_t kCmdMemorySpace = 0x0002;

struct xrt_mem_rec
{
  uint64_t addr;
  uint64_t size;
  void*    owner;
};

// One open CU context. count allows a client to open the same CU more than
// once, as XRT does when several kernels name the same CU.
struct xrt_cu_ctx_rec
{
  void*    owner;
  uint32_t cu;
  uint32_t shared;
  uint32_t count;
  uint64_t base;      // the CU's base offset in BAR0, as the shim reported it
};

struct XrtUser_IVars
{
  IOPCIDevice* pci = nullptr;
  // Where this platform keeps the blocks we touch. Filled in Start(); ivars are
  // IONewZero'd and member initializers do not run, so it is set explicitly.
  xrt_dext_platform plat = {};
  // The CU register window in force. Defaults to the XDMA 5.x bounds and moves
  // when a loaded xclbin says its CUs are elsewhere.
  uint32_t cu_win_base = 0;
  uint32_t cu_win_size = 0;
  // The card addresses DMA and the allocator may touch. Defaults to the XDMA
  // 5.x DDR span and moves when a loaded xclbin's memory is elsewhere.
  uint64_t card_min = 0;
  uint64_t card_max = 0;
  // Frequency counters the loaded xclbin brought with it, so a clock can be
  // measured rather than reported as whatever was asked for.
  uint32_t ulp_freq_cnt[XRT_LOAD_CLOCKS] = {};
  IOLock*      dma_lock = nullptr;     // one transfer at a time per function
  // Bounce buffer for sub-descriptor transfers, made once. Item 52: allocating
  // and wiring one per transfer cost ~20 us, the same mistake item 10 fixed for
  // the descriptor ring.
  IOLock*      bounce_lock = nullptr;
  IOLock*      flash_lock = nullptr;   // the SPI controller is stateful
  // Which slave the controller was last prepared for. xbflash resets the IP in
  // prepareXSpi() once per chip rather than once overall, and in dual-quad mode
  // the core can latch per-device state, so the reset is repeated on a change.
  uint32_t     flash_slave = 0xffffffffu;
  IOService*   flash_owner = nullptr;  // the one XrtFlashClient allowed at a time
  // TX-stream shadow (see xrt_dext_abi.h). Only the command header is kept -
  // enough to decode an opcode and its address - not the payload, so a 256-byte
  // page program costs the same 8 bytes as a status poll.
  uint8_t      spi_hdr[8] = {};
  uint8_t      spi_hdr_saved[8] = {};  // spi_hdr as it was at the last launch
  uint32_t     spi_hdr_n = 0;          // bytes captured in spi_hdr
  uint32_t     spi_tx_n = 0;           // bytes pushed to DTR this transaction
  uint32_t     spi_drr_n = 0;          // DRR reads since the last launch
  bool         spi_substituted = false; // this command's opcode was rewritten
  uint32_t     spi_ssr = 0xffffffffu;  // slave select as last written
  uint32_t     spi_guard = XRT_SPI_GUARD_ENFORCE;  // assigned in Start; see there
  // The extended address register as the shadow last saw it set. This is SHADOW
  // STATE, not a statistic: it supplies address bits 27:24 for every 3-byte-address
  // command that follows. It used to live in the audit tally, where resetting the
  // tally silently dropped it and every subsequent address decoded 16 MiB too low -
  // which read as "everything is inside the protected range".
  uint32_t     spi_ear = 0;
  uint32_t     spi_ear_valid = 0;
  struct xrt_dext_spi_audit spi_audit = {};
  IOBufferMemoryDescriptor* bounce = nullptr;
  uint64_t     bounce_va = 0;
  uint64_t     bounce_len = 0;
  bool         opened = false;
  uint8_t      xclbin_uuid[16] = {};   // set after a successful load
  OSData*      sections[XRT_SECTION_SLOTS] = {};
  IOLock*      mem_lock = nullptr;
  bool         asleep = false;          // between SetPowerState(Off) and (On)
  // Set while the management function reprograms or resets the user
  // partition. Owned by the user client that took it, so a process dying
  // mid-download cannot leave the card unusable.
  bool         partition_held = false;
  void*        partition_owner = nullptr;
  uint16_t     command_saved = 0;
  xrt_mem_rec* mem = nullptr;           // sorted by addr
  uint32_t     mem_count = 0;
  // Stage 1 interrupts (user function only)
  IODispatchQueue*           irq_queue = nullptr;
  IOInterruptDispatchSource* irq_src[kXdmaEngines] = {};
  OSAction*                  irq_action[kXdmaEngines] = {};
  bool                       irq_fired = false; // set by DmaInterrupt; read and cleared on irq_queue
  // DmaTransfer stage profile (item 50). Nanoseconds accumulated per stage and
  // the number of transfers, logged and reset every kProfEvery transfers. Cheap
  // enough to leave in: eight clock reads on a path whose fixed cost is ~130 us.
  uint64_t                   prof_ns[7] = { 0, 0, 0, 0, 0, 0, 0 };
  uint64_t                   prof_n = 0;
  // Whether the command register is known to have memory space and bus master
  // set. A PCI config read costs 40-78 us on this machine (item 50), so it is
  // read once rather than on every transfer, and invalidated wherever the card
  // can lose the setting: a hot reset and a sleep.
  bool                       cmd_enabled = false;
  // Which of the two H2C and two C2H channels the shell actually implements.
  // The engine identity register is fixed, so it is read once; only the busy
  // bit is worth re-reading per transfer. Invalidated wherever cmd_enabled is.
  uint32_t                   h2c_mask = 0;
  uint32_t                   c2h_mask = 0;
  bool                       chan_known = false;
  // Whether the XDMA config BAR has been confirmed by probing rather than
  // assumed. The compile-time default is BAR1, which some shells do not have at
  // all - an Innova2's user function carries BARs 0, 2 and 4.
  bool                       xdma_bar_probed = false;
  // Interrupts that are configured but never delivered (item 53: the U50 leaves
  // MSI-X disabled in config space). Every transfer would otherwise wait out the
  // full sleep deadline, so after this many silent transfers the dext stops
  // asking and polls, which is what it would have done anyway.
  uint32_t                   irq_misses = 0;
  bool                       irq_dead = false;
  uint32_t                   irq_vectors = 0; // 0 = poll
  bool                       irq_enabled = false; // XRT_DEXT_DMA_MODE
  uint64_t                   irq_count[kXdmaEngines] = {};
  uint64_t                   dma_irq_batches = 0;
  uint64_t                   dma_poll_batches = 0;
  // The user partition's CU window, mapped into the dext (user function
  // only). Writing a CU register is then a store, not a MemoryWrite32 call
  // into the kernel. Null if the mapping failed; CuWrite falls back.
  IOMemoryMap*               cu_map = nullptr;
  volatile uint32_t*         cu_regs = nullptr;
  // CU contexts and the serialisation of CU starts. cu_lock is taken inside
  // dma_lock where both are needed, never the other way round.
  // The XDMA descriptor ring and the DMA commands, made once and reused.
  // Building them per transfer cost a 16 KiB wired allocation, two
  // IODMACommand creations and a DART mapping before any data moved.
  IOBufferMemoryDescriptor* dma_descs = nullptr;
  IODMACommand*             dma_desc_cmd = nullptr;
  IODMACommand*             dma_host_cmd = nullptr;
  uint64_t                  dma_desc_va = 0;     // descriptor ring, dext side
  uint64_t                  dma_desc_iova = 0;   // and as the card sees it
  IOLock*          cu_lock = nullptr;
  xrt_cu_ctx_rec*  cu_ctx = nullptr;
  uint32_t         cu_ctx_count = 0;
};

bool
XrtUser::init()
{
  if (!super::init())
    return false;
  ivars = IONewZero(XrtUser_IVars, 1);
  if (!ivars)
    return false;
  // IONewZero zero-fills; member initializers in XrtUser_IVars do not run.
  // DMA completion defaults to polling: on Apple silicon an idle CPU lets
  // the SoC fabric clock down and XDMA throughput drops by up to 40%
  // (measured over Thunderbolt 3). XRT_DEXT_DMA_MODE switches to MSI-X.
  ivars->irq_enabled = false;
  ivars->dma_lock = IOLockAlloc();
  ivars->bounce_lock = IOLockAlloc();
  ivars->flash_lock = IOLockAlloc();
  // IONewZero zeroes the ivars and member initialisers do NOT run, so a default
  // other than 0 has to be assigned here. 0 is XRT_SPI_GUARD_OFF, which is the one
  // mode that leaves the factory image writable - exactly the wrong default.
  ivars->spi_guard = XRT_SPI_GUARD_ENFORCE;
  ivars->mem_lock = IOLockAlloc();
  ivars->mem = IONewZero(xrt_mem_rec, XRT_MEM_MAX_ALLOCS);
  ivars->cu_lock = IOLockAlloc();
  ivars->cu_ctx = IONewZero(xrt_cu_ctx_rec, XRT_CU_CTX_MAX);
  return ivars->dma_lock && ivars->mem_lock && ivars->mem && ivars->cu_lock && ivars->cu_ctx;
}

static void default_platform(xrt_dext_platform* p);

kern_return_t
IMPL(XrtUser, Start)
{
  kern_return_t ret = Start(provider, SUPERDISPATCH);
  if (ret != kIOReturnSuccess)
    return ret;

  LOG("build config: " XRT_BUILD_CONFIG);

  ivars->pci = OSDynamicCast(IOPCIDevice, provider);
  if (!ivars->pci) {
    LOG("provider is not an IOPCIDevice");
    return kIOReturnNoDevice;
  }
  ivars->pci->retain();

  // The XDMA 5.x register map, until a shim installs this card's own.
  default_platform(&ivars->plat);
  ivars->cu_win_base = XRT_CU_WINDOW_BASE;
  ivars->cu_win_size = XRT_CU_WINDOW_END - XRT_CU_WINDOW_BASE;
  ivars->card_min = XRT_DMA_CARD_MIN;
  ivars->card_max = XRT_DMA_CARD_MAX;

  ret = ivars->pci->Open(this, 0);
  if (ret != kIOReturnSuccess) {
    LOG("IOPCIDevice::Open failed 0x%x", ret);
    return ret;
  }
  ivars->opened = true;

  // Open() leaves decoding off. Enable Memory Space only; Bus Master
  // (DMA) stays off until the DMA phase.
  uint16_t cmd = 0;
  ivars->pci->ConfigurationRead16(kCfgCommand, &cmd);
  ivars->pci->ConfigurationWrite16(kCfgCommand, cmd | kCmdMemorySpace);

  xrt_dext_device_info info = {};
  CopyDeviceInfo(&info, sizeof(info));
  if (info.func == 1) {
    SetupInterrupts();
    MapCuWindow();
  }
  LOG("started on %02x:%02x.%x vendor 0x%04x device 0x%04x subsys 0x%04x, link status 0x%04x",
      info.bus, info.dev, info.func, info.vendor, info.device, info.subsystem_id, info.link_status);

  // Publish so user space can find us with IOServiceMatching("XrtUser").
  return RegisterService();
}

kern_return_t
IMPL(XrtUser, Stop)
{
  LOG("stopping");
  TeardownInterrupts();
  ivars->cu_regs = nullptr;
  OSSafeReleaseNULL(ivars->cu_map);
  // The descriptor ring was left prepared for reuse; finish it before the
  // device goes.
  if (ivars->dma_desc_cmd)
    ivars->dma_desc_cmd->CompleteDMA(kIODMACommandCompleteDMANoOptions);
  OSSafeReleaseNULL(ivars->dma_desc_cmd);
  OSSafeReleaseNULL(ivars->dma_host_cmd);
  OSSafeReleaseNULL(ivars->dma_descs);
  ivars->dma_desc_va = 0;
  ivars->dma_desc_iova = 0;
  if (ivars->opened && ivars->pci) {
    ivars->pci->Close(this, 0);
    ivars->opened = false;
  }
  return Stop(provider, SUPERDISPATCH);
}

void
XrtUser::free()
{
  if (ivars) {
    ivars->cu_regs = nullptr;
    OSSafeReleaseNULL(ivars->cu_map);
    OSSafeReleaseNULL(ivars->dma_desc_cmd);
    OSSafeReleaseNULL(ivars->dma_host_cmd);
    OSSafeReleaseNULL(ivars->dma_descs);
    OSSafeReleaseNULL(ivars->pci);
    for (auto& sec : ivars->sections)
      OSSafeReleaseNULL(sec);
    OSSafeReleaseNULL(ivars->bounce);
    if (ivars->dma_lock)
      IOLockFree(ivars->dma_lock);
    if (ivars->bounce_lock)
      IOLockFree(ivars->bounce_lock);
    if (ivars->flash_lock)
      IOLockFree(ivars->flash_lock);
    if (ivars->mem_lock)
      IOLockFree(ivars->mem_lock);
    if (ivars->cu_lock)
      IOLockFree(ivars->cu_lock);
    IOSafeDeleteNULL(ivars->mem, xrt_mem_rec, XRT_MEM_MAX_ALLOCS);
    IOSafeDeleteNULL(ivars->cu_ctx, xrt_cu_ctx_rec, XRT_CU_CTX_MAX);
  }
  IOSafeDeleteNULL(ivars, XrtUser_IVars, 1);
  super::free();
}

kern_return_t
IMPL(XrtUser, NewUserClient)
{
  IOService* client = nullptr;
  // The type argument of IOServiceOpen() picks the class. "UserClientProperties"
  // names XrtUserClient, which carries everything except flash and is open to any
  // process. "FlashUserClientProperties" names XrtFlashClient, which carries only
  // the SPI selectors and whose personality sets IOUserClientEntitlements - so the
  // kernel refuses the open unless the caller is signed with
  // XRT_DEXT_FLASH_ENTITLEMENT, before this method is ever reached. An unknown type
  // is refused rather than quietly given the default, which would make a typo in a
  // caller look like a working flash connection.
  const char* props = nullptr;
  switch (type) {
    case XRT_DEXT_CLIENT_DEFAULT: props = "UserClientProperties"; break;
    case XRT_DEXT_CLIENT_FLASH:   props = "FlashUserClientProperties"; break;
    default:
      LOG("NewUserClient: unknown client type %u", type);
      return kIOReturnBadArgument;
  }
  kern_return_t ret = Create(this, props, &client);
  if (ret != kIOReturnSuccess) {
    LOG("Create user client (%s) failed 0x%x", props, ret);
    return ret;
  }
  *userClient = OSDynamicCast(IOUserClient, client);
  if (!*userClient) {
    client->release();
    return kIOReturnError;
  }
  return kIOReturnSuccess;
}

// One dword of configuration space. Read-only and bounded to the extended
// space, which is all the capability walk needs.
kern_return_t
XrtUser::ConfigRead(uint64_t offset, uint32_t* value)
{
  if (!value || !ivars->pci)
    return kIOReturnBadArgument;
  if ((offset & 3) || offset + 4 > XRT_CFG_SPACE_BYTES)
    return kIOReturnBadArgument;
  uint32_t v = 0;
  ivars->pci->ConfigurationRead32(offset, &v);
  *value = v;
  return kIOReturnSuccess;
}

kern_return_t
XrtUser::CopyDeviceInfo(void* out, uint64_t size)
{
  if (!out || size < sizeof(xrt_dext_device_info) || !ivars->pci)
    return kIOReturnBadArgument;

  auto info = static_cast<xrt_dext_device_info*>(out);
  memset(info, 0, sizeof(*info));
  info->abi_version = XRT_DEXT_ABI_VERSION;

  auto pci = ivars->pci;
  uint32_t v = 0;
  pci->ConfigurationRead32(kCfgVendor, &v);
  info->vendor = v & 0xffff;
  info->device = v >> 16;
  pci->ConfigurationRead32(kCfgCommand, &v);
  info->command = v & 0xffff;
  info->status = v >> 16;
  pci->ConfigurationRead32(kCfgRevision, &v);
  info->revision = v & 0xff;
  pci->ConfigurationRead32(kCfgSubsystem, &v);
  info->subsystem_vendor = v & 0xffff;
  info->subsystem_id = v >> 16;

  pci->GetBusDeviceFunction(&info->bus, &info->dev, &info->func);

  uint64_t cap = 0;
  info->link_status = 0xffff;
  if (pci->FindPCICapability(kCapPCIExpress, 0, &cap) == kIOReturnSuccess && cap) {
    uint32_t caps = 0;
    uint16_t lsts = 0;
    pci->ConfigurationRead32(cap + 0x0c, &caps);   // Link Capabilities
    pci->ConfigurationRead16(cap + 0x12, &lsts);   // Link Status
    info->link_caps = caps;
    info->link_status = lsts;
  }
  info->irq_vectors = ivars->irq_enabled ? ivars->irq_vectors : 0;
  info->dma_irq_batches = ivars->dma_irq_batches;
  info->dma_poll_batches = ivars->dma_poll_batches;

  for (uint8_t i = 0; i < 6; ++i) {
    uint8_t idx = 0, type = 0;
    uint64_t sz = 0;
    if (pci->GetBARInfo(i, &idx, &sz, &type) == kIOReturnSuccess) {
      info->bars[i].present = 1;
      info->bars[i].memory_index = idx;
      info->bars[i].type = type;
      info->bars[i].size = sz;
    }
  }
  return kIOReturnSuccess;
}

// Defined further down, needed by LoadBitstream: locate the AXI Quad SPI and
// record it as XRT_BLK_FLASH. Idempotent - it returns at once once the block is
// known.
static void probe_flash_block(XrtUser_IVars* ivars, IOPCIDevice* pci);

// ---------------------------------------------------------------------------
// The platform's register map. Defaults are the XDMA 5.x offsets, so a card
// that never has a map installed behaves exactly as it did before this table
// existed. See xrt_dext_abi.h.
// ---------------------------------------------------------------------------
static void
default_platform(xrt_dext_platform* p)
{
  memset(p, 0, sizeof(*p));
  p->abi = XRT_DEXT_ABI_VERSION;
  p->count = XRT_BLK_MAX;
  p->flags = XRT_PLATFORM_DEFAULT;
  auto set = [&](uint32_t id, uint64_t off, uint64_t sz, uint8_t func) {
    p->blk[id].offset = off;
    p->blk[id].size = sz;
    p->blk[id].bar = 0;
    p->blk[id].func = func;
  };
  set(XRT_BLK_FEATURE_ROM, XRT_DEXT_ROM_OFFSET, XRT_DEXT_ROM_SIZE, 0);
  set(XRT_BLK_ICAP,        XRT_MGMT_ICAP,       0x1000, 0);
  set(XRT_BLK_GATE_ULP,    XRT_MGMT_GATE_ULP,   0x1000, 0);
  set(XRT_BLK_MEMCALIB,    XRT_MGMT_MEMCALIB,   0x1000, 0);
  set(XRT_BLK_CLKWIZ_K1,   XRT_MGMT_CLKWIZ_K1,  0x1000, 0);
  set(XRT_BLK_CLKWIZ_K2,   XRT_MGMT_CLKWIZ_K2,  0x1000, 0);
  set(XRT_BLK_SYSMON,      XRT_MGMT_SYSMON,     0x1000, 0);
  // XDMA 5.x has four firewalls (XRT devices.h XOCL_RES_AF_DSA52) and no UUID
  // ROM or CMC; those stay absent.
  set(XRT_BLK_FIREWALL_0,  0xD0000, 0x1000, 0);
  set(XRT_BLK_FIREWALL_1,  0xE0000, 0x1000, 0);
  set(XRT_BLK_FIREWALL_2,  0xE1000, 0x1000, 0);
  set(XRT_BLK_FIREWALL_3,  0xF0000, 0x1000, 0);
  // The XDMA engine is on the user function, BAR1 on these shells.
  p->blk[XRT_BLK_XDMA] = { 0, 0x40000, XRT_XDMA_BAR, 1, {} };
  // The scheduler MicroBlaze. Its code RAM and reset are on the management
  // function; the queue and status registers answer on both, so they are
  // recorded against the management one and used from there.
  p->blk[XRT_BLK_ERT_FW_MEM] = { XRT_ERT_FW_MEM_DEFAULT, 0x20000, 0, 0, {} };
  p->blk[XRT_BLK_ERT_RESET]  = { XRT_ERT_RESET_DEFAULT,  0x1000,  0, 0, {} };
  p->blk[XRT_BLK_ERT_CQ]     = { XRT_ERT_CQ_DEFAULT,     0x10000, 0, 0, {} };
  p->blk[XRT_BLK_ERT_CSR]    = { XRT_ERT_CSR_DEFAULT,    0x10000, 0, 0, {} };
  // The board controller on these shells, sharing the scheduler's reset GPIO.
  p->blk[XRT_BLK_CMC_REGMAP] = { XRT_CMC_REGMAP_DEFAULT, 0x2000,  0, 0, {} };
  p->blk[XRT_BLK_CMC_FW_MEM] = { XRT_CMC_FW_MEM_DEFAULT, 0x20000, 0, 0, {} };
  p->blk[XRT_BLK_CMC_RESET]  = { XRT_CMC_RESET_DEFAULT,  0x1000,  0, 0, {} };
}

static inline const xrt_dext_block&
blk(const XrtUser_IVars* ivars, uint32_t id)
{
  return ivars->plat.blk[id];
}

static inline bool
blk_present(const XrtUser_IVars* ivars, uint32_t id)
{
  return ivars->plat.blk[id].size != 0;
}

static inline uint32_t
blk_off(const XrtUser_IVars* ivars, uint32_t id)
{
  return static_cast<uint32_t>(ivars->plat.blk[id].offset);
}

// A 32-bit read that never reached a register. Apple silicon returns filler
// rather than faulting, and the patterns are not 0 or all-ones alone:
// 0xdeadfa11 for an unclaimed read and 0xdec0de1c have both been seen from this
// card. Treating filler as data is how a plausible-looking value gets believed.
static inline uint8_t
xdma_bar(const XrtUser_IVars* ivars)
{
  const auto& b = ivars->plat.blk[XRT_BLK_XDMA];
  return b.size ? b.bar : XRT_XDMA_BAR;
}

static inline uint32_t
cu_win_base(const XrtUser_IVars* ivars) { return ivars->cu_win_base; }

static inline uint32_t
cu_win_end(const XrtUser_IVars* ivars) { return ivars->cu_win_base + ivars->cu_win_size; }

static inline bool
is_filler(uint32_t v)
{
  return v == 0xffffffff || v == 0xdeadfa11 || v == 0xdec0de1c;
}

kern_return_t
XrtUser::ReadFeatureRom(void* out, uint64_t size)
{
  if (!out || size < XRT_DEXT_ROM_SIZE || !ivars->pci)
    return kIOReturnBadArgument;
  if (ivars->asleep)
    return kIOReturnOffline;

  // A dynamic-IP platform has no feature ROM at all; say so rather than
  // reading whatever happens to sit at the XDMA 5.x offset.
  if (!blk_present(ivars, XRT_BLK_FEATURE_ROM))
    return kIOReturnUnsupported;
  const uint32_t rom_off = blk_off(ivars, XRT_BLK_FEATURE_ROM);

  uint8_t idx = 0, type = 0;
  uint64_t bar_size = 0;
  if (ivars->pci->GetBARInfo(blk(ivars, XRT_BLK_FEATURE_ROM).bar, &idx, &bar_size, &type) != kIOReturnSuccess)
    return kIOReturnNoResources;
  if (bar_size < rom_off + XRT_DEXT_ROM_SIZE)
    return kIOReturnNoSpace;

  // Check the magic before touching the rest of the ROM window.
  uint32_t magic = 0;
  ivars->pci->MemoryRead32(idx, rom_off, &magic);
  if (magic != XRT_DEXT_ROM_MAGIC) {
    LOG("feature ROM magic mismatch: 0x%08x", magic);
    return kIOReturnNotFound;
  }

  auto words = static_cast<uint32_t*>(out);
  for (uint32_t i = 0; i < XRT_DEXT_ROM_SIZE / 4; ++i)
    ivars->pci->MemoryRead32(idx, rom_off + i * 4, &words[i]);
  return kIOReturnSuccess;
}

// Install this card's register map. Refused once the map has been replaced or
// an xclbin is loaded: the map decides where ICAP writes land, so it may only
// be set while the card is still in its default, unprogrammed state. Every
// entry is checked against the BAR it names, so a bad table cannot turn into a
// write outside the BAR.
// Move the CU register window to where a loaded xclbin's CUs actually are.
// XDMA 5.x shells put their user partition at a fixed place and the defaults
// cover it; another shell does not, and a U50's verify kernel sits below the
// default base.
//
// Refused while any CU context is open, for the same reason the read-write CU
// page is a fast path and not an isolation boundary: DriverKit cannot revoke a
// mapping already handed out, so the window must not move under one.
kern_return_t
XrtUser::SetCuWindow(uint64_t offset, uint64_t length)
{
  auto pci = ivars->pci;
  if (!pci)
    return kIOReturnNoDevice;
  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 1)
    return kIOReturnNotPermitted;

  // The window is mapped and indexed as pages, and CuExec hands out one page of
  // it at a time, so both ends have to be page aligned.
  if (!length || (offset % XRT_CU_RW_PAGE) || (length % XRT_CU_RW_PAGE)
      || length > XRT_CU_WINDOW_MAX || offset + length < offset)
    return kIOReturnBadArgument;
  uint8_t idx = 0, type = 0;
  uint64_t bar_size = 0;
  if (pci->GetBARInfo(0, &idx, &bar_size, &type) != kIOReturnSuccess)
    return kIOReturnNoResources;
  if (offset + length > bar_size) {
    LOG("SetCuWindow refused: 0x%llx+0x%llx is outside BAR0 (0x%llx)", offset, length, bar_size);
    return kIOReturnBadArgument;
  }

  IOLockLock(ivars->cu_lock);
  if (ivars->cu_ctx_count) {
    IOLockUnlock(ivars->cu_lock);
    LOG("SetCuWindow refused: %u CU context(s) are open", ivars->cu_ctx_count);
    return kIOReturnBusy;
  }
  if (ivars->cu_win_base == offset && ivars->cu_win_size == length) {
    IOLockUnlock(ivars->cu_lock);
    return kIOReturnSuccess;              // already there; nothing to rebuild
  }
  ivars->cu_win_base = static_cast<uint32_t>(offset);
  ivars->cu_win_size = static_cast<uint32_t>(length);
  // The dext's own mapping covers the old window; drop it and remake it.
  ivars->cu_regs = nullptr;
  OSSafeReleaseNULL(ivars->cu_map);
  IOLockUnlock(ivars->cu_lock);

  MapCuWindow();
  LOG("CU window moved to 0x%llx..0x%llx", offset, offset + length);
  return kIOReturnSuccess;
}

// One 32-bit write to the management function's BAR0, for an Innova-2 shell's
// own control registers. Everything about the target is checked here, not in
// user space: the function, the shell (by the feature ROM's VBNV), the
// alignment and the window. Anything else is refused before the BAR is touched.
//
// Innova-2 specific, so a generic XRT build leaves it out: build.sh defines
// XRT_MACOS_ENABLE_INNOVA2=0 by default and the method below then refuses
// without ever looking at a BAR. The selector number stays reserved in
// xrt_dext_abi.h either way - ABI numbering must not depend on a build flag.
#if XRT_MACOS_ENABLE_INNOVA2
static bool
reg_write_in_window(uint64_t offset)
{
  if (offset & 3)
    return false;
  const uint64_t end = offset + 4;
  if (offset >= XRT_DEXT_REG_WRITE_WIN0_BASE &&
      end <= XRT_DEXT_REG_WRITE_WIN0_BASE + XRT_DEXT_REG_WRITE_WIN0_SIZE)
    return true;
  if (offset >= XRT_DEXT_REG_WRITE_WIN1_BASE &&
      end <= XRT_DEXT_REG_WRITE_WIN1_BASE + XRT_DEXT_REG_WRITE_WIN1_SIZE)
    return true;
  return false;
}
#endif

kern_return_t
XrtUser::RegWrite(uint64_t offset, uint32_t value, uint32_t* readback)
{
#if !XRT_MACOS_ENABLE_INNOVA2
  (void)offset; (void)value;
  if (readback)
    *readback = 0;
  return kIOReturnUnsupported;      // not an Innova-2 build
#else
  if (!readback)
    return kIOReturnBadArgument;
  *readback = 0;
  auto pci = ivars->pci;
  if (!pci)
    return kIOReturnNoDevice;
  if (ivars->asleep)
    return kIOReturnOffline;
  if (!reg_write_in_window(offset))
    return kIOReturnBadArgument;

  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 0)
    return kIOReturnNotPermitted;      // management function only

  // FeatureRomHeader (xclfeatures.h): VBNVName is bytes 88..151.
  uint8_t rom[XRT_DEXT_ROM_SIZE] = {};
  if (ReadFeatureRom(rom, sizeof(rom)) != kIOReturnSuccess)
    return kIOReturnNotPermitted;
  const char prefix[] = XRT_DEXT_REG_WRITE_VBNV_PREFIX;
  if (memcmp(rom + 88, prefix, sizeof(prefix) - 1) != 0) {
    LOG("reg write refused: shell is not an Innova-2 shell");
    return kIOReturnNotPermitted;
  }

  uint8_t bar = 0, type = 0;
  uint64_t size = 0;
  if (pci->GetBARInfo(0, &bar, &size, &type) != kIOReturnSuccess || offset + 4 > size)
    return kIOReturnNoResources;

  pci->MemoryWrite32(bar, static_cast<uint32_t>(offset), value);
  uint32_t v = 0;
  pci->MemoryRead32(bar, static_cast<uint32_t>(offset), &v);
  *readback = v;
  LOG("reg write BAR0+0x%llx = 0x%08x (read back 0x%08x)", offset, value, v);
  return kIOReturnSuccess;
#endif
}

// Measure a kernel clock with the counter the loaded xclbin brought with it.
// This is the frequency the hardware is actually running at, as opposed to the
// one the download asked the wizard for.
kern_return_t
XrtUser::ClockMeasure(uint32_t index, uint32_t* mhz)
{
  if (!mhz || index >= XRT_LOAD_CLOCKS)
    return kIOReturnBadArgument;
  *mhz = 0;
  auto pci = ivars->pci;
  if (!pci)
    return kIOReturnNoDevice;
  if (ivars->asleep)
    return kIOReturnOffline;
  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 0)
    return kIOReturnNotPermitted;
  const uint32_t base = ivars->ulp_freq_cnt[index];
  if (!base)
    return kIOReturnUnsupported;      // no counter for this clock
  uint8_t bar = 0, type = 0;
  uint64_t size = 0;
  if (pci->GetBARInfo(0, &bar, &size, &type) != kIOReturnSuccess || base + 0x1000 > size)
    return kIOReturnNoResources;
  auto rd = [&](uint32_t off) { uint32_t v = 0; pci->MemoryRead32(bar, off, &v); return v; };

  // The only write is the start bit, into the counter's own control register.
  pci->MemoryWrite32(bar, base, XRT_FREQ_CNT_MEASURE_START);
  uint32_t status = 0;
  for (int i = 0; i < 10; ++i) {
    status = rd(base);
    if (is_filler(status))
      return kIOReturnNotResponding;
    if ((status & XRT_FREQ_CNT_STATUS_MASK) == XRT_FREQ_CNT_MEASURE_DONE)
      break;
    IOSleep(1);
  }
  if ((status & XRT_FREQ_CNT_STATUS_MASK) != XRT_FREQ_CNT_MEASURE_DONE)
    return kIOReturnTimeout;
  uint32_t raw = rd(base + ((status & XRT_FREQ_CNT_V5_ENABLED) ? XRT_FREQ_CNT_V5_VALUE
                                                              : XRT_FREQ_CNT_VALUE));
  // These counters report kHz, whatever xocl's variable naming suggests. The
  // evidence: on a U50 the three of them read 300000, 500000 and 450000 against
  // requested frequencies of 300, 500 and 450 MHz.
  *mhz = raw / 1000;
  return kIOReturnSuccess;
}

// Bring the scheduler MicroBlaze up and configure it.
//
// The load is the same shape as the board controller's: hold the reset, copy the
// image into its code RAM, release. What makes this worth doing in one call is
// the proof at the end - a configure command written into the queue and read
// back completed. Copying the image only shows the RAM accepted writes; a
// completed command shows the MicroBlaze is executing them.
// Ask the board controller for its board info. Everything past the plain sensor
// registers - the board's name, its maximum power level - comes over its
// mailbox, so this runs the exchange and hands the raw key/length/value blob
// back for the caller to pick apart.
kern_return_t
XrtUser::CmcBoardInfo(void* out, uint64_t size)
{
  auto res = static_cast<xrt_dext_cmc_bdinfo*>(out);
  if (!out || size < sizeof(*res))
    return kIOReturnBadArgument;
  memset(res, 0, sizeof(*res));
  auto pci = ivars->pci;
  if (!pci)
    return res->status = kIOReturnNoDevice;
  if (ivars->asleep)
    return res->status = kIOReturnOffline;
  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 0)
    return res->status = kIOReturnNotPermitted;
  if (!blk_present(ivars, XRT_BLK_CMC_REGMAP))
    return res->status = kIOReturnUnsupported;

  uint8_t bar = 0, type = 0;
  uint64_t barsz = 0;
  if (pci->GetBARInfo(0, &bar, &barsz, &type) != kIOReturnSuccess)
    return res->status = kIOReturnNoResources;
  const uint32_t regmap = blk_off(ivars, XRT_BLK_CMC_REGMAP);
  const uint64_t regroom = blk(ivars, XRT_BLK_CMC_REGMAP).size;
  auto rd = [&](uint32_t off) { uint32_t v = 0; pci->MemoryRead32(bar, regmap + off, &v); return v; };
  auto wr = [&](uint32_t off, uint32_t v) { pci->MemoryWrite32(bar, regmap + off, v); };

  if (rd(XRT_CMC_MAGIC_REG) != XRT_CMC_MAGIC)
    return res->status = kIOReturnNotReady;         // the controller is not running

  // Where its mailbox buffer is. xocl rejects 0 and filler here too.
  uint32_t off = rd(XRT_CMC_MSG_OFFSET_REG);
  res->msg_offset = off;
  if (!off || is_filler(off) || off + 4 > regroom) {
    LOG("CmcBoardInfo: mailbox offset 0x%x is not usable", off);
    return res->status = kIOReturnNotReady;
  }

  // One word: no payload, opcode BOARD_INFO in the top byte.
  wr(off, XRT_CMC_OP_BOARD_INFO << 24);
  uint32_t ctrl = rd(XRT_CMC_CONTROL_REG);
  wr(XRT_CMC_CONTROL_REG, ctrl | XRT_CMC_PKT_OWNER);

  // The controller clears the owner bit when it has answered.
  bool answered = false;
  for (int i = 0; i < 200; ++i) {                   // up to ~2 s
    if (!(rd(XRT_CMC_CONTROL_REG) & XRT_CMC_PKT_OWNER)) {
      answered = true;
      break;
    }
    IOSleep(10);
  }
  res->error = rd(XRT_CMC_MSG_ERROR_REG);
  if (!answered) {
    LOG("CmcBoardInfo: the controller did not take the packet");
    return res->status = kIOReturnTimeout;
  }

  uint32_t hdr = rd(off);
  uint32_t payload = hdr & 0xfff;                   // payload_sz is the low 12 bits
  if (payload > XRT_CMC_BDINFO_MAX)
    payload = XRT_CMC_BDINFO_MAX;
  if (off + 4 + payload > regroom)
    return res->status = kIOReturnNoSpace;
  for (uint32_t i = 0; i < payload; i += 4) {
    uint32_t w = rd(off + 4 + i);
    for (uint32_t b = 0; b < 4 && i + b < payload; ++b)
      res->data[i + b] = static_cast<uint8_t>((w >> (8 * b)) & 0xff);
  }
  res->bytes = payload;
  LOG("CmcBoardInfo: %u bytes from mailbox at 0x%x, error 0x%x", payload, off, res->error);
  return res->status = kIOReturnSuccess;
}

kern_return_t
XrtUser::ErtStart(const uint32_t* words, uint64_t count, void* out)
{
  auto res = static_cast<xrt_dext_ert_result*>(out);
  memset(res, 0, sizeof(*res));
  auto pci = ivars->pci;
  if (!pci)
    return res->status = kIOReturnNoDevice;
  if (ivars->asleep)
    return res->status = kIOReturnOffline;
  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 0)
    return res->status = kIOReturnNotPermitted;

  res->step = XRT_ERT_STEP_CHECK;
  static const uint32_t needed[] = { XRT_BLK_ERT_FW_MEM, XRT_BLK_ERT_RESET,
                                     XRT_BLK_ERT_CQ, XRT_BLK_ERT_CSR };
  for (uint32_t k = 0; k < sizeof(needed) / sizeof(needed[0]); ++k)
    if (!blk_present(ivars, needed[k])) {
      LOG("ErtStart refused: block %u is not on this platform", needed[k]);
      return res->status = kIOReturnUnsupported;
    }
  uint8_t bar = 0, type = 0;
  uint64_t size = 0;
  if (pci->GetBARInfo(0, &bar, &size, &type) != kIOReturnSuccess)
    return res->status = kIOReturnNoResources;
  const uint32_t fwmem = blk_off(ivars, XRT_BLK_ERT_FW_MEM);
  const uint32_t reset = blk_off(ivars, XRT_BLK_ERT_RESET);
  const uint32_t cq    = blk_off(ivars, XRT_BLK_ERT_CQ);
  const uint32_t csr   = blk_off(ivars, XRT_BLK_ERT_CSR);
  const uint64_t fwroom = blk(ivars, XRT_BLK_ERT_FW_MEM).size;
  for (uint32_t k = 0; k < sizeof(needed) / sizeof(needed[0]); ++k)
    if (blk(ivars, needed[k]).offset + blk(ivars, needed[k]).size > size)
      return res->status = kIOReturnNoResources;
  auto rd = [&](uint32_t off) { uint32_t v = 0; pci->MemoryRead32(bar, off, &v); return v; };
  auto wr = [&](uint32_t off, uint32_t v) { pci->MemoryWrite32(bar, off, v); };

  res->image_word0 = rd(fwmem);
  res->was_self_jump = XRT_ERT_SELF_JUMP(res->image_word0) ? 1 : 0;

  // The input describes both images: the scheduler's, and the board
  // controller's if this shell wants one. They share a reset, so xocl's
  // load_xmc() copies both before releasing it - releasing the controller onto
  // whatever its RAM happens to hold is the difference this tests.
  if (!words || count * 4 < sizeof(xrt_dext_ert_images))
    return res->status = kIOReturnBadArgument;
  auto hdr = reinterpret_cast<const xrt_dext_ert_images*>(words);
  const uint32_t hdr_words = sizeof(xrt_dext_ert_images) / 4;
  const uint64_t sched_words = hdr->sched_bytes / 4;
  const uint64_t cmc_words = hdr->cmc_bytes / 4;
  if ((hdr->sched_bytes % 4) || (hdr->cmc_bytes % 4)
      || hdr_words + sched_words + cmc_words != count)
    return res->status = kIOReturnBadArgument;
  const uint32_t release = hdr->gpio_release ? hdr->gpio_release : XRT_ERT_GPIO_ENABLED;
  const uint32_t* sched = words + hdr_words;
  const uint32_t* cmc = sched + sched_words;
  if (sched_words == 0 || sched_words * 4 > fwroom || sched_words * 4 > XRT_ERT_IMAGE_MAX) {
    LOG("ErtStart refused: scheduler image of %llu words does not fit %llu bytes",
        sched_words, fwroom);
    return res->status = kIOReturnBadArgument;
  }

  uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

  res->step = XRT_ERT_STEP_HOLD_RESET;
  wr(reset, XRT_ERT_GPIO_RESET);
  IOSleep(1);
  if (rd(reset) != XRT_ERT_GPIO_RESET) {
    res->gpio_after = rd(reset);
    LOG("ErtStart: reset GPIO did not hold (0x%x)", res->gpio_after);
    return res->status = kIOReturnIOError;
  }

  res->step = XRT_ERT_STEP_COPY;
  // The controller first, then the scheduler, as load_xmc() does.
  if (cmc_words && blk_present(ivars, XRT_BLK_CMC_FW_MEM)) {
    const uint32_t cmc_mem = blk_off(ivars, XRT_BLK_CMC_FW_MEM);
    const uint64_t cmc_room = blk(ivars, XRT_BLK_CMC_FW_MEM).size;
    if (cmc_words * 4 <= cmc_room && cmc_mem + cmc_words * 4 <= size) {
      for (uint64_t i = 0; i < cmc_words; ++i)
        wr(cmc_mem + static_cast<uint32_t>(i * 4), cmc[i]);
      res->cmc_words_copied = static_cast<uint32_t>(cmc_words);
    }
    else
      LOG("ErtStart: controller image of %llu words does not fit; skipped", cmc_words);
  }
  for (uint64_t i = 0; i < sched_words; ++i)
    wr(fwmem + static_cast<uint32_t>(i * 4), sched[i]);
  res->words_copied = static_cast<uint32_t>(sched_words);
  res->image_word0 = rd(fwmem);

  res->step = XRT_ERT_STEP_RELEASE;
  wr(reset, release);
  IOSleep(10);
  res->gpio_after = rd(reset);
  if (!(res->gpio_after & XRT_ERT_GPIO_ENABLED)) {
    LOG("ErtStart: reset GPIO did not release (wrote 0x%x, reads 0x%x)",
        release, res->gpio_after);
    return res->status = kIOReturnIOError;
  }
  LOG("ErtStart: released with 0x%x, GPIO reads 0x%x", release, res->gpio_after);

  // --- configure it, and use the answer as the liveness test
  res->step = XRT_ERT_STEP_CONFIGURE;
  // ert_configure_cmd: header, slot_size, num_cus, cu_shift, cu_base_addr,
  // features, then one address per CU. Configure for the CU window this driver
  // already knows, one CU, polling rather than interrupts.
  const uint32_t num_cus = 1;
  const uint32_t payload = 5 + num_cus;        // words after the header
  const uint32_t header = (XRT_ERT_STATE_NEW & 0xf)
                        | ((payload & 0x7ff) << 12)
                        | ((XRT_ERT_OP_CONFIGURE & 0x1f) << 23);
  uint32_t pkt[8] = {
    header,
    XRT_ERT_SLOT_SIZE,
    num_cus,
    16,                                        // cu_shift: 64 KiB per CU
    cu_win_base(ivars),
    0x3,                                       // features: ert | polling
    cu_win_base(ivars),
  };
  // Write the payload first and the header last, so the scheduler cannot see a
  // command marked NEW before the rest of it has landed.
  for (uint32_t i = 1; i < payload + 1; ++i)
    wr(cq + i * 4, pkt[i]);
  wr(cq, pkt[0]);

  // Ring the doorbell for slot 0.
  wr(csr + XRT_ERT_CQ_STATUS_ENABLE, 0x1);
  wr(csr + XRT_ERT_CQ_STATUS_REG, 0x1);

  res->step = XRT_ERT_STEP_WAIT_CONFIG;
  uint64_t tc = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  for (int i = 0; i < 200; ++i) {              // up to ~2 s
    res->config_header = rd(cq);
    res->config_state = res->config_header & 0xf;
    if (res->config_state == XRT_ERT_STATE_COMPLETED)
      break;
    IOSleep(10);
  }
  res->config_ms = static_cast<uint32_t>((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - tc) / 1000000);
  res->csr_status = rd(csr + XRT_ERT_STATUS_REG);
  res->cu_status = rd(csr + XRT_ERT_CU_STATUS_REG);
  res->elapsed_ms = static_cast<uint32_t>((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1000000);

  if (res->config_state != XRT_ERT_STATE_COMPLETED) {
    LOG("ErtStart: configure not completed, slot 0 header 0x%x after %u ms",
        res->config_header, res->config_ms);
    return res->status = kIOReturnNotResponding;
  }
  res->step = XRT_ERT_STEP_COMPLETE;
  LOG("ErtStart: scheduler running, configure completed in %u ms", res->config_ms);
  return res->status = kIOReturnSuccess;
}

// Bring the board controller up. Mirrors xocl subdev/xmc.c load_xmc(): hold the
// reset, copy the image into the controller's RAM, release the reset, wait for
// the register map to come alive.
//
// Nothing here is written to flash. The controller's image is loaded from the
// host on every boot, so a failed load costs a reset, not the card.
kern_return_t
XrtUser::LoadCmc(const uint32_t* words, uint64_t count, void* out)
{
  auto res = static_cast<xrt_dext_cmc_result*>(out);
  memset(res, 0, sizeof(*res));
  auto pci = ivars->pci;
  if (!pci)
    return res->status = kIOReturnNoDevice;
  if (ivars->asleep)
    return res->status = kIOReturnOffline;
  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 0)
    return res->status = kIOReturnNotPermitted;

  res->step = XRT_CMC_STEP_CHECK;
  static const uint32_t needed[] = { XRT_BLK_CMC_REGMAP, XRT_BLK_CMC_FW_MEM, XRT_BLK_CMC_RESET };
  for (uint32_t k = 0; k < sizeof(needed) / sizeof(needed[0]); ++k) {
    if (!blk_present(ivars, needed[k])) {
      LOG("LoadCmc refused: block %u is not on this platform", needed[k]);
      return res->status = kIOReturnUnsupported;
    }
  }
  uint8_t bar = 0, type = 0;
  uint64_t size = 0;
  if (pci->GetBARInfo(0, &bar, &size, &type) != kIOReturnSuccess)
    return res->status = kIOReturnNoResources;
  const uint32_t regmap = blk_off(ivars, XRT_BLK_CMC_REGMAP);
  const uint32_t fwmem = blk_off(ivars, XRT_BLK_CMC_FW_MEM);
  const uint32_t reset = blk_off(ivars, XRT_BLK_CMC_RESET);
  const uint64_t fwroom = blk(ivars, XRT_BLK_CMC_FW_MEM).size;
  auto rd = [&](uint32_t off) { uint32_t v = 0; pci->MemoryRead32(bar, off, &v); return v; };
  auto wr = [&](uint32_t off, uint32_t v) { pci->MemoryWrite32(bar, off, v); };

  // Already running: say so and write nothing.
  res->magic = rd(regmap + XRT_CMC_MAGIC_REG);
  if (res->magic == XRT_CMC_MAGIC) {
    res->was_running = 1;
    res->version = rd(regmap + XRT_CMC_VERSION_REG);
    res->cmc_status = rd(regmap + XRT_CMC_STATUS_REG);
    res->step = XRT_CMC_STEP_COMPLETE;
    return res->status = kIOReturnSuccess;
  }
  if (!words || count == 0 || count * 4 > fwroom || count * 4 > XRT_CMC_IMAGE_MAX) {
    LOG("LoadCmc refused: image of %llu words does not fit %llu bytes of controller RAM",
        count, fwroom);
    return res->status = kIOReturnBadArgument;
  }

  uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

  // --- hold the reset
  res->step = XRT_CMC_STEP_HOLD_RESET;
  wr(reset, XRT_CMC_GPIO_RESET);
  IOSleep(1);
  if (rd(reset) != XRT_CMC_GPIO_RESET) {
    LOG("LoadCmc: reset GPIO did not hold (0x%x)", rd(reset));
    res->gpio_after = rd(reset);
    return res->status = kIOReturnIOError;
  }

  // --- copy the image
  res->step = XRT_CMC_STEP_COPY;
  for (uint64_t i = 0; i < count; ++i)
    wr(fwmem + static_cast<uint32_t>(i * 4), words[i]);
  res->words_copied = static_cast<uint32_t>(count);

  // --- release it
  res->step = XRT_CMC_STEP_RELEASE;
  wr(reset, XRT_CMC_GPIO_ENABLED);
  IOSleep(1);
  res->gpio_after = rd(reset);
  if (res->gpio_after != XRT_CMC_GPIO_ENABLED) {
    LOG("LoadCmc: reset GPIO did not release (0x%x)", res->gpio_after);
    return res->status = kIOReturnIOError;
  }

  // --- wait for the register map to mean something
  res->step = XRT_CMC_STEP_WAIT_MAGIC;
  for (int i = 0; i < 200; ++i) {                // up to ~2 s
    res->magic = rd(regmap + XRT_CMC_MAGIC_REG);
    if (res->magic == XRT_CMC_MAGIC)
      break;
    IOSleep(10);
  }
  if (res->magic != XRT_CMC_MAGIC) {
    LOG("LoadCmc: controller did not come up, magic 0x%x", res->magic);
    res->elapsed_ms = static_cast<uint32_t>((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1000000);
    return res->status = kIOReturnNotResponding;
  }

  // --- and for its sensors to be readable
  res->step = XRT_CMC_STEP_WAIT_READY;
  if (blk_present(ivars, XRT_BLK_CMC_MUTEX)) {
    const uint32_t mutex = blk_off(ivars, XRT_BLK_CMC_MUTEX);
    for (int i = 0; i < 200; ++i) {
      res->ready = rd(mutex + XRT_UCS_CHANNEL2);
      if (res->ready & XRT_CMC_REGMAP_READY)
        break;
      IOSleep(10);
    }
  }
  res->version = rd(regmap + XRT_CMC_VERSION_REG);
  res->cmc_status = rd(regmap + XRT_CMC_STATUS_REG);
  res->error = rd(regmap + XRT_CMC_ERROR_REG);
  res->feature = rd(regmap + XRT_CMC_FEATURE_REG);
  res->elapsed_ms = static_cast<uint32_t>((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1000000);
  res->step = XRT_CMC_STEP_COMPLETE;
  LOG("LoadCmc: controller up in %u ms, version 0x%x status 0x%x ready 0x%x",
      res->elapsed_ms, res->version, res->cmc_status, res->ready);
  return res->status = kIOReturnSuccess;
}

// The card addresses DMA and the allocator may touch, from the loaded xclbin's
// memory topology. This is a backstop against a caller naming an address that
// is not memory at all, so it may only shrink to, or move onto, real banks -
// and not while any buffer is outstanding, because those live inside it.
kern_return_t
XrtUser::SetCardRange(uint64_t first, uint64_t limit)
{
  auto pci = ivars->pci;
  if (!pci)
    return kIOReturnNoDevice;
  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 1)
    return kIOReturnNotPermitted;
  if (limit <= first)
    return kIOReturnBadArgument;

  IOLockLock(ivars->mem_lock);
  if (ivars->mem_count) {
    IOLockUnlock(ivars->mem_lock);
    LOG("SetCardRange refused: %u device buffer(s) are outstanding", ivars->mem_count);
    return kIOReturnBusy;
  }
  ivars->card_min = first;
  ivars->card_max = limit;
  IOLockUnlock(ivars->mem_lock);
  LOG("card address range now 0x%llx..0x%llx", first, limit);
  return kIOReturnSuccess;
}

kern_return_t
XrtUser::SetPlatform(const void* in, uint64_t size)
{
  if (!in || size < sizeof(xrt_dext_platform) || !ivars->pci)
    return kIOReturnBadArgument;
  auto want = static_cast<const xrt_dext_platform*>(in);
  if (want->abi != XRT_DEXT_ABI_VERSION || want->count > XRT_BLK_MAX)
    return kIOReturnUnsupported;
  if (!(ivars->plat.flags & XRT_PLATFORM_DEFAULT)) {
    LOG("SetPlatform refused: a map is already installed");
    return kIOReturnNotPermitted;
  }
  for (auto b : ivars->xclbin_uuid)
    if (b != 0)
      return kIOReturnNotPermitted;

  // The flash block is the one block a caller may not place. XRT_DEXT_SPI_REG
  // lets user space name an offset within it, bounded only by the base recorded
  // here - so a map that put XRT_BLK_FLASH at, say, the ICAP or a firewall would
  // turn that selector into an arbitrary write anywhere in the BAR. Only
  // probe_flash_block() may set it, and it only ever picks from its own candidate
  // list. Nothing in XRT sends this block (grep: no writer outside this file), so
  // refusing it costs nothing.
  if (want->blk[XRT_BLK_FLASH].size) {
    LOG("SetPlatform refused: a caller may not place the flash block");
    return kIOReturnNotPermitted;
  }

  for (uint32_t i = 0; i < want->count; ++i) {
    const auto& b = want->blk[i];
    if (!b.size)
      continue;                       // absent is legitimate
    if (b.bar > 5 || b.func > 1)
      return kIOReturnBadArgument;
    uint8_t idx = 0, type = 0;
    uint64_t bar_size = 0;
    if (ivars->pci->GetBARInfo(b.bar, &idx, &bar_size, &type) != kIOReturnSuccess)
      return kIOReturnNoResources;
    if (b.offset + b.size > bar_size || b.offset + b.size < b.offset) {
      LOG("SetPlatform refused: block %u at 0x%llx+0x%llx is outside BAR%u (0x%llx)",
          i, b.offset, b.size, b.bar, bar_size);
      return kIOReturnBadArgument;
    }
  }

  if (want->blk[XRT_BLK_XDMA].size && want->blk[XRT_BLK_XDMA].offset) {
    LOG("SetPlatform refused: XDMA at a non-zero offset (0x%llx) in BAR%u is not handled",
        want->blk[XRT_BLK_XDMA].offset, want->blk[XRT_BLK_XDMA].bar);
    return kIOReturnUnsupported;
  }
  memcpy(&ivars->plat, want, sizeof(ivars->plat));
  ivars->plat.flags &= ~XRT_PLATFORM_DEFAULT;
  ivars->plat.count = XRT_BLK_MAX;
  LOG("SetPlatform: installed; icap 0x%llx gate 0x%llx rom %s",
      ivars->plat.blk[XRT_BLK_ICAP].offset, ivars->plat.blk[XRT_BLK_GATE_ULP].offset,
      ivars->plat.blk[XRT_BLK_FEATURE_ROM].size ? "present" : "absent");
  return kIOReturnSuccess;
}

kern_return_t
XrtUser::GetPlatform(void* out, uint64_t size)
{
  if (!out || size < sizeof(xrt_dext_platform))
    return kIOReturnBadArgument;
  auto p = static_cast<xrt_dext_platform*>(out);
  memcpy(p, &ivars->plat, sizeof(ivars->plat));
  p->cu_window_offset = ivars->cu_win_base;
  p->cu_window_size = ivars->cu_win_size;
  p->card_min = ivars->card_min;
  p->card_max = ivars->card_max;
  return kIOReturnSuccess;
}

kern_return_t
XrtUser::CopyBarMemory(uint32_t bar, IOMemoryDescriptor** memory)
{
  if (!memory || bar > 5 || !ivars->pci)
    return kIOReturnBadArgument;
  uint8_t idx = 0, type = 0;
  uint64_t bar_size = 0;
  if (ivars->pci->GetBARInfo(bar, &idx, &bar_size, &type) != kIOReturnSuccess)
    return kIOReturnNoResources;
  return ivars->pci->_CopyDeviceMemoryWithIndex(idx, memory, this);
}

// ---------------------------------------------------------------------------
// Phase 3: program the user partition. Mirrors xocl icap.c for the legacy
// single-partition XDMA shells: freeze ULP gate, stream the bitstream into
// the AXI HWICAP write FIFO, re-commit the clock wizards (PR can silently
// change MMCM settings), free the gate, wait for MIG calibration.
// ---------------------------------------------------------------------------
// ICAP and gate register offsets WITHIN their blocks; the blocks themselves
// come from the platform table, so these are the only fixed numbers left.
static constexpr uint32_t kIcapWFRel  = 0x100;  // write FIFO
static constexpr uint32_t kIcapCRRel  = 0x10c;  // control
static constexpr uint32_t kIcapSRRel  = 0x110;  // status
static constexpr uint32_t kIcapWFVRel = 0x114;  // write FIFO vacancy
static constexpr uint32_t kGateWRRel  = 0x0;
static constexpr uint32_t kGateRDRel  = 0x8;
static constexpr uint32_t kClkStatus = 0x4;
static constexpr uint32_t kClkCfg0   = 0x200;
static constexpr uint32_t kClkCfg2   = 0x208;
static constexpr uint32_t kClkCfg23  = 0x25c;

// xocl clock_wiz.c find_matching_freq_config(): binary search that rounds
// down to the nearest table entry.
static unsigned
find_matching_freq_config(unsigned freq)
{
  const unsigned size = XRT_CLKWIZ_TABLE_SIZE;
  unsigned start = 0, end = size - 1, idx = size - 1;
  if (freq < xrt_clkwiz_table[0].ocl)
    return 0;
  if (freq > xrt_clkwiz_table[size - 1].ocl)
    return size - 1;
  while (start < end) {
    if (freq == xrt_clkwiz_table[idx].ocl)
      break;
    if (freq < xrt_clkwiz_table[idx].ocl)
      end = idx;
    else
      start = idx + 1;
    idx = start + (end - start) / 2;
  }
  if (freq < xrt_clkwiz_table[idx].ocl)
    idx--;
  return idx;
}

kern_return_t
XrtUser::LoadBitstream(const uint32_t* words, uint64_t count, uint64_t flags,
                       const uint32_t* target_mhz, const uint32_t* ulp_clk,
                       uint32_t ucs, const uint32_t* freq_cnt, void* out)
{
  auto res = static_cast<xrt_dext_load_result*>(out);
  memset(res, 0, sizeof(*res));
  for (int i = 0; i < XRT_LOAD_CLOCKS; ++i)
    res->target_mhz[i] = target_mhz ? target_mhz[i] : 0;
  if (ivars->asleep)
    return res->status = kIOReturnOffline;
  auto pci = ivars->pci;
  if (!pci)
    return res->status = kIOReturnNoDevice;

  // --- CHECK: management function, ROM present, register BAR large enough
  res->step = XRT_LOAD_STEP_CHECK;
  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 0) {
    LOG("LoadBitstream refused: not the management function");
    return res->status = kIOReturnNotPermitted;
  }
  uint8_t bar = 0, type = 0;
  uint64_t size = 0;
  if (pci->GetBARInfo(0, &bar, &size, &type) != kIOReturnSuccess)
    return res->status = kIOReturnNoResources;
  auto rd = [&](uint32_t off) { uint32_t v = 0; pci->MemoryRead32(bar, off, &v); return v; };
  auto wr = [&](uint32_t off, uint32_t v) { pci->MemoryWrite32(bar, off, v); };

  // Every block this sequence touches has to be on the map and inside the BAR.
  // On a dynamic-IP platform the clock wizards and the feature ROM are simply
  // absent, so this refuses before it writes anything - programming such a
  // shell needs more than relocated offsets.
  static const uint32_t needed[] = { XRT_BLK_ICAP, XRT_BLK_GATE_ULP, XRT_BLK_MEMCALIB };
  for (uint32_t k = 0; k < sizeof(needed) / sizeof(needed[0]); ++k) {
    const uint32_t id = needed[k];
    if (!blk_present(ivars, id)) {
      LOG("LoadBitstream refused: block %u is not on this platform", id);
      return res->status = kIOReturnUnsupported;
    }
    if (blk(ivars, id).offset + blk(ivars, id).size > size)
      return res->status = kIOReturnNoResources;
  }
  const uint32_t icap = blk_off(ivars, XRT_BLK_ICAP);
  const uint32_t kIcapWF  = icap + kIcapWFRel;
  const uint32_t kIcapCR  = icap + kIcapCRRel;
  const uint32_t kIcapSR  = icap + kIcapSRRel;
  const uint32_t kIcapWFV = icap + kIcapWFVRel;
  const uint32_t kGateWR  = blk_off(ivars, XRT_BLK_GATE_ULP) + kGateWRRel;
  const uint32_t kGateRD  = blk_off(ivars, XRT_BLK_GATE_ULP) + kGateRDRel;
  const uint32_t kMemCalib = blk_off(ivars, XRT_BLK_MEMCALIB);

  // Only a platform that has a feature ROM is checked against it. Where there
  // is none, the caller has already matched the xclbin's interface uuid against
  // the shell's, which is the same guarantee by another route.
  if (blk_present(ivars, XRT_BLK_FEATURE_ROM)
      && rd(blk_off(ivars, XRT_BLK_FEATURE_ROM)) != XRT_DEXT_ROM_MAGIC)
    return res->status = kIOReturnNotFound;

  // The kernel clock wizards: the platform's own, or the ones the caller found
  // in the xclbin's partition metadata. Either way they must be inside the BAR,
  // because their configuration registers get written.
  uint32_t clk[XRT_LOAD_CLOCKS] = { 0, 0, 0 };
  // True when the clocks live in the partition being downloaded rather than in
  // the base platform. That changes the sequence, not just the addresses.
  bool clocks_in_partition = false;
  if (blk_present(ivars, XRT_BLK_CLKWIZ_K1) && blk_present(ivars, XRT_BLK_CLKWIZ_K2)) {
    clk[0] = blk_off(ivars, XRT_BLK_CLKWIZ_K1);
    clk[1] = blk_off(ivars, XRT_BLK_CLKWIZ_K2);
  }
  else if (ulp_clk && ulp_clk[0]) {
    clocks_in_partition = true;
    for (int i = 0; i < XRT_LOAD_CLOCKS; ++i) {
      clk[i] = ulp_clk[i];
      if (!clk[i])
        continue;
      if ((clk[i] & 3) || clk[i] + 0x1000 > size || clk[i] + 0x1000 < clk[i]) {
        LOG("LoadBitstream refused: clock wizard %d at 0x%x is outside BAR0 (0x%llx)",
            i, clk[i], size);
        return res->status = kIOReturnBadArgument;
      }
      // Nothing can be read back from a wizard that does not exist yet, so a
      // target frequency is the only way to know what to program. A state-only
      // call programs nothing, so it needs none.
      if (!res->target_mhz[i] && !(flags & XRT_DEXT_LOAD_STATE_ONLY)) {
        LOG("LoadBitstream refused: clock wizard %d at 0x%x has no target frequency,"
            " and nothing can be read back from a partition that is not loaded", i, clk[i]);
        return res->status = kIOReturnBadArgument;
      }
    }
    if (ucs && ((ucs & 3) || ucs + 0x1000 > size)) {
      LOG("LoadBitstream refused: UCS at 0x%x is outside BAR0", ucs);
      return res->status = kIOReturnBadArgument;
    }
    LOG("clocks come with the partition: wizards 0x%x 0x%x 0x%x, UCS 0x%x",
        clk[0], clk[1], clk[2], ucs);
    for (int i = 0; i < XRT_LOAD_CLOCKS; ++i) {
      uint32_t c = freq_cnt ? freq_cnt[i] : 0;
      ivars->ulp_freq_cnt[i] = (c && !(c & 3) && c + 0x1000 <= size) ? c : 0;
    }
  }
  else {
    LOG("LoadBitstream refused: no kernel clock wizards, and the caller named none");
    return res->status = kIOReturnUnsupported;
  }
  res->icap_sr_before = rd(kIcapSR);
  res->icap_wfv = rd(kIcapWFV);
  res->gate_before = rd(kGateRD);
  res->calib = rd(kMemCalib);

  // --- CLOCK_SAVE: the configuration to re-commit after the download. Skipped
  // when the clocks are in the partition: there is nothing there to save, and
  // reading anyway returns filler that looks like a locked clock.
  // Whoever used the flash last may have left the SPI core enabled, and it holds
  // STARTUPE3 - the ICAP cannot configure while it does. Disabling an already
  // disabled core costs one write.
  // Probe first. SPE is hardware state that survives a dext restart; the block
  // table entry is not. Without this the guard below is skipped in precisely the
  // case it exists for - a fresh dext facing a core someone else left enabled.
  probe_flash_block(ivars, pci);
  if (blk_present(ivars, XRT_BLK_FLASH)) {
    uint8_t fbar = 0, ftype = 0;
    uint64_t fsize = 0;
    if (pci->GetBARInfo(0, &fbar, &fsize, &ftype) == kIOReturnSuccess) {
      pci->MemoryWrite32(fbar, blk_off(ivars, XRT_BLK_FLASH) + XRT_SPI_SSR, 0xffffffffu);
      pci->MemoryWrite32(fbar, blk_off(ivars, XRT_BLK_FLASH) + XRT_SPI_CR, 0);
    }
  }
  res->step = XRT_LOAD_STEP_CLOCK_SAVE;
  for (int i = 0; i < (clocks_in_partition ? 0 : 2); ++i) {
    res->clk_status[i] = rd(clk[i] + kClkStatus);
    res->clk_cfg0[i] = rd(clk[i] + kClkCfg0);
    res->clk_cfg2[i] = rd(clk[i] + kClkCfg2);
    // A read that never reached a register comes back as filler, and the
    // filler patterns have bit 0 set - so "locked" would be believed. That
    // matters here because these values get written back after the download.
    // A wizard in a user partition that is not loaded reads exactly this way.
    if (is_filler(res->clk_status[i])) {
      LOG("clock wizard %d at 0x%x reads 0x%x: nothing is answering there;"
          " refusing to load", i, clk[i], res->clk_status[i]);
      return res->status = kIOReturnNotResponding;
    }
    if ((res->clk_status[i] & 1) == 0) {
      LOG("clock wizard %d not locked (status 0x%x); refusing to load", i, res->clk_status[i]);
      return res->status = kIOReturnNotReady;
    }
  }
  if (flags & XRT_DEXT_LOAD_STATE_ONLY) {
    res->step = XRT_LOAD_STEP_COMPLETE;
    res->gate_after = res->gate_before;
    return res->status = kIOReturnSuccess;
  }
  if (!words || count == 0)
    return res->status = kIOReturnBadArgument;

  LOG("LoadBitstream: %llu words, ICAP SR 0x%x, WFV %u, gate 0x%x",
      count, res->icap_sr_before, res->icap_wfv, res->gate_before);

  // The user partition is about to change: forget the previous xclbin.
  memset(ivars->xclbin_uuid, 0, sizeof(ivars->xclbin_uuid));
  for (auto& sec : ivars->sections)
    OSSafeReleaseNULL(sec);

  // --- GATE_FREEZE (axigate.c axigate_freeze): read != 0 means open
  res->step = XRT_LOAD_STEP_GATE_FREEZE;
  if (rd(kGateRD) != 0) {
    wr(kGateWR, 0x0);
    IODelay(1);
    (void)rd(kGateRD);
  }

  kern_return_t ret = kIOReturnSuccess;
  uint64_t t0 = 0, t1 = 0;
  t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

  // --- ICAP_WRITE (icap.c bitstream_helper + icap_write)
  res->step = XRT_LOAD_STEP_ICAP_WRITE;
  uint64_t remain = count;
  const uint32_t* p = words;
  while (remain) {
    uint32_t vacancy = rd(kIcapWFV);
    if (vacancy == 0 || vacancy == 0xffffffff) {
      LOG("ICAP no vacancy (0x%x) after %u words", vacancy, res->words_written);
      ret = kIOReturnIOError;
      break;
    }
    uint32_t n = (vacancy < remain) ? vacancy : static_cast<uint32_t>(remain);
    for (uint32_t i = 0; i < n; ++i)
      wr(kIcapWF, p[i]);
    wr(kIcapCR, 0x1);                          // flush FIFO to ICAP
    bool flushed = false;
    for (int i = 0; i < 1000; ++i) {
      if ((rd(kIcapCR) & 0x1) == 0) { flushed = true; break; }
    }
    if (!flushed) {
      LOG("ICAP write flush timeout after %u words", res->words_written);
      ret = kIOReturnTimeout;
      break;
    }
    p += n;
    remain -= n;
    res->words_written += n;
  }
  t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  res->elapsed_ms = static_cast<uint32_t>((t1 - t0) / 1000000);

  // --- ICAP_DONE (icap.c wait_for_done: SR & 0x5)
  if (ret == kIOReturnSuccess) {
    res->step = XRT_LOAD_STEP_ICAP_DONE;
    ret = kIOReturnTimeout;
    for (int i = 0; i < 100; ++i) {
      res->icap_sr_after = rd(kIcapSR);
      if (res->icap_sr_after & 0x5) { ret = kIOReturnSuccess; break; }
      IODelay(50);
    }
    if (ret != kIOReturnSuccess)
      LOG("ICAP done timeout, SR 0x%x", res->icap_sr_after);
  }

  // --- CLOCK_RESTORE (clock_wiz.c clock_ocl_freqscaling_ultrascale, force)
  // PR can silently change MMCM settings, so every wizard is re-committed:
  // with the table entry for the xclbin's frequency, or with the saved
  // configuration when no target is given. Done even after an ICAP failure,
  // before the gate is freed. Lock waits follow clock_wiz_busy(): status
  // must read exactly 1.
  if (ret == kIOReturnSuccess)
    res->step = XRT_LOAD_STEP_CLOCK_RESTORE;
  auto wait_locked = [&](uint32_t base, int cycles, int interval_ms) {
    uint32_t v = rd(base + kClkStatus);
    for (int c = 0; v != 1 && c < cycles; ++c) {
      IOSleep(interval_ms);
      v = rd(base + kClkStatus);
    }
    return v;
  };
  auto program_clocks = [&]() {
  for (int i = 0; i < XRT_LOAD_CLOCKS; ++i) {
    // A platform with fewer wizards than XRT_LOAD_CLOCKS leaves the rest at 0.
    // That is the XDMA 5.x shells, which have two, as well as a partition that
    // brings fewer than three - so this cannot be conditional on where the
    // clocks came from. Reading offset 0 instead lands on whatever is at the
    // bottom of the BAR and reports the clock busy.
    if (!clk[i])
      continue;
    uint32_t cfg0 = res->clk_cfg0[i], cfg2 = res->clk_cfg2[i];
    if (res->target_mhz[i]) {
      unsigned idx = find_matching_freq_config(res->target_mhz[i]);
      cfg0 = xrt_clkwiz_table[idx].config0;
      cfg2 = xrt_clkwiz_table[idx].config2;
      res->table_mhz[i] = xrt_clkwiz_table[idx].ocl;
    }
    if (wait_locked(clk[i], 20, 50) != 1) {
      LOG("clock wizard %d busy before update", i);
      if (ret == kIOReturnSuccess)
        ret = kIOReturnBusy;
      continue;
    }
    wr(clk[i] + kClkCfg0, cfg0);
    wr(clk[i] + kClkCfg2, cfg2);
    IOSleep(10);
    wr(clk[i] + kClkCfg23, 0x7);
    IOSleep(1);
    wr(clk[i] + kClkCfg23, 0x2);
    res->clk_status[i] = wait_locked(clk[i], 100, 100);
    if (res->clk_status[i] != 1) {
      LOG("clock wizard %d did not lock; restoring original configuration", i);
      wr(clk[i] + kClkCfg23, 0x4);
      IOSleep(10);
      wr(clk[i] + kClkCfg23, 0x0);
      if (ret == kIOReturnSuccess)
        ret = kIOReturnTimeout;
      continue;
    }
    // Report what the wizard now holds.
    res->clk_cfg0[i] = rd(clk[i] + kClkCfg0);
    res->clk_cfg2[i] = rd(clk[i] + kClkCfg2);
  }
  };
  if (!clocks_in_partition)
    program_clocks();               // legacy: still inside the frozen gate

  // --- GATE_FREE (axigate.c axigate_free): always, so the shell is usable
  if (ret == kIOReturnSuccess)
    res->step = XRT_LOAD_STEP_GATE_FREE;
  if (rd(kGateRD) == 0) {
    wr(kGateWR, 0x2);
    IODelay(1);
    (void)rd(kGateRD);
    wr(kGateWR, 0x3);
    IODelay(1);
    (void)rd(kGateRD);
  }
  res->gate_after = rd(kGateRD);

  // --- The 2RP tail. The clocks only exist now that the partition is in, so
  // they are programmed here rather than around the download. xocl's order
  // (subdev/clock_wiz.c): reset the gate, set the clocks, enable UCS channel 2.
  // Doing it in another order is documented there as tripping firewalls.
  if (clocks_in_partition && ret == kIOReturnSuccess) {
    res->step = XRT_LOAD_STEP_CLOCK_RESTORE;
    wr(kGateWR, 0x0);
    IODelay(1);
    wr(kGateWR, 0x1);
    IODelay(1);
    (void)rd(kGateRD);

    program_clocks();

    if (ucs) {
      IOSleep(10);
      wr(ucs + XRT_UCS_CHANNEL2, 0x1);
      IOSleep(10);
      res->ucs_after = rd(ucs + XRT_UCS_CHANNEL1);
      LOG("UCS enabled; channel 1 reads 0x%x", res->ucs_after);
    }
  }

  // --- CALIBRATION (icap.c calibrate_mig: 20 x 500 ms)
  if (ret == kIOReturnSuccess) {
    res->step = XRT_LOAD_STEP_CALIBRATION;
    ret = kIOReturnTimeout;
    for (int i = 0; i <= 20; ++i) {
      res->calib = rd(kMemCalib);
      if (res->calib & 1) { ret = kIOReturnSuccess; break; }
      IOSleep(500);
      res->calib_ms += 500;
    }
    if (ret == kIOReturnSuccess)
      res->step = XRT_LOAD_STEP_COMPLETE;
  }

  LOG("LoadBitstream: step %u status 0x%x words %u in %u ms, SR 0x%x, gate 0x%x, calib 0x%x after %u ms",
      res->step, ret, res->words_written, res->elapsed_ms, res->icap_sr_after,
      res->gate_after, res->calib, res->calib_ms);
  return res->status = ret;
}

void
XrtUser::SetXclbinUuid(const uint8_t* uuid)
{
  memcpy(ivars->xclbin_uuid, uuid, sizeof(ivars->xclbin_uuid));
}

void
XrtUser::CopyXclbinUuid(uint8_t* uuid)
{
  memcpy(uuid, ivars->xclbin_uuid, sizeof(ivars->xclbin_uuid));
}

// ---------------------------------------------------------------------------
// Phase 4 self-test: XDMA channel 0, polled, following xocl libxdma.c
// engine_start()/engine_start_mode_config(). Descriptors and data live in
// dext-allocated buffers mapped through the DART with IODMACommand.
// ---------------------------------------------------------------------------
static constexpr uint32_t kXdmaCtrl     = 0x04;
static constexpr uint32_t kXdmaCtrlW1C  = 0x0c;
static constexpr uint32_t kXdmaStatus   = 0x40;
static constexpr uint32_t kXdmaStatusRC = 0x44;
static constexpr uint32_t kXdmaDone     = 0x48;
static constexpr uint32_t kXdmaIrqMask  = 0x90;     // engine interrupt_enable_mask
// IRQ block (XRT_XDMA_IRQ) registers, libxdma struct interrupt_regs
static constexpr uint32_t kIrqChannelEnableW1S = 0x14;
static constexpr uint32_t kIrqChannelEnableW1C = 0x18;
static constexpr uint32_t kIrqChannelVector    = 0xa0;
// Engines 0-3 on MSI-X vectors 0-3, packed 8 bits each (prog_irq_msix_channel)
static constexpr uint32_t kIrqChannelVectorMap = 0x03020100;
// IE_DESC_STOPPED | IE_DESC_COMPLETED
static constexpr uint32_t kIeCompletion = (1u << 1) | (1u << 2);
// IE_DESC_ALIGN_MISMATCH | IE_MAGIC_STOPPED | IE_READ_ERROR | IE_DESC_ERROR
static constexpr uint32_t kIeErrors     = (1u << 3) | (1u << 4) | (0x1fu << 9) | (0x1fu << 19);
static constexpr uint32_t kSgFirstLo    = 0x80;
static constexpr uint32_t kSgFirstHi    = 0x84;
static constexpr uint32_t kSgAdjacent   = 0x88;
static constexpr uint32_t kCtrlRun      = 1u << 0;
// RUN | IE_DESC_ALIGN_MISMATCH | IE_MAGIC_STOPPED | IE_READ_ERROR | IE_DESC_ERROR
static constexpr uint32_t kCtrlStart    = kCtrlRun | (1u << 3) | (1u << 4) | (0x1fu << 9) | (0x1fu << 19);
static constexpr uint32_t kStatBusy     = 1u << 0;
static constexpr uint32_t kStatOk       = (1u << 0) | (1u << 1) | (1u << 2);  // busy, stopped, completed
static constexpr uint16_t kCmdBusMaster = 0x0004;
static constexpr uint32_t kDescMagic    = 0xad4b0000u;
static constexpr uint32_t kDescStopped  = 1u << 0;
static constexpr uint32_t kDescCompleted= 1u << 1;
static constexpr uint32_t kDescEop      = 1u << 4;

struct xdma_desc {
  uint32_t control, bytes, src_lo, src_hi, dst_lo, dst_hi, next_lo, next_hi;
};

kern_return_t
XrtUser::DmaSelfTest(uint64_t card_addr, uint64_t length, uint64_t seed, void* out)
{
  auto res = static_cast<xrt_dext_dma_result*>(out);
  memset(res, 0, sizeof(*res));
  res->card_addr = card_addr;
  res->length = static_cast<uint32_t>(length);
  auto pci = ivars->pci;
  if (!pci)
    return res->status = kIOReturnNoDevice;

  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 1)
    return res->status = kIOReturnNotPermitted;           // XDMA lives on the user function
  if (ivars->partition_held)
    return res->status = kIOReturnBusy;                   // being reprogrammed
  if (length == 0 || length > XRT_DMA_SELFTEST_MAX || (length % 4)
      || card_addr < ivars->card_min || card_addr + length > ivars->card_max)
    return res->status = kIOReturnBadArgument;

  uint8_t bar = 0, type = 0;
  uint64_t bar_size = 0;
  if (pci->GetBARInfo(xdma_bar(ivars), &bar, &bar_size, &type) != kIOReturnSuccess || bar_size < 0x6000)
    return res->status = kIOReturnNoResources;
  auto rd = [&](uint32_t off) { uint32_t v = 0; pci->MemoryRead32(bar, off, &v); return v; };
  auto wr = [&](uint32_t off, uint32_t v) { pci->MemoryWrite32(bar, off, v); };
  const uint32_t h2c = XRT_XDMA_H2C(0), c2h = XRT_XDMA_C2H(0);
  if ((rd(h2c) >> 16) != 0x1fc0 || (rd(c2h) >> 16) != 0x1fc1)
    return res->status = kIOReturnNotFound;
  if ((rd(h2c + kXdmaStatus) & kStatBusy) || (rd(c2h + kXdmaStatus) & kStatBusy))
    return res->status = kIOReturnBusy;

  // --- buffers: data (length) and one page of descriptors
  IOBufferMemoryDescriptor* data = nullptr;
  IOBufferMemoryDescriptor* descs = nullptr;
  IODMACommand* data_dma = nullptr;
  IODMACommand* desc_dma = nullptr;
  kern_return_t ret = kIOReturnSuccess;
  IOAddressSegment data_seg[32] = {};
  IOAddressSegment desc_seg[32] = {};
  uint32_t data_nseg = 32, desc_nseg = 32;
  uint64_t dflags = 0;
  IOAddressSegment data_va = {}, desc_va = {};
  IODMACommandSpecification spec = {};
  spec.options = kIODMACommandSpecificationNoOptions;
  spec.maxAddressBits = 64;

  do {
    if ((ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, length, 16384, &data)) != kIOReturnSuccess)
      break;
    if ((ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 16384, 16384, &descs)) != kIOReturnSuccess)
      break;
    data->SetLength(length);
    descs->SetLength(16384);
    if ((ret = data->GetAddressRange(&data_va)) != kIOReturnSuccess
        || (ret = descs->GetAddressRange(&desc_va)) != kIOReturnSuccess)
      break;
    if ((ret = IODMACommand::Create(pci, kIODMACommandCreateNoOptions, &spec, &data_dma)) != kIOReturnSuccess
        || (ret = IODMACommand::Create(pci, kIODMACommandCreateNoOptions, &spec, &desc_dma)) != kIOReturnSuccess)
      break;
    if ((ret = data_dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, data, 0, length,
                                       &dflags, &data_nseg, data_seg)) != kIOReturnSuccess)
      break;
    dflags = 0;
    if ((ret = desc_dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, descs, 0, 16384,
                                       &dflags, &desc_nseg, desc_seg)) != kIOReturnSuccess)
      break;
    res->segments = data_nseg;
    res->host_iova = data_seg[0].address;
    res->desc_iova = desc_seg[0].address;
    if (desc_nseg != 1 || data_nseg == 0 || data_nseg > 16384 / sizeof(xdma_desc)) {
      ret = kIOReturnUnsupported;
      break;
    }

    // --- bus master on (it stays on: every later DMA needs it)
    uint16_t cmd = 0;
    pci->ConfigurationRead16(kCfgCommand, &cmd);
    pci->ConfigurationWrite16(kCfgCommand, cmd | kCmdMemorySpace | kCmdBusMaster);
    pci->ConfigurationRead16(kCfgCommand, &cmd);
    res->pci_command = cmd;

    auto words = reinterpret_cast<uint32_t*>(data_va.address);
    auto d = reinterpret_cast<volatile xdma_desc*>(desc_va.address);
    const uint32_t n = static_cast<uint32_t>(length / 4);

    // One descriptor per IOVA segment, chained; the last one stops the engine.
    auto build = [&](bool to_card) {
      uint64_t card = card_addr;
      for (uint32_t i = 0; i < data_nseg; ++i) {
        uint64_t host = data_seg[i].address;
        uint64_t next = desc_seg[0].address + (i + 1) * sizeof(xdma_desc);
        bool last = (i + 1 == data_nseg);
        d[i].control = kDescMagic | (last ? (kDescStopped | kDescCompleted | kDescEop) : 0);
        d[i].bytes = static_cast<uint32_t>(data_seg[i].length);
        uint64_t src = to_card ? host : card, dst = to_card ? card : host;
        d[i].src_lo = static_cast<uint32_t>(src);
        d[i].src_hi = static_cast<uint32_t>(src >> 32);
        d[i].dst_lo = static_cast<uint32_t>(dst);
        d[i].dst_hi = static_cast<uint32_t>(dst >> 32);
        d[i].next_lo = last ? 0 : static_cast<uint32_t>(next);
        d[i].next_hi = last ? 0 : static_cast<uint32_t>(next >> 32);
        card += data_seg[i].length;
      }
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
    };

    // engine_start(): first descriptor, 0 extra adjacent, then run.
    auto run = [&](uint32_t eng, uint32_t& status, uint32_t& done, uint32_t& us) {
      (void)rd(eng + kXdmaStatusRC);                       // clear stale status
      wr(eng + XRT_XDMA_SGDMA + kSgFirstLo, static_cast<uint32_t>(desc_seg[0].address));
      wr(eng + XRT_XDMA_SGDMA + kSgFirstHi, static_cast<uint32_t>(desc_seg[0].address >> 32));
      wr(eng + XRT_XDMA_SGDMA + kSgAdjacent, 0);
      uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
      wr(eng + kXdmaCtrl, kCtrlStart);
      kern_return_t r = kIOReturnTimeout;
      for (int i = 0; i < 100000; ++i) {                   // ~1 s
        status = rd(eng + kXdmaStatus);
        done = rd(eng + kXdmaDone);
        if (!(status & kStatBusy) && done >= data_nseg) { r = kIOReturnSuccess; break; }
        if (status & ~kStatOk) { r = kIOReturnIOError; break; }
        IODelay(10);
      }
      us = static_cast<uint32_t>((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1000);
      wr(eng + kXdmaCtrlW1C, kCtrlRun);                    // stop the engine
      uint32_t rc = rd(eng + kXdmaStatusRC);
      if (r == kIOReturnSuccess && (rc & ~kStatOk))
        r = kIOReturnIOError;
      status |= rc;
      return r;
    };

    // --- H2C: pattern to card
    res->step = 1;
    for (uint32_t i = 0; i < n; ++i)
      words[i] = static_cast<uint32_t>(seed * 2654435761u) ^ (i * 0x9e3779b9u) ^ i;
    build(true);
    if ((ret = run(h2c, res->h2c_status, res->h2c_done, res->h2c_us)) != kIOReturnSuccess)
      break;

    // --- C2H: back into the (cleared) same buffer
    res->step = 2;
    memset(words, 0xa5, length);
    build(false);
    if ((ret = run(c2h, res->c2h_status, res->c2h_done, res->c2h_us)) != kIOReturnSuccess)
      break;
    // Make device writes visible before the CPU reads the buffer.
    data_dma->CompleteDMA(kIODMACommandCompleteDMANoOptions);
    desc_dma->CompleteDMA(kIODMACommandCompleteDMANoOptions);
    OSSafeReleaseNULL(data_dma);
    OSSafeReleaseNULL(desc_dma);

    // --- compare
    res->step = 3;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t expect = static_cast<uint32_t>(seed * 2654435761u) ^ (i * 0x9e3779b9u) ^ i;
      if (words[i] != expect) {
        if (!res->mismatches) {
          res->first_bad_index = i;
          res->first_bad_expected = expect;
          res->first_bad_actual = words[i];
        }
        ++res->mismatches;
      }
    }
    if (res->mismatches) {
      ret = kIOReturnIOError;
      break;
    }
    res->step = 4;
  } while (false);

  if (data_dma) { data_dma->CompleteDMA(kIODMACommandCompleteDMANoOptions); OSSafeReleaseNULL(data_dma); }
  if (desc_dma) { desc_dma->CompleteDMA(kIODMACommandCompleteDMANoOptions); OSSafeReleaseNULL(desc_dma); }
  OSSafeReleaseNULL(data);
  OSSafeReleaseNULL(descs);

  LOG("DmaSelfTest: card 0x%llx len %llu segs %u step %u status 0x%x h2c 0x%x/%u %u us c2h 0x%x/%u %u us mismatches %u",
      card_addr, length, res->segments, res->step, ret, res->h2c_status, res->h2c_done, res->h2c_us,
      res->c2h_status, res->c2h_done, res->c2h_us, res->mismatches);
  return res->status = ret;
}

// Generic polled transfer: map the host descriptor through the DART in
// batches of up to 32 segments, one XDMA descriptor per segment, chained.
// Build the descriptor ring and the two DMA commands once. Caller holds
// dma_lock. They stay for the life of the driver: the ring is the dext's own
// memory and one transfer runs at a time.
// Where the AXI Quad SPI sits differs per shell and is not in the default
// table: an Innova2 puts it at 0x40000, a U50's partition metadata says
// 0x1f06000. Probe the candidates and accept one only if it reads like an idle
// controller - SPISR with TX and RX empty and not a filler value, SPICR with the
// reset bits. A dynamic-IP card can also have it installed through SET_PLATFORM,
// which takes precedence.
static const uint32_t kFlashCandidates[] = { 0x40000, 0x1f06000 };

static bool
spi_plausible(uint32_t cr, uint32_t sr)
{
  if (is_filler(cr) || is_filler(sr))
    return false;
  // SPISR reset value is 0xa5: RX empty and TX empty both set.
  if ((sr & (XRT_SPI_SR_RX_EMPTY | XRT_SPI_SR_TX_EMPTY)) != (XRT_SPI_SR_RX_EMPTY | XRT_SPI_SR_TX_EMPTY))
    return false;
  // SPICR after reset has manual slave select and transaction inhibit set.
  return (cr & (XRT_SPI_CR_MANUAL_SS | XRT_SPI_CR_INHIBIT)) != 0;
}

static void
probe_flash_block(XrtUser_IVars* ivars, IOPCIDevice* pci)
{
  if (ivars->plat.blk[XRT_BLK_FLASH].size || !pci)
    return;
  uint8_t bar = 0, type = 0;
  uint64_t size = 0;
  if (pci->GetBARInfo(0, &bar, &size, &type) != kIOReturnSuccess)
    return;
  for (uint32_t off : kFlashCandidates) {
    if (off + 0x80 > size)
      continue;
    uint32_t cr = 0, sr = 0;
    pci->MemoryRead32(bar, off + XRT_SPI_CR, &cr);
    pci->MemoryRead32(bar, off + XRT_SPI_SR, &sr);
    if (!spi_plausible(cr, sr))
      continue;
    ivars->plat.blk[XRT_BLK_FLASH] = { off, 0x1000, 0, 0, {} };
    // Soft-reset the core once, as the known-good reference does in its
    // constructor. The core can be left configured by whatever ran before - this
    // is a dual-quad instance (C_DUAL_QUAD_MODE=1, two devices as x8), and its
    // per-device state is not something a CR write clears. SRR resets the
    // controller only; it cannot touch a flash byte.
    pci->MemoryWrite32(bar, off + XRT_SPI_SRR, XRT_SPI_SRR_RESET);
    IOSleep(10);
    pci->MemoryWrite32(bar, off + XRT_SPI_CR,
                       XRT_SPI_CR_MASTER | XRT_SPI_CR_MANUAL_SS | XRT_SPI_CR_INHIBIT
                       | XRT_SPI_CR_TX_RESET | XRT_SPI_CR_RX_RESET);
    pci->MemoryWrite32(bar, off + XRT_SPI_SSR, 0xffffffffu);
    // Leave the core DISABLED. This used to end with SPE set, which serves no
    // purpose - every transaction writes its own CR - and the core holds
    // STARTUPE3, so an enabled core left behind by the probe is the same hazard
    // SpiTransfer's teardown exists to avoid: the next bitstream download fails
    // after exactly one FIFO of words. LoadBitstream clears it defensively, but
    // not creating the condition is better than relying on that.
    pci->MemoryWrite32(bar, off + XRT_SPI_CR, 0);
    LOG("flash: AXI Quad SPI at BAR0+0x%x (SPICR 0x%x, SPISR 0x%x), core reset", off, cr, sr);
    return;
  }
  LOG("flash: no AXI Quad SPI found at any known offset");
}

// The XDMA config BAR identifies itself: channel H2C0's identity register at
// offset 0 reads 0x1fc0xxxx and C2H0's at 0x1000 reads 0x1fc1xxxx. Probing for
// that beats trusting a default, which is BAR1 for the XDMA 5.x shells and
// absent on others. Called once; the answer is fixed for a given bitstream, and
// it is re-probed wherever the other DMA caches are invalidated.
static void
probe_xdma_bar(XrtUser_IVars* ivars, IOPCIDevice* pci)
{
  if (ivars->xdma_bar_probed || !pci)
    return;
  ivars->xdma_bar_probed = true;
  const uint8_t current = xdma_bar(ivars);
  for (uint8_t cand = 0; cand < 6; ++cand) {
    uint8_t idx = 0, type = 0;
    uint64_t size = 0;
    if (pci->GetBARInfo(cand, &idx, &size, &type) != kIOReturnSuccess || size < 0x6000)
      continue;
    uint32_t h2c = 0, c2h = 0;
    pci->MemoryRead32(idx, XRT_XDMA_H2C(0), &h2c);
    pci->MemoryRead32(idx, XRT_XDMA_C2H(0), &c2h);
    if ((h2c >> 16) != 0x1fc0u || (c2h >> 16) != 0x1fc1u)
      continue;
    if (cand != current) {
      LOG("XDMA engines answer on BAR %u, not the assumed BAR %u; using BAR %u",
          cand, current, cand);
      ivars->plat.blk[XRT_BLK_XDMA].bar = cand;
      ivars->plat.blk[XRT_BLK_XDMA].size = 0x40000;
    }
    return;
  }
  LOG("no BAR answers as an XDMA engine; leaving the assumed BAR %u", current);
}

kern_return_t
XrtUser::BounceAcquire(uint64_t length, IOMemoryDescriptor** md, uint64_t* va)
{
  if (!ivars->bounce_lock)
    return kIOReturnNotReady;
  IOLockLock(ivars->bounce_lock);
  // Grow on demand; in practice length is at most the 4 KiB that IOKit copies
  // inline, but nothing here depends on that.
  if (ivars->bounce && ivars->bounce_len < length) {
    OSSafeReleaseNULL(ivars->bounce);
    ivars->bounce_va = ivars->bounce_len = 0;
  }
  if (!ivars->bounce) {
    uint64_t want = length < kDmaDescBytes ? kDmaDescBytes : length;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, want, 16384,
                                                         &ivars->bounce);
    if (ret != kIOReturnSuccess) {
      IOLockUnlock(ivars->bounce_lock);
      return ret;
    }
    IOAddressSegment seg = {};
    ivars->bounce->SetLength(want);
    if ((ret = ivars->bounce->GetAddressRange(&seg)) != kIOReturnSuccess) {
      OSSafeReleaseNULL(ivars->bounce);
      IOLockUnlock(ivars->bounce_lock);
      return ret;
    }
    ivars->bounce_va = seg.address;
    ivars->bounce_len = want;
    LOG("bounce buffer: %llu bytes, made once", want);
  }
  ivars->bounce->SetLength(length);
  *md = ivars->bounce;
  *va = ivars->bounce_va;
  return kIOReturnSuccess;
}

void
XrtUser::BounceRelease()
{
  if (ivars->bounce_lock)
    IOLockUnlock(ivars->bounce_lock);
}

// Drop the cached DMA mappings so DmaSetup() builds them again.
//
// The descriptor ring is prepared once and left prepared, so one IOVA serves
// every transfer (DmaSetup). A PCIe reset ends the device's old address
// translations, and the engine then fetches descriptors from an IOVA that no
// longer resolves: it goes busy and stays busy with 0 descriptors done, which
// looks exactly like a dead DMA target. The engine needs another reset to come
// back, so one stale mapping costs every later transfer, not just the first.
// Call this wherever a reset may have happened. dma_lock must be held.
static void
drop_dma_mappings(XrtUser_IVars* ivars)
{
  if (ivars->dma_desc_cmd)
    ivars->dma_desc_cmd->CompleteDMA(kIODMACommandCompleteDMANoOptions);
  OSSafeReleaseNULL(ivars->dma_desc_cmd);
  OSSafeReleaseNULL(ivars->dma_host_cmd);
  ivars->dma_desc_iova = 0;
}

kern_return_t
XrtUser::DmaSetup()
{
  if (ivars->dma_desc_cmd && ivars->dma_host_cmd)
    return kIOReturnSuccess;
  auto pci = ivars->pci;
  if (!pci)
    return kIOReturnNoDevice;
  IODMACommandSpecification spec = {};
  spec.maxAddressBits = 64;
  kern_return_t ret = kIOReturnSuccess;
  if (!ivars->dma_descs) {
    if ((ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, kDmaDescBytes, 16384,
                                                &ivars->dma_descs)) != kIOReturnSuccess)
      return ret;
    ivars->dma_descs->SetLength(kDmaDescBytes);
    IOAddressSegment va = {};
    if ((ret = ivars->dma_descs->GetAddressRange(&va)) != kIOReturnSuccess)
      return ret;
    ivars->dma_desc_va = va.address;
  }
  if (!ivars->dma_desc_cmd) {
    IOAddressSegment seg[32] = {};
    uint32_t nseg = 32;
    uint64_t fl = 0;
    if ((ret = IODMACommand::Create(pci, kIODMACommandCreateNoOptions, &spec,
                                    &ivars->dma_desc_cmd)) != kIOReturnSuccess)
      return ret;
    // Prepared once and left prepared, so the ring keeps one IOVA for good.
    if ((ret = ivars->dma_desc_cmd->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                                 ivars->dma_descs, 0, kDmaDescBytes,
                                                 &fl, &nseg, seg)) != kIOReturnSuccess)
      return ret;
    if (nseg != 1)
      return kIOReturnUnsupported;
    ivars->dma_desc_iova = seg[0].address;
  }
  if (!ivars->dma_host_cmd)
    ret = IODMACommand::Create(pci, kIODMACommandCreateNoOptions, &spec, &ivars->dma_host_cmd);
  if (ret == kIOReturnSuccess)
    LOG("DMA descriptor ring ready at IOVA 0x%llx, reused for every transfer", ivars->dma_desc_iova);
  return ret;
}

kern_return_t
XrtUser::DmaTransfer(IOMemoryDescriptor* host, uint64_t host_offset, uint64_t length,
                     uint64_t card_addr, bool to_card, uint32_t* engine_status)
{
  auto pci = ivars->pci;
  if (!pci || !host)
    return kIOReturnBadArgument;
  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 1)
    return kIOReturnNotPermitted;
  if (length == 0 || card_addr < ivars->card_min || card_addr + length > ivars->card_max
      || card_addr + length < card_addr)
    return kIOReturnBadArgument;
  probe_xdma_bar(ivars, pci);
  uint8_t bar = 0, type = 0;
  uint64_t bar_size = 0;
  if (pci->GetBARInfo(xdma_bar(ivars), &bar, &bar_size, &type) != kIOReturnSuccess || bar_size < 0x6000)
    return kIOReturnNoResources;
  auto rd = [&](uint32_t off) { uint32_t v = 0; pci->MemoryRead32(bar, off, &v); return v; };
  auto wr = [&](uint32_t off, uint32_t v) { pci->MemoryWrite32(bar, off, v); };
  // XDMA descriptor length is 28 bits; split longer segments (libxdma
  // caps at XDMA_DESC_BLEN_MAX). Keep chunks page aligned.
  constexpr uint64_t kMaxDescBytes = 0x0ffff000;
  constexpr uint32_t kMaxDescs = 16384 / sizeof(xdma_desc);

  const uint64_t t_enter = prof_now();
  uint64_t t_lock = 0, t_chan = 0, t_cfg = 0, t_setup = 0;
  uint64_t ns_prep = 0, ns_build = 0, ns_wait = 0;
  IOLockLock(ivars->dma_lock);
  t_lock = prof_now();
  if (ivars->asleep) {
    IOLockUnlock(ivars->dma_lock);
    return kIOReturnOffline;
  }
  if (ivars->partition_held) {
    IOLockUnlock(ivars->dma_lock);
    return kIOReturnBusy;      // the partition is being reprogrammed
  }
  // First idle channel in the requested direction (the shell has two).
  uint32_t eng = 0xffffffff;
  uint32_t eng_idx = 0;                                  // libxdma engine number
  if (!ivars->chan_known) {
    // One-off: which channels exist. Two reads per direction, once per dext.
    ivars->h2c_mask = ivars->c2h_mask = 0;
    for (uint32_t ch = 0; ch < 2; ++ch) {
      if ((rd(XRT_XDMA_H2C(ch)) >> 16) == 0x1fc0u)
        ivars->h2c_mask |= 1u << ch;
      if ((rd(XRT_XDMA_C2H(ch)) >> 16) == 0x1fc1u)
        ivars->c2h_mask |= 1u << ch;
    }
    ivars->chan_known = true;
    LOG("dma channels: H2C mask 0x%x, C2H mask 0x%x", ivars->h2c_mask, ivars->c2h_mask);
  }
  const uint32_t mask = to_card ? ivars->h2c_mask : ivars->c2h_mask;
  for (uint32_t ch = 0; ch < 2 && eng == 0xffffffff; ++ch) {
    if (!(mask & (1u << ch)))
      continue;
    uint32_t e = to_card ? XRT_XDMA_H2C(ch) : XRT_XDMA_C2H(ch);
    if (!(rd(e + kXdmaStatus) & kStatBusy)) {
      eng = e;
      eng_idx = (to_card ? 0 : 2) + ch;
    }
  }
  if (eng == 0xffffffff) {
    IOLockUnlock(ivars->dma_lock);
    LOG("DmaTransfer: no idle %s channel", to_card ? "H2C" : "C2H");
    return kIOReturnBusy;
  }
  kern_return_t ret = kIOReturnSuccess;
  uint64_t fl = 0;
  uint32_t status = 0;
  t_chan = prof_now();

  do {
    if (!ivars->cmd_enabled) {
      uint16_t cmd = 0;
      pci->ConfigurationRead16(kCfgCommand, &cmd);
      if ((cmd & (kCmdMemorySpace | kCmdBusMaster)) != (kCmdMemorySpace | kCmdBusMaster))
        pci->ConfigurationWrite16(kCfgCommand, cmd | kCmdMemorySpace | kCmdBusMaster);
      ivars->cmd_enabled = true;
    }
    t_cfg = prof_now();

    // The ring and the DMA commands are made once, not per transfer.
    if ((ret = DmaSetup()) != kIOReturnSuccess)
      break;
    IODMACommand* host_dma = ivars->dma_host_cmd;
    auto d = reinterpret_cast<volatile xdma_desc*>(ivars->dma_desc_va);
    t_setup = prof_now();

    uint64_t done_bytes = 0;
    while (done_bytes < length && ret == kIOReturnSuccess) {
      IOAddressSegment seg[32] = {};
      uint32_t nseg = 32;
      fl = 0;
      const uint64_t t_p0 = prof_now();
      ret = host_dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, host, host_offset + done_bytes,
                                    length - done_bytes, &fl, &nseg, seg);
      const uint64_t t_prep_end = prof_now();
      ns_prep += t_prep_end - t_p0;
      if (ret != kIOReturnSuccess)
        break;
      if (nseg == 0) { ret = kIOReturnDMAError; host_dma->CompleteDMA(kIODMACommandCompleteDMANoOptions); break; }
      // Build descriptors, splitting segments longer than kMaxDescBytes.
      uint64_t batch = 0;
      uint32_t ndesc = 0;
      for (uint32_t i = 0; i < nseg && ndesc < kMaxDescs; ++i) {
        uint64_t off = 0;
        while (off < seg[i].length && ndesc < kMaxDescs) {
          uint64_t chunk = seg[i].length - off;
          if (chunk > kMaxDescBytes)
            chunk = kMaxDescBytes;
          uint64_t card = card_addr + done_bytes + batch;
          uint64_t host = seg[i].address + off;
          uint64_t src = to_card ? host : card, dst = to_card ? card : host;
          uint64_t next = ivars->dma_desc_iova + (ndesc + 1) * sizeof(xdma_desc);
          d[ndesc].control = kDescMagic;
          d[ndesc].bytes = static_cast<uint32_t>(chunk);
          d[ndesc].src_lo = static_cast<uint32_t>(src);
          d[ndesc].src_hi = static_cast<uint32_t>(src >> 32);
          d[ndesc].dst_lo = static_cast<uint32_t>(dst);
          d[ndesc].dst_hi = static_cast<uint32_t>(dst >> 32);
          d[ndesc].next_lo = static_cast<uint32_t>(next);
          d[ndesc].next_hi = static_cast<uint32_t>(next >> 32);
          ++ndesc;
          off += chunk;
          batch += chunk;
        }
        if (off < seg[i].length)
          break;                                       // descriptor page full; rest next batch
      }
      // Terminate the chain on the last descriptor.
      d[ndesc - 1].control = kDescMagic | kDescStopped | kDescCompleted | kDescEop;
      d[ndesc - 1].next_lo = 0;
      d[ndesc - 1].next_hi = 0;
      __atomic_thread_fence(__ATOMIC_SEQ_CST);

      const uint64_t t_b1 = prof_now();
      ns_build += t_b1 - t_prep_end;
      const bool use_irq = ivars->irq_vectors == kXdmaEngines && ivars->irq_enabled
                           && !ivars->irq_dead;
      uint32_t ctrl = kCtrlStart;
      (void)rd(eng + kXdmaStatusRC);
      if (use_irq) {
        // Drop a wakeup left over from an earlier batch, then (re)program
        // the IRQ block every time: a reset or sleep clears it.
        auto iv = ivars;
        ivars->irq_queue->DispatchSync(^{ iv->irq_fired = false; });
        wr(XRT_XDMA_IRQ + kIrqChannelVector, kIrqChannelVectorMap);
        wr(XRT_XDMA_IRQ + kIrqChannelEnableW1S, (1u << kXdmaEngines) - 1);
        wr(eng + kXdmaIrqMask, kIeCompletion | kIeErrors);
        ctrl |= kIeCompletion;
      }
      wr(eng + XRT_XDMA_SGDMA + kSgFirstLo, static_cast<uint32_t>(ivars->dma_desc_iova));
      wr(eng + XRT_XDMA_SGDMA + kSgFirstHi, static_cast<uint32_t>(ivars->dma_desc_iova >> 32));
      wr(eng + XRT_XDMA_SGDMA + kSgAdjacent, 0);
      const uint64_t t_w0 = prof_now();
      wr(eng + kXdmaCtrl, ctrl);
      ret = kIOReturnTimeout;
      bool woke = false;
      const uint64_t t_end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 5000000000ull;  // 5 s per batch
      // An MMIO read over Thunderbolt costs ~3.2 us, so the read itself is the
      // poll interval and there is nothing cheaper to check with. Two
      // consequences. First, only the status register is read while the engine
      // is busy; the completed-descriptor count is read once it is not, which
      // halves the reads on every waiting iteration. Second, the first few
      // polls carry no IODelay at all: a small transfer finishes inside the old
      // 10 us delay and then waited for it, which is where the ~29 us at 4 KiB
      // came from. After that the delay comes back, so a long transfer is not
      // polled hard - those reads share the link with the DMA.
      constexpr uint32_t kSpinPolls = 8;
      uint32_t polls = 0;
      for (;;) {
        status = rd(eng + kXdmaStatus);
        if (!(status & kStatBusy)) {
          uint32_t done = rd(eng + kXdmaDone);
          if (done >= ndesc) { ret = kIOReturnSuccess; break; }
        }
        if (status & ~kStatOk) { ret = kIOReturnIOError; break; }
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) > t_end)
          break;
        if (use_irq) {
          // Sleep on the interrupt queue, which releases it so DmaInterrupt
          // can run there and wake us. The flag closes the race with an
          // interrupt that arrives before the sleep. A lost interrupt costs
          // at most one slice; the status check above then finishes the
          // batch by polling.
          auto iv = ivars;
          auto q = ivars->irq_queue;
          __block bool got = false;
          q->DispatchSync(^{
            if (!iv->irq_fired)
              q->SleepWithDeadline(&iv->irq_fired, kIOTimerClockUptimeRaw,
                                   clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + kIrqSleepNs);
            got = iv->irq_fired;
            iv->irq_fired = false;
          });
          woke |= got;
        }
        else if (++polls > kSpinPolls)
          IODelay(10);
      }
      ns_wait += prof_now() - t_w0;
      wr(eng + kXdmaCtrlW1C, kCtrlRun);
      if (use_irq)
        wr(eng + kXdmaIrqMask, 0);
      if (ret == kIOReturnSuccess)
        ++(woke ? ivars->dma_irq_batches : ivars->dma_poll_batches);
      if (use_irq && ret == kIOReturnSuccess) {
        if (woke)
          ivars->irq_misses = 0;
        else if (++ivars->irq_misses >= kIrqGiveUpAfter) {
          ivars->irq_dead = true;
          LOG("irq: %u transfers completed with no interrupt; polling from now on."
              " The vectors are configured but nothing is delivered - on the U50,"
              " MSI-X is left disabled in config space.", ivars->irq_misses);
        }
      }

      uint32_t rc = rd(eng + kXdmaStatusRC);
      if (ret == kIOReturnSuccess && (rc & ~kStatOk))
        ret = kIOReturnIOError;
      status |= rc;
      host_dma->CompleteDMA(kIODMACommandCompleteDMANoOptions);
      if (ret == kIOReturnSuccess)
        done_bytes += batch;
    }
    if (ret != kIOReturnSuccess)
      LOG("DmaTransfer %s engine 0x%x card 0x%llx len %llu failed 0x%x after %llu bytes, engine status 0x%x",
          to_card ? "H2C" : "C2H", eng, card_addr, length, ret, done_bytes, status);
  } while (false);

  // The ring, its DMA command and the host DMA command are the driver's and
  // stay prepared for the next transfer; only the host mapping is completed,
  // which happens per batch in the loop above.
  // Stage profile. Everything after the wait - CompleteDMA, the status reads
  // and the unlock - lands in the gap between the stage sum and the whole
  // call, which is why both are logged.
  const uint64_t t_done = prof_now();
  ivars->prof_ns[0] += t_lock - t_enter;                       // entry checks + lock
  ivars->prof_ns[1] += t_chan - t_lock;                        // idle-channel search
  ivars->prof_ns[2] += t_cfg ? t_cfg - t_chan : 0;              // ConfigurationRead16 alone
  ivars->prof_ns[6] += (t_setup && t_cfg) ? t_setup - t_cfg : 0; // DmaSetup + the rest
  ivars->prof_ns[3] += ns_prep;                                // PrepareForDMA
  ivars->prof_ns[4] += ns_build;                               // descriptor build
  ivars->prof_ns[5] += ns_wait;                                // doorbell + completion
  if (++ivars->prof_n >= kProfEvery) {
    const uint64_t n = ivars->prof_n;
    uint64_t total = 0;
    for (int i = 0; i < 7; ++i)
      total += ivars->prof_ns[i];
    LOG("dma profile over %llu transfers, ns each: lock %llu chan %llu cfgread %llu"
        " setup %llu prep %llu build %llu wait %llu | stages %llu, last whole call %llu",
        n, ivars->prof_ns[0] / n, ivars->prof_ns[1] / n, ivars->prof_ns[2] / n,
        ivars->prof_ns[6] / n, ivars->prof_ns[3] / n, ivars->prof_ns[4] / n,
        ivars->prof_ns[5] / n, total / n, (t_done - t_enter));
    for (int i = 0; i < 7; ++i)
      ivars->prof_ns[i] = 0;
    ivars->prof_n = 0;
  }
  IOLockUnlock(ivars->dma_lock);
  if (engine_status)
    *engine_status = status;
  return ret;
}

// One AXI Quad SPI transaction. SPI is symmetric - every byte clocked out clocks
// one in - so a caller that wants N bytes back sends the command followed by N
// dummy bytes and reads the tail of what comes back. Keeping the register
// sequence here means user space never names an address, so this selector cannot
// reach anything but the flash controller, whatever the caller asks for.
kern_return_t
XrtUser::SpiTransfer(uint32_t slave, const void* tx, uint32_t tx_len,
                     void* rx, uint32_t rx_cap, void* out)
{
  auto res = static_cast<xrt_dext_spi_result*>(out);
  if (res)
    memset(res, 0, sizeof(*res));
  auto fail = [&](kern_return_t st) { if (res) res->status = st; return st; };

  auto pci = ivars->pci;
  if (!pci || !tx || !rx)
    return fail(kIOReturnBadArgument);
  if (ivars->asleep)
    return fail(kIOReturnNotReady);       // the card can come back reset
  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 0)
    return fail(kIOReturnNotPermitted);       // the flash hangs off the management function
  if (tx_len == 0 || tx_len > XRT_SPI_MAX_XFER || rx_cap > XRT_SPI_MAX_XFER)
    return fail(kIOReturnBadArgument);
  if (slave > 1)
    return fail(kIOReturnBadArgument);        // two chips, striped as x8

  probe_flash_block(ivars, pci);
  if (!blk_present(ivars, XRT_BLK_FLASH))
    return fail(kIOReturnNoResources);
  const uint32_t base = blk_off(ivars, XRT_BLK_FLASH);
  uint8_t bar = 0, type = 0;
  uint64_t size = 0;
  if (pci->GetBARInfo(0, &bar, &size, &type) != kIOReturnSuccess || base + 0x80 > size)
    return fail(kIOReturnNoResources);

  auto rd = [&](uint32_t o) { uint32_t v = 0; pci->MemoryRead32(bar, base + o, &v); return v; };
  auto wr = [&](uint32_t o, uint32_t v) { pci->MemoryWrite32(bar, base + o, v); };
  auto txb = static_cast<const uint8_t*>(tx);
  auto rxb = static_cast<uint8_t*>(rx);

  IOLockLock(ivars->flash_lock);
  if (res)
    res->sr_before = rd(XRT_SPI_SR);

  const uint32_t idle = XRT_SPI_CR_MASTER | XRT_SPI_CR_MANUAL_SS | XRT_SPI_CR_INHIBIT | XRT_SPI_CR_SPE;
  // Reset the core when the target chip changes. This mirrors xbflash, whose
  // prepareXSpi() writes SRR once per chip rather than once per session; a
  // dual-quad core can hold per-device state that a CR write does not clear.
  if (ivars->flash_slave != slave) {
    wr(XRT_SPI_SRR, XRT_SPI_SRR_RESET);
    IOSleep(1);
    wr(XRT_SPI_CR, idle | XRT_SPI_CR_TX_RESET | XRT_SPI_CR_RX_RESET);
    wr(XRT_SPI_SSR, 0xffffffffu);
    wr(XRT_SPI_CR, idle);
    ivars->flash_slave = slave;
  }
  wr(XRT_SPI_CR, idle | XRT_SPI_CR_TX_RESET | XRT_SPI_CR_RX_RESET);   // empty both FIFOs
  wr(XRT_SPI_CR, idle);
  // Drain anything a previous transaction left behind. The FIFO reset above
  // should have done it; doing both is what the known-good reference does.
  for (int guard = 0; guard < XRT_SPI_MAX_XFER * 2 && !(rd(XRT_SPI_SR) & XRT_SPI_SR_RX_EMPTY); ++guard)
    (void)rd(XRT_SPI_DRR);

  for (uint32_t i = 0; i < tx_len; ++i)
    wr(XRT_SPI_DTR, txb[i]);

  // SPISSR is a full 32-bit, active-low field. Masking it to the two chips we
  // care about writes zeros to bits 2..31, which ASSERTS thirty other selects and
  // makes a quad-capable core raise Command_Error. Deassert is all ones.
  wr(XRT_SPI_SSR, ~(1u << slave));
  wr(XRT_SPI_CR, idle & ~XRT_SPI_CR_INHIBIT);                         // clock it out

  // An MMIO read is ~3 us here, so the read itself paces the wait; 256 bytes at
  // the controller's default divisor is well under a millisecond.
  const uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 500000000ull;  // 500 ms
  uint32_t sr = 0;
  const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  for (;;) {
    sr = rd(XRT_SPI_SR);
    if (sr & XRT_SPI_SR_TX_EMPTY)
      break;
    if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) > deadline) {
      // Tear down exactly as the success path does. Writing `idle` here would
      // leave SPE set - the core keeps holding STARTUPE3 and the next bitstream
      // download fails after one FIFO - and would leave flash_slave cached for a
      // transaction that did not complete.
      wr(XRT_SPI_SSR, 0xffffffffu);
      wr(XRT_SPI_CR, 0);
      ivars->flash_slave = 0xffffffffu;
      IOLockUnlock(ivars->flash_lock);
      LOG("flash: SPI transaction timed out, SPISR 0x%x", sr);
      return fail(kIOReturnTimeout);
    }
  }
  const uint64_t waited = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;

  wr(XRT_SPI_CR, idle);                                               // inhibit again
  wr(XRT_SPI_SSR, 0xffffffffu);                                       // deselect everything

  uint32_t got = 0;
  while (got < rx_cap && !(rd(XRT_SPI_SR) & XRT_SPI_SR_RX_EMPTY))
    rxb[got++] = static_cast<uint8_t>(rd(XRT_SPI_DRR) & 0xff);

  if (res) {
    res->sr_after = rd(XRT_SPI_SR);
    res->rx_bytes = got;
    res->slave = slave;
    res->waited_us = static_cast<uint32_t>(waited / 1000);
    res->status = kIOReturnSuccess;
  }
  // Release the core. The primary flash device is reached through the IP's
  // internal STARTUPE3, which is the same configuration primitive the ICAP
  // needs - so leaving SPE set here makes the next bitstream download fail after
  // exactly one FIFO of words with the ICAP status reading 0. Disable the core
  // between transactions and the two stop contending.
  wr(XRT_SPI_CR, 0);
  ivars->flash_slave = 0xffffffffu;    // re-prepare on the next transaction
  IOLockUnlock(ivars->flash_lock);
  return kIOReturnSuccess;
}

// Classify a flash opcode. The lists are xspi.cpp's own COMMAND_* defines; an
// opcode this does not know lands in OTHER, which is the safe answer for a tally
// but would NOT be a safe answer for a refusal - an enforcing version has to
// allowlist rather than denylist.
// Does this opcode carry an address immediately after itself, and how wide?
// Returns 0 for unaddressed. The width is a property of the opcode where the part
// defines one (0x13 and 0x12 are the 4-byte-address forms); otherwise it is the
// 3-byte form, which is what xspi.cpp's FOUR_BYTE_ADDRESSING = false selects.
static uint32_t
spi_addr_bytes(uint8_t op)
{
  switch (op) {
    case 0x13: case 0x12: case 0x21: case 0x5c: case 0xdc: return 4;
    case 0x03: case 0x0b: case 0x3b: case 0xbb: case 0x6b: case 0xeb:
    case 0x02: case 0x32: case 0x38:
    case 0x20: case 0x52: case 0xd8:                       return 3;
    default:                                               return 0;
  }
}

// Does this opcode change anything in a flash DEVICE? Not just content: the
// write-enable latch, the extended address register and the configuration
// registers all count. DRY_RUN suppresses every one of them, so a rehearsal
// touches no device state whatsoever - only the controller's own registers, which
// live in the FPGA.
//
// Suppressing 0x06 and 0xc5 is safe to do because upstream never verifies them:
// writeEnable() and writeRegister() check the CONTROLLER's TX-empty bit and never
// read the device back (xspi.cpp), so a suppressed one is indistinguishable from a
// successful one. And the shadow takes the extended address register's value from
// its own shadowed bytes rather than from the device, so addresses still decode
// correctly with the 0xc5 never having gone out.
static bool
spi_is_device_write(uint8_t op)
{
  switch (op) {
    case 0x06: case 0x04:                        // write enable / disable
    case 0x01: case 0x81: case 0xb1: case 0x61:  // status / configuration registers
    case 0xc5:                                   // extended address register
    case 0x50:                                   // clear flag status register
    case 0x66: case 0x99:                        // reset enable / reset
    case 0x98:                                   // gang block unlock
    case 0x02: case 0x32: case 0x38: case 0x12:  // page program
    case 0x20: case 0x52: case 0xd8: case 0x21: case 0xdc:  // erase
    case 0xc7: case 0x60:                        // bulk erase
      return true;
    default:
      return false;
  }
}

static uint32_t
spi_classify(uint8_t op)
{
  switch (op) {
    case 0x20: case 0x52: case 0xd8: return XRT_SPI_OP_ERASE;
    case 0x02: case 0x32: case 0x38: return XRT_SPI_OP_PROGRAM;
    case 0xc7: case 0x60:            return XRT_SPI_OP_BULK_ERASE;
    case 0xc5:                       return XRT_SPI_OP_EAR_WRITE;
    case 0x03: case 0x0b: case 0x3b: case 0xbb:
    case 0x6b: case 0xeb: case 0x9f: case 0x13: return XRT_SPI_OP_READ;
    default:                         return XRT_SPI_OP_OTHER;
  }
}

// Decode the shadowed command at the moment it becomes irreversible, record what
// it was, and say whether the launching write should actually be performed.
// Caller holds flash_lock.
//
#define SPI_LAUNCH_PROCEED   0u
#define SPI_LAUNCH_SUPPRESS  1u
#define SPI_LAUNCH_REFUSE    2u

// Returns what should happen to the launching write:
//   SPI_LAUNCH_PROCEED  perform it - the ordinary case
//   SPI_LAUNCH_SUPPRESS the opcode was already rewritten to a read (DRY_RUN), so
//                       performing it is harmless; the caller is told it worked
//   SPI_LAUNCH_REFUSE   do NOT perform it, and fail the call (ENFORCE)
static uint32_t
spi_audit_launch(XrtUser_IVars* ivars)
{
  auto& a = ivars->spi_audit;
  a.launches++;
  if (!ivars->spi_hdr_n) {
    // The FIFO was launched without this connection having pushed byte 0 - a
    // command left over across a reset, or a caller doing something the shadow
    // does not model. Counted, because a tally with silent holes is worse than a
    // tally that says it has holes.
    a.undecoded++;
    // Not knowing what a command is, is not a reason to let it through when the
    // whole point of the mode is that nothing should be written. ENFORCE takes the
    // opposite view deliberately: it guards a specific hazard and must not break
    // ordinary work it cannot classify, so an undecoded launch proceeds there.
    return ivars->spi_guard == XRT_SPI_GUARD_DRY_RUN ? SPI_LAUNCH_SUPPRESS
                                                    : SPI_LAUNCH_PROCEED;
  }

  const uint8_t op = ivars->spi_hdr[0];
  const uint32_t cls = spi_classify(op);
  auto& c = a.cls[cls];

  // Which chip. SSR is active low and this instance has two bits.
  for (uint32_t chip = 0; chip < 2; ++chip)
    if (!(ivars->spi_ssr & (1u << chip)))
      a.chip_mask |= (1u << chip);

  const uint32_t nbytes = spi_addr_bytes(op);
  // Reads are decoded as well as writes. It costs nothing, it makes the dry run
  // report which addresses were read as well as written, and it means the address
  // reconstruction can be exercised without sending a single write to a device.
  const bool addressed = nbytes != 0 && ivars->spi_hdr_n >= 1 + nbytes;
  uint32_t addr = 0;
  if (cls == XRT_SPI_OP_EAR_WRITE && ivars->spi_hdr_n >= 2) {
    // 0xc5 carries the new top nibble as its one payload byte. Record it here,
    // at launch, because that is when the device actually takes it.
    ivars->spi_ear = ivars->spi_hdr[1] & 0xf;
    ivars->spi_ear_valid = 1;
    a.ear = ivars->spi_ear;
    a.ear_valid = 1;
  } else if (addressed) {
    a.addr_bytes_seen |= (nbytes == 4) ? 2u : 1u;
    if (nbytes == 4)
      addr = (uint32_t)ivars->spi_hdr[1] << 24 | (uint32_t)ivars->spi_hdr[2] << 16
           | (uint32_t)ivars->spi_hdr[3] << 8  | (uint32_t)ivars->spi_hdr[4];
    else
      // Only 24 bits are on the wire; bits 27:24 come from the extended address
      // register, set by a SEPARATE earlier 0xc5 transaction. Without that the top
      // nibble reads as 0 and sector 1 would be indistinguishable from sector 0,
      // which is why the shadow tracks it.
      addr = (ivars->spi_ear_valid ? (ivars->spi_ear & 0xfu) << 24 : 0u)
           | (uint32_t)ivars->spi_hdr[1] << 16 | (uint32_t)ivars->spi_hdr[2] << 8
           | (uint32_t)ivars->spi_hdr[3];
  }

  if (addressed) {
    if (!c.count || addr < c.min_addr) c.min_addr = addr;
    if (!c.count || addr > c.max_addr) c.max_addr = addr;
    if (a.first_write_n < 8
        && (cls == XRT_SPI_OP_ERASE || cls == XRT_SPI_OP_PROGRAM)) {
      a.first_write_addr[a.first_write_n] = addr;
      a.first_write_op[a.first_write_n] = op;
      a.first_write_n++;
    }
  }
  c.count++;
  c.last_opcode = op;

  // Log the rare and consequential ones only. An erase per 256 KiB sector and one
  // 0xc5 per sector change is a few hundred lines for a whole image; logging every
  // page program would be hundreds of thousands, which is how a useful log becomes
  // an unreadable one. The tally above covers the programs.
  if (cls == XRT_SPI_OP_BULK_ERASE)
    LOG("flash AUDIT: BULK ERASE (opcode 0x%{public}02x) - the entire part, no address. "
        "NOT refused: the shadow is log-only", op);
  else if (cls == XRT_SPI_OP_ERASE)
    LOG("flash AUDIT: erase 0x%{public}02x at chip-local 0x%{public}08x (ear %{public}s0x%{public}x, %{public}u bytes)",
        op, addr, a.ear_valid ? "" : "unset ", a.ear, ivars->spi_tx_n);
  else if (cls == XRT_SPI_OP_EAR_WRITE)
    LOG("flash AUDIT: extended address register <- 0x%{public}x", ivars->spi_ear);
  else if (cls == XRT_SPI_OP_PROGRAM && a.cls[XRT_SPI_OP_PROGRAM].count <= 8)
    LOG("flash AUDIT: program 0x%{public}02x at chip-local 0x%{public}08x (%{public}u bytes)",
        op, addr, ivars->spi_tx_n);
  else if (cls == XRT_SPI_OP_READ && addressed && a.cls[XRT_SPI_OP_READ].count <= 8)
    LOG("flash AUDIT: read 0x%{public}02x at chip-local 0x%{public}08x (%{public}u bytes)",
        op, addr, ivars->spi_tx_n);

  // In DRY_RUN suppress every command that would change device state, not merely
  // the destructive ones. The stricter rule costs nothing (upstream verifies none
  // of them) and buys a rehearsal that leaves the flash chips bit-for-bit and
  // register-for-register as they were. Reads and status polls pass through, which
  // is what keeps the caller's state machine on the same path as a real run.
  // --- the policy, and it is now one rule: a BULK ERASE is a violation. Opcode 0xc7
  // takes no address and takes out the whole part, and nothing legitimate issues one -
  // upstream's bulkErase() is defined and never called. So this costs nothing on any
  // card, which is the test a rule has to pass to be on by default.
  //
  // There was an address rule here - refuse erase/program below a ceiling, later a
  // configurable range - and it was removed. Linux guards no addresses at all; the
  // golden image is at 0 on the AXI-QSPI parts and HIGH on the QSPI-PS parts; and even
  // on one card the legitimate addresses depend on the image, so a range that fits one
  // flow refuses another. Evaluated in EVERY mode, so a DRY_RUN still reports it.
  if (cls == XRT_SPI_OP_BULK_ERASE) {
    a.refused++;
    a.last_refused_addr = addr;
    a.last_refused_op = op;
    if (ivars->spi_guard == XRT_SPI_GUARD_ENFORCE) {
      LOG("flash REFUSED: bulk erase (opcode 0x%{public}02x) - no address, whole part, "
          "factory image included - not sent", op);
      return SPI_LAUNCH_REFUSE;
    }
    LOG("flash AUDIT: bulk erase 0x%{public}02x WOULD be refused if enforcing", op);
  }

  if (ivars->spi_substituted) {
    a.suppressed++;
    ivars->spi_substituted = false;
    if (a.suppressed <= 8 || cls == XRT_SPI_OP_BULK_ERASE)
      LOG("flash AUDIT: SUPPRESSED - the opcode was rewritten to a read on its way "
          "into the FIFO, so this command did not reach the device");
    return SPI_LAUNCH_SUPPRESS;
  }
  return SPI_LAUNCH_PROCEED;
}

static void
spi_shadow_clear(XrtUser_IVars* ivars)
{
  ivars->spi_hdr_n = 0;
  ivars->spi_tx_n = 0;
  ivars->spi_drr_n = 0;
  ivars->spi_substituted = false;
}

// One 32-bit access to one AXI Quad SPI register.
//
// SpiTransfer above keeps the whole register sequence in the driver, so user
// space never names an address. This does the opposite: the caller names an
// offset, and the driver's job is to bound it. It exists so the upstream
// xbflash.qspi programming path (xspi.cpp) can be reused unchanged - erase and
// program sequencing is the one flash operation where a mistake costs a card,
// and upstream's has been run against these parts for years. The bound is the
// flash block itself: 4-byte aligned, below XRT_SPI_REG_WINDOW, and offset from
// the base the probe found rather than from anything the caller supplies. The
// AXI Quad SPI core is all there is in that window.
kern_return_t
XrtUser::SpiReg(uint32_t reg, uint32_t value, bool write, uint32_t* out)
{
  if (out)
    *out = 0;

  auto pci = ivars->pci;
  if (!pci)
    return kIOReturnNoDevice;
  if (ivars->asleep)
    return kIOReturnNotReady;             // the card can come back reset; see SetPowerState
  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 0)
    return kIOReturnNotPermitted;         // the flash hangs off the management function
  // An allowlist, not a range: see XRT_SPI_REG_ALLOWED.
  static const uint32_t allowed[] = XRT_SPI_REG_ALLOWED;
  bool ok = false;
  for (uint32_t a : allowed)
    if (reg == a) { ok = true; break; }
  if (!ok)
    return kIOReturnBadArgument;

  probe_flash_block(ivars, pci);
  if (!blk_present(ivars, XRT_BLK_FLASH))
    return kIOReturnNoResources;
  const uint32_t base = blk_off(ivars, XRT_BLK_FLASH);
  uint8_t bar = 0, type = 0;
  uint64_t size = 0;
  if (pci->GetBARInfo(0, &bar, &size, &type) != kIOReturnSuccess
      || static_cast<uint64_t>(base) + 0x80 > size)   // 64-bit: base is a truncated u32
    return kIOReturnNoResources;

  IOLockLock(ivars->flash_lock);
  uint32_t wr_value = value;              // what actually reaches the register
  if (write) {
    // --- the TX-stream shadow. Everything here happens BEFORE the write, because
    // the SPICR write that clears TRANS_INHIBIT is what starts the transfer: after
    // it, the command is on its way to the device. This is therefore also the only
    // place a refusal could go, and an enforcing version would return from here
    // without performing the write. Today it only records.
    switch (reg) {
      case XRT_SPI_DTR: {
        const uint8_t byte = static_cast<uint8_t>(value & 0xff);
        const bool first = ivars->spi_hdr_n == 0;
        // Byte 0 is the opcode, the next few are the address. The payload is not
        // kept: it cannot change what the command means.
        if (ivars->spi_hdr_n < sizeof(ivars->spi_hdr))
          ivars->spi_hdr[ivars->spi_hdr_n++] = byte;
        if (ivars->spi_tx_n < 0xffffffffu)
          ivars->spi_tx_n++;
        // SUPPRESSION, and it belongs exactly here: rewrite the OPCODE as it is
        // written, turning the command into a read of the same length at the same
        // address. Everything else the caller queues - address bytes, payload - goes
        // through untouched, so the transfer has the caller's own length and timing
        // and the byte counts are right by construction.
        //
        // Three earlier designs tried to suppress at the LAUNCH instead, and each
        // broke on the same fact: SPI is symmetric and finalTransfer() decrements its
        // byte count once per byte RECEIVED, so anything that changes how many bytes
        // come back hangs the caller for ever. Withholding the launch returned none;
        // deasserting every slave select returned none (the core will not clock with
        // no slave selected); and rebuilding the FIFO returned one byte of two,
        // because the caller re-asserts TRANS_INHIBIT as soon as TX drains and
        // truncates whatever is still shifting. Changing one byte on the way in
        // avoids all of it - there is nothing left to get wrong about counts.
        if (first && ivars->spi_guard == XRT_SPI_GUARD_DRY_RUN
            && spi_is_device_write(byte)) {
          const uint32_t nb = spi_addr_bytes(byte);
          wr_value = (nb == 4) ? 0x13u        // READ, 4-byte address
                   : (nb == 3) ? 0x03u        // READ, 3-byte address
                               : 0x05u;       // read status register
          ivars->spi_substituted = true;
        }
        break;
      }
      case XRT_SPI_SSR:
        ivars->spi_ssr = value;
        break;
      case XRT_SPI_SRR:
        spi_shadow_clear(ivars);     // the core is being reset; so is the shadow
        break;
      case XRT_SPI_CR: {
        // Read the live value rather than caching one: the caller does its own
        // read-modify-write of this register, and a cached copy that drifts would
        // mis-detect the launch. One MMIO read against a 14 us call floor.
        uint32_t cur = 0;
        pci->MemoryRead32(bar, base + XRT_SPI_CR, &cur);
        const bool launching = (cur & XRT_SPI_CR_INHIBIT) && !(value & XRT_SPI_CR_INHIBIT);
        if (launching) {
          // The substitution below needs the command header and length, which
          // spi_audit_launch's clear discards, so copy them first.
          const uint32_t sent = ivars->spi_tx_n;
          const uint8_t op = ivars->spi_hdr_n ? ivars->spi_hdr[0] : 0;
          memcpy(ivars->spi_hdr_saved, ivars->spi_hdr, sizeof(ivars->spi_hdr_saved));
          const uint32_t verdict = spi_audit_launch(ivars);
          spi_shadow_clear(ivars);   // the next command starts from empty
          if (verdict == SPI_LAUNCH_REFUSE) {
            // Do NOT launch, and empty the FIFOs so the queued command cannot be
            // launched by a later write either. Then fail the call: upstream turns a
            // register-write failure into an exception and aborts, which is what
            // should happen to an operation heading for the factory image. Unlike
            // DRY_RUN there is no reason to let the caller believe it worked - the
            // point here is to stop, not to rehearse.
            pci->MemoryWrite32(bar, base + XRT_SPI_CR,
                               (cur | XRT_SPI_CR_INHIBIT)
                               | XRT_SPI_CR_TX_RESET | XRT_SPI_CR_RX_RESET);
            pci->MemoryWrite32(bar, base + XRT_SPI_SSR, 0xffffffffu);
            ivars->spi_ssr = 0xffffffffu;
            ivars->flash_slave = 0xffffffffu;
            IOLockUnlock(ivars->flash_lock);
            return kIOReturnNotPermitted;
          }
          if (verdict == SPI_LAUNCH_SUPPRESS) {
            // Nothing to do at the launch: the opcode was already rewritten on its
            // way into the FIFO, so what is about to be clocked out is a read. Let
            // the caller's own launching write happen unchanged.
            ivars->spi_audit.last_suppressed_sent = sent;
            (void)op;
          }
        } else if (value & (XRT_SPI_CR_TX_RESET | XRT_SPI_CR_RX_RESET)) {
          spi_shadow_clear(ivars);   // FIFO reset: whatever was queued is gone
        }
        break;
      }
      default:
        break;
    }
    pci->MemoryWrite32(bar, base + reg, wr_value);
  } else {
    uint32_t v = 0;
    pci->MemoryRead32(bar, base + reg, &v);
    if (out)
      *out = v;
    // How many bytes the caller has drained since the last launch. If this stops
    // short of what was sent, its byte count can never reach zero and it spins -
    // which is the failure mode three suppression designs hit, so it is worth being
    // able to read rather than infer.
    if (reg == XRT_SPI_DRR) {
      ivars->spi_drr_n++;
      ivars->spi_audit.drr_since_launch = ivars->spi_drr_n;
    }
  }
  // The caller can leave the core in any state, including a different chip
  // selected, so the next SpiTransfer must re-prepare rather than trust its
  // cached slave. Note this does NOT restore SPE: a caller that enables the core
  // is responsible for clearing it, or the next ICAP download fails on
  // STARTUPE3 - the same hazard SpiTransfer's teardown avoids. LoadBitstream
  // clears it defensively for exactly this reason.
  ivars->flash_slave = 0xffffffffu;
  IOLockUnlock(ivars->flash_lock);
  return kIOReturnSuccess;
}

// Take exclusive ownership of the flash controller for one connection.
//
// A whole erase or page program is hundreds of single register accesses through
// SpiReg, and the controller is one shared stateful thing: a SpiTransfer arriving
// in the middle of that sequence re-prepares the core with SRR, which truncates
// whatever command was in flight. Per-access locking cannot prevent that, because
// the unit that needs to be atomic is the sequence, not the access. So ownership is
// per CONNECTION, taken when the client starts.
kern_return_t
XrtUser::FlashClaim(IOService* owner)
{
  if (!owner)
    return kIOReturnBadArgument;
  IOLockLock(ivars->flash_lock);
  if (ivars->flash_owner && ivars->flash_owner != owner) {
    IOLockUnlock(ivars->flash_lock);
    return kIOReturnBusy;
  }
  ivars->flash_owner = owner;
  IOLockUnlock(ivars->flash_lock);
  return kIOReturnSuccess;
}

// Release it, and leave the controller safe for whoever comes next.
//
// Called from XrtFlashClient::Stop, so it runs when the owning process exits
// normally AND when it is killed. That second case is the one that matters: the
// SPI core holds STARTUPE3, upstream's xspi.cpp never disables it, and on some
// error paths it calls exit() directly - so relying on a user-space destructor to
// clear SPE leaves the next bitstream download broken. Deselect every chip and
// disable the core here and none of that depends on the caller behaving.
void
XrtUser::FlashRelease(IOService* owner)
{
  IOLockLock(ivars->flash_lock);
  if (ivars->flash_owner != owner) {
    IOLockUnlock(ivars->flash_lock);
    return;
  }
  auto pci = ivars->pci;
  if (pci && !ivars->asleep && blk_present(ivars, XRT_BLK_FLASH)) {
    uint8_t bar = 0, type = 0;
    uint64_t size = 0;
    if (pci->GetBARInfo(0, &bar, &size, &type) == kIOReturnSuccess) {
      const uint32_t base = blk_off(ivars, XRT_BLK_FLASH);
      pci->MemoryWrite32(bar, base + XRT_SPI_SSR, 0xffffffffu);
      pci->MemoryWrite32(bar, base + XRT_SPI_CR, 0);
    }
  }
  ivars->flash_slave = 0xffffffffu;
  ivars->flash_owner = nullptr;
  spi_shadow_clear(ivars);
  IOLockUnlock(ivars->flash_lock);
  LOG("flash: claim released, core disabled");
}

// Hand back what the shadow made of the commands it saw. The tally survives the
// connection that produced it, so a program run can be inspected after the tool
// that did it has exited - which is the normal case for a dry run.
kern_return_t
XrtUser::SpiRegAudit(void* out, uint64_t size, bool reset)
{
  if (!out || size < sizeof(xrt_dext_spi_audit))
    return kIOReturnBadArgument;
  IOLockLock(ivars->flash_lock);
  ivars->spi_audit.abi = XRT_DEXT_ABI_VERSION;
  ivars->spi_audit.mode = ivars->spi_guard;
  ivars->spi_audit.ear = ivars->spi_ear;
  ivars->spi_audit.ear_valid = ivars->spi_ear_valid;
  memcpy(out, &ivars->spi_audit, sizeof(ivars->spi_audit));
  if (reset) {
    memset(&ivars->spi_audit, 0, sizeof(ivars->spi_audit));
    spi_shadow_clear(ivars);
  }
  IOLockUnlock(ivars->flash_lock);
  return kIOReturnSuccess;
}

// Choose what the shadow does with what it decodes. ENFORCE is refused: the
// ceiling it would apply has to be expressed in chip-local addresses, and which
// chip-local address corresponds to the factory image depends on the striping -
// which is what DRY_RUN is for establishing. Shipping an enforcement rule built on
// an unverified assumption would be worse than shipping none.
kern_return_t
XrtUser::SpiRegMode(uint32_t mode, uint32_t* was)
{
  if (mode > XRT_SPI_GUARD_ENFORCE)
    return kIOReturnBadArgument;
  IOLockLock(ivars->flash_lock);
  if (was)
    *was = ivars->spi_guard;
  ivars->spi_guard = mode;
  IOLockUnlock(ivars->flash_lock);
  LOG("flash: shadow mode <- %{public}s",
      mode == XRT_SPI_GUARD_DRY_RUN
        ? "DRY RUN (every device write suppressed, nothing reaches the device)"
      : mode == XRT_SPI_GUARD_ENFORCE
        ? "ENFORCE (bulk erase refused)"
        : "OFF (no guard at all)");
  return kIOReturnSuccess;
}

// Map the CU window of user BAR0 into the dext's own address space. Each
// IOPCIDevice::MemoryWrite32 is a call into the kernel; through a mapping a
// register write is a store to device memory, and a whole command's
// registers cost one call from user space instead of two.
void
XrtUser::MapCuWindow()
{
  auto pci = ivars->pci;
  if (!pci || ivars->cu_regs)
    return;
  uint8_t idx = 0, type = 0;
  uint64_t bar_size = 0;
  if (pci->GetBARInfo(0, &idx, &bar_size, &type) != kIOReturnSuccess
      || bar_size < cu_win_end(ivars)) {
    LOG("CU window mapping: BAR0 is %llu bytes, need %u", bar_size, cu_win_end(ivars));
    return;
  }
  IOMemoryDescriptor* md = nullptr;
  kern_return_t ret = pci->_CopyDeviceMemoryWithIndex(idx, &md, this);
  if (ret != kIOReturnSuccess || !md) {
    LOG("CU window mapping: device memory for BAR0 failed 0x%x", ret);
    return;
  }
  IOMemoryMap* map = nullptr;
  ret = md->CreateMapping(kIOMemoryMapCacheModeInhibit, 0, ivars->cu_win_base,
                          ivars->cu_win_size, 0, &map);
  OSSafeReleaseNULL(md);
  if (ret != kIOReturnSuccess || !map) {
    LOG("CU window mapping failed 0x%x, falling back to MemoryWrite32", ret);
    OSSafeReleaseNULL(map);
    return;
  }
  ivars->cu_map = map;
  ivars->cu_regs = reinterpret_cast<volatile uint32_t*>(map->GetAddress());
  LOG("CU window 0x%x..0x%x mapped in the dext at %p", cu_win_base(ivars), cu_win_end(ivars),
      const_cast<uint32_t*>(ivars->cu_regs));
}

kern_return_t
XrtUser::CuWrite(uint64_t offset, const uint32_t* values, uint64_t count)
{
  auto pci = ivars->pci;
  bool no_map = (offset & XRT_CU_WRITE_NO_MAP) != 0;
  offset &= ~XRT_CU_WRITE_NO_MAP;
  if (!pci || !values || count == 0 || count > XRT_CU_WRITE_MAX_WORDS || (offset & 3))
    return kIOReturnBadArgument;
  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 1)
    return kIOReturnNotPermitted;
  if (offset < cu_win_base(ivars) || offset + count * 4 > cu_win_end(ivars))
    return kIOReturnNotPermitted;
  if (ivars->asleep)
    return kIOReturnOffline;
  if (ivars->partition_held)
    return kIOReturnBusy;        // the partition is being reprogrammed
  if (ivars->cu_regs && !no_map) {
    volatile uint32_t* regs = ivars->cu_regs + (offset - cu_win_base(ivars)) / 4;
    for (uint64_t i = 0; i < count; ++i)
      regs[i] = values[i];
    return kIOReturnSuccess;
  }
  uint8_t bar = 0, type = 0;
  uint64_t size = 0;
  if (pci->GetBARInfo(0, &bar, &size, &type) != kIOReturnSuccess || size < cu_win_end(ivars))
    return kIOReturnNoResources;
  for (uint64_t i = 0; i < count; ++i)
    pci->MemoryWrite32(bar, offset + i * 4, values[i]);
  return kIOReturnSuccess;
}

// Arguments and start in one call: the shim's scheduler writes a command's
// register map and sets ap_start without returning to user space in between.
kern_return_t
XrtUser::CuExec(uint64_t cu_base, uint64_t arg_offset, const uint32_t* values, uint64_t count,
                uint32_t* ctrl_out)
{
  auto pci = ivars->pci;
  if (ctrl_out)
    *ctrl_out = 0;
  // ap_ctrl_chain is a property of the CU, which only the caller knows - it
  // reads it out of the xclbin's ip_layout - so it rides in with the base.
  const bool chain = (cu_base & XRT_CU_EXEC_CHAIN) != 0;
  cu_base &= ~XRT_CU_EXEC_CHAIN;
  if (!pci || count > XRT_CU_WRITE_MAX_WORDS || (cu_base & 3) || (arg_offset & 3)
      || (count && !values))
    return kIOReturnBadArgument;
  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 1)
    return kIOReturnNotPermitted;
  // Both the control register and the argument range must be inside the
  // user partition's CU window. cu_base is bounded on its own first, with
  // no arithmetic on it, so that nothing below can wrap: it is then under
  // 2^25, arg_offset is under 2^32 and count * 4 is at most 4 KiB.
  if (cu_base < cu_win_base(ivars) || cu_base >= cu_win_end(ivars))
    return kIOReturnNotPermitted;
  if (arg_offset + count * 4 > cu_win_end(ivars) - cu_base)
    return kIOReturnNotPermitted;
  if (ivars->asleep)
    return kIOReturnOffline;
  if (ivars->partition_held)
    return kIOReturnBusy;        // the partition is being reprogrammed

  uint8_t bar = 0, type = 0;
  uint64_t size = 0;
  bool mapped = ivars->cu_regs != nullptr;
  if (!mapped && (pci->GetBARInfo(0, &bar, &size, &type) != kIOReturnSuccess
                  || size < cu_win_end(ivars)))
    return kIOReturnNoResources;

  // Checking that the CU is idle and starting it has to be one step. The
  // scheduler lives in the XRT library, one per process, so two processes
  // could otherwise both read an idle CU, both write their arguments, and
  // one command would run with the other's. cu_lock makes the pair atomic
  // for every process on this card.
  //
  // The CU window is mapped cache-inhibited, which is Device memory on
  // arm64: the stores reach the card in program order, so the arguments land
  // before ap_start without a barrier. A barrier here is not free - it waits
  // for the posted write to drain over Thunderbolt.
  // Reading the control register clears ap_done, so this read can consume the
  // completion of the previous command before its owner has polled for it.
  // That is harmless: the shim also treats idle without ap_start as complete.
  volatile uint32_t* base = mapped ? ivars->cu_regs + (cu_base - cu_win_base(ivars)) / 4 : nullptr;
  IOLockLock(ivars->cu_lock);
  uint32_t ctrl = 0;
  if (mapped)
    ctrl = base[0];
  else
    pci->MemoryRead32(bar, cu_base, &ctrl);
  // An ap_ctrl_chain CU parks holding ap_done until it is acknowledged, and
  // will accept nothing until then. Acknowledge it here, inside the lock we
  // already hold and off the back of a read we already did, so it costs no
  // extra call - and so that a CU left parked by a process that died between
  // done and the acknowledgement is recovered by whoever starts it next.
  if (chain && (ctrl & XRT_CU_AP_DONE) && !is_filler(ctrl)) {
    if (mapped)
      base[0] = XRT_CU_AP_CONTINUE;
    else
      pci->MemoryWrite32(bar, cu_base, XRT_CU_AP_CONTINUE);
    if (mapped)
      ctrl = base[0];
    else
      pci->MemoryRead32(bar, cu_base, &ctrl);
  }
  if (ctrl_out)
    *ctrl_out = ctrl;
  if (!(ctrl & XRT_CU_AP_IDLE) || (ctrl & XRT_CU_AP_START)) {
    IOLockUnlock(ivars->cu_lock);
    return kIOReturnBusy;              // someone else is running on this CU
  }
  if (mapped) {
    volatile uint32_t* args = base + arg_offset / 4;
    for (uint64_t i = 0; i < count; ++i)
      args[i] = values[i];
    base[0] = XRT_CU_AP_START;
  }
  else {
    for (uint64_t i = 0; i < count; ++i)
      pci->MemoryWrite32(bar, cu_base + arg_offset + i * 4, values[i]);
    pci->MemoryWrite32(bar, cu_base, XRT_CU_AP_START);
  }
  IOLockUnlock(ivars->cu_lock);
  return kIOReturnSuccess;
}

kern_return_t
XrtUser::SetSection(uint32_t slot, const void* data, uint64_t size)
{
  if (slot >= XRT_SECTION_SLOTS || !data || size == 0 || size > XRT_SECTION_MAX_BYTES)
    return kIOReturnBadArgument;
  OSSafeReleaseNULL(ivars->sections[slot]);
  ivars->sections[slot] = OSData::withBytes(data, size);
  return ivars->sections[slot] ? kIOReturnSuccess : kIOReturnNoMemory;
}

OSData*
XrtUser::CopySection(uint32_t slot)
{
  if (slot >= XRT_SECTION_SLOTS || !ivars->sections[slot])
    return nullptr;
  ivars->sections[slot]->retain();
  return ivars->sections[slot];
}

// ---------------------------------------------------------------------------
// Phase 7: device memory allocator. First fit within [bank_base,
// bank_base + bank_size), 4 KiB granularity, records sorted by address.
// ---------------------------------------------------------------------------
kern_return_t
XrtUser::MemAlloc(uint64_t bank_base, uint64_t bank_size, uint64_t size, void* owner, uint64_t* addr)
{
  if (!addr || size == 0 || bank_size == 0 || bank_base < ivars->card_min
      || bank_base + bank_size > ivars->card_max || bank_base + bank_size < bank_base)
    return kIOReturnBadArgument;
  size = (size + XRT_MEM_ALLOC_ALIGN - 1) & ~uint64_t(XRT_MEM_ALLOC_ALIGN - 1);
  if (size > bank_size)
    return kIOReturnNoSpace;

  IOLockLock(ivars->mem_lock);
  kern_return_t ret = kIOReturnNoSpace;
  if (ivars->mem_count >= XRT_MEM_MAX_ALLOCS) {
    IOLockUnlock(ivars->mem_lock);
    return kIOReturnNoResources;
  }
  const uint64_t bank_end = bank_base + bank_size;
  uint64_t cand = bank_base;
  uint32_t pos = 0;                      // insertion index
  for (; pos < ivars->mem_count; ++pos) {
    const auto& r = ivars->mem[pos];
    if (r.addr + r.size <= bank_base)
      continue;                          // before this bank
    if (r.addr >= bank_end)
      break;                             // past this bank
    if (r.addr >= cand + size)
      break;                             // gap before r fits
    if (r.addr + r.size > cand)
      cand = r.addr + r.size;
  }
  if (cand + size <= bank_end) {
    for (uint32_t i = ivars->mem_count; i > pos; --i)
      ivars->mem[i] = ivars->mem[i - 1];
    ivars->mem[pos] = { cand, size, owner };
    ++ivars->mem_count;
    *addr = cand;
    ret = kIOReturnSuccess;
  }
  IOLockUnlock(ivars->mem_lock);
  return ret;
}

kern_return_t
XrtUser::MemFree(uint64_t addr, void* owner)
{
  IOLockLock(ivars->mem_lock);
  kern_return_t ret = kIOReturnNotFound;
  for (uint32_t i = 0; i < ivars->mem_count; ++i) {
    if (ivars->mem[i].addr != addr)
      continue;
    if (ivars->mem[i].owner != owner) {
      ret = kIOReturnNotPermitted;     // another process's buffer
      break;
    }
    for (uint32_t j = i; j + 1 < ivars->mem_count; ++j)
      ivars->mem[j] = ivars->mem[j + 1];
    --ivars->mem_count;
    ret = kIOReturnSuccess;
    break;
  }
  IOLockUnlock(ivars->mem_lock);
  return ret;
}

void
XrtUser::MemFreeOwner(void* owner)
{
  IOLockLock(ivars->mem_lock);
  uint32_t out = 0, freed = 0;
  for (uint32_t i = 0; i < ivars->mem_count; ++i) {
    if (ivars->mem[i].owner == owner) {
      ++freed;
      continue;
    }
    ivars->mem[out++] = ivars->mem[i];
  }
  ivars->mem_count = out;
  IOLockUnlock(ivars->mem_lock);
  if (freed)
    LOG("released %u device buffer(s) of a closed client", freed);
}

void
XrtUser::MemStat(uint64_t bank_base, uint64_t bank_size, uint64_t* used, uint64_t* count)
{
  IOLockLock(ivars->mem_lock);
  uint64_t u = 0, c = 0;
  for (uint32_t i = 0; i < ivars->mem_count; ++i) {
    const auto& r = ivars->mem[i];
    if (r.addr >= bank_base && r.addr < bank_base + bank_size) {
      u += r.size;
      ++c;
    }
  }
  IOLockUnlock(ivars->mem_lock);
  *used = u;
  *count = c;
}

// ---------------------------------------------------------------------------
// CU contexts.
//
// xocl keeps these in the kernel because the kernel schedules the CUs. Here
// the scheduler is in the XRT library, one per process, so nothing else can
// stop two processes from driving the same CU: the extension has to hold the
// contexts and refuse the conflicting ones.
//
// An exclusive context excludes every other connection. A shared one
// excludes only exclusive contexts, matching xrt::hw_context::access_mode.
// Contexts belong to the connection that opened them and go away with it.
// ---------------------------------------------------------------------------
kern_return_t
XrtUser::CuCtxOpen(uint32_t cu, uint32_t shared, uint64_t base, void* owner)
{
  if (!owner || cu >= XRT_CU_MAX || shared > XRT_CU_CTX_SHARED)
    return kIOReturnBadArgument;
  // The base is only used to decide which page a read-write mapping would
  // cover, so it has to be inside the CU window; it is the shim's word for
  // which CU this is, which is why the mapping is a fast path and not an
  // isolation boundary (see xrt_dext_abi.h).
  if (base < cu_win_base(ivars) || base >= cu_win_end(ivars) || (base & 3))
    return kIOReturnBadArgument;
  uint8_t bus = 0, dev = 0, func = 0;
  if (!ivars->pci)
    return kIOReturnNoDevice;
  ivars->pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 1)
    return kIOReturnNotPermitted;

  IOLockLock(ivars->cu_lock);
  // Already open on this connection: count it, provided the mode agrees.
  for (uint32_t i = 0; i < ivars->cu_ctx_count; ++i) {
    auto& r = ivars->cu_ctx[i];
    if (r.cu == cu && r.owner == owner) {
      if (r.shared != shared) {
        IOLockUnlock(ivars->cu_lock);
        LOG("CU %u already open on this connection in the other mode", cu);
        return kIOReturnNotPermitted;
      }
      ++r.count;
      IOLockUnlock(ivars->cu_lock);
      return kIOReturnSuccess;
    }
  }
  // Someone else's context may conflict.
  for (uint32_t i = 0; i < ivars->cu_ctx_count; ++i) {
    const auto& r = ivars->cu_ctx[i];
    if (r.cu != cu || r.owner == owner)
      continue;
    if (shared != XRT_CU_CTX_SHARED || r.shared != XRT_CU_CTX_SHARED) {
      IOLockUnlock(ivars->cu_lock);
      LOG("CU %u context refused: held %s elsewhere, wanted %s", cu,
          r.shared ? "shared" : "exclusively", shared ? "shared" : "exclusively");
      return kIOReturnExclusiveAccess;
    }
  }
  if (ivars->cu_ctx_count >= XRT_CU_CTX_MAX) {
    IOLockUnlock(ivars->cu_lock);
    return kIOReturnNoResources;
  }
  ivars->cu_ctx[ivars->cu_ctx_count++] = { owner, cu, shared, 1, base };
  IOLockUnlock(ivars->cu_lock);
  return kIOReturnSuccess;
}

kern_return_t
XrtUser::CuCtxClose(uint32_t cu, void* owner)
{
  if (!owner || cu >= XRT_CU_MAX)
    return kIOReturnBadArgument;
  IOLockLock(ivars->cu_lock);
  for (uint32_t i = 0; i < ivars->cu_ctx_count; ++i) {
    auto& r = ivars->cu_ctx[i];
    if (r.cu != cu || r.owner != owner)
      continue;
    if (--r.count == 0)
      ivars->cu_ctx[i] = ivars->cu_ctx[--ivars->cu_ctx_count];
    IOLockUnlock(ivars->cu_lock);
    return kIOReturnSuccess;
  }
  IOLockUnlock(ivars->cu_lock);
  return kIOReturnNotFound;
}

// The owning client closed its connection or died.
void
XrtUser::CuCtxCloseOwner(void* owner)
{
  if (!owner || !ivars || !ivars->cu_lock || !ivars->cu_ctx)
    return;
  IOLockLock(ivars->cu_lock);
  uint32_t out = 0, dropped = 0;
  for (uint32_t i = 0; i < ivars->cu_ctx_count; ++i) {
    if (ivars->cu_ctx[i].owner == owner) {
      ++dropped;
      continue;
    }
    ivars->cu_ctx[out++] = ivars->cu_ctx[i];
  }
  ivars->cu_ctx_count = out;
  IOLockUnlock(ivars->cu_lock);
  if (dropped)
    LOG("released %u CU context(s) of a closed client", dropped);
}

// Page of BAR0 to map read-write for owner's exclusive context on cu. Refused
// unless owner holds cu exclusively, the CU's base is page aligned (otherwise
// the page would reach below it), and no other connection has a context on a
// CU in the same page.
kern_return_t
XrtUser::CuRwPage(uint32_t cu, void* owner, uint64_t* offset)
{
  if (!owner || cu >= XRT_CU_MAX || !offset)
    return kIOReturnBadArgument;
  *offset = 0;
  IOLockLock(ivars->cu_lock);
  uint64_t base = 0;
  bool exclusive = false;
  for (uint32_t i = 0; i < ivars->cu_ctx_count; ++i) {
    const auto& r = ivars->cu_ctx[i];
    if (r.cu == cu && r.owner == owner) {
      base = r.base;
      exclusive = r.shared == XRT_CU_CTX_EXCLUSIVE;
      break;
    }
  }
  if (!exclusive) {
    IOLockUnlock(ivars->cu_lock);
    return kIOReturnNotPermitted;
  }
  if (base & (XRT_CU_RW_PAGE - 1)) {
    IOLockUnlock(ivars->cu_lock);
    LOG("CU %u base 0x%llx is not page aligned; no read-write mapping", cu, base);
    return kIOReturnUnsupported;
  }
  // Another connection with a context on a CU in this page would be exposed.
  for (uint32_t i = 0; i < ivars->cu_ctx_count; ++i) {
    const auto& r = ivars->cu_ctx[i];
    if (r.owner == owner)
      continue;
    if ((r.base & ~(uint64_t)(XRT_CU_RW_PAGE - 1)) == base) {
      IOLockUnlock(ivars->cu_lock);
      LOG("CU %u page shared with CU %u of another connection; no mapping", cu, r.cu);
      return kIOReturnBusy;
    }
  }
  IOLockUnlock(ivars->cu_lock);
  *offset = base;
  return kIOReturnSuccess;
}

kern_return_t
XrtUser::CopyCuPageMemory(uint32_t cu, void* owner, IOMemoryDescriptor** memory)
{
  if (!memory || !ivars->pci)
    return kIOReturnBadArgument;
  uint8_t bus = 0, dev = 0, func = 0;
  ivars->pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 1)
    return kIOReturnNotPermitted;
  uint64_t offset = 0;
  kern_return_t ret = CuRwPage(cu, owner, &offset);
  if (ret != kIOReturnSuccess)
    return ret;
  uint8_t idx = 0, type = 0;
  uint64_t bar_size = 0;
  if (ivars->pci->GetBARInfo(0, &idx, &bar_size, &type) != kIOReturnSuccess
      || bar_size < offset + XRT_CU_RW_PAGE)
    return kIOReturnNoResources;
  IOMemoryDescriptor* bar = nullptr;
  ret = ivars->pci->_CopyDeviceMemoryWithIndex(idx, &bar, this);
  if (ret != kIOReturnSuccess || !bar) {
    OSSafeReleaseNULL(bar);
    return ret == kIOReturnSuccess ? kIOReturnNoResources : ret;
  }
  ret = IOMemoryDescriptor::CreateSubMemoryDescriptor(kIOMemoryDirectionInOut, offset,
                                                      XRT_CU_RW_PAGE, bar, memory);
  OSSafeReleaseNULL(bar);
  if (ret == kIOReturnSuccess)
    LOG("CU %u page 0x%llx mapped read-write for one connection", cu, offset);
  return ret;
}

// ---------------------------------------------------------------------------
// Keeping off the partition while it is reprogrammed.
//
// The two PCI functions are separate dext processes, so the management
// function cannot reach this state directly. The shim takes the hold on the
// user function before it starts an ICAP download or a hot reset and
// releases it afterwards. While held, DMA and CU register writes are refused
// for every process, not only the one that took the hold: an access that
// reaches the partition while the ULP gate is frozen is what trips the AXI
// firewalls.
//
// This cannot stop a process reading the CU window through its read-only BAR
// mapping, which the dext has no way to revoke. Such a read returns all ones
// while the partition is down, and the shim's scheduler treats that as a
// failure rather than waiting for ever.
// ---------------------------------------------------------------------------
kern_return_t
XrtUser::PartitionHold(void* owner)
{
  auto pci = ivars->pci;
  if (!pci || !owner)
    return kIOReturnBadArgument;
  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 1)
    return kIOReturnNotPermitted;

  // Taking dma_lock waits for a transfer already in flight to finish.
  IOLockLock(ivars->dma_lock);
  if (ivars->partition_held && ivars->partition_owner != owner) {
    IOLockUnlock(ivars->dma_lock);
    LOG("partition hold refused: already held");
    return kIOReturnBusy;
  }
  // cu_lock inside dma_lock: a CU start in progress finishes first, and none
  // can begin while the flag is being set.
  IOLockLock(ivars->cu_lock);
  ivars->partition_held = true;
  ivars->partition_owner = owner;
  IOLockUnlock(ivars->cu_lock);
  // Stop the engines so nothing is mid-stream when the partition goes away,
  // as the sleep path does.
  uint8_t bar = 0, type = 0;
  uint64_t size = 0;
  if (!ivars->asleep
      && pci->GetBARInfo(xdma_bar(ivars), &bar, &size, &type) == kIOReturnSuccess && size >= 0x6000)
    for (uint32_t ch = 0; ch < 2; ++ch) {
      pci->MemoryWrite32(bar, XRT_XDMA_H2C(ch) + kXdmaCtrlW1C, kCtrlRun);
      pci->MemoryWrite32(bar, XRT_XDMA_C2H(ch) + kXdmaCtrlW1C, kCtrlRun);
    }
  IOLockUnlock(ivars->dma_lock);
  LOG("partition held for reprogramming");
  return kIOReturnSuccess;
}

kern_return_t
XrtUser::PartitionRelease(void* owner)
{
  if (!owner)
    return kIOReturnBadArgument;
  IOLockLock(ivars->dma_lock);
  if (ivars->partition_held && ivars->partition_owner != owner) {
    IOLockUnlock(ivars->dma_lock);
    return kIOReturnNotPermitted;
  }
  bool was_held = ivars->partition_held;
  ivars->partition_held = false;
  ivars->partition_owner = nullptr;
  // Whatever held the partition may have reset or reprogrammed the card, and
  // this is the user function - the one that caches these, and the one HotReset
  // cannot reach because it runs in the other function's process. Re-read them
  // rather than trust a cache a reset may have invalidated; the cost is one
  // config read per release, not per transfer.
  ivars->cmd_enabled = false;
  ivars->chan_known = false;
  ivars->xdma_bar_probed = false;
  ivars->irq_dead = false;
  ivars->irq_misses = 0;
  drop_dma_mappings(ivars);
  IOLockUnlock(ivars->dma_lock);
  if (was_held)
    LOG("partition released");
  return kIOReturnSuccess;
}

// The owning client closed its connection or died.
void
XrtUser::PartitionReleaseOwner(void* owner)
{
  if (!owner || !ivars || !ivars->dma_lock)
    return;
  IOLockLock(ivars->dma_lock);
  bool ours = ivars->partition_held && ivars->partition_owner == owner;
  if (ours) {
    ivars->partition_held = false;
    ivars->partition_owner = nullptr;
  }
  IOLockUnlock(ivars->dma_lock);
  if (ours)
    LOG("partition hold released: owner went away");
}

// ---------------------------------------------------------------------------
// Phase 7: hot reset. The MicroBlaze on these shells runs no firmware on
// macOS (image is the cold-boot self-jump), so xclmgmt's mb_stop/mb_start
// reduce to nothing and are skipped.
// ---------------------------------------------------------------------------

kern_return_t
XrtUser::HotReset(void* out)
{
  auto res = static_cast<xrt_dext_reset_result*>(out);
  memset(res, 0, sizeof(*res));
  auto pci = ivars->pci;
  if (!pci)
    return res->status = kIOReturnNoDevice;
  if (ivars->asleep)
    return res->status = kIOReturnOffline;
  uint8_t bus = 0, dev = 0, func = 0;
  pci->GetBusDeviceFunction(&bus, &dev, &func);
  if (func != 0)
    return res->status = kIOReturnNotPermitted;
  uint8_t bar = 0, type = 0;
  uint64_t size = 0;
  if (pci->GetBARInfo(0, &bar, &size, &type) != kIOReturnSuccess || size < 0x100000)
    return res->status = kIOReturnNoResources;
  auto rd = [&](uint32_t off) { uint32_t v = 0; pci->MemoryRead32(bar, off, &v); return v; };
  auto wr = [&](uint32_t off, uint32_t v) { pci->MemoryWrite32(bar, off, v); };
  // Firewalls come from the platform table: four on XDMA 5.x, six on a U50.
  auto fw = [&](int i) { return blk_off(ivars, XRT_BLK_FIREWALL_0 + i); };
  auto fw_present = [&](int i) { return blk_present(ivars, XRT_BLK_FIREWALL_0 + i); };
  auto fw_state = [&](uint32_t* outv) {
    uint32_t tripped = 0;
    for (int i = 0; i < XRT_MGMT_FIREWALLS; ++i) {
      if (!fw_present(i)) {
        outv[i] = 0;
        continue;
      }
      uint32_t a = rd(fw(i) + XRT_FW_FAULT_STATUS);
      uint32_t b = rd(fw(i) + XRT_FW_SI_FAULT_STATUS);
      outv[i] = (a & ~XRT_FW_BUSY_MASK) | (b & ~XRT_FW_BUSY_MASK);
      if (a == 0xffffffff)
        outv[i] = 0xffffffff;
      if (outv[i])
        ++tripped;
    }
    return tripped;
  };
  fw_state(res->fw_before);

  // The user partition and any running DMA are lost across the reset.
  memset(ivars->xclbin_uuid, 0, sizeof(ivars->xclbin_uuid));
  for (auto& sec : ivars->sections)
    OSSafeReleaseNULL(sec);

  uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  LOG("HotReset: firewalls before 0x%x 0x%x 0x%x 0x%x",
      res->fw_before[0], res->fw_before[1], res->fw_before[2], res->fw_before[3]);
  // Saves and restores config space; blocks until the link is back.
  res->reset_status = pci->Reset(kIOPCIDeviceResetTypeHotReset, 0);
  if (res->reset_status != kIOReturnSuccess) {
    LOG("HotReset: IOPCIDevice::Reset failed 0x%x", res->reset_status);
    res->elapsed_ms = static_cast<uint32_t>((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1000000);
    return res->status = res->reset_status;
  }
  uint16_t cmd = 0;
  pci->ConfigurationRead16(kCfgCommand, &cmd);
  if (!(cmd & kCmdMemorySpace))
    pci->ConfigurationWrite16(kCfgCommand, cmd | kCmdMemorySpace);
  pci->ConfigurationRead16(kCfgCommand, &res->command_after);
  IOSleep(20);

  // xclmgmt_hot_reset: clear tripped firewalls, up to 10 rounds.
  kern_return_t ret = kIOReturnSuccess;
  uint32_t now[XRT_MGMT_FIREWALLS];
  while (fw_state(now) && res->clear_rounds < 10) {
    for (int i = 0; i < XRT_MGMT_FIREWALLS; ++i) {
      if (!fw_present(i) || !now[i] || now[i] == 0xffffffff)
        continue;
      for (int t = 0; t < 50 && (rd(fw(i)) & XRT_FW_BUSY_MASK); ++t)
        IOSleep(10);
      if (rd(fw(i) + XRT_FW_FAULT_STATUS) & ~XRT_FW_BUSY_MASK)
        wr(fw(i) + XRT_FW_UNBLOCK_CTRL, 1);
      if (rd(fw(i) + XRT_FW_SI_FAULT_STATUS) & ~XRT_FW_BUSY_MASK)
        wr(fw(i) + XRT_FW_SI_UNBLOCK_CTRL, 1);
    }
    ++res->clear_rounds;
    IOSleep(20);
  }
  if (fw_state(res->fw_after))
    ret = kIOReturnIOError;

  // A platform without these blocks reports 0 for them rather than reading a
  // fixed offset that means nothing on it, and its liveness cannot be judged
  // by a ROM magic that does not exist.
  const bool have_rom = blk_present(ivars, XRT_BLK_FEATURE_ROM);
  res->rom_magic_after = have_rom ? rd(blk_off(ivars, XRT_BLK_FEATURE_ROM)) : 0;
  res->gate_after = blk_present(ivars, XRT_BLK_GATE_ULP)
                    ? rd(blk_off(ivars, XRT_BLK_GATE_ULP) + kGateRDRel) : 0;
  for (int i = 0; i < 2; ++i)
    res->clk_status_after[i] = blk_present(ivars, XRT_BLK_CLKWIZ_K1 + i)
                               ? rd(blk_off(ivars, XRT_BLK_CLKWIZ_K1 + i) + 0x4) : 0;
  if (have_rom && res->rom_magic_after != XRT_DEXT_ROM_MAGIC)
    ret = kIOReturnNotResponding;
  res->elapsed_ms = static_cast<uint32_t>((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1000000);
  // NOTE: the DMA caches are deliberately NOT cleared here. HotReset only runs
  // on function 0 and DmaTransfer only on function 1, and the two functions are
  // separate dext processes with separate ivars - so clearing them here touched
  // a copy nothing would ever read, while the user function kept its stale one.
  // PartitionRelease is the user-side call the shim makes after a reset or a
  // download, and it does the invalidation instead.
  LOG("HotReset: done 0x%x in %u ms, cmd 0x%x, rom 0x%x, gate 0x%x, firewalls after 0x%x 0x%x 0x%x 0x%x",
      ret, res->elapsed_ms, res->command_after, res->rom_magic_after, res->gate_after,
      res->fw_after[0], res->fw_after[1], res->fw_after[2], res->fw_after[3]);
  return res->status = ret;
}

// ---------------------------------------------------------------------------
// Phase 7: system sleep and wake.
// ---------------------------------------------------------------------------
kern_return_t
IMPL(XrtUser, SetPowerState)
{
  auto pci = ivars->pci;
  uint8_t bus = 0, dev = 0, func = 0;
  if (pci)
    pci->GetBusDeviceFunction(&bus, &dev, &func);

  if (pci && ivars->opened && powerFlags == kIOServicePowerCapabilityOff && !ivars->asleep) {
    // Wait for any transfer in flight, then stop the engines and block all
    // hardware access until wake.
    IOLockLock(ivars->dma_lock);
    pci->ConfigurationRead16(kCfgCommand, &ivars->command_saved);
    if (func == 1) {
      uint8_t bar = 0, type = 0;
      uint64_t size = 0;
      if (pci->GetBARInfo(xdma_bar(ivars), &bar, &size, &type) == kIOReturnSuccess)
        for (uint32_t ch = 0; ch < 2; ++ch) {
          pci->MemoryWrite32(bar, XRT_XDMA_H2C(ch) + kXdmaCtrlW1C, kCtrlRun);
          pci->MemoryWrite32(bar, XRT_XDMA_C2H(ch) + kXdmaCtrlW1C, kCtrlRun);
        }
    }
    ivars->asleep = true;
    ivars->cmd_enabled = false;   // the card can lose it across a sleep
    ivars->chan_known = false;
    IOLockUnlock(ivars->dma_lock);
    LOG("sleep: %02x:%02x.%x quiesced (command 0x%x)", bus, dev, func, ivars->command_saved);
  }
  else if (pci && ivars->opened && powerFlags != kIOServicePowerCapabilityOff && ivars->asleep) {
    // Restore decoding (and bus mastering on the user function), then check
    // the shell is still there.
    uint16_t cmd = 0;
    pci->ConfigurationRead16(kCfgCommand, &cmd);
    uint16_t want = cmd | (ivars->command_saved & (kCmdMemorySpace | kCmdBusMaster));
    if (want != cmd)
      pci->ConfigurationWrite16(kCfgCommand, want);
    uint8_t bar = 0, type = 0;
    uint64_t size = 0;
    uint32_t magic = 0;
    if (blk_present(ivars, XRT_BLK_FEATURE_ROM)
        && pci->GetBARInfo(blk(ivars, XRT_BLK_FEATURE_ROM).bar, &bar, &size, &type) == kIOReturnSuccess)
      pci->MemoryRead32(bar, blk_off(ivars, XRT_BLK_FEATURE_ROM), &magic);
    // Sleep can reset the card: on the test system (Thunderbolt enclosure)
    // the XDMA engines come back reset and the user partition is gone while
    // the static shell and its ROM survive. Nothing on the management side
    // proves the partition survived, so always require a reload.
    bool had_xclbin = false;
    for (auto b : ivars->xclbin_uuid)
      had_xclbin |= (b != 0);
    memset(ivars->xclbin_uuid, 0, sizeof(ivars->xclbin_uuid));
    for (auto& sec : ivars->sections)
      OSSafeReleaseNULL(sec);
    ivars->asleep = false;
    LOG("wake: %02x:%02x.%x command 0x%x -> 0x%x, ROM magic 0x%x%s%s", bus, dev, func, cmd, want, magic,
        magic == XRT_DEXT_ROM_MAGIC ? "" : " (shell not responding)",
        had_xclbin ? ", xclbin must be reloaded" : "");
  }
  return SetPowerState(powerFlags, SUPERDISPATCH);
}

// ---------------------------------------------------------------------------
// Stage 1: MSI-X completion interrupts for the XDMA engines.
// ---------------------------------------------------------------------------
void
XrtUser::SetupInterrupts()
{
  auto pci = ivars->pci;
  kern_return_t ret = pci->ConfigureInterrupts(kIOInterruptTypePCIMessagedX, kXdmaEngines, kXdmaEngines, 0);
  if (ret != kIOReturnSuccess) {
    LOG("irq: ConfigureInterrupts(MSI-X, %u) failed 0x%x, DMA will poll", kXdmaEngines, ret);
    return;
  }
  // The provider lists its interrupt sources by index; take the MSI-X ones
  // in order. Log the types so the index layout can be checked.
  uint32_t idx[kXdmaEngines] = {};
  uint32_t found = 0;
  for (uint32_t i = 0; i < 16 && found < kXdmaEngines; ++i) {
    uint64_t type = 0;
    kern_return_t r = IOInterruptDispatchSource::GetInterruptType(pci, i, &type);
    LOG("irq: source %u type 0x%llx (0x%x)", i, type, r);
    if (r != kIOReturnSuccess)
      continue;
    if (type & kIOInterruptTypePCIMessagedX)
      idx[found++] = i;
  }
  if (found < kXdmaEngines) {
    LOG("irq: found %u MSI-X sources, need %u, DMA will poll", found, kXdmaEngines);
    return;
  }
  do {
    // Reentrant: DmaTransfer sleeps on this queue and releases it.
    if ((ret = IODispatchQueue::Create("Interrupt", kIODispatchQueueReentrant, 0, &ivars->irq_queue)) != kIOReturnSuccess)
      break;
    if ((ret = SetDispatchQueue("Interrupt", ivars->irq_queue)) != kIOReturnSuccess)
      break;
    for (uint32_t e = 0; e < kXdmaEngines && ret == kIOReturnSuccess; ++e) {
      if ((ret = IOInterruptDispatchSource::Create(pci, idx[e], ivars->irq_queue, &ivars->irq_src[e])) != kIOReturnSuccess)
        break;
      if ((ret = CreateActionDmaInterrupt(sizeof(uint32_t), &ivars->irq_action[e])) != kIOReturnSuccess)
        break;
      *static_cast<uint32_t*>(ivars->irq_action[e]->GetReference()) = e;
      if ((ret = ivars->irq_src[e]->SetHandler(ivars->irq_action[e])) != kIOReturnSuccess)
        break;
      ret = ivars->irq_src[e]->SetEnableWithCompletion(true, ^{ });
    }
  } while (false);
  if (ret != kIOReturnSuccess) {
    // Do not tear down from here. Cancel is asynchronous and its completion runs
    // after this function has released the source and the queue it needs, which
    // segfaults during Start and leaves no instance matched on either function.
    // Decline the vectors and let Stop do it. The BAR guard below had this
    // fixed; this path did not, and it is reachable from any failure of
    // IODispatchQueue::Create, IOInterruptDispatchSource::Create, SetHandler or
    // SetEnableWithCompletion.
    LOG("irq: setup failed 0x%x, DMA will poll", ret);
    ivars->irq_vectors = 0;
    return;
  }
  // Route engine n to vector n and enable the channel interrupts; each
  // engine's own mask stays 0 until a transfer arms it.
  uint8_t bar = 0, type = 0;
  uint64_t size = 0;
  // Claiming vectors when the programming was skipped is how a card with no
  // BAR at the expected index ends up waiting for interrupts that were never
  // enabled. At Start the platform table is not installed yet, so xdma_bar()
  // is still the XDMA 5.x default - which the U50, with no BAR1 at all, does
  // not have. DmaTransfer reprograms the block per transfer anyway.
  if (pci->GetBARInfo(xdma_bar(ivars), &bar, &size, &type) != kIOReturnSuccess) {
    // Leave the sources alone and simply do not claim the vectors: Stop tears
    // them down. Calling TeardownInterrupts here segfaults, because Cancel is
    // asynchronous and its completion runs after this function has released
    // the source and the queue it needs.
    LOG("irq: no BAR %u yet, leaving DMA on polling", xdma_bar(ivars));
    ivars->irq_vectors = 0;
    return;
  }
  for (uint32_t ch = 0; ch < 2; ++ch) {
    pci->MemoryWrite32(bar, XRT_XDMA_H2C(ch) + kXdmaIrqMask, 0);
    pci->MemoryWrite32(bar, XRT_XDMA_C2H(ch) + kXdmaIrqMask, 0);
  }
  pci->MemoryWrite32(bar, XRT_XDMA_IRQ + kIrqChannelVector, kIrqChannelVectorMap);
  pci->MemoryWrite32(bar, XRT_XDMA_IRQ + kIrqChannelEnableW1S, (1u << kXdmaEngines) - 1);
  ivars->irq_vectors = kXdmaEngines;
  LOG("irq: MSI-X sources %u %u %u %u serve H2C0 H2C1 C2H0 C2H1", idx[0], idx[1], idx[2], idx[3]);
}

kern_return_t
XrtUser::SetDmaMode(uint64_t mode, uint64_t* now)
{
  if (mode > XRT_DMA_MODE_QUERY)
    return kIOReturnBadArgument;
  IOLockLock(ivars->dma_lock);                      // not in the middle of a transfer
  if (mode != XRT_DMA_MODE_QUERY)
    ivars->irq_enabled = (mode == XRT_DMA_MODE_IRQ);
  bool irq = ivars->irq_enabled && ivars->irq_vectors == kXdmaEngines;
  IOLockUnlock(ivars->dma_lock);
  if (mode != XRT_DMA_MODE_QUERY)
    LOG("DMA completion set to %s", irq ? "MSI-X" : "polling");
  if (now)
    *now = irq ? XRT_DMA_MODE_IRQ : XRT_DMA_MODE_POLL;
  return kIOReturnSuccess;
}

void
XrtUser::TeardownInterrupts()
{
  if (ivars->irq_vectors && ivars->pci && ivars->opened) {
    uint8_t bar = 0, type = 0;
    uint64_t size = 0;
    if (ivars->pci->GetBARInfo(xdma_bar(ivars), &bar, &size, &type) == kIOReturnSuccess)
      ivars->pci->MemoryWrite32(bar, XRT_XDMA_IRQ + kIrqChannelEnableW1C, (1u << kXdmaEngines) - 1);
  }
  ivars->irq_vectors = 0;
  for (uint32_t e = 0; e < kXdmaEngines; ++e) {
    if (ivars->irq_src[e]) {
      // Both of these take a completion block and dereference it: a null one
      // is a segfault inside Cancel_Impl, not a no-op. Two existing failure
      // paths passed nullptr and had simply never been reached.
      ivars->irq_src[e]->SetEnableWithCompletion(false, ^{ });
      ivars->irq_src[e]->Cancel(^{ });
    }
    OSSafeReleaseNULL(ivars->irq_src[e]);
    OSSafeReleaseNULL(ivars->irq_action[e]);
  }
  OSSafeReleaseNULL(ivars->irq_queue);
}

void
IMPL(XrtUser, DmaInterrupt)
{
  uint32_t e = *static_cast<uint32_t*>(action->GetReference());
  if (e < kXdmaEngines && ++ivars->irq_count[e] <= 2)
    LOG("irq: engine %u interrupt, count %llu", e, count);
  // Runs on irq_queue, so it cannot interleave with the waiter's check.
  ivars->irq_fired = true;
  if (ivars->irq_queue)
    ivars->irq_queue->Wakeup(&ivars->irq_fired);
}
