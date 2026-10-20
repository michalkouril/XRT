// Read-only dump of an AXI Quad SPI register block on the management BAR.
//
// Nothing here writes. The point is to confirm the controller is present and
// answering before any code that can issue a command exists: a block that is
// absent reads back filler (0xffffffff / 0xdeadfa11 / 0xdec0de1c), and every
// one of those has bit 0 set, so they are easy to mistake for a real status.
//
// usage: spi_peek [offset]      default 0x40000, the Innova2 shell's controller
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include "xrt_dext_abi.h"

// Xilinx AXI Quad SPI v3.2, register offsets within the block.
struct { uint32_t off; const char* name; } regs[] = {
  { 0x1c, "IPISR      " }, { 0x28, "IPIER      " }, { 0x40, "SRR (wo)   " },
  { 0x60, "SPICR      " }, { 0x64, "SPISR      " }, { 0x68, "SPI_DTR(wo)" },
  { 0x6c, "SPI_DRR(ro)" }, { 0x70, "SPISSR     " }, { 0x74, "TX_FIFO_OCY" },
  { 0x78, "RX_FIFO_OCY" },
};

static bool
is_filler(uint32_t v)
{
  return v == 0xffffffffu || v == 0xdeadfa11u || v == 0xdec0de1cu;
}

// Scan the whole BAR for anything that looks like an idle AXI Quad SPI, rather
// than trusting one candidate offset. A shell can carry more than one instance,
// and the flash controller is not necessarily the first.
static void
scan_for_spi(volatile uint32_t* bar, uint64_t len)
{
  std::printf("scanning BAR0 for AXI Quad SPI blocks...\n");
  int hits = 0;
  for (uint64_t off = 0; off + 0x80 <= len; off += 0x1000) {
    uint32_t cr = bar[(off + 0x60) / 4];
    uint32_t sr = bar[(off + 0x64) / 4];
    uint32_t ssr = bar[(off + 0x70) / 4];
    if (is_filler(cr) || is_filler(sr))
      continue;
    // SPISR reset value is 0xa5; after a transaction it settles at 0x25. Both
    // have RX empty and TX empty set. SPICR must have the reset bits.
    if ((sr & 0x5) != 0x5 || (cr & 0x180) == 0)
      continue;
    std::printf("  0x%06llx  SPICR 0x%08x  SPISR 0x%08x  SPISSR 0x%08x\n",
                (unsigned long long)off, cr, sr, ssr);
    ++hits;
  }
  std::printf("  %d candidate block(s)\n", hits);
}

int
main(int argc, char** argv)
{
  const uint32_t base = (argc > 1 && strcmp(argv[1], "scan") != 0)
                        ? (uint32_t)strtoul(argv[1], nullptr, 0) : 0x40000;
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it))
    return 0;
  int found = 0;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_connect_t conn = 0;
    if (IOServiceOpen(svc, mach_task_self(), 0, &conn)) { IOObjectRelease(svc); continue; }
    xrt_dext_device_info info{}; size_t sz = sizeof(info);
    // A discarded status leaves info zeroed, which would read as func 0.
    if (IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &sz)
        || info.func != 0) { IOServiceClose(conn); IOObjectRelease(svc); continue; }
    mach_vm_address_t a = 0; mach_vm_size_t l = 0;
    if (IOConnectMapMemory64(conn, 0, mach_task_self(), &a, &l, kIOMapAnywhere | kIOMapInhibitCache)) {
      puts("BAR0 map failed"); IOServiceClose(conn); IOObjectRelease(svc); continue;
    }
    std::printf("mgmt %02x:%02x.%u  BAR0 %llu KiB  block at 0x%x\n",
                info.bus, info.dev, info.func, (unsigned long long)(l >> 10), base);
    if (base + 0x80 > l) { std::printf("  block is past the end of BAR0\n"); }
    else {
      auto r = [&](uint32_t o) { return reinterpret_cast<volatile uint32_t*>(a)[(base + o) / 4]; };
      int filler = 0;
      for (auto& g : regs) {
        uint32_t v = r(g.off);
        filler += is_filler(v);
        std::printf("  +0x%02x %s 0x%08x%s\n", g.off, g.name, v, is_filler(v) ? "   <- filler" : "");
      }
      std::printf("  %s\n", filler >= 4
                  ? "nothing is answering here: the offset or the shell is wrong"
                  : "the controller answers");
    }
    if (argc > 1 && strcmp(argv[1], "scan") == 0)
      scan_for_spi(reinterpret_cast<volatile uint32_t*>(a), l);
    IOConnectUnmapMemory64(conn, 0, mach_task_self(), a);
    IOServiceClose(conn); IOObjectRelease(svc); ++found;
  }
  IOObjectRelease(it);
  if (!found) puts("no management function with an attached dext");
  return 0;
}
