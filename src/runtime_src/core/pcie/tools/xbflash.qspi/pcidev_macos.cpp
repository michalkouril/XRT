// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// The macOS backend for xbflash.qspi's device abstraction.
//
// On Linux this class mmaps the card's BAR and the flasher reaches the AXI Quad
// SPI controller with plain loads and stores. macOS has no such mapping to hand
// a user process: the card belongs to a DriverKit extension. So every register
// access becomes one call into the dext, which does the MMIO itself.
//
// What crosses this boundary is an offset WITHIN the SPI controller, not within
// a BAR. The dext locates the controller by probing and adds the base, so this
// process cannot name an address outside it; the dext additionally checks the
// offset against an allowlist of the seven registers this file's caller actually
// uses (XRT_SPI_REG_ALLOWED). The command-line --bar and --bar-offset are
// therefore ignored here, and saying so is better than silently accepting a
// number that does nothing.
//
// The point of going through this class rather than writing a macOS flasher is
// that xspi.cpp - the erase, program, bitstream-guard and dual-quad striping
// sequencing - then compiles unchanged. Programming flash is the one operation
// on this card where being wrong costs the card, and upstream's sequencing has
// been run against these parts for years.

#ifdef __APPLE__

#include <IOKit/IOKitLib.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include "pcidev.h"
#include "xrt_dext_abi.h"

namespace pcidev {

// The dext enforces this too, and its copy is the one that matters. Checking here
// as well turns a miscomputed offset into a legible local error instead of a
// kIOReturnBadArgument from the driver.
static bool
spi_reg_allowed(uint64_t off)
{
  static const uint32_t allowed[] = XRT_SPI_REG_ALLOWED;
  for (uint32_t a : allowed)
    if (off == a)
      return true;
  return false;
}

// The management function is where the boot flash hangs off. Find the dext
// instance bound to it, matching the BDF the caller named.
static io_connect_t
open_mgmt(uint16_t bus, uint16_t dev, uint16_t func)
{
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS),
                                   &it) != kIOReturnSuccess)
    return 0;

  io_connect_t found = 0;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_connect_t conn = 0;
    if (IOServiceOpen(svc, mach_task_self(), XRT_DEXT_CLIENT_FLASH, &conn) == kIOReturnSuccess) {
      xrt_dext_device_info info{};
      size_t sz = sizeof(info);
      if (IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0,
                                    &info, &sz) == kIOReturnSuccess
          && info.func == 0
          && info.bus == bus
          && info.dev == dev) {
        // The boot flash hangs off function 0, so that is the instance to talk
        // to whichever function of the pair the caller named: the bus and device
        // are what have to agree.
        found = conn;
        IOObjectRelease(svc);
        break;
      }
      IOServiceClose(conn);
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  return found;
}

pci_device::
pci_device(const std::string& sysfs, int ubar, size_t flash_off, std::string flash_type)
  : user_bar_index(ubar), flash_type_str(flash_type)
{
  uint16_t dom = 0, b = 0, d = 0, f = 0;
  if (sscanf(sysfs.c_str(), "%hx:%hx.%hx", &b, &d, &f) < 3 &&
      sscanf(sysfs.c_str(), "%hx:%hx:%hx.%hx", &dom, &b, &d, &f) < 4)
    throw std::runtime_error("Couldn't parse entry name " + sysfs);

  domain = dom;
  bus = b;
  dev = d;
  func = f;

  // Offsets crossing this class are offsets within the controller, so the base
  // is zero and the dext supplies the real one. Anything the caller passed for
  // --bar / --bar-offset cannot be honoured.
  flash_offset = 0;
  if (flash_off != static_cast<size_t>(-1) && flash_off != 0)
    std::cout << "note: --bar-offset is ignored on macOS; the driver locates the "
                 "QSPI controller itself" << std::endl;
  if (ubar != 0)
    std::cout << "note: --bar is ignored on macOS" << std::endl;

  // Retry: the flash user client is exclusive, and releasing the claim is
  // ASYNCHRONOUS - IOServiceClose returns before the dext's Stop runs. So an open
  // right after another flash tool exits can be refused even though nothing holds it
  // any more. Running the documented procedure as a script (flash_audit --dry-run,
  // then this) hits exactly that.
  for (int attempt = 0; attempt < 30 && !conn; ++attempt) {
    conn = open_mgmt(bus, dev, func);
    if (!conn)
      usleep(100000);
  }
  if (!conn)
    throw std::runtime_error("no management function with an attached dext for " + sysfs
                             + " (or its flash client is held by another process)");
}

pci_device::
~pci_device()
{
  if (conn) {
    // Leave the controller disabled. It reaches the primary flash device through
    // the FPGA's internal STARTUPE3, which is the same configuration primitive
    // the ICAP needs, so a core left enabled makes the next bitstream download
    // fail after exactly one FIFO of words. xspi.cpp has no reason to know that.
    uint64_t in[3] = { XRT_SPI_SSR, 0xffffffffu, 1 };
    uint64_t out = 0; uint32_t nout = 1;
    IOConnectCallScalarMethod(conn, XRT_DEXT_SPI_REG, in, 3, &out, &nout);
    in[0] = XRT_SPI_CR; in[1] = 0;
    nout = 1;
    IOConnectCallScalarMethod(conn, XRT_DEXT_SPI_REG, in, 3, &out, &nout);
    IOServiceClose(conn);
    conn = 0;
  }
}

// No per-subdevice character devices on macOS. Returning a failure is what puts
// XSPI_Flasher on its user-space register path, which is the only path here.
int
pci_device::
open(const std::string&, int)
{
  return -1;
}

void
pci_device::
close(int)
{
}

int
pci_device::
pcieBarRead(uint64_t offset, void* buf, uint64_t len)
{
  std::lock_guard<std::mutex> l(lock);
  if (!buf || len != 4 || !spi_reg_allowed(offset))
    return -EINVAL;

  uint64_t in[3] = { offset, 0, 0 };          // 0: read
  uint64_t out = 0;
  uint32_t nout = 1;
  kern_return_t kr = IOConnectCallScalarMethod(conn, XRT_DEXT_SPI_REG, in, 3, &out, &nout);
  if (kr != kIOReturnSuccess) {
    std::cout << "SPI register read at 0x" << std::hex << offset << std::dec
              << " failed: 0x" << std::hex << kr << std::dec << std::endl;
    return -EIO;
  }
  uint32_t v = static_cast<uint32_t>(out);
  std::memcpy(buf, &v, sizeof(v));
  return 0;
}

int
pci_device::
pcieBarWrite(uint64_t offset, const void* buf, uint64_t len)
{
  std::lock_guard<std::mutex> l(lock);
  if (!buf || len != 4 || !spi_reg_allowed(offset))
    return -EINVAL;

  uint32_t v = 0;
  std::memcpy(&v, buf, sizeof(v));
  uint64_t in[3] = { offset, v, 1 };          // 1: write
  uint64_t out = 0;
  uint32_t nout = 1;
  kern_return_t kr = IOConnectCallScalarMethod(conn, XRT_DEXT_SPI_REG, in, 3, &out, &nout);
  if (kr != kIOReturnSuccess) {
    std::cout << "SPI register write at 0x" << std::hex << offset << std::dec
              << " failed: 0x" << std::hex << kr << std::dec << std::endl;
    return -EIO;
  }
  return 0;
}

} // namespace pcidev

#endif // __APPLE__
