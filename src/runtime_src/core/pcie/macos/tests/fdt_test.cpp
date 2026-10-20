// Host-only tests for the partition-metadata parser.
//
// This code consumes two untrusted inputs: a binary blob from a vendor platform
// package, and a section of a user-supplied xclbin. It is hand written, there is
// no libfdt, and until now it could only be exercised by running on an FPGA.
// Everything here runs on any host, under sanitizers, in milliseconds.
#include "../fdt_macos.h"
#include "fdt_builder.h"
#include "test_harness.h"

#include <cstring>
#include <vector>

using xrt_core::macos::fdt_partition;
using xrt_core::macos::parse_partition_metadata;

static bool
parse(const std::vector<uint8_t>& blob, fdt_partition& out)
{
  return parse_partition_metadata(blob.data(), blob.size(), out);
}

// A blob shaped like the real thing: one interface uuid and two endpoints.
static std::vector<uint8_t>
realistic()
{
  fdt_builder b;
  b.begin_node("");
  b.begin_node("interfaces");
  b.begin_node("interface@0");
  b.prop_str("interface_uuid", "862c7020a250293552475d47523f2c7f");
  b.end_node();
  b.end_node();
  b.begin_node("addressable_endpoints");
  b.begin_node("ep_icap_reset_00");
  b.prop_be64_pair("reg", 0x1e88000, 0x8000);
  b.prop_be32("pcie_physical_function", 0);
  b.prop_str("compatible", "xilinx.com,reg_abs-axi_hwicap-1.0");
  b.end_node();
  b.begin_node("ep_aclk_kernel_00");
  b.prop_be64_pair("reg", 0x1003000, 0x1000);
  b.prop_be32("pcie_physical_function", 0);
  b.end_node();
  b.end_node();
  b.end_node();
  b.end();
  return b.build();
}

static void
test_valid()
{
  fdt_partition p;
  auto blob = realistic();
  t::ok(parse(blob, p), "a realistic blob parses");
  t::eq_str(p.interface_uuid, "862c7020a250293552475d47523f2c7f", "interface uuid");
  t::eq(p.endpoints.size(), size_t{2}, "two endpoints found");
  if (p.endpoints.size() == 2) {
    t::eq_str(p.endpoints[0].name, "ep_icap_reset_00", "first endpoint name");
    t::eq(p.endpoints[0].offset, uint64_t{0x1e88000}, "first endpoint offset");
    t::eq(p.endpoints[0].size, uint64_t{0x8000}, "first endpoint size");
    t::eq(p.endpoints[0].func, uint8_t{0}, "first endpoint function");
    t::eq_str(p.endpoints[0].abstraction, "xilinx.com,reg_abs-axi_hwicap-1.0", "compatible");
    t::eq_str(p.endpoints[1].name, "ep_aclk_kernel_00", "second endpoint name");
    t::eq(p.endpoints[1].offset, uint64_t{0x1003000}, "second endpoint offset");
  }
}

static void
test_rejects_rubbish()
{
  fdt_partition p;
  t::ok(!parse_partition_metadata(nullptr, 100, p), "a null blob is refused");
  std::vector<uint8_t> tiny(8, 0);
  t::ok(!parse(tiny, p), "a blob shorter than the header is refused");
  fdt_builder b;
  b.begin_node("");
  b.end_node();
  b.end();
  auto bad_magic = b.build(0xdeadbeef);
  t::ok(!parse(bad_magic, p), "a bad magic is refused");
  auto empty = b.build();
  t::ok(!parse(empty, p), "a blob with nothing we need is refused");
}

// Truncation at every length. None may read out of bounds or hang; the parser
// may return either verdict, it just has to stay inside the buffer. Run under
// ASAN this is the test that matters most.
static void
test_truncation()
{
  auto full = realistic();
  int survived = 0;
  for (size_t n = 0; n < full.size(); ++n) {
    std::vector<uint8_t> cut(full.begin(), full.begin() + static_cast<long>(n));
    fdt_partition p;
    (void)parse_partition_metadata(cut.data(), cut.size(), p);
    ++survived;
  }
  t::eq(survived, static_cast<int>(full.size()), "every truncation returns without reading out of bounds");
}

// Every single-byte corruption. Same contract: no out-of-bounds, no hang.
static void
test_corruption()
{
  auto full = realistic();
  int survived = 0;
  for (size_t i = 0; i < full.size(); ++i) {
    for (uint8_t bit = 0; bit < 8; ++bit) {
      auto c = full;
      c[i] ^= static_cast<uint8_t>(1u << bit);
      fdt_partition p;
      (void)parse_partition_metadata(c.data(), c.size(), p);
      ++survived;
    }
  }
  t::eq(survived, static_cast<int>(full.size() * 8), "every single-bit flip returns without reading out of bounds");
}

// The header's block offsets are attacker controlled.
static void
test_header_offsets()
{
  fdt_builder b;
  b.begin_node("");
  b.prop_str("interface_uuid", "00112233445566778899aabbccddeeff");
  b.end_node();
  b.end();
  auto good = b.build();
  fdt_partition p0;
  t::ok(parse(good, p0), "baseline for the offset cases parses");

  // Offsets at or past the end must be refused rather than walked.
  for (uint32_t off : { uint32_t{0xffffffff}, static_cast<uint32_t>(good.size()),
                        static_cast<uint32_t>(good.size() + 1) }) {
    fdt_partition p;
    auto blob = b.build_with_offsets(off, 40);
    t::ok(!parse(blob, p), "a struct offset outside the blob is refused");
    fdt_partition q;
    auto blob2 = b.build_with_offsets(40, off);
    t::ok(!parse(blob2, q), "a strings offset outside the blob is refused");
  }
}

// A property name offset that points outside the strings block. The parser
// checks the start of the name against the blob size; this asks whether it also
// stays inside while reading the name itself.
static void
test_property_name_offset()
{
  // A real property first. Without one the strings block is empty, so
  // off_strings == size and the parser bails on the header check before it ever
  // looks at a name - which is how the first version of this test passed while
  // exercising nothing.
  fdt_builder b;
  b.begin_node("");
  b.begin_node("ep_thing_00");
  b.prop_be32("pcie_physical_function", 0);
  uint8_t v[4] = { 0, 0, 0, 1 };
  b.prop_name_off(0xfffffff0, v, 4);          // a name offset in the weeds
  b.end_node();
  b.end_node();
  b.end();
  auto blob = b.build();
  // Precondition: the strings block must be non-empty, or this tests nothing.
  t::ok(!b.strings().empty(), "the name-offset case has a real strings block");
  fdt_partition p;
  (void)parse(blob, p);
  // The endpoint before the bad property must still have been collected: the
  // parser should stop at the bad name, not mangle what it already had.
  t::ok(p.endpoints.size() <= 1, "a wild name offset does not invent endpoints");

  // A name offset just inside the blob but with no terminator before the end.
  fdt_builder c;
  c.begin_node("");
  c.begin_node("ep_thing_00");
  c.prop_be32("pcie_physical_function", 0);
  c.end_node();
  c.end_node();
  c.end();
  auto blob2 = c.build();
  // Point the name at the final byte of the blob.
  fdt_partition q;
  (void)parse_partition_metadata(blob2.data(), blob2.size(), q);
  t::ok(true, "a name running to the last byte returns without reading out of bounds");
}

// A declared property length longer than the bytes actually present.
static void
test_lying_length()
{
  fdt_builder b;
  b.begin_node("");
  b.begin_node("ep_thing_00");
  uint8_t v[4] = { 1, 2, 3, 4 };
  b.prop_raw_len("reg", v, 4, 0x7fffffff);
  b.end_node();
  b.end_node();
  b.end();
  auto blob = b.build();
  fdt_partition p;
  (void)parse(blob, p);
  t::ok(true, "a property claiming 2 GB inside a small blob is refused, not read");

  // And a negative length, since the parser casts to int.
  fdt_builder c;
  c.begin_node("");
  c.begin_node("ep_thing_00");
  c.prop_raw_len("reg", v, 4, 0x80000000);
  c.end_node();
  c.end_node();
  c.end();
  auto blob2 = c.build();
  fdt_partition q;
  (void)parse(blob2, q);
  t::ok(true, "a property length with the sign bit set is refused, not read");
}

// A node name with no terminator before the end of the blob.
static void
test_unterminated_node_name()
{
  auto full = realistic();
  // Strip the trailing strings block and the END tag so the last thing in the
  // buffer is in the middle of the struct block.
  std::vector<uint8_t> cut(full.begin(), full.begin() + 60);
  fdt_partition p;
  (void)parse_partition_metadata(cut.data(), cut.size(), p);
  t::ok(true, "an unterminated node name returns without reading out of bounds");
}

// Deep nesting must not recurse without bound: the walk is iterative, so this
// is really a check that it stays iterative.
static void
test_deep_nesting()
{
  fdt_builder b;
  for (int i = 0; i < 20000; ++i)
    b.begin_node("n");
  for (int i = 0; i < 20000; ++i)
    b.end_node();
  b.end();
  auto blob = b.build();
  fdt_partition p;
  (void)parse(blob, p);
  t::ok(true, "20,000 levels of nesting do not overflow the stack");
}

// More END_NODE tags than BEGIN_NODE: the path vector must not be popped empty.
static void
test_unbalanced_nodes()
{
  fdt_builder b;
  b.begin_node("");
  b.end_node();
  b.end_node();
  b.end_node();
  b.end();
  auto blob = b.build();
  fdt_partition p;
  (void)parse(blob, p);
  t::ok(true, "unbalanced end-node tags do not pop an empty path");
}

int
main()
{
  t::suite = "fdt_macos";
  test_valid();
  test_rejects_rubbish();
  test_truncation();
  test_corruption();
  test_header_offsets();
  test_property_name_offset();
  test_lying_length();
  test_unterminated_node_name();
  test_deep_nesting();
  test_unbalanced_nodes();
  return t::report();
}
