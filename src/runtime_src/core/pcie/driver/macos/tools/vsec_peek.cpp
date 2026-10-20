// SPDX-License-Identifier: Apache-2.0
// Reconnaissance for dynamic-IP platforms such as the Alveo U50, whose blocks
// are not at the XDMA 5.x offsets this driver otherwise assumes.
//
// Walks the PCIe extended capability list of both functions, finds the vendor
// capability that locates the platform's own description, and dumps the table
// it points at: the UUID ROM, the flash controller, platform info, the
// mailbox. Read-only throughout - configuration space and BAR reads only.
//
// usage: vsec_peek [--dump-rom]
#include <IOKit/IOKitLib.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../XrtUser/xrt_dext_abi.h"

namespace {

// PCIe extended capability list
constexpr uint32_t kExtCapStart   = 0x100;
constexpr uint16_t kExtCapVendor  = 0x000b;   // PCI_EXT_CAP_ID_VNDR
// Vendor capability ids the Linux driver looks for
constexpr uint16_t kVsecAlf       = 0x0020;   // locates the platform description
constexpr uint16_t kVsecXlat      = 0x0040;   // address translation / pass through

// The dword at the platform-info block says which kind of platform this is.
constexpr uint32_t kPlatRecovery  = 0;        // golden image, no user shell
constexpr uint32_t kPlat1RP       = 1;
constexpr uint32_t kPlat2RP       = 2;

const char*
plat_name(uint32_t v)
{
  switch (v) {
  case kPlatRecovery: return "RECOVERY (golden image: no user shell is loaded)";
  case kPlat1RP:      return "1RP (single reconfigurable partition)";
  case kPlat2RP:      return "2RP (base plus reconfigurable partition)";
  default:            return "unrecognised";
  }
}

// The logic uuid as the Linux driver builds it: dwords read downwards from
// offset 12, each printed as %08x. That string is what names the directory
// the platform's description is looked up in.
std::string
logic_uuid(const volatile uint32_t* rom)
{
  char buf[33] = {};
  int j = 0;
  for (int i = 12 / 4; i >= 0 && j < 32; --i, j += 8)
    std::snprintf(buf + j, 9, "%08x", rom[i]);
  return buf;
}

const char*
entry_name(uint32_t type)
{
  switch (type) {
  case 0x50: return "UUID ROM (replaces the fixed feature ROM)";
  case 0x51: return "flash controller";
  case 0x52: return "platform info";
  case 0x53: return "mailbox";
  case 0x54: return "XGQ";
  case 0x55: return "XGQ VMR payload";
  default:   return "unknown";
  }
}

struct fn
{
  io_connect_t conn = 0;
  uint16_t vendor = 0, device = 0, subsys = 0;
  uint8_t bus = 0, dev = 0, func = 0;
  uint64_t bar_size[6] = {};
  bool bar_present[6] = {};
};

bool
cfg(io_connect_t c, uint64_t off, uint32_t* out)
{
  uint64_t in = off, o = 0;
  uint32_t n = 1;
  if (IOConnectCallScalarMethod(c, XRT_DEXT_CFG_READ, &in, 1, &o, &n) != KERN_SUCCESS)
    return false;
  *out = static_cast<uint32_t>(o);
  return true;
}

// Map a BAR read-only and return its base, or nullptr.
volatile uint32_t*
map_bar(io_connect_t c, uint32_t bar, uint64_t* len_out)
{
  mach_vm_address_t a = 0;
  mach_vm_size_t l = 0;
  if (IOConnectMapMemory64(c, XRT_DEXT_MAP_BAR(bar), mach_task_self(), &a, &l,
                           kIOMapAnywhere | kIOMapInhibitCache) != KERN_SUCCESS)
    return nullptr;
  if (len_out)
    *len_out = l;
  return reinterpret_cast<volatile uint32_t*>(a);
}

void
hexdump(const volatile uint32_t* p, size_t words)
{
  std::string ascii;
  for (size_t i = 0; i < words; ++i) {
    if (i % 4 == 0)
      std::printf("      +%03zx:", i * 4);
    uint32_t v = p[i];
    std::printf(" %08x", v);
    for (int b = 0; b < 4; ++b) {
      char ch = static_cast<char>((v >> (8 * b)) & 0xff);
      ascii += std::isprint(static_cast<unsigned char>(ch)) ? ch : '.';
    }
    if (i % 4 == 3) {
      std::printf("  |%s|\n", ascii.c_str());
      ascii.clear();
    }
  }
  if (!ascii.empty())
    std::printf("  |%s|\n", ascii.c_str());
}

// The table the vendor capability points at.
void
dump_vsec_table(const fn& f, uint32_t bar, uint64_t off, bool dump_rom)
{
  if (bar > 5 || !f.bar_present[bar]) {
    std::printf("    table is in BAR%u, which this function does not expose\n", bar);
    return;
  }
  uint64_t len = 0;
  auto base = map_bar(f.conn, bar, &len);
  if (!base) {
    std::printf("    could not map BAR%u\n", bar);
    return;
  }
  if (off + 16 > len) {
    std::printf("    offset 0x%llx is past the end of BAR%u (%llu bytes)\n", off, bar, len);
    return;
  }
  auto hdr = base + off / 4;
  uint32_t format = hdr[0], length = hdr[1], entry_sz = hdr[2] & 0xff;
  std::printf("    header: format 0x%08x  length %u  entry size %u\n", format, length, entry_sz);
  if (entry_sz == 0 || length <= 16 || off + length > len) {
    std::printf("    header does not look valid; stopping\n");
    return;
  }
  for (uint32_t i = 16; i + 8 <= length; i += entry_sz) {
    uint32_t lo = hdr[i / 4], hi = hdr[i / 4 + 1];
    uint32_t type = lo & 0xff, rev = (lo >> 8) & 0x1f, ebar = (lo >> 13) & 0x7;
    uint64_t addr = (static_cast<uint64_t>(hi) << 16) | ((lo & 0xffff0000u) >> 16);
    if (type == 0xff)
      break;                    // end of table
    std::printf("    type 0x%02x rev %u  BAR%u + 0x%010llx   %s\n",
                type, rev, ebar, addr, entry_name(type));

    // Reach into the blocks that say what this platform is.
    if (ebar > 5 || !f.bar_present[ebar])
      continue;
    uint64_t blen = len;
    auto bb = base;
    if (ebar != bar) {
      blen = 0;
      bb = map_bar(f.conn, ebar, &blen);
    }
    if (!bb || addr + 64 > blen)
      continue;
    auto blk = bb + addr / 4;

    if (type == 0x50) {
      auto uuid = logic_uuid(blk);
      std::printf("      logic uuid: %s\n", uuid.c_str());
      std::printf("      the platform description is looked up as\n");
      std::printf("        <firmware dir>/%s/partition.xsabin\n", uuid.c_str());
      if (dump_rom) {
        std::printf("      first 256 bytes:\n");
        hexdump(blk, 64);
      }
    }
    else if (type == 0x52) {
      uint32_t v = blk[0];
      std::printf("      platform type %u: %s\n", v, plat_name(v));
      if (dump_rom) {
        std::printf("      first 64 bytes:\n");
        hexdump(blk, 16);
      }
    }
  }
}

void
walk(const fn& f, bool dump_rom)
{
  std::printf("\n%02x:%02x.%x  %04x:%04x subsys %04x\n", f.bus, f.dev, f.func,
              f.vendor, f.device, f.subsys);
  for (uint32_t b = 0; b < 6; ++b)
    if (f.bar_present[b])
      std::printf("  BAR%u: %llu KiB\n", b, f.bar_size[b] / 1024);

  uint32_t off = kExtCapStart, guard = 0;
  bool any = false, found_alf = false;
  while (off && off < XRT_CFG_SPACE_BYTES && guard++ < 64) {
    uint32_t hdr = 0;
    if (!cfg(f.conn, off, &hdr) || hdr == 0 || hdr == 0xffffffff)
      break;
    uint16_t id = hdr & 0xffff;
    uint32_t ver = (hdr >> 16) & 0xf, next = (hdr >> 20) & 0xffc;
    any = true;
    if (id == kExtCapVendor) {
      uint32_t vh = 0;
      cfg(f.conn, off + 4, &vh);
      uint16_t vsec_id = vh & 0xffff;
      uint32_t vsec_rev = (vh >> 16) & 0xf, vsec_len = (vh >> 20) & 0xfff;
      std::printf("  ext cap 0x%04x (vendor) at 0x%03x: vsec id 0x%04x rev %u len %u%s\n",
                  id, off, vsec_id, vsec_rev, vsec_len,
                  vsec_id == kVsecAlf  ? "  <- locates the platform description"
                : vsec_id == kVsecXlat ? "  <- address translation"
                                       : "");
      if (vsec_id == kVsecAlf) {
        found_alf = true;
        uint32_t lo = 0, hi = 0;
        cfg(f.conn, off + 8, &lo);
        cfg(f.conn, off + 12, &hi);
        uint32_t bar = lo & 0xf;
        uint64_t voff = (static_cast<uint64_t>(hi) << 32) | (lo & 0xfffffff0u);
        std::printf("    points at BAR%u + 0x%llx\n", bar, voff);
        dump_vsec_table(f, bar, voff, dump_rom);
      }
    }
    else {
      std::printf("  ext cap 0x%04x at 0x%03x (ver %u)\n", id, off, ver);
    }
    if (next <= off)
      break;
    off = next;
  }
  if (!any)
    std::printf("  no extended capabilities: this is not a dynamic-IP platform\n");
  else if (!found_alf)
    std::printf("  no vendor capability 0x%04x: the platform does not describe itself this way\n",
                kVsecAlf);
}

} // namespace

int
main(int argc, char** argv)
{
  bool dump_rom = argc > 1 && std::string(argv[1]) == "--dump-rom";

  io_iterator_t it = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it)) {
    std::fprintf(stderr, "xrt_user dext not attached\n");
    return 2;
  }
  std::vector<fn> fns;
  while (io_service_t svc = IOIteratorNext(it)) {
    fn f;
    if (IOServiceOpen(svc, mach_task_self(), 0, &f.conn) == KERN_SUCCESS) {
      xrt_dext_device_info info{};
      size_t sz = sizeof(info);
      if (IOConnectCallStructMethod(f.conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &sz)
          == KERN_SUCCESS) {
        f.vendor = info.vendor; f.device = info.device; f.subsys = info.subsystem_id;
        f.bus = info.bus; f.dev = info.dev; f.func = info.func;
        for (int b = 0; b < 6; ++b) {
          f.bar_present[b] = info.bars[b].present;
          f.bar_size[b] = info.bars[b].size;
        }
        if (info.abi_version != XRT_DEXT_ABI_VERSION)
          std::printf("warning: tool built for ABI %u, dext reports %u\n",
                      XRT_DEXT_ABI_VERSION, info.abi_version);
        fns.push_back(f);
      }
      else
        IOServiceClose(f.conn);
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  if (fns.empty()) {
    std::fprintf(stderr, "no functions answered\n");
    return 2;
  }
  for (const auto& f : fns)
    walk(f, dump_rom);
  for (const auto& f : fns)
    IOServiceClose(f.conn);
  std::printf("\n");
  return 0;
}
