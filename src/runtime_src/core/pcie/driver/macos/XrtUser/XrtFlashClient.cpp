// SPDX-License-Identifier: Apache-2.0
#include <os/log.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOUserClient.h>
#include <DriverKit/OSData.h>
#include <string.h>

#include "XrtUser.h"
#include "XrtFlashClient.h"
#include "xrt_dext_abi.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "xrt_flash client: " fmt, ##__VA_ARGS__)

struct XrtFlashClient_IVars
{
  XrtUser* driver = nullptr;
  bool     claimed = false;
};

bool
XrtFlashClient::init()
{
  if (!super::init())
    return false;
  ivars = IONewZero(XrtFlashClient_IVars, 1);
  return ivars != nullptr;
}

kern_return_t
IMPL(XrtFlashClient, Start)
{
  kern_return_t ret = Start(provider, SUPERDISPATCH);
  if (ret != kIOReturnSuccess)
    return ret;
  ivars->driver = OSDynamicCast(XrtUser, provider);
  if (!ivars->driver)
    return kIOReturnNoDevice;

  // One connection at a time. An erase or a page program is hundreds of single
  // register accesses, and a second connection doing its own SPI transaction in
  // the middle of that resets the controller under it - which leaves a partially
  // written image. Refusing the second open is how that is prevented, and it is
  // checked before the connection exists rather than per call.
  ret = ivars->driver->FlashClaim(this);
  if (ret != kIOReturnSuccess) {
    LOG("refusing the connection: the flash is already claimed (0x%x)", ret);
    return ret;
  }
  ivars->claimed = true;
  ivars->driver->retain();
  return kIOReturnSuccess;
}

kern_return_t
IMPL(XrtFlashClient, Stop)
{
  // Reached whether the process closed the connection or was killed. Dropping the
  // claim here is what makes a SIGKILL mid-program safe to recover from: the
  // release also disables the SPI core, which otherwise keeps holding STARTUPE3
  // and breaks the next bitstream download. A user-space destructor cannot cover
  // this - and upstream's xspi.cpp calls exit() directly on some error paths, so
  // it is not a rare case.
  if (ivars && ivars->claimed) {
    if (auto driver = OSDynamicCast(XrtUser, provider))
      driver->FlashRelease(this);
    ivars->claimed = false;
  }
  return Stop(provider, SUPERDISPATCH);
}

void
XrtFlashClient::free()
{
  if (ivars && ivars->driver)
    ivars->driver->release();
  IOSafeDeleteNULL(ivars, XrtFlashClient_IVars, 1);
  super::free();
}

// XRT_DEXT_GET_DEVICE_INFO, so a caller can confirm which function it reached
// without also opening a default client. Read-only.
static kern_return_t
flash_device_info(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtFlashClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;

  xrt_dext_device_info info = {};
  kern_return_t ret = driver->CopyDeviceInfo(&info, sizeof(info));
  if (ret != kIOReturnSuccess)
    return ret;
  args->structureOutput = OSData::withBytes(&info, sizeof(info));
  return args->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

// XRT_DEXT_SPI_XFER: one whole transaction, the register sequence owned by the
// driver. This is the interface for reads.
static kern_return_t
flash_spi_xfer(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtFlashClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  const uint32_t slave = static_cast<uint32_t>(args->scalarInput[0]);
  const uint32_t want = static_cast<uint32_t>(args->scalarInput[1]);
  if (want > XRT_SPI_MAX_XFER)
    return kIOReturnBadArgument;
  if (!args->structureInput || args->structureInputDescriptor)
    return kIOReturnBadArgument;
  const uint32_t tx_len = static_cast<uint32_t>(args->structureInput->getLength());
  if (tx_len == 0 || tx_len > XRT_SPI_MAX_XFER)
    return kIOReturnBadArgument;

  uint8_t rx[XRT_SPI_MAX_XFER] = {};
  xrt_dext_spi_result res = {};
  kern_return_t ret = driver->SpiTransfer(slave, args->structureInput->getBytesNoCopy(),
                                          tx_len, rx, want, &res);
  args->structureOutput = OSData::withBytes(rx, res.rx_bytes ? res.rx_bytes : 1);
  if (!args->structureOutput)
    return kIOReturnNoMemory;
  if (args->scalarOutputCount >= 6) {
    args->scalarOutput[0] = res.status;
    args->scalarOutput[1] = res.sr_before;
    args->scalarOutput[2] = res.sr_after;
    args->scalarOutput[3] = res.rx_bytes;
    args->scalarOutput[4] = res.slave;
    args->scalarOutput[5] = res.waited_us;
  }
  return ret;
}

// XRT_DEXT_SPI_REG: one 32-bit access to one register on the allowlist. This is
// what lets upstream's erase and program sequencing run unchanged, and it is the
// reason this whole class is entitlement-gated.
static kern_return_t
flash_spi_reg(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtFlashClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  const uint32_t reg   = static_cast<uint32_t>(args->scalarInput[0]);
  const uint32_t value = static_cast<uint32_t>(args->scalarInput[1]);
  const bool write     = args->scalarInput[2] != 0;
  uint32_t got = 0;
  kern_return_t ret = driver->SpiReg(reg, value, write, &got);
  if (args->scalarOutputCount >= 1)
    args->scalarOutput[0] = got;
  return ret;
}

// XRT_DEXT_SPI_REG_AUDIT: what the TX-stream shadow decoded. Read-only, and the
// point of the dry run - it says which flash addresses a program run reached.
static kern_return_t
flash_spi_audit(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtFlashClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  const bool reset = args->scalarInput[0] != 0;
  xrt_dext_spi_audit a = {};
  kern_return_t ret = driver->SpiRegAudit(&a, sizeof(a), reset);
  if (ret != kIOReturnSuccess)
    return ret;
  args->structureOutput = OSData::withBytes(&a, sizeof(a));
  return args->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

// XRT_DEXT_SPI_REG_MODE: choose audit-only or dry-run. See XRT_SPI_GUARD_*.
static kern_return_t
flash_spi_mode(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtFlashClient, target);
  auto driver = self ? OSDynamicCast(XrtUser, self->GetProvider()) : nullptr;
  if (!driver)
    return kIOReturnNoDevice;
  uint32_t was = 0;
  kern_return_t ret = driver->SpiRegMode(static_cast<uint32_t>(args->scalarInput[0]), &was);
  if (args->scalarOutputCount >= 1)
    args->scalarOutput[0] = was;
  return ret;
}

// Deliberately sparse: every selector this class does not name is unreachable
// through it, and every selector the DEFAULT client names is unreachable here.
// That is the point of the split - a flash connection can do nothing but flash.
static const IOUserClientMethodDispatch s_methods[XRT_DEXT_SELECTOR_COUNT] = {
  [XRT_DEXT_GET_DEVICE_INFO] = {
    .function = &flash_device_info,
    .checkCompletionExists = false,
    .checkScalarInputCount = 0,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = sizeof(xrt_dext_device_info),
  },
  [XRT_DEXT_SPI_XFER] = {
    .function = &flash_spi_xfer,
    .checkCompletionExists = false,
    .checkScalarInputCount = 2,
    .checkStructureInputSize = kIOUserClientVariableStructureSize,
    .checkScalarOutputCount = 6,
    .checkStructureOutputSize = kIOUserClientVariableStructureSize,
  },
  [XRT_DEXT_SPI_REG] = {
    .function = &flash_spi_reg,
    .checkCompletionExists = false,
    .checkScalarInputCount = 3,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 1,
    .checkStructureOutputSize = 0,
  },
  [XRT_DEXT_SPI_REG_AUDIT] = {
    .function = &flash_spi_audit,
    .checkCompletionExists = false,
    .checkScalarInputCount = 1,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = sizeof(xrt_dext_spi_audit),
  },
  [XRT_DEXT_SPI_REG_MODE] = {
    .function = &flash_spi_mode,
    .checkCompletionExists = false,
    .checkScalarInputCount = 1,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 1,
    .checkStructureOutputSize = 0,
  },
};

kern_return_t
XrtFlashClient::ExternalMethod(uint64_t selector, IOUserClientMethodArguments* arguments,
                               const IOUserClientMethodDispatch*, OSObject*, void*)
{
  if (selector >= XRT_DEXT_SELECTOR_COUNT)
    return kIOReturnUnsupported;
  // Most of the table is zero-filled, because this class names only three
  // selectors. A null function is how "not on this client" is expressed.
  if (!s_methods[selector].function)
    return kIOReturnUnsupported;
  return super::ExternalMethod(selector, arguments, &s_methods[selector], this, nullptr);
}
