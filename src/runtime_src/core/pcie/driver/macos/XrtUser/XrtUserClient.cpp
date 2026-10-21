// SPDX-License-Identifier: Apache-2.0
#include <os/log.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOUserClient.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/OSData.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <string.h>

#include "XrtUser.h"
#include "XrtUserClient.h"
#include "xrt_dext_abi.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "xrt_user client: " fmt, ##__VA_ARGS__)

struct XrtUserClient_IVars
{
  XrtUser* driver = nullptr;
};

bool
XrtUserClient::init()
{
  if (!super::init())
    return false;
  ivars = IONewZero(XrtUserClient_IVars, 1);
  return ivars != nullptr;
}

kern_return_t
IMPL(XrtUserClient, Start)
{
  kern_return_t ret = Start(provider, SUPERDISPATCH);
  if (ret != kIOReturnSuccess)
    return ret;
  ivars->driver = OSDynamicCast(XrtUser, provider);
  if (!ivars->driver)
    return kIOReturnNoDevice;
  ivars->driver->retain();
  return kIOReturnSuccess;
}

kern_return_t
IMPL(XrtUserClient, Stop)
{
  // The owning process closed its connection or died: release its buffers,
  // and any hold it had on the partition, so a process that dies part way
  // through a download does not leave the card refusing work.
  if (auto driver = OSDynamicCast(XrtUser, provider)) {
    driver->MemFreeOwner(this);
    driver->PartitionReleaseOwner(this);
    driver->CuCtxCloseOwner(this);
  }
  return Stop(provider, SUPERDISPATCH);
}

void
XrtUserClient::free()
{
  if (ivars)
    OSSafeReleaseNULL(ivars->driver);
  IOSafeDeleteNULL(ivars, XrtUserClient_IVars, 1);
  super::free();
}

static kern_return_t
get_device_info(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  if (!self)
    return kIOReturnBadArgument;
  // Access the driver through the provider we retained in Start.
  auto driver = OSDynamicCast(XrtUser, self->GetProvider());
  if (!driver)
    return kIOReturnNoDevice;

  xrt_dext_device_info info = {};
  kern_return_t ret = driver->CopyDeviceInfo(&info, sizeof(info));
  if (ret != kIOReturnSuccess)
    return ret;
  args->structureOutput = OSData::withBytes(&info, sizeof(info));
  return args->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

// Either function: the vendor capability may live on only one of them.
static kern_return_t
cfg_read(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  uint32_t value = 0;
  kern_return_t ret = driver->ConfigRead(args->scalarInput[0], &value);
  if (args->scalarOutputCount >= 1)
    args->scalarOutput[0] = value;
  return ret;
}

static kern_return_t
read_rom(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  uint8_t rom[XRT_DEXT_ROM_SIZE] = {};
  kern_return_t ret = driver->ReadFeatureRom(rom, sizeof(rom));
  if (ret != kIOReturnSuccess)
    return ret;
  args->structureOutput = OSData::withBytes(rom, sizeof(rom));
  return args->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

static kern_return_t
clock_measure(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  uint32_t mhz = 0;
  kern_return_t ret = driver->ClockMeasure(static_cast<uint32_t>(args->scalarInput[0]), &mhz);
  if (ret != kIOReturnSuccess)
    return ret;
  args->scalarOutput[0] = mhz;
  args->scalarOutputCount = 1;
  return kIOReturnSuccess;
}

// Management function only, bounded to two Innova-2 windows; the driver
// checks the function, the shell and the offset. Left out of a generic XRT
// build, which makes selector 36 answer kIOReturnUnsupported through the null
// check in ExternalMethod below - the same answer an older dext gives.
#ifndef XRT_MACOS_ENABLE_INNOVA2
#define XRT_MACOS_ENABLE_INNOVA2 0
#endif

#if XRT_MACOS_ENABLE_INNOVA2
static kern_return_t
reg_write(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  uint32_t readback = 0;
  kern_return_t ret = driver->RegWrite(args->scalarInput[0],
                                       static_cast<uint32_t>(args->scalarInput[1]),
                                       &readback);
  if (ret != kIOReturnSuccess)
    return ret;
  args->scalarOutput[0] = readback;
  args->scalarOutputCount = 1;
  return kIOReturnSuccess;
}
#endif

static kern_return_t
cmc_board_info(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  xrt_dext_cmc_bdinfo info = {};
  kern_return_t ret = driver->CmcBoardInfo(&info, sizeof(info));
  if (ret != kIOReturnSuccess && info.status == 0)
    return ret;
  args->structureOutput = OSData::withBytes(&info, sizeof(info));
  return args->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

static kern_return_t
ert_start(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  const uint32_t* words = nullptr;
  uint64_t bytes = 0;
  IOMemoryMap* map = nullptr;
  if (args->structureInput) {
    words = static_cast<const uint32_t*>(args->structureInput->getBytesNoCopy());
    bytes = args->structureInput->getLength();
  }
  else if (args->structureInputDescriptor) {
    if (args->structureInputDescriptor->CreateMapping(kIOMemoryMapReadOnly, 0, 0, 0, 0, &map)
        != kIOReturnSuccess)
      return kIOReturnVMError;
    words = reinterpret_cast<const uint32_t*>(map->GetAddress());
    bytes = map->GetLength();
  }
  if ((bytes % 4) != 0) {
    OSSafeReleaseNULL(map);
    return kIOReturnBadArgument;
  }
  xrt_dext_ert_result res = {};
  driver->ErtStart(words, bytes / 4, &res);
  OSSafeReleaseNULL(map);
  args->structureOutput = OSData::withBytes(&res, sizeof(res));
  return args->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

static kern_return_t
load_cmc(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;

  // Small images arrive as OSData, larger ones as a descriptor of the caller's
  // buffer, the same way the bitstream does.
  const uint32_t* words = nullptr;
  uint64_t bytes = 0;
  IOMemoryMap* map = nullptr;
  if (args->structureInput) {
    words = static_cast<const uint32_t*>(args->structureInput->getBytesNoCopy());
    bytes = args->structureInput->getLength();
  }
  else if (args->structureInputDescriptor) {
    if (args->structureInputDescriptor->CreateMapping(kIOMemoryMapReadOnly, 0, 0, 0, 0, &map)
        != kIOReturnSuccess)
      return kIOReturnVMError;
    words = reinterpret_cast<const uint32_t*>(map->GetAddress());
    bytes = map->GetLength();
  }
  if ((bytes % 4) != 0) {
    OSSafeReleaseNULL(map);
    return kIOReturnBadArgument;
  }

  xrt_dext_cmc_result res = {};
  driver->LoadCmc(words, bytes / 4, &res);
  OSSafeReleaseNULL(map);
  args->structureOutput = OSData::withBytes(&res, sizeof(res));
  return args->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

static kern_return_t
set_card_range(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  return driver->SetCardRange(args->scalarInput[0], args->scalarInput[1]);
}

static kern_return_t
set_cu_window(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  return driver->SetCuWindow(args->scalarInput[0], args->scalarInput[1]);
}

static kern_return_t
set_platform(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver || !args->structureInput)
    return kIOReturnBadArgument;
  return driver->SetPlatform(args->structureInput->getBytesNoCopy(),
                             args->structureInput->getLength());
}

static kern_return_t
get_platform(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  xrt_dext_platform plat = {};
  kern_return_t ret = driver->GetPlatform(&plat, sizeof(plat));
  if (ret != kIOReturnSuccess)
    return ret;
  args->structureOutput = OSData::withBytes(&plat, sizeof(plat));
  return args->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

static kern_return_t
load_bitstream(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  uint64_t flags = args->scalarInput[0];
  uint32_t target_mhz[XRT_LOAD_CLOCKS] = { static_cast<uint32_t>(args->scalarInput[1]),
                                          static_cast<uint32_t>(args->scalarInput[2]),
                                          static_cast<uint32_t>(args->scalarInput[8]) };

  // Small inputs arrive as OSData, large ones (the bitstream) as a
  // descriptor of the caller's buffer that we map read-only.
  const uint32_t* words = nullptr;
  uint64_t bytes = 0;
  IOMemoryMap* map = nullptr;
  if (args->structureInput) {
    words = static_cast<const uint32_t*>(args->structureInput->getBytesNoCopy());
    bytes = args->structureInput->getLength();
  }
  else if (args->structureInputDescriptor) {
    kern_return_t ret = args->structureInputDescriptor->CreateMapping(
      kIOMemoryMapReadOnly, 0, 0, 0, 0, &map);
    if (ret != kIOReturnSuccess) {
      LOG("mapping bitstream failed 0x%x", ret);
      return ret;
    }
    words = reinterpret_cast<const uint32_t*>(map->GetAddress());
    bytes = map->GetLength();
  }
  if ((bytes % 4) != 0) {
    OSSafeReleaseNULL(map);
    return kIOReturnBadArgument;
  }

  uint32_t ulp_clk[XRT_LOAD_CLOCKS] = { static_cast<uint32_t>(args->scalarInput[5]),
                                       static_cast<uint32_t>(args->scalarInput[6]),
                                       static_cast<uint32_t>(args->scalarInput[7]) };
  uint32_t ucs = static_cast<uint32_t>(args->scalarInput[9]);
  uint32_t freq_cnt[XRT_LOAD_CLOCKS] = { static_cast<uint32_t>(args->scalarInput[10]),
                                        static_cast<uint32_t>(args->scalarInput[11]),
                                        static_cast<uint32_t>(args->scalarInput[12]) };
  xrt_dext_load_result res = {};
  driver->LoadBitstream(words, bytes / 4, flags, target_mhz, ulp_clk, ucs, freq_cnt, &res);
  if (res.status == kIOReturnSuccess && !(flags & XRT_DEXT_LOAD_STATE_ONLY)) {
    uint8_t uuid[16];
    memcpy(uuid, &args->scalarInput[3], 8);
    memcpy(uuid + 8, &args->scalarInput[4], 8);
    driver->SetXclbinUuid(uuid);
  }
  OSSafeReleaseNULL(map);
  args->structureOutput = OSData::withBytes(&res, sizeof(res));
  // The result struct carries the status; the call itself succeeds so the
  // caller always gets the diagnostics.
  return args->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

static kern_return_t
get_xclbin_uuid(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  uint8_t uuid[16];
  driver->CopyXclbinUuid(uuid);
  args->structureOutput = OSData::withBytes(uuid, sizeof(uuid));
  return args->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

static kern_return_t
dma_selftest(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  xrt_dext_dma_result res = {};
  driver->DmaSelfTest(args->scalarInput[0], args->scalarInput[1], args->scalarInput[2], &res);
  args->structureOutput = OSData::withBytes(&res, sizeof(res));
  return args->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

static kern_return_t
sync(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  uint64_t card = args->scalarInput[0], length = args->scalarInput[1];
  bool to_card = args->scalarInput[2] == XRT_DMA_TO_CARD;
  uint32_t status = 0;
  kern_return_t ret = kIOReturnSuccess;

  // Large buffers arrive as descriptors of the caller's memory: DMA straight
  // into / out of it. Small ones arrive as OSData (in) or must be returned
  // as OSData (out): go through a dext bounce buffer.
  IOMemoryDescriptor* direct = to_card ? args->structureInputDescriptor : args->structureOutputDescriptor;
  if (direct) {
    uint64_t dlen = 0;
    direct->GetLength(&dlen);
    if (dlen < length)
      return kIOReturnBadArgument;
    ret = driver->DmaTransfer(direct, 0, length, card, to_card, &status);
  }
  else {
    if (to_card && (!args->structureInput || args->structureInput->getLength() < length))
      return kIOReturnBadArgument;
    // The bounce buffer is the driver's and is made once: allocating and wiring
    // one per transfer cost about 20 us, which at this size was a quarter of the
    // whole call (item 52). Acquire holds a lock until Release.
    IOMemoryDescriptor* bounce = nullptr;
    uint64_t bva = 0;
    if ((ret = driver->BounceAcquire(length, &bounce, &bva)) != kIOReturnSuccess)
      return ret;
    if (to_card)
      memcpy(reinterpret_cast<void*>(bva), args->structureInput->getBytesNoCopy(), length);
    ret = driver->DmaTransfer(bounce, 0, length, card, to_card, &status);
    if (ret == kIOReturnSuccess && !to_card) {
      args->structureOutput = OSData::withBytes(reinterpret_cast<const void*>(bva), length);
      if (!args->structureOutput)
        ret = kIOReturnNoMemory;
    }
    driver->BounceRelease();
  }
  if (args->scalarOutputCount >= 1)
    args->scalarOutput[0] = status;
  return ret;
}

static kern_return_t
cu_write(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver || !args->structureInput)
    return kIOReturnBadArgument;
  auto len = args->structureInput->getLength();
  if (len == 0 || (len % 4))
    return kIOReturnBadArgument;
  return driver->CuWrite(args->scalarInput[0],
                         static_cast<const uint32_t*>(args->structureInput->getBytesNoCopy()), len / 4);
}

// Does nothing, and accepts any shape of arguments: the cost of this call
// is the floor of a call into the dext, and varying the arguments shows
// what each part of the shape costs (tools/lat_parts.cpp).
static kern_return_t
nop(OSObject*, void*, IOUserClientMethodArguments* args)
{
  for (uint32_t i = 0; i < args->scalarOutputCount; ++i)
    args->scalarOutput[i] = 0;
  return kIOReturnSuccess;
}

// One call per command. The argument words normally ride in the scalar
// array: a structure input of any size costs about 10 us on top of a call,
// while extra scalars cost nothing (tools/lat_parts.cpp).
static kern_return_t
cu_exec(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver || args->scalarInputCount < 2)
    return kIOReturnBadArgument;
  uint64_t cu_base = args->scalarInput[0];
  uint64_t arg_offset = args->scalarInput[1] & 0xffffffffULL;
  uint64_t count = args->scalarInput[1] >> 32;

  uint32_t ctrl = 0;
  kern_return_t ret;
  if (count > XRT_CU_EXEC_INLINE_WORDS) {
    // Long register maps: the words come in the structure input.
    if (!args->structureInput || args->structureInput->getLength() != count * 4)
      return kIOReturnBadArgument;
    ret = driver->CuExec(cu_base, arg_offset,
                         static_cast<const uint32_t*>(args->structureInput->getBytesNoCopy()),
                         count, &ctrl);
  }
  else {
    if (args->scalarInputCount < 2 + (count + 1) / 2)
      return kIOReturnBadArgument;
    uint32_t words[XRT_CU_EXEC_INLINE_WORDS];
    for (uint64_t i = 0; i < count; ++i)
      words[i] = static_cast<uint32_t>(args->scalarInput[2 + i / 2] >> (32 * (i & 1)));
    ret = driver->CuExec(cu_base, arg_offset, words, count, &ctrl);
  }
  // Hand back what the control register held, so the caller does not need a
  // read of its own: an MMIO read over Thunderbolt costs 3.3 us.
  if (args->scalarOutputCount >= 1)
    args->scalarOutput[0] = ctrl;
  return ret;
}

static kern_return_t
set_section(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver || !args->structureInput)
    return kIOReturnBadArgument;
  return driver->SetSection(static_cast<uint32_t>(args->scalarInput[0]),
                            args->structureInput->getBytesNoCopy(), args->structureInput->getLength());
}

static kern_return_t
get_section(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnBadArgument;
  OSData* data = driver->CopySection(static_cast<uint32_t>(args->scalarInput[0]));
  if (!data)
    return kIOReturnNotFound;
  uint64_t total = data->getLength();
  uint64_t offset = args->scalarInput[1];
  args->scalarOutput[0] = total;
  kern_return_t ret = kIOReturnSuccess;
  if (offset < total) {
    uint64_t n = total - offset;
    if (n > XRT_SECTION_CHUNK)
      n = XRT_SECTION_CHUNK;
    args->structureOutput = OSData::withBytes(static_cast<const uint8_t*>(data->getBytesNoCopy()) + offset, n);
    if (!args->structureOutput)
      ret = kIOReturnNoMemory;
  }
  data->release();
  return ret;
}

static XrtUser*
user_driver(XrtUserClient* self)
{
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return nullptr;
  uint8_t info[sizeof(xrt_dext_device_info)] = {};
  driver->CopyDeviceInfo(info, sizeof(info));
  // Device memory belongs to the user function.
  return reinterpret_cast<xrt_dext_device_info*>(info)->func == 1 ? driver : nullptr;
}

static kern_return_t
mem_alloc(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = user_driver(self);
  if (!driver)
    return kIOReturnNotPermitted;
  uint64_t addr = 0;
  kern_return_t ret = driver->MemAlloc(args->scalarInput[0], args->scalarInput[1], args->scalarInput[2],
                                       self, &addr);
  args->scalarOutput[0] = addr;
  return ret;
}

static kern_return_t
mem_free(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = user_driver(self);
  if (!driver)
    return kIOReturnNotPermitted;
  return driver->MemFree(args->scalarInput[0], self);
}

static kern_return_t
mem_stat(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = user_driver(self);
  if (!driver)
    return kIOReturnNotPermitted;
  uint64_t used = 0, count = 0;
  driver->MemStat(args->scalarInput[0], args->scalarInput[1], &used, &count);
  args->scalarOutput[0] = used;
  args->scalarOutput[1] = count;
  return kIOReturnSuccess;
}

static kern_return_t
cu_ctx_open(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = user_driver(self);
  if (!driver)
    return kIOReturnNotPermitted;
  return driver->CuCtxOpen(static_cast<uint32_t>(args->scalarInput[0]),
                           static_cast<uint32_t>(args->scalarInput[1]),
                           args->scalarInput[2], self);
}

static kern_return_t
cu_ctx_close(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = user_driver(self);
  if (!driver)
    return kIOReturnNotPermitted;
  return driver->CuCtxClose(static_cast<uint32_t>(args->scalarInput[0]), self);
}

static kern_return_t
partition_hold(OSObject* target, void*, IOUserClientMethodArguments*)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = user_driver(self);
  if (!driver)
    return kIOReturnNotPermitted;
  return driver->PartitionHold(self);
}

static kern_return_t
partition_release(OSObject* target, void*, IOUserClientMethodArguments*)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = user_driver(self);
  if (!driver)
    return kIOReturnNotPermitted;
  return driver->PartitionRelease(self);
}

static kern_return_t
hot_reset(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  xrt_dext_reset_result res = {};
  driver->HotReset(&res);
  args->structureOutput = OSData::withBytes(&res, sizeof(res));
  return args->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

static kern_return_t
dma_mode(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtUserClient, target);
  auto driver = user_driver(self);
  if (!driver)
    return kIOReturnNotPermitted;
  uint64_t now = 0;
  kern_return_t ret = driver->SetDmaMode(args->scalarInput[0], &now);
  args->scalarOutput[0] = now;
  return ret;
}

// The flash selectors are NOT here. XRT_DEXT_SPI_XFER and XRT_DEXT_SPI_REG live on
// XrtFlashClient, which is entitlement-gated and exclusive - see XrtFlashClient.cpp
// and XRT_DEXT_CLIENT_FLASH. This client cannot reach the flash controller at all,
// which is the whole point: those selectors can erase the factory image.

static const IOUserClientMethodDispatch s_methods[XRT_DEXT_SELECTOR_COUNT] = {
  [XRT_DEXT_GET_DEVICE_INFO] = {
    .function = &get_device_info,
    .checkCompletionExists = false,
    .checkScalarInputCount = 0,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = sizeof(xrt_dext_device_info),
  },
  [XRT_DEXT_READ_ROM] = {
    .function = &read_rom,
    .checkCompletionExists = false,
    .checkScalarInputCount = 0,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = XRT_DEXT_ROM_SIZE,
  },
  [XRT_DEXT_LOAD_BITSTREAM] = {
    .function = &load_bitstream,
    .checkCompletionExists = false,
    .checkScalarInputCount = 13,
    .checkStructureInputSize = kIOUserClientVariableStructureSize,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = sizeof(xrt_dext_load_result),
  },
  [XRT_DEXT_GET_XCLBIN_UUID] = {
    .function = &get_xclbin_uuid,
    .checkCompletionExists = false,
    .checkScalarInputCount = 0,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = 16,
  },
  [XRT_DEXT_DMA_SELFTEST] = {
    .function = &dma_selftest,
    .checkCompletionExists = false,
    .checkScalarInputCount = 3,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = sizeof(xrt_dext_dma_result),
  },
  [XRT_DEXT_SYNC] = {
    .function = &sync,
    .checkCompletionExists = false,
    .checkScalarInputCount = 3,
    .checkStructureInputSize = kIOUserClientVariableStructureSize,
    .checkScalarOutputCount = 1,
    .checkStructureOutputSize = kIOUserClientVariableStructureSize,
  },
  [XRT_DEXT_CU_WRITE] = {
    .function = &cu_write,
    .checkCompletionExists = false,
    .checkScalarInputCount = 1,
    .checkStructureInputSize = kIOUserClientVariableStructureSize,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_SET_SECTION] = {
    .function = &set_section,
    .checkCompletionExists = false,
    .checkScalarInputCount = 1,
    .checkStructureInputSize = kIOUserClientVariableStructureSize,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_GET_SECTION] = {
    .function = &get_section,
    .checkCompletionExists = false,
    .checkScalarInputCount = 2,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 1,
    .checkStructureOutputSize = kIOUserClientVariableStructureSize,
  },
  [XRT_DEXT_MEM_ALLOC] = {
    .function = &mem_alloc,
    .checkCompletionExists = false,
    .checkScalarInputCount = 3,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 1,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_MEM_FREE] = {
    .function = &mem_free,
    .checkCompletionExists = false,
    .checkScalarInputCount = 1,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_MEM_STAT] = {
    .function = &mem_stat,
    .checkCompletionExists = false,
    .checkScalarInputCount = 2,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 2,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_HOT_RESET] = {
    .function = &hot_reset,
    .checkCompletionExists = false,
    .checkScalarInputCount = 0,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = sizeof(xrt_dext_reset_result),
  },
  [XRT_DEXT_DMA_MODE] = {
    .function = &dma_mode,
    .checkCompletionExists = false,
    .checkScalarInputCount = 1,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 1,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_NOP] = {
    .function = &nop,
    .checkCompletionExists = false,
    .checkScalarInputCount = kIOUserClientVariableStructureSize,
    .checkStructureInputSize = kIOUserClientVariableStructureSize,
    .checkScalarOutputCount = kIOUserClientVariableStructureSize,
    .checkStructureOutputSize = kIOUserClientVariableStructureSize,
  },
  [XRT_DEXT_CU_EXEC] = {
    .function = &cu_exec,
    .checkCompletionExists = false,
    .checkScalarInputCount = kIOUserClientVariableStructureSize,
    .checkStructureInputSize = kIOUserClientVariableStructureSize,
    .checkScalarOutputCount = 1,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_PARTITION_HOLD] = {
    .function = &partition_hold,
    .checkCompletionExists = false,
    .checkScalarInputCount = 0,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_PARTITION_RELEASE] = {
    .function = &partition_release,
    .checkCompletionExists = false,
    .checkScalarInputCount = 0,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_CU_CTX_OPEN] = {
    .function = &cu_ctx_open,
    .checkCompletionExists = false,
    .checkScalarInputCount = 3,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_CFG_READ] = {
    .function = &cfg_read,
    .checkCompletionExists = false,
    .checkScalarInputCount = 1,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 1,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_CU_CTX_CLOSE] = {
    .function = &cu_ctx_close,
    .checkCompletionExists = false,
    .checkScalarInputCount = 1,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_SET_PLATFORM] = {
    .function = &set_platform,
    .checkCompletionExists = false,
    .checkScalarInputCount = 0,
    .checkStructureInputSize = sizeof(xrt_dext_platform),
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_SET_CU_WINDOW] = {
    .function = &set_cu_window,
    .checkCompletionExists = false,
    .checkScalarInputCount = 2,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_CLOCK_MEASURE] = {
    .function = &clock_measure,
    .checkCompletionExists = false,
    .checkScalarInputCount = 1,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 1,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_CMC_BOARD_INFO] = {
    .function = &cmc_board_info,
    .checkCompletionExists = false,
    .checkScalarInputCount = 0,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = sizeof(xrt_dext_cmc_bdinfo),
  },
  [XRT_DEXT_ERT_START] = {
    .function = &ert_start,
    .checkCompletionExists = false,
    .checkScalarInputCount = 0,
    .checkStructureInputSize = kIOUserClientVariableStructureSize,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = sizeof(xrt_dext_ert_result),
  },
  [XRT_DEXT_LOAD_CMC] = {
    .function = &load_cmc,
    .checkCompletionExists = false,
    .checkScalarInputCount = 0,
    .checkStructureInputSize = kIOUserClientVariableStructureSize,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = sizeof(xrt_dext_cmc_result),
  },
  [XRT_DEXT_SET_CARD_RANGE] = {
    .function = &set_card_range,
    .checkCompletionExists = false,
    .checkScalarInputCount = 2,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_GET_PLATFORM] = {
    .function = &get_platform,
    .checkCompletionExists = false,
    .checkScalarInputCount = 0,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = sizeof(xrt_dext_platform),
  },
#if XRT_MACOS_ENABLE_INNOVA2
  [XRT_DEXT_REG_WRITE] = {
    .function = &reg_write,
    .checkCompletionExists = false,
    .checkScalarInputCount = 2,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 1,
    .checkStructureOutputSize = 0,
  },
#endif
};

kern_return_t
IMPL(XrtUserClient, CopyClientMemoryForType)
{
  auto driver = OSDynamicCast(XrtUser, GetProvider());
  if (!driver)
    return kIOReturnNoDevice;

  // One CU's register page, read-write, for a client holding that CU
  // exclusively. This is the only mapping that is not read-only; the driver
  // decides whether this client may have it.
  if (type >= XRT_DEXT_MAP_CU_RW_BASE) {
    uint32_t cu = static_cast<uint32_t>(type - XRT_DEXT_MAP_CU_RW_BASE);
    kern_return_t ret = driver->CopyCuPageMemory(cu, this, memory);
    if (ret != kIOReturnSuccess)
      LOG("no read-write mapping for CU %u: 0x%x", cu, ret);
    return ret;
  }

  kern_return_t ret = driver->CopyBarMemory(static_cast<uint32_t>(type), memory);
  if (ret == kIOReturnSuccess && options)
    *options |= kIOUserClientMemoryReadOnly;   // whole BARs stay read-only
  if (ret != kIOReturnSuccess)
    LOG("map BAR%llu failed 0x%x", type, ret);
  return ret;
}

kern_return_t
XrtUserClient::ExternalMethod(uint64_t selector, IOUserClientMethodArguments* arguments,
                              const IOUserClientMethodDispatch*, OSObject*, void*)
{
  if (selector >= XRT_DEXT_SELECTOR_COUNT)
    return kIOReturnUnsupported;
  // The selector enum has numeric gaps (25 to 27), so those table entries are
  // zero-filled and their function pointer is null. Whether the framework
  // null-checks it before calling is not something to rely on.
  if (!s_methods[selector].function)
    return kIOReturnUnsupported;
  return super::ExternalMethod(selector, arguments, &s_methods[selector], this, nullptr);
}
