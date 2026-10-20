// SPDX-License-Identifier: Apache-2.0
#include <os/log.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <PCIDriverKit/PCIDriverKit.h>

#include "XrtCx5.h"
#include "XrtCx5Client.h"
#include "xrt_cx5_abi.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "xrt_cx5: " fmt, ##__VA_ARGS__)

// PCI extended capability walk. The Mellanox gateway lives in a Vendor-Specific
// Capability, and there are two kinds with confusingly similar names:
//   - the legacy VSC, ID 0x09, in the 0x40..0xff capability list
//   - the extended VSEC, ID 0x000b, in the 0x100+ extended list
// mstflint's pciconf uses the LEGACY one (0x09) for ICMD, so that is what is looked
// for, while the extended list is walked too and logged - guessing which one this
// device presents would just be another thing to get wrong later.
struct XrtCx5_IVars
{
  IOPCIDevice* pci = nullptr;
  bool         opened = false;
  uint32_t     vsc = 0;            // legacy Vendor-Specific Capability offset
  uint32_t     vsec = 0;           // extended VSEC, informational
  uint32_t     gateway_ok = 0;     // set once a read has actually worked
  IOLock*      lock = nullptr;     // one gateway, one transaction at a time
};

bool
XrtCx5::init()
{
  if (!super::init())
    return false;
  ivars = IONewZero(XrtCx5_IVars, 1);
  if (!ivars)
    return false;
  ivars->lock = IOLockAlloc();
  return ivars->lock != nullptr;
}

kern_return_t
IMPL(XrtCx5, Start)
{
  kern_return_t ret = Start(provider, SUPERDISPATCH);
  if (ret != kIOReturnSuccess)
    return ret;

  ivars->pci = OSDynamicCast(IOPCIDevice, provider);
  if (!ivars->pci) {
    LOG("provider is not an IOPCIDevice");
    return kIOReturnNoDevice;
  }
  ivars->pci->retain();

  ret = ivars->pci->Open(this, 0);
  if (ret != kIOReturnSuccess) {
    LOG("IOPCIDevice::Open failed 0x%{public}x", ret);
    return ret;
  }
  ivars->opened = true;

  uint16_t vendor = 0, device = 0;
  ivars->pci->ConfigurationRead16(0x00, &vendor);
  ivars->pci->ConfigurationRead16(0x02, &device);
  uint8_t bus = 0, dev = 0, fn = 0;
  ivars->pci->GetBusDeviceFunction(&bus, &dev, &fn);
  LOG("claimed %{public}02x:%{public}02x.%{public}x  %{public}04x:%{public}04x "
      "(displacing AppleEthernetMLX5)", bus, dev, fn, vendor, device);

  // --- legacy capability list, looking for Vendor-Specific (0x09)
  uint8_t cap = 0;
  ivars->pci->ConfigurationRead8(0x34, &cap);          // capabilities pointer
  uint32_t vsc = 0;
  for (int guard = 0; cap >= 0x40 && guard < 48; ++guard) {
    uint8_t id = 0, next = 0;
    ivars->pci->ConfigurationRead8(cap, &id);
    ivars->pci->ConfigurationRead8(cap + 1, &next);
    if (id == 0x09) {
      uint8_t len = 0;
      ivars->pci->ConfigurationRead8(cap + 2, &len);
      LOG("legacy Vendor-Specific Capability at 0x%{public}02x, length %{public}u",
          cap, len);
      vsc = cap;
      ivars->vsc = cap;
      break;
    }
    if (next == cap)
      break;                                            // malformed list
    cap = next;
  }
  if (!vsc)
    LOG("no legacy Vendor-Specific Capability (0x09) found - ICMD needs it");

  // --- extended list, logged for completeness
  uint32_t off = 0x100;
  for (int guard = 0; off && guard < 64; ++guard) {
    uint32_t hdr = 0;
    ivars->pci->ConfigurationRead32(off, &hdr);
    if (!hdr || hdr == 0xffffffffu)
      break;
    const uint32_t id = hdr & 0xffff;
    if (id == 0x000b) {
      LOG("extended VSEC (0x000b) at 0x%{public}x", off);
      ivars->vsec = off;
    }
    const uint32_t next = (hdr >> 20) & 0xffc;
    if (next <= off)
      break;
    off = next;
  }

  RegisterService();
  return kIOReturnSuccess;
}

kern_return_t
IMPL(XrtCx5, Stop)
{
  if (ivars && ivars->opened && ivars->pci) {
    ivars->pci->Close(this, 0);
    ivars->opened = false;
  }
  return Stop(provider, SUPERDISPATCH);
}

void
XrtCx5::free()
{
  if (ivars && ivars->lock)
    IOLockFree(ivars->lock);
  if (ivars && ivars->pci)
    ivars->pci->release();
  IOSafeDeleteNULL(ivars, XrtCx5_IVars, 1);
  super::free();
}


// ---------------------------------------------------------------------------------
// The Vendor-Specific Capability gateway.
//
// Reproduced from mstflint's pciconf access method, which is how `mlxreg` reaches a
// Mellanox device with no driver loaded. The sequence per transfer:
//
//   1. take the semaphore - read a ticket from COUNTER, write it to SEMAPHORE, read
//      SEMAPHORE back and check it is still ours. Another agent (firmware, another
//      host function) can be mid-transfer, so this retries.
//   2. select the address space in CTRL and check CTRL's status field says it was
//      accepted. A space the device does not implement fails here rather than
//      returning plausible rubbish.
//   3. for a read: write the address to ADDR, poll ADDR until bit 31 clears, read DATA.
//      for a write: write DATA first, then ADDR with bit 31 set, and poll.
//   4. release the semaphore by writing 0.
//
// UNVERIFIED AGAINST HARDWARE at the time of writing - Apple's AppleEthernetMLX5 owns
// the device and macOS refuses to unload a bootKC dext, so this has never executed. The
// offsets and the status encoding are the parts most likely to be wrong; `gateway_ok`
// in xrt_cx5_info exists so a caller can tell "implemented" from "known to work".
// ---------------------------------------------------------------------------------

static kern_return_t
sem_take(XrtCx5_IVars* ivars)
{
  for (int attempt = 0; attempt < 64; ++attempt) {
    uint32_t ticket = 0, got = 0;
    ivars->pci->ConfigurationRead32(ivars->vsc + XRT_CX5_VSC_COUNTER, &ticket);
    ivars->pci->ConfigurationWrite32(ivars->vsc + XRT_CX5_VSC_SEMAPHORE, ticket);
    ivars->pci->ConfigurationRead32(ivars->vsc + XRT_CX5_VSC_SEMAPHORE, &got);
    if (got == ticket)
      return kIOReturnSuccess;
    IOSleep(1);
  }
  return kIOReturnBusy;
}

static void
sem_give(XrtCx5_IVars* ivars)
{
  ivars->pci->ConfigurationWrite32(ivars->vsc + XRT_CX5_VSC_SEMAPHORE, 0);
}

static kern_return_t
space_select(XrtCx5_IVars* ivars, uint32_t space)
{
  ivars->pci->ConfigurationWrite32(ivars->vsc + XRT_CX5_VSC_CTRL, space);
  uint32_t ctrl = 0;
  ivars->pci->ConfigurationRead32(ivars->vsc + XRT_CX5_VSC_CTRL, &ctrl);
  const uint32_t status = (ctrl >> XRT_CX5_VSC_CTRL_STATUS_SHIFT) & XRT_CX5_VSC_CTRL_STATUS_MASK;
  if (status != XRT_CX5_VSC_STATUS_OK) {
    LOG("address space 0x%{public}x refused, CTRL 0x%{public}08x (status %{public}u)",
        space, ctrl, status);
    return kIOReturnUnsupported;
  }
  return kIOReturnSuccess;
}

static kern_return_t
wait_addr(XrtCx5_IVars* ivars, uint32_t* addr_out)
{
  for (int guard = 0; guard < 2000; ++guard) {
    uint32_t a = 0;
    ivars->pci->ConfigurationRead32(ivars->vsc + XRT_CX5_VSC_ADDR, &a);
    if (!(a & XRT_CX5_VSC_ADDR_BUSY)) {
      if (addr_out)
        *addr_out = a;
      return kIOReturnSuccess;
    }
    IODelay(10);
  }
  return kIOReturnTimeout;
}

kern_return_t
XrtCx5::GatewayRead(uint32_t space, uint32_t addr, uint32_t* value)
{
  if (!value)
    return kIOReturnBadArgument;
  if (space != XRT_CX5_AS_CR_SPACE && space != XRT_CX5_AS_ICMD)
    return kIOReturnBadArgument;   // an unconsidered space is not one to expose
  if (addr & 3u)
    return kIOReturnBadArgument;
  if (!ivars->pci || !ivars->vsc)
    return kIOReturnNoResources;

  IOLockLock(ivars->lock);
  kern_return_t ret = sem_take(ivars);
  if (ret == kIOReturnSuccess) {
    ret = space_select(ivars, space);
    if (ret == kIOReturnSuccess) {
      ivars->pci->ConfigurationWrite32(ivars->vsc + XRT_CX5_VSC_ADDR, addr & ~XRT_CX5_VSC_ADDR_BUSY);
      ret = wait_addr(ivars, nullptr);
      if (ret == kIOReturnSuccess) {
        ivars->pci->ConfigurationRead32(ivars->vsc + XRT_CX5_VSC_DATA, value);
        ivars->gateway_ok = 1;
      }
    }
    sem_give(ivars);
  }
  IOLockUnlock(ivars->lock);
  return ret;
}

kern_return_t
XrtCx5::GatewayWrite(uint32_t space, uint32_t addr, uint32_t value)
{
  if (space != XRT_CX5_AS_CR_SPACE && space != XRT_CX5_AS_ICMD)
    return kIOReturnBadArgument;
  if (addr & 3u)
    return kIOReturnBadArgument;
  if (!ivars->pci || !ivars->vsc)
    return kIOReturnNoResources;

  IOLockLock(ivars->lock);
  kern_return_t ret = sem_take(ivars);
  if (ret == kIOReturnSuccess) {
    ret = space_select(ivars, space);
    if (ret == kIOReturnSuccess) {
      ivars->pci->ConfigurationWrite32(ivars->vsc + XRT_CX5_VSC_DATA, value);
      ivars->pci->ConfigurationWrite32(ivars->vsc + XRT_CX5_VSC_ADDR,
                                       (addr & ~XRT_CX5_VSC_ADDR_BUSY) | XRT_CX5_VSC_ADDR_BUSY);
      ret = wait_addr(ivars, nullptr);
    }
    sem_give(ivars);
  }
  IOLockUnlock(ivars->lock);
  return ret;
}

kern_return_t
XrtCx5::CopyInfo(void* out, uint64_t size)
{
  if (!out || size < sizeof(xrt_cx5_info))
    return kIOReturnBadArgument;
  auto i = static_cast<xrt_cx5_info*>(out);
  memset(i, 0, sizeof(*i));
  i->abi = XRT_CX5_ABI_VERSION;
  if (ivars->pci) {
    ivars->pci->ConfigurationRead16(0x00, &i->vendor);
    ivars->pci->ConfigurationRead16(0x02, &i->device);
    ivars->pci->GetBusDeviceFunction(&i->bus, &i->dev, &i->func);
  }
  i->vsc_offset = ivars->vsc;
  i->vsec_offset = ivars->vsec;
  i->gateway_ok = ivars->gateway_ok;
  return kIOReturnSuccess;
}

kern_return_t
IMPL(XrtCx5, NewUserClient)
{
  IOService* client = nullptr;
  kern_return_t ret = Create(this, "UserClientProperties", &client);
  if (ret != kIOReturnSuccess) {
    LOG("Create user client failed 0x%{public}x", ret);
    return ret;
  }
  *userClient = OSDynamicCast(IOUserClient, client);
  if (!*userClient) {
    client->release();
    return kIOReturnError;
  }
  return kIOReturnSuccess;
}
