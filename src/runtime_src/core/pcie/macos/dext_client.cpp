// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
#include "dext_client.h"
#include "fdt_macos.h"
#include "xrt_dext_abi.h"

#include "core/common/error.h"
#include "core/common/message.h"
#include "xrt/detail/xclbin.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <system_error>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

namespace {

// bus:dev:func of the IOPCIDevice a dext instance is attached to.
bool
parent_bdf(io_service_t svc, unsigned int& bus, unsigned int& dev, unsigned int& func)
{
  io_registry_entry_t parent = IO_OBJECT_NULL;
  if (IORegistryEntryGetParentEntry(svc, kIOServicePlane, &parent) != KERN_SUCCESS)
    return false;
  bool ok = false;
  if (auto prop = IORegistryEntryCreateCFProperty(parent, CFSTR("pcidebug"), kCFAllocatorDefault, 0)) {
    char buf[64] = {0};
    if (CFGetTypeID(prop) == CFStringGetTypeID()
        && CFStringGetCString(static_cast<CFStringRef>(prop), buf, sizeof(buf), kCFStringEncodingUTF8))
      ok = std::sscanf(buf, "%u:%u:%u", &bus, &dev, &func) == 3;
    CFRelease(prop);
  }
  IOObjectRelease(parent);
  return ok;
}

// Open the dext user client attached to bus/dev/func; IO_OBJECT_NULL if none.
io_connect_t
open_function(uint16_t want_bus, uint16_t want_dev, uint16_t want_func)
{
  io_iterator_t it = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it) != KERN_SUCCESS)
    return IO_OBJECT_NULL;
  io_connect_t conn = IO_OBJECT_NULL;
  while (io_service_t svc = IOIteratorNext(it)) {
    unsigned int bus = 0, dev = 0, func = 0;
    if (!conn && parent_bdf(svc, bus, dev, func)
        && bus == want_bus && dev == want_dev && func == want_func)
      IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  return conn;
}

// Mirrors xrt_xclbin_parse_header() in xocl: header length, or 0.
size_t
bit_header_length(const unsigned char* d, size_t size, uint32_t& bitlen)
{
  size_t i = 0;
  auto u16 = [&]() { unsigned v = d[i] << 8 | d[i + 1]; i += 2; return v; };
  if (size < 64)
    return 0;
  unsigned magic_len = u16();
  for (unsigned k = 0; k + 1 < magic_len; ++k, ++i)
    if (d[i] != ((k % 2) ? 0xf0 : 0x0f))
      return 0;
  ++i;
  if (u16() != 0x01)
    return 0;
  for (char field : {'a', 'b', 'c', 'd'}) {
    if (i + 3 > size || d[i++] != field)
      return 0;
    unsigned len = u16();
    if (i + len > size || len == 0 || d[i + len - 1] != '\0')
      return 0;
    i += len;
  }
  if (i + 5 > size || d[i++] != 'e')
    return 0;
  bitlen = (uint32_t)d[i] << 24 | (uint32_t)d[i + 1] << 16 | (uint32_t)d[i + 2] << 8 | d[i + 3];
  i += 4;
  return (i + bitlen <= size) ? i : 0;
}

std::optional<FeatureRomHeader>
read_rom_uncached(uint16_t want_bus, uint16_t want_dev)
{
  io_iterator_t it = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it) != KERN_SUCCESS)
    return std::nullopt;

  std::optional<FeatureRomHeader> result;
  while (io_service_t svc = IOIteratorNext(it)) {
    unsigned int bus = 0, dev = 0, func = 0;
    if (!result && parent_bdf(svc, bus, dev, func) && bus == want_bus && dev == want_dev) {
      io_connect_t conn = IO_OBJECT_NULL;
      if (IOServiceOpen(svc, mach_task_self(), 0, &conn) == KERN_SUCCESS) {
        uint8_t rom[XRT_DEXT_ROM_SIZE] = {};
        size_t size = sizeof(rom);
        if (IOConnectCallStructMethod(conn, XRT_DEXT_READ_ROM, nullptr, 0, rom, &size) == KERN_SUCCESS
            && size >= sizeof(FeatureRomHeader)) {
          FeatureRomHeader hdr;
          std::memcpy(&hdr, rom, sizeof(hdr));
          result = hdr;
        }
        IOServiceClose(conn);
      }
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  return result;
}


// PCIe extended capability list, and the vendor capability that dynamic-IP
// platforms use to say where their own description lives. Mirrors
// xocl_subdev_find_vsec_offset() and xocl_subdev_vsec() in the Linux driver.
constexpr uint32_t kExtCapStart    = 0x100;
constexpr uint16_t kExtCapVendor   = 0x000b;   // PCI_EXT_CAP_ID_VNDR
constexpr uint16_t kVsecAlf        = 0x0020;   // locates the platform description
constexpr uint32_t kVsecUuidRom    = 0x50;     // entry type of the UUID ROM
constexpr uint32_t kVsecEndOfTable = 0xff;

// One dword of a function's PCI configuration space through its dext.
bool
cfg_read(io_connect_t conn, uint64_t offset, uint32_t& out)
{
  uint64_t in = offset, value = 0;
  uint32_t nout = 1;
  if (IOConnectCallScalarMethod(conn, XRT_DEXT_CFG_READ, &in, 1, &value, &nout) != KERN_SUCCESS)
    return false;
  out = static_cast<uint32_t>(value);
  return true;
}

// Configuration-space offset of the vendor capability that points at the
// platform description table, or 0 if this function has none.
uint32_t
find_vsec_offset(io_connect_t conn)
{
  uint32_t off = kExtCapStart;
  for (int guard = 0; off >= kExtCapStart && off < XRT_CFG_SPACE_BYTES && guard < 64; ++guard) {
    uint32_t hdr = 0;
    if (!cfg_read(conn, off, hdr) || hdr == 0 || hdr == 0xffffffff)
      break;
    if ((hdr & 0xffff) == kExtCapVendor) {
      uint32_t vh = 0;
      if (cfg_read(conn, off + 4, vh) && (vh & 0xffff) == kVsecAlf)
        return off;
    }
    off = (hdr >> 20) & 0xffc;      // next capability, 0 ends the list
  }
  return 0;
}

} // namespace

namespace xrt_core::macos {

std::optional<FeatureRomHeader>
read_feature_rom(uint16_t bus, uint16_t dev)
{
  // The ROM is fixed for the life of the shell; cache successful reads.
  static std::mutex mutex;
  static std::map<std::pair<uint16_t, uint16_t>, FeatureRomHeader> cache;
  std::lock_guard lk(mutex);
  auto key = std::make_pair(bus, dev);
  if (auto it = cache.find(key); it != cache.end())
    return it->second;
  auto rom = read_rom_uncached(bus, dev);
  if (rom)
    cache[key] = *rom;
  return rom;
}

// ---------------------------------------------------------------------------
// The platform's register map.
//
// A dynamic-IP platform describes itself, but not anywhere the dext can reach:
// the vendor capability on the card names only the UUID ROM, the flash, platform
// info and the mailbox - not ICAP, the ULP gate, memory calibration or the
// firewalls. Those are in the partition metadata that ships with the platform
// package, on the host filesystem, which a DriverKit extension cannot read. So
// the shim reads it and installs the result.
// ---------------------------------------------------------------------------

// Endpoint names in the partition metadata, and the block each one is.
struct endpoint_map { const char* name; uint32_t blk; };
static const endpoint_map kEndpoints[] = {
  { "ep_fpga_configuration_00", XRT_BLK_ICAP        },
  { "ep_pr_isolate_ulp_00",     XRT_BLK_GATE_ULP    },
  { "ep_ddr_mem_calib_00",      XRT_BLK_MEMCALIB    },
  { "ep_uuid_rom_blp_00",       XRT_BLK_UUID_ROM    },
  { "ep_cmc_regmap_00",         XRT_BLK_CMC_REGMAP  },
  { "ep_xdma_00",               XRT_BLK_XDMA        },
  { "ep_cmc_firmware_mem_00",   XRT_BLK_CMC_FW_MEM  },
  { "ep_cmc_reset_00",          XRT_BLK_CMC_RESET   },
  { "ep_cmc_mutex_00",          XRT_BLK_CMC_MUTEX   },
};

// Where platform packages are installed. XRT's own xclbin search uses this same
// path shape (see searchSSV2Xclbin in core/tools/common/tests), three levels
// under the firmware directory.
static constexpr const char* kFirmwareDir = "/opt/xilinx/firmware";

// The platform package directory whose partition metadata carries this logic
// uuid, or empty. Everything the package can tell us about the card hangs off
// this: the register map, the shell's name, the test xclbins beside it.
static std::string
find_package_dir(const std::string& uuid);

// The partition metadata of the platform whose logic uuid is `uuid`, or empty.
static std::string
find_partition_metadata(const std::string& uuid)
{
  std::error_code ec;
  std::filesystem::path root{kFirmwareDir};
  if (!std::filesystem::is_directory(root, ec))
    return {};
  // <firmware>/<board>/<family>/<partition>/partition_metadata.json
  for (const auto& a : std::filesystem::directory_iterator(root, ec)) {
    if (!a.is_directory())
      continue;
    for (const auto& b : std::filesystem::directory_iterator(a.path(), ec)) {
      if (!b.is_directory())
        continue;
      for (const auto& c : std::filesystem::directory_iterator(b.path(), ec)) {
        if (!c.is_directory())
          continue;
        auto json = c.path() / "partition_metadata.json";
        if (!std::filesystem::exists(json, ec))
          continue;
        try {
          boost::property_tree::ptree pt;
          boost::property_tree::read_json(json.string(), pt);
          if (pt.get<std::string>("partition_metadata.logic_uuid", "") == uuid)
            return json.string();
        }
        catch (const std::exception&) {
          continue;                 // unreadable or not what we are looking for
        }
      }
    }
  }
  return {};
}

// Build a register map from a platform's partition metadata. False if the file
// does not describe the blocks we need.
static bool
platform_from_metadata(const std::string& json, xrt_dext_platform& plat)
{
  boost::property_tree::ptree pt;
  try {
    boost::property_tree::read_json(json, pt);
  }
  catch (const std::exception&) {
    return false;
  }
  auto eps = pt.get_child_optional("partition_metadata.addressable_endpoints");
  if (!eps)
    return false;

  memset(&plat, 0, sizeof(plat));
  plat.abi = XRT_DEXT_ABI_VERSION;
  plat.count = XRT_BLK_MAX;

  auto place = [&](uint32_t id, const boost::property_tree::ptree& ep) {
    auto off = ep.get<std::string>("offset", "");
    auto rng = ep.get<std::string>("range", "");
    auto bar = ep.get<std::string>("pcie_base_address_register", "");
    auto pf  = ep.get<std::string>("pcie_physical_function", "");
    if (off.empty() || rng.empty() || bar.empty() || pf.empty())
      return;
    plat.blk[id].offset = std::strtoull(off.c_str(), nullptr, 0);
    plat.blk[id].size   = std::strtoull(rng.c_str(), nullptr, 0);
    plat.blk[id].bar    = static_cast<uint8_t>(std::strtoul(bar.c_str(), nullptr, 0));
    plat.blk[id].func   = static_cast<uint8_t>(std::strtoul(pf.c_str(), nullptr, 0));
  };

  int firewalls = 0;
  for (const auto& [name, ep] : *eps) {
    for (const auto& m : kEndpoints)
      if (name == m.name)
        place(m.blk, ep);
    // Firewalls are numbered as they come; which is which does not matter,
    // the driver reads and clears all of them.
    if (name.rfind("ep_firewall_", 0) == 0 && firewalls < XRT_MGMT_FIREWALLS)
      place(XRT_BLK_FIREWALL_0 + firewalls++, ep);
  }

  // ICAP is the one block nothing works without.
  return plat.blk[XRT_BLK_ICAP].size != 0;
}

std::optional<xrt_dext_platform>
get_platform(uint16_t bus, uint16_t dev, uint16_t func)
{
  io_connect_t conn = open_function(bus, dev, func);
  if (!conn)
    return std::nullopt;
  xrt_dext_platform plat = {};
  size_t size = sizeof(plat);
  kern_return_t kr = IOConnectCallStructMethod(conn, XRT_DEXT_GET_PLATFORM, nullptr, 0, &plat, &size);
  IOServiceClose(conn);
  if (kr != KERN_SUCCESS || size < sizeof(plat))
    return std::nullopt;
  return plat;
}

void
install_platform(uint16_t bus, uint16_t dev)
{
  // Once per process: the answer cannot change under us, and a second install
  // is refused by the dext anyway.
  static std::mutex mutex;
  static std::map<std::pair<uint16_t, uint16_t>, bool> done;
  std::lock_guard lk(mutex);
  if (done[std::make_pair(bus, dev)])
    return;
  done[std::make_pair(bus, dev)] = true;

  auto current = get_platform(bus, dev, 0);
  if (!current)
    return;                                   // no dext on the management function
  auto user = get_platform(bus, dev, 1);
  if (!(current->flags & XRT_PLATFORM_DEFAULT)
      && (!user || !(user->flags & XRT_PLATFORM_DEFAULT)))
    return;                                   // another process installed it first

  auto uuid = logic_uuid(bus, dev);
  if (uuid.empty())
    return;                                   // XDMA 5.x: the defaults are right

  auto json = find_partition_metadata(uuid);
  if (json.empty()) {
    xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
                            "no platform package for logic uuid " + uuid + " under "
                            + kFirmwareDir + "; the driver is using XDMA 5.x register offsets,"
                            " which this card does not use");
    return;
  }
  xrt_dext_platform plat = {};
  if (!platform_from_metadata(json, plat)) {
    xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
                            "could not read a register map from " + json);
    return;
  }

  // Each function gets only its own blocks: most are on the management
  // function, but the XDMA engine is on the user one, and a block installed on
  // the wrong function would be validated against the wrong BAR.
  for (uint16_t func : {0, 1}) {
    xrt_dext_platform mine = plat;
    for (uint32_t i = 0; i < XRT_BLK_MAX; ++i)
      if (mine.blk[i].size && mine.blk[i].func != func)
        mine.blk[i] = {};
    io_connect_t conn = open_function(bus, dev, func);
    if (!conn)
      continue;
    kern_return_t kr = IOConnectCallStructMethod(conn, XRT_DEXT_SET_PLATFORM, &mine, sizeof(mine), nullptr, nullptr);
    IOServiceClose(conn);
    if (kr != KERN_SUCCESS && kr != kIOReturnNotPermitted)
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
                              "installing the register map on function " + std::to_string(func)
                              + " failed: " + std::to_string(kr));
  }
  xrt_core::message::send(xrt_core::message::severity_level::info, "XRT",
                          "register map for logic uuid " + uuid + " taken from " + json);

  // With the map in place the board controller can be found, so start it: it
  // runs from RAM and is the card's only source of sensors. Idempotent - it
  // reports itself already running and writes nothing in that case.
  load_cmc(bus, dev);
}

// The interface uuid of the shell in this card, from the platform package: what
// an xclbin has to have been built against. XDMA 5.x cards carry this in the
// feature ROM instead.
static std::string
shell_interface_uuid(uint16_t bus, uint16_t dev)
{
  auto uuid = logic_uuid(bus, dev);
  if (uuid.empty())
    return {};
  auto json = find_partition_metadata(uuid);
  if (json.empty())
    return {};
  try {
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(json, pt);
    for (const auto& [key, iface] : pt.get_child("partition_metadata.interfaces")) {
      (void)key;
      auto v = iface.get<std::string>("interface_uuid", "");
      if (!v.empty())
        return v;
    }
  }
  catch (const std::exception&) {
    return {};
  }
  return {};
}

// What an xclbin says it was built against, and what its user partition brings
// with it. On a dynamic-IP platform the kernel clock wizards are in here, not in
// the base platform.
static bool
xclbin_partition(const ::axlf* top, xrt_core::macos::fdt_partition& part)
{
  auto base = reinterpret_cast<const char*>(top);
  for (uint32_t i = 0; i < top->m_header.m_numSections; ++i) {
    const auto& sh = top->m_sections[i];
    if (sh.m_sectionKind != PARTITION_METADATA)
      continue;
    return xrt_core::macos::parse_partition_metadata(base + sh.m_sectionOffset,
                                                     sh.m_sectionSize, part);
  }
  return false;
}


static std::string hex64(uint64_t v);

// The same spelling loaded_xclbin_uuid() returns, so the two can be compared.
static std::string
uuid_string(const unsigned char* u)
{
  static const unsigned char zero[16] = {};
  if (std::memcmp(u, zero, 16) == 0)
    return {};
  char buf[37];
  std::snprintf(buf, sizeof(buf),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
  return buf;
}

static std::string
find_package_dir(const std::string& uuid)
{
  auto json = find_partition_metadata(uuid);
  if (json.empty())
    return {};
  return std::filesystem::path(json).parent_path().string();
}

std::string
shell_vbnv(uint16_t bus, uint16_t dev)
{
  static std::mutex mutex;
  static std::map<std::pair<uint16_t, uint16_t>, std::string> cache;
  std::lock_guard lk(mutex);
  auto key = std::make_pair(bus, dev);
  if (auto it = cache.find(key); it != cache.end())
    return it->second;

  auto uuid = logic_uuid(bus, dev);
  if (uuid.empty())
    return {};                       // an XDMA 5.x card: its ROM has the name
  auto dir = find_package_dir(uuid);
  if (dir.empty())
    return {};

  std::string name;
  auto path = std::filesystem::path(dir) / "partition.xsabin";
  std::ifstream f(path, std::ios::binary);
  if (f) {
    // Only the header is needed, and these files are ~88 MiB.
    std::vector<char> head(sizeof(::axlf));
    if (f.read(head.data(), static_cast<std::streamsize>(head.size()))) {
      auto top = reinterpret_cast<const ::axlf*>(head.data());
      auto vbnv = reinterpret_cast<const char*>(top->m_header.m_platformVBNV);
      name.assign(vbnv, strnlen(vbnv, sizeof(top->m_header.m_platformVBNV)));
      // Normalise as XRT does (firmware_image.cpp): v:b:n:a.b -> v_b_n_a_b.
      for (auto& c : name)
        if (c == ':' || c == '.')
          c = '_';
    }
  }
  if (!name.empty())
    cache[key] = name;
  return name;
}

// Bring the board controller up, so the card has sensors. Its firmware ships in
// the platform package; the controller is a RAM-loaded microcontroller, so this
// has to happen once per power cycle rather than being flashed.
//
// Returns the controller's version, or 0 if it could not be started. Not an
// error to call on a platform with no controller.
uint32_t
clock_measure(uint16_t bus, uint16_t dev, uint32_t index)
{
  io_connect_t conn = open_function(bus, dev, 0);
  if (!conn)
    return 0;
  uint64_t in = index, out = 0;
  uint32_t nout = 1;
  kern_return_t kr = IOConnectCallScalarMethod(conn, XRT_DEXT_CLOCK_MEASURE, &in, 1, &out, &nout);
  IOServiceClose(conn);
  return kr == KERN_SUCCESS ? static_cast<uint32_t>(out) : 0;
}

std::vector<uint8_t>
cmc_board_info(uint16_t bus, uint16_t dev, uint8_t key)
{
  // The whole blob comes back in one exchange, so fetch it once and keep it:
  // the board's name and its power rating do not change while it is plugged in.
  static std::mutex mutex;
  static std::map<std::pair<uint16_t, uint16_t>, std::vector<uint8_t>> cache;
  std::lock_guard lk(mutex);
  auto ck = std::make_pair(bus, dev);
  auto it = cache.find(ck);
  if (it == cache.end()) {
    std::vector<uint8_t> blob;
    if (io_connect_t conn = open_function(bus, dev, 0)) {
      xrt_dext_cmc_bdinfo info = {};
      size_t sz = sizeof(info);
      kern_return_t kr = IOConnectCallStructMethod(conn, XRT_DEXT_CMC_BOARD_INFO,
                                                  nullptr, 0, &info, &sz);
      IOServiceClose(conn);
      if (kr == KERN_SUCCESS && info.status == 0 && info.bytes
          && info.bytes <= sizeof(info.data))
        blob.assign(info.data, info.data + info.bytes);
    }
    it = cache.emplace(ck, std::move(blob)).first;
  }
  const auto& blob = it->second;

  // key, length, value - walked rather than indexed, because which keys a board
  // carries varies.
  for (size_t i = 0; i + 2 <= blob.size();) {
    uint8_t k = blob[i], len = blob[i + 1];
    i += 2;
    if (i + len > blob.size())
      break;
    if (k == key)
      return std::vector<uint8_t>(blob.begin() + i, blob.begin() + i + len);
    i += len;
  }
  return {};
}

uint32_t
load_cmc(uint16_t bus, uint16_t dev)
{
  std::error_code ec;
  std::string image;
  auto uuid = logic_uuid(bus, dev);
  std::string dir;
  if (uuid.empty()) {
    // An XDMA 5.x shell has no platform package to take the image from, but it
    // does have a controller if its feature ROM says so - and then the image is
    // the one XRT ships. Without this the card reports no board sensors unless
    // somebody runs a tool by hand.
    auto rom = read_feature_rom(bus, dev);
    if (!rom || !(rom->FeatureBitMap & 0x4))     // BOARD_MGMT_ENBLD
      return 0;
    std::string shipped = "/opt/xilinx/xrt/share/fw/cmc.bin";
    if (std::filesystem::exists(shipped, ec))
      image = shipped;
  }
  else {
    dir = find_package_dir(uuid);
    if (dir.empty())
      return 0;
  }

  // <package>/firmware/cmc-*/cmc-*.bin
  auto fw = std::filesystem::path(dir.empty() ? "/nonexistent" : dir) / "firmware";
  if (image.empty() && std::filesystem::is_directory(fw, ec)) {
    for (const auto& d : std::filesystem::directory_iterator(fw, ec)) {
      if (d.path().filename().string().rfind("cmc", 0) != 0)
        continue;
      for (const auto& f : std::filesystem::directory_iterator(d.path(), ec))
        if (f.path().extension() == ".bin") {
          image = f.path().string();
          break;
        }
      if (!image.empty())
        break;
    }
  }
  if (image.empty()) {
    xrt_core::message::send(xrt_core::message::severity_level::info, "XRT",
                            "no board controller firmware in " + fw.string()
                            + "; this card will report no sensors");
    return 0;
  }

  std::ifstream f(image, std::ios::binary);
  if (!f)
    return 0;
  std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (bytes.empty() || bytes.size() % 4)
    bytes.resize((bytes.size() + 3) & ~size_t(3), 0);

  io_connect_t conn = open_function(bus, dev, 0);
  if (!conn)
    return 0;
  xrt_dext_cmc_result res = {};
  size_t rsz = sizeof(res);
  kern_return_t kr = IOConnectCallStructMethod(conn, XRT_DEXT_LOAD_CMC,
                                               bytes.data(), bytes.size(), &res, &rsz);
  IOServiceClose(conn);
  if (kr != KERN_SUCCESS || res.status != 0) {
    xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
                            "could not start the board controller (step " + std::to_string(res.step)
                            + ", status " + std::to_string(res.status) + ", magic "
                            + hex64(res.magic) + "); this card will report no sensors");
    return 0;
  }
  if (!res.was_running)
    xrt_core::message::send(xrt_core::message::severity_level::info, "XRT",
                            "board controller started from " + image + " in "
                            + std::to_string(res.elapsed_ms) + " ms, version "
                            + hex64(res.version));
  return res.version;
}

void
set_card_range(uint16_t bus, uint16_t dev, const ::axlf* top)
{
  auto base = reinterpret_cast<const char*>(top);
  const mem_topology* mem = nullptr;
  for (uint32_t i = 0; i < top->m_header.m_numSections; ++i) {
    if (top->m_sections[i].m_sectionKind == MEM_TOPOLOGY) {
      mem = reinterpret_cast<const mem_topology*>(base + top->m_sections[i].m_sectionOffset);
      break;
    }
  }
  if (!mem || mem->m_count <= 0)
    return;

  uint64_t lo = UINT64_MAX, hi = 0;
  for (int32_t i = 0; i < mem->m_count; ++i) {
    const auto& m = mem->m_mem_data[i];
    if (!m.m_used)
      continue;
    if (m.m_type != MEM_DDR3 && m.m_type != MEM_DDR4
        && m.m_type != MEM_DRAM && m.m_type != MEM_HBM)
      continue;                               // not somewhere we DMA into
    lo = std::min(lo, static_cast<uint64_t>(m.m_base_address));
    hi = std::max(hi, m.m_base_address + m.m_size * 1024);   // m_size is KB
  }
  if (lo == UINT64_MAX || hi <= lo)
    return;

  // Only move when it has to move, so a card whose banks already fall inside
  // the default span keeps exactly the bounds it has always had.
  if (lo >= XRT_DMA_CARD_MIN && hi <= XRT_DMA_CARD_MAX)
    return;

  io_connect_t conn = open_function(bus, dev, 1);
  if (!conn)
    return;
  uint64_t in[2] = { lo, hi };
  kern_return_t kr = IOConnectCallScalarMethod(conn, XRT_DEXT_SET_CARD_RANGE, in, 2, nullptr, nullptr);
  IOServiceClose(conn);
  if (kr != KERN_SUCCESS) {
    xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
                            "could not set the card address range to 0x" + hex64(lo) + "..0x"
                            + hex64(hi) + ": " + std::to_string(kr)
                            + "; allocating device memory for this xclbin will be refused");
    return;
  }
  xrt_core::message::send(xrt_core::message::severity_level::info, "XRT",
                          "card address range set to 0x" + hex64(lo) + "..0x" + hex64(hi));
}

void
set_cu_window(uint16_t bus, uint16_t dev, const ::axlf* top)
{
  auto base = reinterpret_cast<const char*>(top);
  const ip_layout* ips = nullptr;
  for (uint32_t i = 0; i < top->m_header.m_numSections; ++i) {
    if (top->m_sections[i].m_sectionKind == IP_LAYOUT) {
      ips = reinterpret_cast<const ip_layout*>(base + top->m_sections[i].m_sectionOffset);
      break;
    }
  }
  if (!ips || ips->m_count <= 0)
    return;

  uint64_t lo = UINT64_MAX, hi = 0;
  for (int32_t i = 0; i < ips->m_count; ++i) {
    const auto& ip = ips->m_ip_data[i];
    if (ip.m_type != IP_KERNEL)
      continue;
    uint64_t a = ip.m_base_address;
    if (a == static_cast<uint64_t>(-1) || !a)
      continue;                               // no address: not addressable
    lo = std::min(lo, a);
    hi = std::max(hi, a + XRT_CU_SPAN);
  }
  if (lo == UINT64_MAX)
    return;                                   // no addressable kernels

  // Only move the window when it has to move. A card whose CUs already fall
  // inside the default keeps exactly the bounds it has always had, which
  // matters because the XDMA 5.x path cannot be regression tested right now.
  if (lo >= XRT_CU_WINDOW_BASE && hi <= XRT_CU_WINDOW_END)
    return;

  uint64_t off = lo & ~static_cast<uint64_t>(XRT_CU_RW_PAGE - 1);
  uint64_t len = ((hi - off) + XRT_CU_RW_PAGE - 1) & ~static_cast<uint64_t>(XRT_CU_RW_PAGE - 1);

  io_connect_t conn = open_function(bus, dev, 1);
  if (!conn)
    return;
  uint64_t in[2] = { off, len };
  kern_return_t kr = IOConnectCallScalarMethod(conn, XRT_DEXT_SET_CU_WINDOW, in, 2, nullptr, nullptr);
  IOServiceClose(conn);
  if (kr != KERN_SUCCESS) {
    xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
                            "could not move the CU register window to 0x" + hex64(off) + "+0x"
                            + hex64(len) + ": " + std::to_string(kr)
                            + "; this xclbin's CUs are outside the window the driver looks in,"
                              " so starting them will be refused");
    return;
  }
  xrt_core::message::send(xrt_core::message::severity_level::info, "XRT",
                          "CU register window moved to 0x" + hex64(off) + "+0x" + hex64(len));
}

bool
dext_attached(uint16_t bus, uint16_t dev)
{
  for (uint16_t func : {0, 1}) {
    if (io_connect_t conn = open_function(bus, dev, func)) {
      IOServiceClose(conn);
      return true;
    }
  }
  return false;
}

static std::string
hex64(uint64_t v)
{
  char b[24];
  std::snprintf(b, sizeof(b), "%llx", (unsigned long long)v);
  return b;
}

std::string
logic_uuid(uint16_t bus, uint16_t dev)
{
  // Fixed until the card is reflashed, so cache it. A negative answer is
  // cached only once the dext has actually told us there is no such
  // capability; failing to reach the dext at all is not an answer.
  static std::mutex mutex;
  static std::map<std::pair<uint16_t, uint16_t>, std::string> cache;
  std::lock_guard lk(mutex);
  auto key = std::make_pair(bus, dev);
  if (auto it = cache.find(key); it != cache.end())
    return it->second;

  // The UUID ROM is reachable only through the management function.
  io_connect_t conn = open_function(bus, dev, 0);
  if (!conn)
    return {};

  uint32_t cap = find_vsec_offset(conn);
  if (!cap) {
    IOServiceClose(conn);
    cache[key] = {};                // an XDMA 5.x shell; it will not grow one
    return {};
  }
  uint32_t lo = 0, hi = 0;
  bool ok = cfg_read(conn, cap + 8, lo) && cfg_read(conn, cap + 12, hi);
  IOServiceClose(conn);
  if (!ok)
    return {};

  // The capability names a BAR and an offset within it; the table there lists
  // the platform's blocks, each with its own BAR and offset.
  uint64_t table_len = 0;
  auto table = map_bar(bus, dev, 0, lo & 0xf, &table_len);
  uint64_t table_off = (static_cast<uint64_t>(hi) << 32) | (lo & 0xfffffff0u);
  if (!table || table_off + 16 > table_len)
    return {};

  auto hdr = table + table_off / 4;
  uint32_t length = hdr[1], entry_sz = hdr[2] & 0xff;
  if (entry_sz == 0 || entry_sz % 4 || length <= 16 || table_off + length > table_len)
    return {};

  for (uint32_t i = 16; i + 8 <= length; i += entry_sz) {
    uint32_t elo = hdr[i / 4], ehi = hdr[i / 4 + 1];
    uint32_t type = elo & 0xff;
    if (type == kVsecEndOfTable)
      break;
    if (type != kVsecUuidRom)
      continue;

    uint64_t rom_len = 0;
    auto rom = map_bar(bus, dev, 0, (elo >> 13) & 0x7, &rom_len);
    uint64_t rom_off = (static_cast<uint64_t>(ehi) << 16) | ((elo & 0xffff0000u) >> 16);
    if (!rom || rom_off % 4 || rom_off + 16 > rom_len)
      return {};

    // The string is the four dwords in reverse order, as the Linux driver
    // builds it, because that is the spelling the firmware directory uses.
    auto p = rom + rom_off / 4;
    char buf[33] = {};
    for (int w = 3, j = 0; w >= 0; --w, j += 8)
      std::snprintf(buf + j, 9, "%08x", p[w]);
    cache[key] = buf;
    return buf;
  }

  cache[key] = {};                  // has the capability, but no UUID ROM in it
  return {};
}

volatile uint32_t*
map_bar(uint16_t want_bus, uint16_t want_dev, uint16_t want_func, uint32_t want_bar, uint64_t* size_out)
{
  struct mapping { volatile uint32_t* base; uint64_t size; };
  static std::mutex mutex;
  static std::map<std::tuple<uint16_t, uint16_t, uint16_t, uint32_t>, mapping> maps;
  std::lock_guard lk(mutex);
  auto key = std::make_tuple(want_bus, want_dev, want_func, want_bar);
  if (auto it = maps.find(key); it != maps.end()) {
    if (size_out) *size_out = it->second.size;
    return it->second.base;
  }

  io_iterator_t it = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it) != KERN_SUCCESS)
    return nullptr;
  mapping result{nullptr, 0};
  while (io_service_t svc = IOIteratorNext(it)) {
    unsigned int bus = 0, dev = 0, func = 0;
    if (!result.base && parent_bdf(svc, bus, dev, func)
        && bus == want_bus && dev == want_dev && func == want_func) {
      io_connect_t conn = IO_OBJECT_NULL;
      if (IOServiceOpen(svc, mach_task_self(), 0, &conn) == KERN_SUCCESS) {
        mach_vm_address_t addr = 0;
        mach_vm_size_t len = 0;
        if (IOConnectMapMemory64(conn, XRT_DEXT_MAP_BAR(want_bar), mach_task_self(), &addr, &len,
                                 kIOMapAnywhere | kIOMapInhibitCache) == KERN_SUCCESS)
          result = {reinterpret_cast<volatile uint32_t*>(addr), len};
        else
          IOServiceClose(conn);
        // On success the connection stays open for the life of the mapping.
      }
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  if (result.base)
    maps[key] = result;
  if (size_out)
    *size_out = result.size;
  return result.base;
}

void
load_xclbin(uint16_t bus, uint16_t dev, const axlf* top)
{
  if (!top || std::memcmp(top->m_magic, "xclbin2", 7) != 0)
    throw xrt_core::error(std::errc::invalid_argument, "not an xclbin2 image");
  if (top->m_header.m_mode == XCLBIN_FLAT)
    throw xrt_core::error(std::errc::not_supported, "flat xclbins are not supported");

  // Same shell: static UUID and feature ROM timestamp, as xclmgmt checks.
  // Filled in below for a platform whose clocks come with the xclbin; left 0 to
  // mean "use the platform's own wizards".
  uint32_t ulp_clk[XRT_LOAD_CLOCKS] = { 0, 0, 0 };
  uint32_t freq_cnt[XRT_LOAD_CLOCKS] = { 0, 0, 0 };
  uint32_t ucs = 0;
  auto hex = [](uint32_t v) { char b[16]; std::snprintf(b, sizeof(b), "%x", v); return std::string(b); };

  // Which flow this card takes is decided by a positive signal: the PCIe VSEC
  // capability that names the platform's block table, which is what
  // logic_uuid() walks. That is the macOS reading of xocl's
  // XOCL_DSAFLAG_DYNAMIC_IP, and it is deliberately not "the feature ROM read
  // failed". Keying it off a failed ROM read means any transient failure on an
  // XDMA 5.x card silently routes into the dynamic-IP flow - which, now that
  // programming is on by default (see the guard below), is the flow that
  // programs. logic_uuid() caches an absent capability permanently and a failed
  // read not at all, so an unreachable card does not get mistaken for either
  // kind of platform.
  if (logic_uuid(bus, dev).empty()) {
    // XDMA 5.x: the feature ROM carries the shell's build timestamp, and the
    // xclbin header carries the same value. That is the whole of xocl's check -
    // `xocl_verify_timestamp` in subdev/icap.c, with no uuid comparison at all.
    auto rom = read_feature_rom(bus, dev);
    if (!rom)
      throw xrt_core::error(std::errc::not_supported,
                            "this card has neither a dynamic-IP capability nor a readable feature"
                            " ROM, so there is nothing to check the xclbin against; if the card was"
                            " working a moment ago this is a fault, not an unknown platform");
    if (rom->TimeSinceEpoch != top->m_header.m_featureRomTimeStamp)
      throw xrt_core::error(std::errc::invalid_argument,
                            "xclbin was not built for this shell: its feature ROM timestamp is "
                            + std::to_string(top->m_header.m_featureRomTimeStamp)
                            + ", the card reports " + std::to_string(rom->TimeSinceEpoch));
    // The uuid is compared only when the ROM actually carries one. Requiring it
    // unconditionally was stricter than xocl and rejected a legitimate xclbin on
    // a shell whose ROM leaves the field zero.
    static const unsigned char zero_uuid[16] = {};
    if (std::memcmp(rom->uuid, zero_uuid, 16) != 0
        && std::memcmp(rom->uuid, top->m_header.m_interface_uuid, 16) != 0)
      throw xrt_core::error(std::errc::invalid_argument,
                            "xclbin was not built for this shell: its interface uuid does not match"
                            " the one in the card's feature ROM");
  }
  else {
    if (!dext_attached(bus, dev))
      throw xrt_core::error(std::errc::no_such_device, "xrt_user dext not attached to this card");

    // A dynamic-IP shell's xclbin leaves the header's interface uuid zeroed -
    // both uuids are in partition metadata instead, the shell's in its package
    // and the xclbin's in a section.
    auto shell = shell_interface_uuid(bus, dev);
    xrt_core::macos::fdt_partition part;
    if (!xclbin_partition(top, part))
      throw xrt_core::error(std::errc::not_supported,
                            "this xclbin carries no partition metadata, and this shell has no"
                            " feature ROM, so there is nothing to check it against");
    if (shell.empty())
      throw xrt_core::error(std::errc::not_supported,
                            "no platform package for this card, so the xclbin cannot be checked"
                            " against the shell");
    if (shell != part.interface_uuid)
      throw xrt_core::error(std::errc::invalid_argument,
                            "xclbin was not built for this shell: it wants interface "
                            + part.interface_uuid + ", the card has " + shell);

    // The xclbin matches. Its user partition brings the kernel clock wizards, so
    // find them here - only the caller has the xclbin. The order is xocl's
    // (subdev/clock_wiz.c): ep_aclk_kernel_00 takes the data clock,
    // ep_aclk_kernel_01 the kernel clock, ep_aclk_hbm_00 the system clock.
    for (const auto& ep : part.endpoints) {
      if (ep.name.rfind("ep_aclk_kernel_00", 0) == 0)
        ulp_clk[0] = static_cast<uint32_t>(ep.offset);
      else if (ep.name.rfind("ep_aclk_kernel_01", 0) == 0)
        ulp_clk[1] = static_cast<uint32_t>(ep.offset);
      else if (ep.name.rfind("ep_aclk_hbm_00", 0) == 0)
        ulp_clk[2] = static_cast<uint32_t>(ep.offset);
      else if (ep.name.rfind("ep_ucs_control_status_00", 0) == 0)
        ucs = static_cast<uint32_t>(ep.offset);
      // The counters pair with the wizards, in the same order.
      else if (ep.name.rfind("ep_freq_cnt_aclk_kernel_00", 0) == 0)
        freq_cnt[0] = static_cast<uint32_t>(ep.offset);
      else if (ep.name.rfind("ep_freq_cnt_aclk_kernel_01", 0) == 0)
        freq_cnt[1] = static_cast<uint32_t>(ep.offset);
      else if (ep.name.rfind("ep_freq_cnt_aclk_hbm_00", 0) == 0)
        freq_cnt[2] = static_cast<uint32_t>(ep.offset);
    }
    if (!ulp_clk[0] || !ulp_clk[1])
      throw xrt_core::error(std::errc::not_supported,
                            "this xclbin's partition metadata names no pair of kernel clock"
                            " wizards, so the download sequence has nothing to program");

    // Programming a dynamic-IP shell was off by default while it was untried
    // here - the gate is pr_isolate rather than the XDMA axigate, and getting
    // the ICAP sequence wrong leaves the card needing a reset. It has since
    // been exercised: the U50 programs and reprograms, xrt-smi validate passes
    // its whole suite, and hbm_bandwidth runs at 158 GB/s across 12 banks. So
    // it is on, with an escape hatch to turn it off again without a rebuild if
    // a new card misbehaves.
    if (getenv("XRT_MACOS_NO_PROGRAM_ULP"))
      throw xrt_core::error(std::errc::not_supported,
                            "xclbin matches this shell; its clock wizards are at 0x"
                            + hex(ulp_clk[0]) + ", 0x" + hex(ulp_clk[1]) + ", 0x" + hex(ulp_clk[2])
                            + " and the UCS at 0x" + hex(ucs)
                            + ", but programming a dynamic-IP shell is disabled by"
                            " XRT_MACOS_NO_PROGRAM_ULP in the environment");
  }

  auto base = reinterpret_cast<const char*>(top);
  const axlf_section_header* clk_hdr = nullptr;
  const axlf_section_header* bit_hdr = nullptr;
  for (uint32_t i = 0; i < top->m_header.m_numSections; ++i) {
    if (top->m_sections[i].m_sectionKind == CLOCK_FREQ_TOPOLOGY)
      clk_hdr = &top->m_sections[i];
    if (top->m_sections[i].m_sectionKind == BITSTREAM)
      bit_hdr = &top->m_sections[i];
  }
  if (!bit_hdr)
    throw xrt_core::error(std::errc::invalid_argument, "xclbin has no BITSTREAM section");

  // Clock targets, in xocl's order: data, kernel, then system clocks. That is
  // also the order of the wizards, whether they are the platform's own K1/K2 or
  // the ones the partition brings.
  uint32_t target[XRT_LOAD_CLOCKS] = { 0, 0, 0 };
  if (clk_hdr) {
    auto topo = reinterpret_cast<const clock_freq_topology*>(base + clk_hdr->m_sectionOffset);
    int sys = 2;
    for (int16_t i = 0; i < topo->m_count; ++i) {
      const auto& c = topo->m_clock_freq[i];
      if (c.m_type == CT_DATA)
        target[0] = c.m_freq_Mhz;
      else if (c.m_type == CT_KERNEL)
        target[1] = c.m_freq_Mhz;
      else if (c.m_type == CT_SYSTEM && sys < XRT_LOAD_CLOCKS)
        target[sys++] = c.m_freq_Mhz;
    }
  }

  // Bitstream payload after the .bit header; big-endian words to host order.
  auto bit = reinterpret_cast<const unsigned char*>(base + bit_hdr->m_sectionOffset);
  uint32_t bitlen = 0;
  size_t hdrlen = bit_header_length(bit, bit_hdr->m_sectionSize, bitlen);
  if (!hdrlen || (bitlen % 4) != 0)
    throw xrt_core::error(std::errc::invalid_argument, "invalid .bit header in BITSTREAM section");
  std::vector<uint32_t> words(bitlen / 4);
  for (size_t i = 0; i < words.size(); ++i) {
    const unsigned char* w = bit + hdrlen + i * 4;
    words[i] = (uint32_t)w[0] << 24 | (uint32_t)w[1] << 16 | (uint32_t)w[2] << 8 | w[3];
  }

  // Already programmed with this xclbin: nothing to do. xocl does the same
  // (icap compares the cached uuid), and without it every process that opens
  // the device re-downloads the whole bitstream - seconds each - and they
  // contend for the partition hold while doing it.
  auto want = uuid_string(reinterpret_cast<const unsigned char*>(&top->m_header.uuid));
  if (!want.empty() && want == loaded_xclbin_uuid(bus, dev))
    return;

  io_connect_t conn = open_function(bus, dev, 0);
  if (!conn)
    throw xrt_core::error(std::errc::no_such_device, "xrt_user dext not attached to the management function");

  // Keep every process off the partition for the length of the download. An
  // access that lands while the ULP gate is frozen trips the shell's
  // firewalls and takes the card down until it is reset. Released by the
  // destructor, including if the download throws below.
  partition_hold hold{bus, dev};

  // With the hold ours, whoever we queued behind has finished. If they loaded
  // the same xclbin there is nothing left to do.
  if (!want.empty() && want == loaded_xclbin_uuid(bus, dev)) {
    IOServiceClose(conn);
    return;
  }

  uint64_t args[13] = { 0, target[0], target[1], 0, 0,
                       ulp_clk[0], ulp_clk[1], ulp_clk[2], target[2], ucs,
                       freq_cnt[0], freq_cnt[1], freq_cnt[2] };
  std::memcpy(&args[3], &top->m_header.uuid, 8);
  std::memcpy(&args[4], reinterpret_cast<const uint8_t*>(&top->m_header.uuid) + 8, 8);
  xrt_dext_load_result res = {};
  size_t rsz = sizeof(res);
  kern_return_t kr = IOConnectCallMethod(conn, XRT_DEXT_LOAD_BITSTREAM, args, 13,
                                         words.data(), words.size() * 4, nullptr, nullptr, &res, &rsz);
  if (kr != KERN_SUCCESS)
    IOServiceClose(conn);
  if (kr != KERN_SUCCESS)
    throw xrt_core::error(std::errc::io_error, "LOAD_BITSTREAM call failed: " + std::to_string(kr));
  if (res.status == 0) {
    // The CUs and the memory of this xclbin may not be where the driver looks
    // by default, so point it at them before anything tries to use them.
    set_cu_window(bus, dev, top);
    set_card_range(bus, dev, top);

    // Keep the metadata other processes need (xocl keeps it in the kernel).
    const std::pair<axlf_section_kind, uint32_t> keep[] = {
      {MEM_TOPOLOGY, XRT_SECTION_MEM_TOPOLOGY},
      {IP_LAYOUT, XRT_SECTION_IP_LAYOUT},
      {CONNECTIVITY, XRT_SECTION_CONNECTIVITY},
      // Only present when the xclbin groups banks behind an argument; the
      // readers fall back to the plain sections when they are missing.
      {ASK_GROUP_TOPOLOGY, XRT_SECTION_GROUP_TOPOLOGY},
      {ASK_GROUP_CONNECTIVITY, XRT_SECTION_GROUP_CONNECTIVITY},
    };
    for (auto [kind, slot] : keep) {
      for (uint32_t i = 0; i < top->m_header.m_numSections; ++i) {
        const auto& sh = top->m_sections[i];
        if (sh.m_sectionKind != kind || sh.m_sectionSize == 0 || sh.m_sectionSize > XRT_SECTION_MAX_BYTES)
          continue;
        uint64_t s = slot;
        IOConnectCallMethod(conn, XRT_DEXT_SET_SECTION, &s, 1, base + sh.m_sectionOffset, sh.m_sectionSize,
                            nullptr, nullptr, nullptr, nullptr);
        break;
      }
    }
  }
  IOServiceClose(conn);
  if (res.status != 0) {
    char msg[256];
    std::snprintf(msg, sizeof(msg),
                  "xclbin download failed at step %u (status 0x%x): %u words written, ICAP SR 0x%x, "
                  "gate 0x%x, clocks 0x%x/0x%x, calibration 0x%x",
                  res.step, res.status, res.words_written, res.icap_sr_after, res.gate_after,
                  res.clk_status[0], res.clk_status[1], res.calib);
    throw xrt_core::error(std::errc::io_error, msg);
  }
}

std::string
loaded_xclbin_uuid(uint16_t bus, uint16_t dev)
{
  io_connect_t conn = open_function(bus, dev, 0);
  if (!conn)
    return {};
  uint8_t uuid[16] = {};
  size_t size = sizeof(uuid);
  kern_return_t kr = IOConnectCallStructMethod(conn, XRT_DEXT_GET_XCLBIN_UUID, nullptr, 0, uuid, &size);
  IOServiceClose(conn);
  static const uint8_t zero[16] = {};
  if (kr != KERN_SUCCESS || std::memcmp(uuid, zero, 16) == 0)
    return {};
  char buf[37];
  std::snprintf(buf, sizeof(buf),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
                uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
  return buf;
}

// One connection per process to the user function, kept open.
static io_connect_t
user_conn(uint16_t bus, uint16_t dev)
{
  static std::mutex mutex;
  static std::map<std::pair<uint16_t, uint16_t>, io_connect_t> conns;
  std::lock_guard lk(mutex);
  auto key = std::make_pair(bus, dev);
  if (auto it = conns.find(key); it != conns.end())
    return it->second;
  io_connect_t conn = open_function(bus, dev, 1);
  if (!conn)
    throw xrt_core::error(std::errc::no_such_device, "xrt_user dext not attached to the user function");
  conns[key] = conn;
  return conn;
}

uint64_t
mem_alloc(uint16_t bus, uint16_t dev, uint64_t bank_base, uint64_t bank_size, uint64_t size)
{
  uint64_t in[3] = { bank_base, bank_size, size };
  uint64_t addr = 0;
  uint32_t nout = 1;
  kern_return_t kr = IOConnectCallScalarMethod(user_conn(bus, dev), XRT_DEXT_MEM_ALLOC, in, 3, &addr, &nout);
  if (kr != KERN_SUCCESS)
    throw xrt_core::error(std::errc::not_enough_memory,
                          "device memory allocation of " + std::to_string(size) + " bytes failed: " + std::to_string(kr));
  return addr;
}

void
mem_free(uint16_t bus, uint16_t dev, uint64_t addr)
{
  uint64_t in = addr;
  IOConnectCallScalarMethod(user_conn(bus, dev), XRT_DEXT_MEM_FREE, &in, 1, nullptr, nullptr);
}

std::pair<uint64_t, uint64_t>
mem_stat(uint16_t bus, uint16_t dev, uint64_t bank_base, uint64_t bank_size)
{
  uint64_t in[2] = { bank_base, bank_size };
  uint64_t out[2] = { 0, 0 };
  uint32_t nout = 2;
  if (IOConnectCallScalarMethod(user_conn(bus, dev), XRT_DEXT_MEM_STAT, in, 2, out, &nout) != KERN_SUCCESS)
    return {0, 0};
  return {out[0], out[1]};
}

void
cu_write(uint16_t bus, uint16_t dev, uint64_t offset, const uint32_t* values, size_t count)
{
  uint64_t off = offset;
  kern_return_t kr = IOConnectCallMethod(user_conn(bus, dev), XRT_DEXT_CU_WRITE, &off, 1,
                                         values, count * 4, nullptr, nullptr, nullptr, nullptr);
  if (kr != KERN_SUCCESS)
    throw xrt_core::error(std::errc::io_error, "CU register write at 0x" + std::to_string(offset)
                          + " failed: " + std::to_string(kr));
}

// The argument words ride in the scalar array when they fit. A structure
// input of any size costs about 10 us on top of a call into the dext, while
// extra scalars are free; a command's whole register map is normally well
// under the 28 words that fit (driver/macos/tools/lat_parts.cpp).
bool
cu_exec(uint16_t bus, uint16_t dev, uint64_t cu_base, uint64_t arg_offset,
        const uint32_t* values, size_t count, uint32_t* ctrl)
{
  uint64_t in[16] = { cu_base, arg_offset | (uint64_t(count) << 32) };
  uint32_t nin = 2;
  const void* struct_in = nullptr;
  size_t struct_bytes = 0;
  if (count > XRT_CU_EXEC_INLINE_WORDS) {
    struct_in = values;
    struct_bytes = count * 4;
  }
  else {
    for (size_t i = 0; i < count; ++i)
      in[2 + i / 2] |= uint64_t(values[i]) << (32 * (i & 1));
    nin = 2 + uint32_t((count + 1) / 2);
  }
  uint64_t out = 0;
  uint32_t nout = 1;
  kern_return_t kr = IOConnectCallMethod(user_conn(bus, dev), XRT_DEXT_CU_EXEC, in, nin,
                                         struct_in, struct_bytes, &out, &nout, nullptr, nullptr);
  if (ctrl)
    *ctrl = static_cast<uint32_t>(out);
  if (kr == kIOReturnBusy)
    return false;              // another process got the CU first; try again
  if (kr != KERN_SUCCESS)
    throw xrt_core::error(std::errc::io_error, "starting the CU at 0x" + std::to_string(cu_base)
                          + " failed: " + std::to_string(kr));
  return true;
}

void
cu_ctx_open(uint16_t bus, uint16_t dev, uint32_t cuidx, bool shared, uint64_t base)
{
  uint64_t in[3] = { cuidx, shared ? uint64_t{XRT_CU_CTX_SHARED} : uint64_t{XRT_CU_CTX_EXCLUSIVE},
                     base };
  kern_return_t kr = IOConnectCallScalarMethod(user_conn(bus, dev), XRT_DEXT_CU_CTX_OPEN,
                                              in, 3, nullptr, nullptr);
  if (kr == kIOReturnExclusiveAccess)
    throw xrt_core::error(std::errc::device_or_resource_busy,
                          "CU " + std::to_string(cuidx) + " is already open in a conflicting mode "
                          "by another process or context");
  if (kr != KERN_SUCCESS)
    throw xrt_core::error(std::errc::io_error, "opening a context on CU " + std::to_string(cuidx)
                          + " failed: " + std::to_string(kr));
}

void
cu_ctx_close(uint16_t bus, uint16_t dev, uint32_t cuidx)
{
  uint64_t in = cuidx;
  IOConnectCallScalarMethod(user_conn(bus, dev), XRT_DEXT_CU_CTX_CLOSE, &in, 1, nullptr, nullptr);
}

// The mappings live on the process's connection to the user function, one per
// CU, and are kept so that a second hardware context on the same CU reuses
// one rather than mapping twice.
namespace {

std::mutex& cu_rw_mutex() { static std::mutex m; return m; }

std::map<std::tuple<uint16_t, uint16_t, uint32_t>, mach_vm_address_t>&
cu_rw_maps()
{
  static std::map<std::tuple<uint16_t, uint16_t, uint32_t>, mach_vm_address_t> m;
  return m;
}

} // namespace

volatile uint32_t*
map_cu_rw(uint16_t bus, uint16_t dev, uint32_t cuidx)
{
  std::lock_guard lk(cu_rw_mutex());
  auto key = std::make_tuple(bus, dev, cuidx);
  auto& maps = cu_rw_maps();
  if (auto it = maps.find(key); it != maps.end())
    return reinterpret_cast<volatile uint32_t*>(it->second);
  io_connect_t conn = IO_OBJECT_NULL;
  try {
    conn = user_conn(bus, dev);
  }
  catch (const std::exception&) {
    return nullptr;
  }
  mach_vm_address_t addr = 0;
  mach_vm_size_t len = 0;
  if (IOConnectMapMemory64(conn, XRT_DEXT_MAP_CU_RW(cuidx), mach_task_self(), &addr, &len,
                           kIOMapAnywhere | kIOMapInhibitCache) != KERN_SUCCESS)
    return nullptr;            // not granted; the caller uses cu_exec instead
  maps[key] = addr;
  return reinterpret_cast<volatile uint32_t*>(addr);
}

void
unmap_cu_rw(uint16_t bus, uint16_t dev, uint32_t cuidx)
{
  std::lock_guard lk(cu_rw_mutex());
  auto& maps = cu_rw_maps();
  auto it = maps.find(std::make_tuple(bus, dev, cuidx));
  if (it == maps.end())
    return;
  IOConnectUnmapMemory64(user_conn(bus, dev), XRT_DEXT_MAP_CU_RW(cuidx), mach_task_self(),
                         it->second);
  maps.erase(it);
}

void
dma_sync(uint16_t bus, uint16_t dev, uint64_t card_addr, void* host, size_t length, bool to_card)
{
  // Set XRT_MACOS_SYNC_PROFILE=1 to split this call three ways: the work before
  // the trap, the trap itself, and whatever XRT spends above us (the caller's
  // measurement minus the total here). Items 50 and 51 took everything they
  // could out of the dext; the remainder is on this side and had never been
  // looked at. Off unless asked for - it is two clock reads per sync.
  static const bool profile = getenv("XRT_MACOS_SYNC_PROFILE") != nullptr;
  uint64_t t0 = 0, t1 = 0;
  if (profile)
    t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  io_connect_t conn = user_conn(bus, dev);
  uint64_t args[3] = { card_addr, length, uint64_t(to_card ? XRT_DMA_TO_CARD : XRT_DMA_TO_HOST) };
  uint64_t status = 0;
  uint32_t nout = 1;
  size_t outsz = length;
  if (profile)
    t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  kern_return_t kr = to_card
    ? IOConnectCallMethod(conn, XRT_DEXT_SYNC, args, 3, host, length, &status, &nout, nullptr, nullptr)
    : IOConnectCallMethod(conn, XRT_DEXT_SYNC, args, 3, nullptr, 0, &status, &nout, host, &outsz);
  if (profile) {
    static std::mutex pm;
    static uint64_t n = 0, ns_pre = 0, ns_call = 0;
    const uint64_t t2 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    std::lock_guard lk(pm);
    ns_pre += t1 - t0;
    ns_call += t2 - t1;
    if (++n >= 2000) {
      std::fprintf(stderr, "[sync profile] %llu syncs: conn+args %llu ns, IOConnectCallMethod %llu ns\n",
                   (unsigned long long)n, (unsigned long long)(ns_pre / n),
                   (unsigned long long)(ns_call / n));
      n = ns_pre = ns_call = 0;
    }
  }
  if (kr != KERN_SUCCESS) {
    char msg[160];
    std::snprintf(msg, sizeof(msg), "DMA %s card 0x%llx, %zu bytes failed: 0x%x (engine status 0x%llx)",
                  to_card ? "to card" : "from card", (unsigned long long)card_addr, length, kr,
                  (unsigned long long)status);
    throw xrt_core::error(std::errc::io_error, msg);
  }
}

// The hold is taken on the user function, which is where DMA and CU writes
// come from. user_conn() throws if no dext is attached there; that is not an
// error for us, because nothing could then be reaching the partition.
partition_hold::partition_hold(uint16_t bus, uint16_t dev)
  : m_bus(bus), m_dev(dev)
{
  try {
    // Another process programming the card holds this, and a download takes
    // seconds, so wait for our turn rather than pressing on without it:
    // proceeding unheld means every access afterwards is refused, which is how
    // concurrent loads failed outright instead of queueing.
    auto conn = user_conn(bus, dev);
    kern_return_t kr = kIOReturnBusy;
    for (int i = 0; i < 300 && kr != KERN_SUCCESS; ++i) {   // up to ~30 s
      kr = IOConnectCallScalarMethod(conn, XRT_DEXT_PARTITION_HOLD, nullptr, 0, nullptr, nullptr);
      if (kr != KERN_SUCCESS)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (kr == KERN_SUCCESS)
      m_held = true;
    else
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
                              "could not hold the user partition before reprogramming it: "
                              + std::to_string(kr)
                              + ". Another process may be programming the card.");
  }
  catch (const std::exception&) {
    // No dext on the user function: nothing to keep off the partition.
  }
}

partition_hold::~partition_hold()
{
  if (!m_held)
    return;
  try {
    IOConnectCallScalarMethod(user_conn(m_bus, m_dev), XRT_DEXT_PARTITION_RELEASE,
                              nullptr, 0, nullptr, nullptr);
  }
  catch (const std::exception&) {
    // The connection is gone, which releases the hold in the dext anyway.
  }
}

void
hot_reset(uint16_t bus, uint16_t dev)
{
  // A reset takes the partition down just as a download does.
  partition_hold hold{bus, dev};

  io_connect_t conn = open_function(bus, dev, 0);
  if (!conn)
    throw xrt_core::error(std::errc::no_such_device, "xrt_user dext not attached to the management function");
  xrt_dext_reset_result res = {};
  size_t sz = sizeof(res);
  kern_return_t kr = IOConnectCallStructMethod(conn, XRT_DEXT_HOT_RESET, nullptr, 0, &res, &sz);
  IOServiceClose(conn);
  if (kr != KERN_SUCCESS)
    throw xrt_core::error(std::errc::io_error, "HOT_RESET call failed: " + std::to_string(kr));
  if (res.status != 0) {
    char msg[256];
    std::snprintf(msg, sizeof(msg),
                  "hot reset failed (status 0x%x, reset 0x%x): ROM magic 0x%x, firewalls 0x%x 0x%x 0x%x 0x%x",
                  res.status, res.reset_status, res.rom_magic_after,
                  res.fw_after[0], res.fw_after[1], res.fw_after[2], res.fw_after[3]);
    throw xrt_core::error(std::errc::io_error, msg);
  }
}

std::vector<char>
loaded_section(uint16_t bus, uint16_t dev, uint32_t slot)
{
  io_connect_t conn = open_function(bus, dev, 0);
  if (!conn)
    return {};
  std::vector<char> result;
  uint64_t total = 0;
  do {
    uint64_t in[2] = { slot, result.size() };
    uint64_t out = 0;
    uint32_t nout = 1;
    char chunk[XRT_SECTION_CHUNK];
    size_t n = sizeof(chunk);
    kern_return_t kr = IOConnectCallMethod(conn, XRT_DEXT_GET_SECTION, in, 2, nullptr, 0,
                                           &out, &nout, chunk, &n);
    if (kr != KERN_SUCCESS || out > XRT_SECTION_MAX_BYTES) {
      result.clear();
      break;
    }
    total = out;
    if (n == 0)
      break;
    result.insert(result.end(), chunk, chunk + n);
  } while (result.size() < total);
  IOServiceClose(conn);
  if (result.size() != total)
    result.clear();
  return result;
}

} // xrt_core::macos
