// SPDX-License-Identifier: Apache-2.0
#include <os/log.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOUserClient.h>
#include <DriverKit/OSData.h>
#include <string.h>

#include "XrtCx5.h"
#include "XrtCx5Client.h"
#include "xrt_cx5_abi.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "xrt_cx5 client: " fmt, ##__VA_ARGS__)

struct XrtCx5Client_IVars { XrtCx5* driver = nullptr; };

bool
XrtCx5Client::init()
{
  if (!super::init())
    return false;
  ivars = IONewZero(XrtCx5Client_IVars, 1);
  return ivars != nullptr;
}

kern_return_t
IMPL(XrtCx5Client, Start)
{
  kern_return_t ret = Start(provider, SUPERDISPATCH);
  if (ret != kIOReturnSuccess)
    return ret;
  ivars->driver = OSDynamicCast(XrtCx5, provider);
  if (!ivars->driver)
    return kIOReturnNoDevice;
  ivars->driver->retain();
  return kIOReturnSuccess;
}

kern_return_t
IMPL(XrtCx5Client, Stop)
{
  return Stop(provider, SUPERDISPATCH);
}

void
XrtCx5Client::free()
{
  if (ivars && ivars->driver)
    ivars->driver->release();
  IOSafeDeleteNULL(ivars, XrtCx5Client_IVars, 1);
  super::free();
}

static kern_return_t
cx5_info(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtCx5Client, target);
  auto drv = self ? OSDynamicCast(XrtCx5, self->GetProvider()) : nullptr;
  if (!drv)
    return kIOReturnNoDevice;
  xrt_cx5_info info = {};
  kern_return_t ret = drv->CopyInfo(&info, sizeof(info));
  if (ret != kIOReturnSuccess)
    return ret;
  args->structureOutput = OSData::withBytes(&info, sizeof(info));
  return args->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

static kern_return_t
cx5_gw_read(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtCx5Client, target);
  auto drv = self ? OSDynamicCast(XrtCx5, self->GetProvider()) : nullptr;
  if (!drv)
    return kIOReturnNoDevice;
  uint32_t v = 0;
  kern_return_t ret = drv->GatewayRead(static_cast<uint32_t>(args->scalarInput[0]),
                                       static_cast<uint32_t>(args->scalarInput[1]), &v);
  if (args->scalarOutputCount >= 1)
    args->scalarOutput[0] = v;
  return ret;
}

static kern_return_t
cx5_gw_write(OSObject* target, void*, IOUserClientMethodArguments* args)
{
  auto self = OSDynamicCast(XrtCx5Client, target);
  auto drv = self ? OSDynamicCast(XrtCx5, self->GetProvider()) : nullptr;
  if (!drv)
    return kIOReturnNoDevice;
  return drv->GatewayWrite(static_cast<uint32_t>(args->scalarInput[0]),
                           static_cast<uint32_t>(args->scalarInput[1]),
                           static_cast<uint32_t>(args->scalarInput[2]));
}

static const IOUserClientMethodDispatch s_methods[XRT_CX5_SELECTOR_COUNT] = {
  [XRT_CX5_GET_INFO] = {
    .function = &cx5_info,
    .checkCompletionExists = false,
    .checkScalarInputCount = 0,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = sizeof(xrt_cx5_info),
  },
  [XRT_CX5_GATEWAY_READ] = {
    .function = &cx5_gw_read,
    .checkCompletionExists = false,
    .checkScalarInputCount = 2,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 1,
    .checkStructureOutputSize = 0,
  },
  [XRT_CX5_GATEWAY_WRITE] = {
    .function = &cx5_gw_write,
    .checkCompletionExists = false,
    .checkScalarInputCount = 3,
    .checkStructureInputSize = 0,
    .checkScalarOutputCount = 0,
    .checkStructureOutputSize = 0,
  },
};

kern_return_t
XrtCx5Client::ExternalMethod(uint64_t selector, IOUserClientMethodArguments* arguments,
                             const IOUserClientMethodDispatch*, OSObject*, void*)
{
  if (selector >= XRT_CX5_SELECTOR_COUNT)
    return kIOReturnUnsupported;
  if (!s_methods[selector].function)
    return kIOReturnUnsupported;
  return super::ExternalMethod(selector, arguments, &s_methods[selector], this, nullptr);
}
