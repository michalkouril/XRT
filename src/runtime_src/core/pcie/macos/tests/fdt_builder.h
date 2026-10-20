// Builds flattened-device-tree blobs for the tests, including deliberately
// malformed ones. The parser under test consumes a blob shipped in a vendor
// platform package and a section of a user-supplied xclbin, so malformed input
// is a realistic case and not a hypothetical.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

class fdt_builder
{
public:
  // Property names live in a strings block; the struct block refers to them by
  // offset, which is what makes a lying offset interesting to test.
  uint32_t
  str(const std::string& name)
  {
    auto off = static_cast<uint32_t>(m_strings.size());
    m_strings.insert(m_strings.end(), name.begin(), name.end());
    m_strings.push_back('\0');
    return off;
  }

  void begin_node(const std::string& name)
  {
    be32(m_struct, 0x1);
    m_struct.insert(m_struct.end(), name.begin(), name.end());
    m_struct.push_back('\0');
    pad4(m_struct);
  }

  void end_node() { be32(m_struct, 0x2); }

  void prop(const std::string& name, const void* value, uint32_t len)
  {
    be32(m_struct, 0x3);
    be32(m_struct, len);
    be32(m_struct, str(name));
    auto p = static_cast<const uint8_t*>(value);
    m_struct.insert(m_struct.end(), p, p + len);
    pad4(m_struct);
  }

  void prop_str(const std::string& name, const std::string& v)
  {
    prop(name, v.c_str(), static_cast<uint32_t>(v.size() + 1));
  }

  void prop_be64_pair(const std::string& name, uint64_t a, uint64_t b)
  {
    uint8_t v[16];
    for (int i = 0; i < 8; ++i) v[i] = static_cast<uint8_t>(a >> (56 - 8 * i));
    for (int i = 0; i < 8; ++i) v[8 + i] = static_cast<uint8_t>(b >> (56 - 8 * i));
    prop(name, v, 16);
  }

  void prop_be32(const std::string& name, uint32_t a)
  {
    uint8_t v[4] = { static_cast<uint8_t>(a >> 24), static_cast<uint8_t>(a >> 16),
                     static_cast<uint8_t>(a >> 8), static_cast<uint8_t>(a) };
    prop(name, v, 4);
  }

  // A raw property whose declared length is not the number of bytes actually
  // written: the "lying length field" case.
  void prop_raw_len(const std::string& name, const void* value, uint32_t written, uint32_t declared)
  {
    be32(m_struct, 0x3);
    be32(m_struct, declared);
    be32(m_struct, str(name));
    auto p = static_cast<const uint8_t*>(value);
    m_struct.insert(m_struct.end(), p, p + written);
    pad4(m_struct);
  }

  // A property whose name offset points at the given absolute offset in the
  // strings block, whatever is there.
  void prop_name_off(uint32_t name_off, const void* value, uint32_t len)
  {
    be32(m_struct, 0x3);
    be32(m_struct, len);
    be32(m_struct, name_off);
    auto p = static_cast<const uint8_t*>(value);
    m_struct.insert(m_struct.end(), p, p + len);
    pad4(m_struct);
  }

  void end() { be32(m_struct, 0x9); }

  // Assemble: header, then struct block, then strings block.
  std::vector<uint8_t>
  build(uint32_t magic = 0xd00dfeed) const
  {
    const uint32_t hdr = 40;
    const uint32_t off_struct = hdr;
    const uint32_t off_strings = off_struct + static_cast<uint32_t>(m_struct.size());
    std::vector<uint8_t> out;
    be32(out, magic);
    be32(out, off_strings + static_cast<uint32_t>(m_strings.size()));   // totalsize
    be32(out, off_struct);
    be32(out, off_strings);
    be32(out, 0);            // off_mem_rsvmap
    be32(out, 17);           // version
    be32(out, 16);           // last_comp_version
    be32(out, 0);            // boot_cpuid_phys
    be32(out, static_cast<uint32_t>(m_strings.size()));
    be32(out, static_cast<uint32_t>(m_struct.size()));
    out.insert(out.end(), m_struct.begin(), m_struct.end());
    out.insert(out.end(), m_strings.begin(), m_strings.end());
    return out;
  }

  // Same, but the header's block offsets are whatever the caller says.
  std::vector<uint8_t>
  build_with_offsets(uint32_t off_struct, uint32_t off_strings) const
  {
    auto out = build();
    write_be32(out, 8, off_struct);
    write_be32(out, 12, off_strings);
    return out;
  }

  const std::vector<uint8_t>& strings() const { return m_strings; }

private:
  static void be32(std::vector<uint8_t>& v, uint32_t x)
  {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
  }
  static void write_be32(std::vector<uint8_t>& v, size_t at, uint32_t x)
  {
    v[at] = static_cast<uint8_t>(x >> 24);
    v[at + 1] = static_cast<uint8_t>(x >> 16);
    v[at + 2] = static_cast<uint8_t>(x >> 8);
    v[at + 3] = static_cast<uint8_t>(x);
  }
  static void pad4(std::vector<uint8_t>& v)
  {
    while (v.size() % 4)
      v.push_back(0);
  }
  std::vector<uint8_t> m_struct;
  std::vector<uint8_t> m_strings;
};
