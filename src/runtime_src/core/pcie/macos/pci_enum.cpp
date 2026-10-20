// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
#include "pci_enum.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>

namespace {

constexpr uint16_t xilinx_vendor_id = 0x10ee;

// IOKit publishes PCI ID properties as 4-byte little-endian CFData.
bool
get_data_u32(io_service_t svc, const char* key, uint32_t& out)
{
  auto cfkey = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
  auto prop = IORegistryEntryCreateCFProperty(svc, cfkey, kCFAllocatorDefault, 0);
  CFRelease(cfkey);
  if (!prop)
    return false;
  bool ok = false;
  if (CFGetTypeID(prop) == CFDataGetTypeID()) {
    auto data = static_cast<CFDataRef>(prop);
    uint32_t v = 0;
    auto len = std::min<CFIndex>(CFDataGetLength(data), sizeof(v));
    CFDataGetBytes(data, CFRangeMake(0, len), reinterpret_cast<UInt8*>(&v));
    out = v;
    ok = true;
  }
  else if (CFGetTypeID(prop) == CFNumberGetTypeID()) {
    int64_t v = 0;
    ok = CFNumberGetValue(static_cast<CFNumberRef>(prop), kCFNumberSInt64Type, &v);
    out = static_cast<uint32_t>(v);
  }
  CFRelease(prop);
  return ok;
}

std::string
get_string(io_service_t svc, const char* key)
{
  auto cfkey = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
  auto prop = IORegistryEntryCreateCFProperty(svc, cfkey, kCFAllocatorDefault, 0);
  CFRelease(cfkey);
  if (!prop)
    return {};
  std::string result;
  char buf[256] = {0};
  if (CFGetTypeID(prop) == CFStringGetTypeID()) {
    if (CFStringGetCString(static_cast<CFStringRef>(prop), buf, sizeof(buf), kCFStringEncodingUTF8))
      result = buf;
  }
  else if (CFGetTypeID(prop) == CFDataGetTypeID()) {
    auto data = static_cast<CFDataRef>(prop);
    auto len = std::min<CFIndex>(CFDataGetLength(data), sizeof(buf) - 1);
    CFDataGetBytes(data, CFRangeMake(0, len), reinterpret_cast<UInt8*>(buf));
    result = buf;
  }
  CFRelease(prop);
  return result;
}

// "assigned-addresses" is an Open Firmware style array of 5 x u32 cells per
// BAR: phys.hi (space code, BAR register), address low, address high,
// size low, size high. Cells are host-endian.
std::vector<xrt_core::macos::pci_bar>
get_bars(io_service_t svc)
{
  std::vector<xrt_core::macos::pci_bar> bars;
  auto prop = IORegistryEntryCreateCFProperty(svc, CFSTR("assigned-addresses"), kCFAllocatorDefault, 0);
  if (!prop)
    return bars;
  if (CFGetTypeID(prop) == CFDataGetTypeID()) {
    auto data = static_cast<CFDataRef>(prop);
    auto len = CFDataGetLength(data);
    std::vector<uint32_t> cells(len / sizeof(uint32_t));
    CFDataGetBytes(data, CFRangeMake(0, cells.size() * sizeof(uint32_t)), reinterpret_cast<UInt8*>(cells.data()));
    for (size_t i = 0; i + 5 <= cells.size(); i += 5) {
      uint32_t phys_hi = cells[i];
      unsigned int reg = phys_hi & 0xff;
      if (reg < 0x10 || reg > 0x24)
        continue;                      // expansion ROM or config space entry
      xrt_core::macos::pci_bar bar;
      bar.index = (reg - 0x10) / 4;
      bar.addr  = static_cast<uint64_t>(cells[i + 1]) | (static_cast<uint64_t>(cells[i + 2]) << 32);
      bar.size  = static_cast<uint64_t>(cells[i + 3]) | (static_cast<uint64_t>(cells[i + 4]) << 32);
      bar.is64  = ((phys_hi >> 24) & 0x3) == 0x3;
      bar.prefetchable = (phys_hi >> 30) & 0x1;
      bars.push_back(bar);
    }
  }
  CFRelease(prop);
  std::sort(bars.begin(), bars.end(), [](auto& a, auto& b) { return a.index < b.index; });
  return bars;
}

xrt_core::macos::pci_function
read_function(io_service_t svc)
{
  xrt_core::macos::pci_function fn;
  uint32_t v = 0;
  if (get_data_u32(svc, "vendor-id", v))           fn.vendor = v & 0xffff;
  if (get_data_u32(svc, "device-id", v))           fn.device = v & 0xffff;
  if (get_data_u32(svc, "subsystem-vendor-id", v)) fn.subsystem_vendor = v & 0xffff;
  if (get_data_u32(svc, "subsystem-id", v))        fn.subsystem_id = v & 0xffff;
  if (get_data_u32(svc, "revision-id", v))         fn.revision = v & 0xff;
  if (get_data_u32(svc, "IOPCIExpressLinkCapabilities", v)) fn.link_caps = v;
  if (get_data_u32(svc, "IOPCIExpressLinkStatus", v))       fn.link_status = v;

  // "pcidebug" is "bus:device:function" as assigned by the macOS PCI family.
  auto dbg = get_string(svc, "pcidebug");
  unsigned int b = 0, d = 0, f = 0;
  if (std::sscanf(dbg.c_str(), "%u:%u:%u", &b, &d, &f) == 3) {
    fn.bus = b; fn.dev = d; fn.func = f;
  }

  io_string_t path = {0};
  if (IORegistryEntryGetPath(svc, kIOServicePlane, path) == KERN_SUCCESS)
    fn.registry_path = path;

  fn.bars = get_bars(svc);
  return fn;
}

} // namespace

namespace xrt_core::macos {

std::vector<alveo_card>
enumerate_cards()
{
  std::map<std::tuple<uint16_t, uint16_t, uint16_t>, alveo_card> cards;

  io_iterator_t it = IO_OBJECT_NULL;
  // IOServiceGetMatchingServices consumes the matching dictionary.
  if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOPCIDevice"), &it) != KERN_SUCCESS)
    return {};

  while (io_service_t svc = IOIteratorNext(it)) {
    uint32_t vendor = 0;
    if (get_data_u32(svc, "vendor-id", vendor) && (vendor & 0xffff) == xilinx_vendor_id) {
      auto fn = read_function(svc);
      auto& card = cards[{fn.domain, fn.bus, fn.dev}];
      // XRT's shell convention: PF0 is management, PF1 is user.
      if (fn.func == 0) { card.mgmt = fn; card.has_mgmt = true; }
      else if (fn.func == 1) { card.user = fn; card.has_user = true; }
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);

  std::vector<alveo_card> result;
  for (auto& [key, card] : cards)
    result.push_back(card);
  return result;
}

} // xrt_core::macos
