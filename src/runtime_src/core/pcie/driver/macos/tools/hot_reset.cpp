// SPDX-License-Identifier: Apache-2.0
// PCIe hot reset of the card, straight through the dext (XRT_DEXT_HOT_RESET on
// the management function). What xrt-smi's reset does, without needing the
// installed runtime to agree about reset type names.
//
// This exists because an XDMA engine can be left wedged - status BUSY with 0
// descriptors done - by a transfer aimed at an AXI target that never answers,
// and nothing short of a link reset clears it: the Run bit is already 0, and
// stopping and restarting the engine, reloading the xclbin and taking the
// partition hold all leave BUSY set. xdma_peek shows the state; this clears it.
//
// It takes the user partition down with it, so whatever was loaded has to be
// loaded again afterwards.
//
// The partition hold around the reset is not optional. HotReset runs in the
// management function's dext process and cannot reach the user function's, so
// the hold and its release are the only thing that tells the user side its
// cached DMA mappings and register state are stale. Without it the engine
// wedges on the first transfer after the reset - busy, 0 descriptors done - and
// needs yet another reset. This is what the shim's hot_reset does
// (dext_client.cpp), and the reason it is spelled out here.
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include "xrt_dext_abi.h"

// The user function's dext instance, or 0 if it is not there.
static io_connect_t
open_user_function()
{
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it))
    return 0;
  io_connect_t found = 0;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_connect_t conn;
    if (IOServiceOpen(svc, mach_task_self(), 0, &conn))
      continue;
    xrt_dext_device_info info{};
    size_t sz = sizeof(info);
    if (IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &sz) == KERN_SUCCESS
        && info.func == 1) {
      found = conn;
      break;
    }
    IOServiceClose(conn);
  }
  IOObjectRelease(it);
  return found;
}

int
main()
{
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it)) {
    std::fprintf(stderr, "no %s service - is the dext loaded?\n", XRT_DEXT_USER_CLASS);
    return 1;
  }
  int rc = 1;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_connect_t conn;
    if (IOServiceOpen(svc, mach_task_self(), 0, &conn))
      continue;
    xrt_dext_device_info info{};
    size_t sz = sizeof(info);
    // A discarded status leaves info zeroed, which would read as func 0.
    if (IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &sz)
        || info.func != 0) {
      IOServiceClose(conn);
      continue;
    }
    // Hold the user partition across the reset. The hold is owned by the
    // connection, so it is released when this process closes it or dies.
    io_connect_t uconn = open_user_function();
    if (uconn) {
      kern_return_t h = IOConnectCallScalarMethod(uconn, XRT_DEXT_PARTITION_HOLD,
                                                  nullptr, 0, nullptr, nullptr);
      if (h) {
        std::printf("partition hold refused 0x%x - another process is holding it\n", h);
        IOServiceClose(uconn);
        IOServiceClose(conn);
        break;
      }
    }
    else {
      std::printf("warning: no user function to hold; its DMA state will be stale\n");
    }

    xrt_dext_reset_result res{};
    size_t rsz = sizeof(res);
    kern_return_t kr = IOConnectCallStructMethod(conn, XRT_DEXT_HOT_RESET, nullptr, 0, &res, &rsz);
    IOServiceClose(conn);
    if (uconn) {
      // The release is what drops the caches a reset invalidated.
      IOConnectCallScalarMethod(uconn, XRT_DEXT_PARTITION_RELEASE, nullptr, 0, nullptr, nullptr);
      IOServiceClose(uconn);
    }
    if (kr) {
      std::printf("HOT_RESET call failed 0x%x\n", kr);
      break;
    }
    std::printf("hot reset %02x:%02x.%u: status 0x%x, reset 0x%x, %u ms, "
                "PCI command after 0x%04x, %u firewall clear round(s)\n",
                info.bus, info.dev, info.func, res.status, res.reset_status,
                res.elapsed_ms, res.command_after, res.clear_rounds);
    // XRT_MGMT_FIREWALLS is 4 on an XDMA 5.x shell and 6 on a U50, so loop
    // rather than name four of them.
    std::printf("  firewalls before");
    for (uint32_t i = 0; i < XRT_MGMT_FIREWALLS; ++i)
      std::printf(" 0x%08x", res.fw_before[i]);
    std::printf("\n  firewalls after ");
    for (uint32_t i = 0; i < XRT_MGMT_FIREWALLS; ++i)
      std::printf(" 0x%08x", res.fw_after[i]);
    std::printf("\n  feature ROM magic 0x%08x (expect 0x%08x), gate 0x%x, clock status 0x%x 0x%x\n",
                res.rom_magic_after, XRT_DEXT_ROM_MAGIC, res.gate_after,
                res.clk_status_after[0], res.clk_status_after[1]);
    rc = res.status ? 1 : 0;
  }
  IOObjectRelease(it);
  if (rc == 1)
    std::fprintf(stderr, "no management function found\n");
  return rc;
}
