// Exercise the exact code path xbflash.qspi's xspi.cpp will use on macOS:
// pcidev::pci_device construction against the dext, pcieBarRead/pcieBarWrite,
// and the destructor's release of the SPI core.
//
// Read-only with respect to flash. The single write is SPISSR = 0xffffffff,
// which deasserts every slave select, so no command can be clocked to a device.
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include "pcidev.h"
#include "xrt_dext_abi.h"

static uint32_t
rd(pcidev::pci_device& dev, uint32_t off, int& fail)
{
  uint32_t v = 0;
  if (dev.pcieBarRead(off, &v, 4) != 0) { std::printf("  read @0x%02x FAILED\n", off); fail++; }
  return v;
}

int
main(int argc, char** argv)
{
  const char* bdf = argc > 1 ? argv[1] : "43:00.0";
  int fail = 0;
  try {
    // INVALID_OFFSET is what main.cpp passes when --bar-offset is absent.
    pcidev::pci_device dev(bdf, 0, static_cast<size_t>(-1));
    std::printf("opened %s; flash_offset reported as 0x%zx (0 means the driver "
                "supplies the base)\n", bdf, dev.get_flash_offset());

    std::printf("  SPICR  0x%08x\n", rd(dev, XRT_SPI_CR, fail));
    std::printf("  SPISR  0x%08x\n", rd(dev, XRT_SPI_SR, fail));
    std::printf("  SPISSR 0x%08x\n", rd(dev, XRT_SPI_SSR, fail));

    uint32_t all = 0xffffffffu;
    if (dev.pcieBarWrite(XRT_SPI_SSR, &all, 4) != 0) { puts("  SSR write FAILED"); fail++; }
    uint32_t back = rd(dev, XRT_SPI_SSR, fail);
    std::printf("  SPISSR after write 0x%08x %s\n", back,
                (back & 3) == 3 ? "(both deasserted)" : "(UNEXPECTED)");
    if ((back & 3) != 3) fail++;

    // What xspi.cpp would be refused if it ever computed an offset wrongly.
    if (dev.pcieBarRead(0x40000, &back, 4) == 0) { puts("  out-of-window read ACCEPTED - BUG"); fail++; }
    else puts("  out-of-window read refused");
    if (dev.pcieBarRead(XRT_SPI_CR, &back, 8) == 0) { puts("  8-byte access ACCEPTED - BUG"); fail++; }
    else puts("  8-byte access refused");
  }
  catch (const std::exception& e) {
    std::printf("pci_device(%s) threw: %s\n", bdf, e.what());
    return 2;
  }
  // The destructor has now run: it should have left the core disabled.
  try {
    pcidev::pci_device dev(bdf, 0, static_cast<size_t>(-1));
    uint32_t cr = rd(dev, XRT_SPI_CR, fail);
    std::printf("after destructor, SPICR 0x%08x %s\n", cr,
                (cr & XRT_SPI_CR_SPE) ? "(SPE STILL SET - the ICAP will fail)" : "(core disabled)");
    if (cr & XRT_SPI_CR_SPE) fail++;
  }
  catch (const std::exception& e) { std::printf("reopen threw: %s\n", e.what()); return 2; }

  std::printf("%d check(s) failed\n", fail);
  return fail ? 1 : 0;
}
