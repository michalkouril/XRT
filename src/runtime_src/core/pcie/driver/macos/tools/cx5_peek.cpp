// Read the ConnectX-5's identity and, through its Vendor-Specific Capability gateway,
// a dword of an address space. This is the foundation for reading the ConnectX's boot
// verdict (FPGA_CTRL), which is the only place that records whether the FPGA's User
// image ran and whether the ConnectX accepted it.
//
// Read-only by default. --write exists because the gateway needs both directions for
// anything beyond CR reads, but it is not used by the default invocation.
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include "xrt_cx5_abi.h"

static io_connect_t
open_cx5()
{
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_CX5_CLASS), &it))
    return 0;
  io_connect_t found = 0;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_connect_t c = 0;
    if (!IOServiceOpen(svc, mach_task_self(), 0, &c)) {
      found = c;
      IOObjectRelease(svc);
      break;
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  return found;
}

int
main(int argc, char** argv)
{
  io_connect_t c = open_cx5();
  if (!c) {
    puts("no XrtCx5 service.\n"
         "  The ConnectX is probably still owned by com.apple.DriverKit-AppleEthernetMLX5:\n"
         "  macOS refuses to unload a bootKC dext, so the nub is only freed by\n"
         "  re-enumeration - a Thunderbolt replug or an enclosure power cycle.");
    return 2;
  }

  xrt_cx5_info info{};
  size_t sz = sizeof(info);
  kern_return_t kr = IOConnectCallStructMethod(c, XRT_CX5_GET_INFO, nullptr, 0, &info, &sz);
  if (kr) { std::printf("GET_INFO failed 0x%x\n", kr); IOServiceClose(c); return 1; }

  std::printf("XrtCx5 abi %u   %02x:%02x.%x   %04x:%04x\n",
              info.abi, info.bus, info.dev, info.func, info.vendor, info.device);
  if (info.vsc_offset)
    std::printf("  legacy Vendor-Specific Capability: 0x%02x\n", info.vsc_offset);
  else
    puts("  legacy Vendor-Specific Capability: ABSENT - the ICMD gateway needs it, so\n"
         "  nothing below can work on this device");
  if (info.vsec_offset)
    std::printf("  extended VSEC (0x000b):            0x%x\n", info.vsec_offset);
  std::printf("  a gateway read has %s\n",
              info.gateway_ok ? "succeeded at least once" : "NOT yet succeeded");

  if (!info.vsc_offset) { IOServiceClose(c); return 1; }

  // A few CR-space dwords. On an Innova-2 the FPGA's own register map lives here, so
  // these addresses come from innova2_app.py's transcription of the vendor sources.
  struct { uint32_t addr; const char* what; } probes[] = {
    { 0x900000, "image version"  },
    { 0x900004, "image date"     },
    { 0x900008, "image time"     },
    { 0x90000C, "FPGA device"    },
    { 0x000024, "power"          },
  };
  if (argc > 1 && !std::strcmp(argv[1], "--addr") && argc > 2) {
    uint32_t a = (uint32_t)strtoul(argv[2], nullptr, 0);
    uint64_t in[2] = { XRT_CX5_AS_CR_SPACE, a };
    uint64_t out = 0; uint32_t n = 1;
    kr = IOConnectCallScalarMethod(c, XRT_CX5_GATEWAY_READ, in, 2, &out, &n);
    std::printf("  CR 0x%06x = %s0x%08x\n", a, kr ? "FAILED " : "", (uint32_t)out);
    IOServiceClose(c);
    return kr ? 1 : 0;
  }

  puts("\nCR space through the gateway:");
  int fails = 0;
  for (auto& p : probes) {
    uint64_t in[2] = { XRT_CX5_AS_CR_SPACE, p.addr };
    uint64_t out = 0; uint32_t n = 1;
    kr = IOConnectCallScalarMethod(c, XRT_CX5_GATEWAY_READ, in, 2, &out, &n);
    if (kr) { std::printf("  %-14s 0x%06x  FAILED 0x%x\n", p.what, p.addr, kr); fails++; }
    else     std::printf("  %-14s 0x%06x  0x%08x\n", p.what, p.addr, (uint32_t)out);
  }
  if (fails)
    puts("\nNote: CR space is refused by the ConnectX while the FPGA's USER image runs -\n"
         "that is its documented behaviour, not necessarily a fault here.");
  IOServiceClose(c);
  return fails ? 1 : 0;
}
