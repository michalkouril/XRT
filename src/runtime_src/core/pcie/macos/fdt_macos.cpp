// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
#include "fdt_macos.h"

#include <cstring>
#include <libkern/OSByteOrder.h>

namespace {

constexpr uint32_t kFdtMagic      = 0xd00dfeed;
constexpr uint32_t kFdtBeginNode  = 0x1;
constexpr uint32_t kFdtEndNode    = 0x2;
constexpr uint32_t kFdtProp       = 0x3;
constexpr uint32_t kFdtNop        = 0x4;
constexpr uint32_t kFdtEnd        = 0x9;

struct fdt_header
{
  uint32_t magic, totalsize, off_dt_struct, off_dt_strings, off_mem_rsvmap;
  uint32_t version, last_comp_version, boot_cpuid_phys;
  uint32_t size_dt_strings, size_dt_struct;
};

inline uint32_t be32(uint32_t v) { return OSSwapBigToHostInt32(v); }

// The blob's own header says where the struct block starts, so nothing
// guarantees a 4-byte aligned pointer: reading through a uint32_t* there is
// undefined behaviour, which UBSAN flags on a corrupted blob
// (tests/fdt_test.cpp). memcpy compiles to the same load when the address does
// happen to be aligned.
inline uint32_t
be32_at(const char* p)
{
  uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return OSSwapBigToHostInt32(v);
}

inline const char*
align4(const char* p, const char* base)
{
  size_t off = static_cast<size_t>(p - base);
  return base + ((off + 3) & ~static_cast<size_t>(3));
}

// A property value as it appears in the partition metadata: a big-endian
// integer of 4 or 8 bytes, or a pair of them for the 64-bit reg pairs.
uint64_t
be_int(const char* v, int len)
{
  uint64_t r = 0;
  for (int i = 0; i < len; ++i)
    r = (r << 8) | static_cast<uint8_t>(v[i]);
  return r;
}

std::string
hex_uuid(const char* v, int len)
{
  // The property is a string in these blobs; accept raw bytes too.
  if (len > 0 && v[len - 1] == '\0') {
    std::string s(v, static_cast<size_t>(len - 1));
    bool hex = !s.empty();
    for (char c : s)
      hex &= (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (hex) {
      for (auto& c : s)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
      return s;
    }
  }
  std::string s;
  char buf[3];
  for (int i = 0; i < len; ++i) {
    std::snprintf(buf, sizeof(buf), "%02x", static_cast<uint8_t>(v[i]));
    s += buf;
  }
  return s;
}

} // namespace

namespace xrt_core::macos {

bool
parse_partition_metadata(const void* blob, size_t size, fdt_partition& out)
{
  if (!blob || size < sizeof(fdt_header))
    return false;
  auto base = static_cast<const char*>(blob);
  auto end = base + size;
  auto hdr = static_cast<const fdt_header*>(blob);
  if (be32(hdr->magic) != kFdtMagic)
    return false;
  uint32_t off_struct = be32(hdr->off_dt_struct);
  uint32_t off_str = be32(hdr->off_dt_strings);
  if (off_struct >= size || off_str >= size)
    return false;

  const char* strings = base + off_str;
  const char* p = base + off_struct;

  // Node path as we descend, so a property can be attributed to the endpoint
  // it belongs to. Only the last component is ever needed.
  std::vector<std::string> path;
  auto in = [&](const char* q, size_t n) { return q >= base && q <= end && static_cast<size_t>(end - q) >= n; };

  while (in(p, 4)) {
    uint32_t tag = be32_at(p);
    p += 4;
    if (tag == kFdtEnd)
      break;
    if (tag == kFdtNop)
      continue;
    if (tag == kFdtEndNode) {
      if (!path.empty())
        path.pop_back();
      continue;
    }
    if (tag == kFdtBeginNode) {
      size_t max = static_cast<size_t>(end - p);
      size_t len = strnlen(p, max);
      if (len == max)
        break;
      path.emplace_back(p, len);
      p = align4(p + len + 1, base);
      continue;
    }
    if (tag != kFdtProp)
      break;                       // unknown tag: the blob is not what we think
    if (!in(p, 8))
      break;
    int len = static_cast<int>(be32_at(p));
    uint32_t name_off = be32_at(p + 4);
    p += 8;
    // uint64 arithmetic: off_str and name_off are both attacker controlled and
    // their sum wraps in 32 bits, which would pass the check below.
    if (len < 0 || !in(p, static_cast<size_t>(len))
        || static_cast<uint64_t>(off_str) + name_off >= size)
      break;
    const char* name = strings + name_off;
    // The name must terminate inside the blob before anything may compare it.
    // Bounding the start is not enough: a name running to the last byte with no
    // NUL sent strcmp off the end of the buffer, which a truncated blob
    // reproduces (tests/fdt_test.cpp).
    size_t name_max = static_cast<size_t>(end - name);
    if (strnlen(name, name_max) == name_max)
      break;
    const char* value = p;
    p = align4(p + len, base);

    const std::string& node = path.empty() ? std::string() : path.back();

    if (!std::strcmp(name, "interface_uuid") && out.interface_uuid.empty())
      out.interface_uuid = hex_uuid(value, len);
    else if (!std::strcmp(name, "logic_uuid") && out.logic_uuid.empty())
      out.logic_uuid = hex_uuid(value, len);
    else if (node.rfind("ep_", 0) == 0) {
      // Endpoints appear as nodes under addressable_endpoints; collect the
      // properties the driver needs as they go past.
      if (out.endpoints.empty() || out.endpoints.back().name != node) {
        fdt_endpoint ep;
        ep.name = node;
        out.endpoints.push_back(ep);
      }
      auto& ep = out.endpoints.back();
      if (!std::strcmp(name, "reg") && len >= 16) {
        // <bar-hi bar-lo size-hi size-lo> as two 64-bit cells
        ep.offset = be_int(value, 8);
        ep.size = be_int(value + 8, 8);
      }
      else if (!std::strcmp(name, "pcie_physical_function") && len >= 4)
        ep.func = static_cast<uint8_t>(be_int(value, 4));
      else if (!std::strcmp(name, "pcie_bar_mapping") && len >= 4)
        ep.bar = static_cast<uint8_t>(be_int(value, 4));
      else if (!std::strcmp(name, "compatible") && ep.abstraction.empty())
        ep.abstraction = std::string(value, strnlen(value, static_cast<size_t>(len)));
    }
  }

  return !out.interface_uuid.empty() || !out.endpoints.empty();
}

} // xrt_core::macos
