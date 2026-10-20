// SPDX-License-Identifier: Apache-2.0
// Phase 3 test tool: program an xclbin into the card through the xrt_user
// dext on the management function.
// Usage: xrt_dext_load <file.xclbin> [--state-only] [--ulp-clk <hex>,<hex>]
//
// --ulp-clk names the kernel clock wizards to save and re-commit, for a
// platform that keeps them in the user partition the xclbin brings rather than
// in the base platform (a U50: 0x1003000,0x1004000). Without it the platform's
// own wizards are used, which is right for an XDMA 5.x card.
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "../XrtUser/xrt_dext_abi.h"
#include "../XrtUser/clkwiz_table.h"
#include "xclfeatures.h"
#include "xrt/detail/xclbin.h"

static const char* step_name[] = {
  "check", "clock save", "gate freeze", "ICAP write", "ICAP done",
  "clock restore", "gate free", "calibration", "complete"
};

// Open the dext instance on the management function (func 0) of the first card.
static io_connect_t
open_mgmt(unsigned& bus, unsigned& dev)
{
  io_iterator_t it = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it) != KERN_SUCCESS)
    return IO_OBJECT_NULL;
  io_connect_t conn = IO_OBJECT_NULL;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_registry_entry_t parent = IO_OBJECT_NULL;
    unsigned b = 0, d = 0, f = 9;
    if (IORegistryEntryGetParentEntry(svc, kIOServicePlane, &parent) == KERN_SUCCESS) {
      if (auto prop = IORegistryEntryCreateCFProperty(parent, CFSTR("pcidebug"), kCFAllocatorDefault, 0)) {
        char buf[64] = {0};
        if (CFStringGetCString(static_cast<CFStringRef>(prop), buf, sizeof(buf), kCFStringEncodingUTF8))
          std::sscanf(buf, "%u:%u:%u", &b, &d, &f);
        CFRelease(prop);
      }
      IOObjectRelease(parent);
    }
    if (!conn && f == 0 && IOServiceOpen(svc, mach_task_self(), 0, &conn) == KERN_SUCCESS) {
      bus = b;
      dev = d;
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  return conn;
}

// Mirrors xrt_xclbin_parse_header() in xocl: returns the header length, or 0.
static size_t
bit_header_length(const unsigned char* d, size_t size, uint32_t& bitlen)
{
  size_t i = 0;
  auto u16 = [&]() { unsigned v = d[i] << 8 | d[i + 1]; i += 2; return v; };
  if (size < 64)
    return 0;
  unsigned magic_len = u16();
  for (unsigned k = 0; k < magic_len - 1; ++k, ++i)
    if (d[i] != ((k % 2) ? 0xf0 : 0x0f))
      return 0;
  ++i;                                           // null end of magic
  if (u16() != 0x01)
    return 0;
  for (char field : {'a', 'b', 'c', 'd'}) {
    if (i + 3 > size || d[i++] != field)
      return 0;
    unsigned len = u16();
    if (i + len > size || len == 0 || d[i + len - 1] != '\0')
      return 0;
    if (field == 'b')
      std::printf("  bitstream part    : %s\n", reinterpret_cast<const char*>(d + i));
    i += len;
  }
  if (i + 5 > size || d[i++] != 'e')
    return 0;
  bitlen = (uint32_t)d[i] << 24 | (uint32_t)d[i + 1] << 16 | (uint32_t)d[i + 2] << 8 | d[i + 3];
  i += 4;
  return (i + bitlen <= size) ? i : 0;
}

// UltraScale clock wizard frequency, as xocl clock_wiz.c computes it.
static unsigned
clkwiz_mhz(uint32_t cfg0, uint32_t cfg2)
{
  uint64_t div0 = cfg0 & 0xff, mul0 = (cfg0 >> 8) & 0xff, mul_frac0 = 0;
  if (cfg0 & (1u << 26))
    mul_frac0 = (cfg0 >> 16) & 0x3ff;
  mul0 = mul0 * 1000 + mul_frac0;
  div0 *= 1000;
  uint64_t div1 = cfg2 & 0xff, div_frac1 = 0;
  if (cfg2 & (1u << 18))
    div_frac1 = (cfg2 >> 8) & 0x3ff;
  div1 = div1 * 1000 + div_frac1;
  div0 *= div1;
  mul0 *= 1000;
  return div0 ? (unsigned)((100 * mul0) / div0) : 0;
}

// Same round-down lookup as the dext (xocl find_matching_freq_config).
static unsigned
table_mhz(unsigned freq)
{
  const unsigned size = XRT_CLKWIZ_TABLE_SIZE;
  unsigned start = 0, end = size - 1, idx = size - 1;
  if (freq < xrt_clkwiz_table[0].ocl) return xrt_clkwiz_table[0].ocl;
  if (freq > xrt_clkwiz_table[size - 1].ocl) return xrt_clkwiz_table[size - 1].ocl;
  while (start < end) {
    if (freq == xrt_clkwiz_table[idx].ocl) break;
    if (freq < xrt_clkwiz_table[idx].ocl) end = idx; else start = idx + 1;
    idx = start + (end - start) / 2;
  }
  if (freq < xrt_clkwiz_table[idx].ocl) idx--;
  return xrt_clkwiz_table[idx].ocl;
}

static void
print_result(const xrt_dext_load_result& r)
{
  std::printf("result: status 0x%x, reached step '%s'\n", r.status,
              r.step <= XRT_LOAD_STEP_COMPLETE ? step_name[r.step] : "?");
  std::printf("  ICAP   SR before 0x%x after 0x%x, FIFO vacancy %u, %u words in %u ms\n",
              r.icap_sr_before, r.icap_sr_after, r.icap_wfv, r.words_written, r.elapsed_ms);
  std::printf("  gate   before 0x%x after 0x%x (0 = frozen)\n", r.gate_before, r.gate_after);
  static const char* kClockName[XRT_LOAD_CLOCKS] = { "data", "kernel", "system" };
  for (int i = 0; i < XRT_LOAD_CLOCKS; ++i)
    std::printf("  clock  %-6s status 0x%x, %u MHz (cfg0 0x%08x cfg2 0x%08x)"
                " target %u MHz, table %u MHz\n", kClockName[i],
                r.clk_status[i], clkwiz_mhz(r.clk_cfg0[i], r.clk_cfg2[i]),
                r.clk_cfg0[i], r.clk_cfg2[i], r.target_mhz[i], r.table_mhz[i]);
  std::printf("  memory calibration 0x%x (bit0 = done) after %u ms\n", r.calib, r.calib_ms);
  if (r.ucs_after)
    std::printf("  UCS    channel 1 after enable 0x%x (bit0 = clocks were shut down)\n", r.ucs_after);
}

int main(int argc, char** argv)
{
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <file.xclbin> [--state-only] [--ulp-clk <hex>,<hex>]\n", argv[0]);
    return 2;
  }
  bool state_only = false;
  uint64_t ulp_clk[3] = { 0, 0, 0 };
  uint64_t ucs_off = 0;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--state-only") == 0)
      state_only = true;
    else if (std::strcmp(argv[i], "--ucs") == 0 && i + 1 < argc)
      std::sscanf(argv[++i], "%llx", &ucs_off);
    else if (std::strcmp(argv[i], "--ulp-clk") == 0 && i + 1 < argc) {
      if (std::sscanf(argv[++i], "%llx,%llx,%llx", &ulp_clk[0], &ulp_clk[1], &ulp_clk[2]) < 2) {
        std::fprintf(stderr, "--ulp-clk wants two or three hex offsets, comma separated\n");
        return 2;
      }
    }
  }

  std::ifstream f(argv[1], std::ios::binary);
  std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (buf.size() < sizeof(axlf) || std::memcmp(buf.data(), "xclbin2", 7) != 0) {
    std::fprintf(stderr, "%s: not an xclbin2 file\n", argv[1]);
    return 1;
  }
  auto top = reinterpret_cast<const axlf*>(buf.data());
  if (top->m_header.m_length > buf.size()) {
    std::fprintf(stderr, "truncated xclbin\n");
    return 1;
  }
  std::printf("xclbin %s: %llu bytes, %u sections, mode %u\n", argv[1],
              (unsigned long long)top->m_header.m_length, top->m_header.m_numSections, top->m_header.m_mode);

  unsigned bus = 0, dev = 0;
  io_connect_t conn = open_mgmt(bus, dev);
  if (!conn) {
    std::fprintf(stderr, "no xrt_user dext on a management function\n");
    return 1;
  }
  std::printf("management function %02x:%02x.0\n", bus, dev);

  // 1. Same shell: static UUID and feature ROM timestamp, as xclmgmt checks.
  // A dynamic-IP platform has no feature ROM and its xclbin leaves the header
  // uuid zeroed, so both sides of that comparison are missing; the shim checks
  // those through partition metadata instead. This tool is for reading state,
  // so it says what it cannot check and carries on rather than stopping.
  uint8_t rom_raw[XRT_DEXT_ROM_SIZE] = {};
  size_t sz = sizeof(rom_raw);
  if (IOConnectCallStructMethod(conn, XRT_DEXT_READ_ROM, nullptr, 0, rom_raw, &sz) != KERN_SUCCESS) {
    std::printf("  shell match       : no feature ROM on this platform, not checked here\n");
    if (!state_only) {
      std::fprintf(stderr, "refusing to program: this tool cannot check the xclbin against a shell"
                           " with no feature ROM; use xrt-smi/XRT for that\n");
      return 1;
    }
  }
  else {
    FeatureRomHeader rom;
    std::memcpy(&rom, rom_raw, sizeof(rom));
    bool uuid_ok = std::memcmp(rom.uuid, top->m_header.m_interface_uuid, 16) == 0;
    bool ts_ok = rom.TimeSinceEpoch == top->m_header.m_featureRomTimeStamp;
    std::printf("  shell match       : UUID %s, ROM timestamp %s\n", uuid_ok ? "ok" : "MISMATCH", ts_ok ? "ok" : "MISMATCH");
    if (!uuid_ok || !ts_ok || top->m_header.m_mode == XCLBIN_FLAT) {
      std::fprintf(stderr, "refusing: xclbin is not for this shell (or is a flat image)\n");
      return 1;
    }
  }

  // 2. Current state.
  xrt_dext_load_result res = {};
  size_t rsz = sizeof(res);
  uint64_t state_args[10] = { XRT_DEXT_LOAD_STATE_ONLY, 0, 0, 0, 0,
                             ulp_clk[0], ulp_clk[1], ulp_clk[2], 0, ucs_off };
  if (IOConnectCallMethod(conn, XRT_DEXT_LOAD_BITSTREAM, state_args, 10, nullptr, 0,
                          nullptr, nullptr, &res, &rsz) != KERN_SUCCESS) {
    std::fprintf(stderr, "LOAD_BITSTREAM (state) call failed\n");
    return 1;
  }
  std::printf("current card state:\n");
  print_result(res);
  if (res.status != 0)
    return 1;
  unsigned cur[XRT_LOAD_CLOCKS];
  for (int i = 0; i < XRT_LOAD_CLOCKS; ++i)
    cur[i] = clkwiz_mhz(res.clk_cfg0[i], res.clk_cfg2[i]);

  // data, kernel, then system clocks - the order of the wizards. 0 = keep.
  uint32_t target[XRT_LOAD_CLOCKS] = { 0, 0, 0 };
  int sysidx = 2;
  const axlf_section_header* clk_hdr = nullptr;
  const axlf_section_header* bit_hdr = nullptr;
  for (uint32_t i = 0; i < top->m_header.m_numSections; ++i) {
    if (top->m_sections[i].m_sectionKind == CLOCK_FREQ_TOPOLOGY)
      clk_hdr = &top->m_sections[i];
    if (top->m_sections[i].m_sectionKind == BITSTREAM)
      bit_hdr = &top->m_sections[i];
  }
  if (clk_hdr) {
    auto topo = reinterpret_cast<const clock_freq_topology*>(buf.data() + clk_hdr->m_sectionOffset);
    for (int16_t i = 0; i < topo->m_count; ++i) {
      const auto& c = topo->m_clock_freq[i];
      int idx = (c.m_type == CT_DATA) ? 0 : (c.m_type == CT_KERNEL) ? 1
              : (c.m_type == CT_SYSTEM && sysidx < XRT_LOAD_CLOCKS) ? sysidx++ : -1;
      std::printf("  xclbin clock      : %s %u MHz (wizard now %u MHz, table entry %u MHz)\n",
                  c.m_name, c.m_freq_Mhz, idx >= 0 ? cur[idx] : 0, table_mhz(c.m_freq_Mhz));
      if (idx >= 0)
        target[idx] = c.m_freq_Mhz;
    }
  }
  if (!bit_hdr) {
    std::fprintf(stderr, "xclbin has no BITSTREAM section\n");
    return 1;
  }

  // 3. Bitstream payload after the .bit header; big-endian words to host
  //    order, as icap_write() does with be32_to_cpu.
  auto bit = reinterpret_cast<const unsigned char*>(buf.data() + bit_hdr->m_sectionOffset);
  uint32_t bitlen = 0;
  size_t hdrlen = bit_header_length(bit, bit_hdr->m_sectionSize, bitlen);
  if (!hdrlen || (bitlen % 4) != 0) {
    std::fprintf(stderr, "invalid .bit header in BITSTREAM section\n");
    return 1;
  }
  std::vector<uint32_t> words(bitlen / 4);
  for (size_t i = 0; i < words.size(); ++i) {
    const unsigned char* w = bit + hdrlen + i * 4;
    words[i] = (uint32_t)w[0] << 24 | (uint32_t)w[1] << 16 | (uint32_t)w[2] << 8 | w[3];
  }
  std::printf("  bitstream         : %u bytes (%zu words) after %zu-byte header\n", bitlen, words.size(), hdrlen);

  if (state_only) {
    std::printf("state only: nothing written\n");
    return 0;
  }

  // 4. Program.
  std::printf("programming...\n");
  uint64_t load_args[10] = { 0, target[0], target[1], 0, 0,
                            ulp_clk[0], ulp_clk[1], ulp_clk[2], target[2], ucs_off };
  std::memcpy(&load_args[3], &top->m_header.uuid, 8);
  std::memcpy(&load_args[4], reinterpret_cast<const uint8_t*>(&top->m_header.uuid) + 8, 8);
  rsz = sizeof(res);
  kern_return_t kr = IOConnectCallMethod(conn, XRT_DEXT_LOAD_BITSTREAM, load_args, 10,
                                         words.data(), words.size() * 4, nullptr, nullptr, &res, &rsz);
  if (kr != KERN_SUCCESS) {
    std::fprintf(stderr, "LOAD_BITSTREAM call failed: 0x%x\n", kr);
    return 1;
  }
  print_result(res);
  IOServiceClose(conn);
  return res.status == 0 ? 0 : 1;
}
