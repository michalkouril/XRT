// The XDMA interrupt block, read straight from the mapped BAR.
//
// Item 48: on the U50 the MSI-X vectors configure cleanly and nothing is ever
// delivered. This says which half is at fault - whether the XDMA core raises
// the interrupt at all (channel_int_request / _pending set) or whether it
// raises it and the delivery never lands.
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include "../XrtUser/xrt_dext_abi.h"

int
main(int argc, char** argv)
{
  // The XDMA BAR differs by card: BAR1 on the VCU1525, BAR2 on the U50, which
  // has no BAR1 at all. Default to trying both.
  const int want = argc > 1 ? atoi(argv[1]) : -1;
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it))
    return 0;
  io_service_t svc = IOIteratorNext(it);
  if (!svc) { puts("no user function"); return 1; }
  io_connect_t conn = 0;
  if (IOServiceOpen(svc, mach_task_self(), 0, &conn) != KERN_SUCCESS) { puts("open failed"); return 1; }

  mach_vm_address_t a = 0; mach_vm_size_t l = 0;
  int bar = -1;
  const int cands[3] = { want, 1, 2 };
  for (int ci = 0; ci < 3; ++ci) {
    const int cand = cands[ci];
    if (cand < 0) continue;
    if (!IOConnectMapMemory64(conn, cand, mach_task_self(), &a, &l, kIOMapAnywhere | kIOMapInhibitCache)) {
      bar = cand; break;
    }
  }
  if (bar < 0) { puts("map of the XDMA BAR failed on 1 and 2"); return 1; }
  std::printf("XDMA registers via BAR%d\n", bar);
  auto r = [&](uint32_t off) { return reinterpret_cast<volatile uint32_t*>(a)[off / 4]; };
  std::printf("XDMA BAR mapped, %llu KiB\n", (unsigned long long)(l >> 10));

  // libxdma struct interrupt_regs, at XRT_XDMA_IRQ.
  const uint32_t B = XRT_XDMA_IRQ;
  std::printf("irq block identifier      0x%08x  (expect 0x1fc2....)\n", r(B + 0x00));
  std::printf("user_int_enable           0x%08x\n", r(B + 0x04));
  std::printf("channel_int_enable        0x%08x  <- we write 0xf here\n", r(B + 0x10));
  std::printf("user_int_request          0x%08x\n", r(B + 0x40));
  std::printf("channel_int_request       0x%08x  <- core raising it\n", r(B + 0x44));
  std::printf("user_int_pending          0x%08x\n", r(B + 0x48));
  std::printf("channel_int_pending       0x%08x  <- core raising it\n", r(B + 0x4c));
  for (int i = 0; i < 2; ++i)
    std::printf("user_msi_vector[%d..%d]     0x%08x\n", i * 4, i * 4 + 3, r(B + 0x80 + i * 4));
  for (int i = 0; i < 2; ++i)
    std::printf("channel_msi_vector[%d..%d]  0x%08x  <- we write 0x03020100 at +0xa0\n",
                i * 4, i * 4 + 3, r(B + 0xa0 + i * 4));

  std::printf("\nper-engine interrupt_enable_mask (+0x90):\n");
  for (int dir = 0; dir < 2; ++dir)
    for (int ch = 0; ch < 2; ++ch) {
      uint32_t base = dir * 0x1000 + ch * 0x100;
      std::printf("  %s%d id 0x%08x  ie_mask 0x%08x  status 0x%08x\n",
                  dir ? "C2H" : "H2C", ch, r(base), r(base + 0x90), r(base + 0x40));
    }

  // MSI-X capability in config space, via the dext.
  std::printf("\nMSI-X capability (config space):\n");
  for (uint32_t off = 0x34; off <= 0x34; off += 4) {
    uint64_t in[1] = { off }, out[1] = { 0 }; uint32_t n = 1;
    if (IOConnectCallScalarMethod(conn, XRT_DEXT_CFG_READ, in, 1, out, &n) == KERN_SUCCESS)
      std::printf("  cap pointer at 0x34    0x%08x\n", (uint32_t)out[0]);
  }
  // Walk the capability list for MSI-X (id 0x11).
  uint64_t in[1] = { 0x34 }, out[1] = { 0 }; uint32_t n = 1;
  IOConnectCallScalarMethod(conn, XRT_DEXT_CFG_READ, in, 1, out, &n);
  uint32_t ptr = out[0] & 0xfc;
  for (int i = 0; i < 48 && ptr >= 0x40; ++i) {
    in[0] = ptr; n = 1;
    if (IOConnectCallScalarMethod(conn, XRT_DEXT_CFG_READ, in, 1, out, &n) != KERN_SUCCESS)
      break;
    uint32_t v = out[0];
    if ((v & 0xff) == 0x11) {
      std::printf("  MSI-X cap at 0x%02x      0x%08x\n", ptr, v);
      std::printf("    table size           %u\n", ((v >> 16) & 0x7ff) + 1);
      std::printf("    enabled              %s\n", (v & (1u << 31)) ? "yes" : "NO");
      std::printf("    function mask        %s\n", (v & (1u << 30)) ? "MASKED" : "clear");
      // Where the table and the pending-bit array live. The low 3 bits are the
      // BIR - which BAR holds them - and the rest is the offset within it.
      for (int k = 1; k <= 2; ++k) {
        in[0] = ptr + 4 * k; n = 1;
        if (IOConnectCallScalarMethod(conn, XRT_DEXT_CFG_READ, in, 1, out, &n) != KERN_SUCCESS)
          break;
        uint32_t w = out[0];
        std::printf("    %s  BAR%u + 0x%x\n", k == 1 ? "table  " : "PBA    ",
                    w & 0x7, w & ~0x7u);
      }
      break;
    }
    ptr = (v >> 8) & 0xfc;
  }
  return 0;
}
