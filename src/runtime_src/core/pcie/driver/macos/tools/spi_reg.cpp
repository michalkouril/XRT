// Exercise XRT_DEXT_SPI_REG, the bounded register accessor the macOS xbflash.qspi
// port runs on.
//
// Reads are pure observation. The one write this does is SPISSR = 0xffffffff,
// which DEselects both chips, followed by SPICR = 0, which disables the core -
// the state the driver leaves it in anyway. Neither can reach a flash byte: no
// command has been clocked out, and with every slave select deasserted none can
// be. Nothing is written to flash here.
#include <IOKit/IOKitLib.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include "xrt_dext_abi.h"

static io_connect_t
open_func(uint8_t want)
{
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it))
    return 0;
  io_connect_t found = 0;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_connect_t conn = 0;
    if (!IOServiceOpen(svc, mach_task_self(), XRT_DEXT_CLIENT_FLASH, &conn)) {
      xrt_dext_device_info info{}; size_t sz = sizeof(info);
      // A failed call leaves info zeroed, and func 0 would then "match" whatever
      // this connection actually is - so the status has to be checked.
      if (!IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &sz)
          && info.func == want) { found = conn; IOObjectRelease(svc); break; }
      IOServiceClose(conn);
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  return found;
}

static kern_return_t
reg(io_connect_t conn, uint32_t off, uint32_t val, bool write, uint32_t* got)
{
  uint64_t in[3] = { off, val, write ? 1u : 0u };
  uint64_t out = 0; uint32_t nout = 1;
  kern_return_t kr = IOConnectCallScalarMethod(conn, XRT_DEXT_SPI_REG, in, 3, &out, &nout);
  if (got) *got = static_cast<uint32_t>(out);
  return kr;
}

// Releasing the exclusive flash claim is ASYNCHRONOUS: IOServiceClose returns before
// the dext's Stop runs, so an open right after another flash tool exits can be refused
// with kIOReturnBusy. Retry briefly rather than fail - being refused while someone
// else genuinely holds it is correct behaviour, and a few hundred ms tells the two
// apart. Running the documented procedure as a script (flash_audit then xbflash) hits
// this, so it is not just a test concern.
static io_connect_t
open_flash_retry()
{
  for (int attempt = 0; attempt < 30; ++attempt) {
    io_connect_t c = open_func(0);
    if (c)
      return c;
    usleep(100000);
  }
  return 0;
}

int
main()
{
  io_connect_t conn = open_flash_retry();
  if (!conn) { puts("no management function with an attached dext"); return 2; }

  int fail = 0;

  // --- every register on the allowlist, which is every register xspi.cpp uses.
  // Reading DTR and DRR is harmless: DTR is write-only (reads undefined) and
  // draining DRR while RX is empty returns whatever the core drives.
  struct { uint32_t off; const char* name; } r[] = {
    { XRT_SPI_SRR,    "SRR     " }, { XRT_SPI_CR,     "SPICR   " },
    { XRT_SPI_SR,     "SPISR   " }, { XRT_SPI_DTR,    "DTR     " },
    { XRT_SPI_DRR,    "DRR     " }, { XRT_SPI_SSR,    "SPISSR  " },
    { XRT_SPI_TX_OCY, "TX_OCY  " },
  };
  puts("read through XRT_DEXT_SPI_REG (the whole allowlist):");
  for (auto& x : r) {
    uint32_t v = 0;
    kern_return_t kr = reg(conn, x.off, 0, false, &v);
    if (kr) { std::printf("  %s @0x%02x  FAILED 0x%x\n", x.name, x.off, kr); fail++; }
    else      std::printf("  %s @0x%02x  0x%08x\n", x.name, x.off, v);
  }

  // --- the bound. Each of these must be refused, and refused for the stated
  // reason: an accessor that lets user space name an offset is only as good as
  // the check on it.
  puts("\nbounds (all must be refused):");
  struct { uint32_t off; const char* why; } bad[] = {
    { 0x62,               "unaligned (2)" },
    { 0x61,               "unaligned (1)" },
    { XRT_SPI_RX_OCY,     "RX_OCY: real, unused" },
    { 0x1c,               "DGIER: real, unused" },
    { 0x0,                "offset zero" },
    { 0x1000,             "the old window's end" },
    { 0x1004,             "past the old window" },
    { 0x40000,            "another block's base" },
    { 0xfffffffcu,        "wraps" },
    { 0x60 + 0x80,        "an alias of SPICR, if the core decodes low bits" },
  };
  for (auto& x : bad) {
    uint32_t v = 0;
    kern_return_t kr = reg(conn, x.off, 0, false, &v);
    bool refused = kr != kIOReturnSuccess;
    std::printf("  0x%08x %-22s %s (0x%x)\n", x.off, x.why,
                refused ? "refused" : "ACCEPTED - BUG", kr);
    if (!refused) fail++;
  }

  // --- a write, read back. SSR all-ones deselects both chips; then disable the
  // core. This is the state the driver leaves behind after a transaction.
  puts("\nwrite SPISSR = 0xffffffff (both chips deselected), read back:");
  uint32_t v = 0;
  if (reg(conn, XRT_SPI_SSR, 0xffffffffu, true, &v)) { puts("  write FAILED"); fail++; }
  else {
    reg(conn, XRT_SPI_SSR, 0, false, &v);
    std::printf("  SPISSR now 0x%08x %s\n", v,
                (v & 0x3) == 0x3 ? "(both deasserted, as written)" : "(UNEXPECTED)");
    if ((v & 0x3) != 0x3) fail++;
  }
  reg(conn, XRT_SPI_CR, 0, true, &v);            // release STARTUPE3
  reg(conn, XRT_SPI_CR, 0, false, &v);
  std::printf("  SPICR now 0x%08x %s\n", v,
              (v & XRT_SPI_CR_SPE) ? "(SPE STILL SET - the ICAP will fail)" : "(core disabled)");
  if (v & XRT_SPI_CR_SPE) fail++;

  // --- the function gate. The flash hangs off function 0; the same selector on
  // the user function must be refused, and nothing above proves that because
  // open_mgmt() deliberately picks function 0.
  puts("\nfunction gate:");
  io_connect_t user = open_func(1);
  if (!user)
    puts("  no user function present - gate not exercised");
  else {
    uint32_t v = 0;
    uint64_t in[3] = { XRT_SPI_CR, 0, 0 };
    uint32_t nout = 1; uint64_t out = 0;
    kern_return_t kr = IOConnectCallScalarMethod(user, XRT_DEXT_SPI_REG, in, 3, &out, &nout);
    std::printf("  SPI_REG on func 1: %s (0x%x)\n",
                kr ? "refused" : "ACCEPTED - BUG", kr);
    if (!kr) fail++;
    (void)v;
    IOServiceClose(user);
  }

  // --- what actually gated this. Root is deliberately NOT the gate: DriverKit
  // gives a dext no way to ask whether its caller is root, so the check is an
  // entitlement on this binary, verified by the kernel at IOServiceOpen. Strip
  // org.xrtmac.flash-access from a copy of this tool and the open fails - for root
  // too. flash_gate_test covers that; this line just says which rule applied.
  std::printf("\nran as uid %u; reached the flash client because this binary carries %s\n",
              (unsigned)getuid(), XRT_DEXT_FLASH_ENTITLEMENT);

  IOServiceClose(conn);
  std::printf("\n%d check(s) failed\n", fail);
  return fail ? 1 : 0;
}
