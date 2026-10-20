// SPDX-License-Identifier: Apache-2.0
// Is the board controller alive, and does it have a sensor table?
//
// The macOS port has no board-controller support, so an Alveo whose sensors sit
// behind a CMC (a U50) reports no temperatures at all - SYSMON is FPGA-die only
// and the newer shells have none. Before writing any of that support, the thing
// worth knowing is whether the controller is already running with a live sensor
// table (in which case reading it is all that is needed) or whether its firmware
// has to be loaded first (which is a write to the card, and a bigger job).
//
// xocl's test: the magic word at offset 0 of the CMC register map reads
// 0x74736574 when the controller is up (subdev/xmc.c, XMC_MAGIC_REG).
//
// Read-only: maps the management BAR and reads. Writes nothing.
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstring>
#include "../XrtUser/xrt_dext_abi.h"

namespace {

constexpr uint32_t kMagic = 0x74736574;      // "test"

struct reg { const char* name; uint32_t off; };
// A few of xocl's offsets (subdev/xmc.c). Each rail is a triple of
// max/avg/instant, so only the first of each is read here.
const reg kRegs[] = {
  { "magic",        0x00 }, { "version",      0x04 },
  { "status",       0x08 }, { "error",        0x0c },
  { "feature",      0x10 }, { "sensor",       0x14 },
  { "12V PEX",      0x20 }, { "3V3 PEX",      0x2c },
  { "3V3 AUX",      0x38 }, { "12V AUX",      0x44 },
  { "VCC1V2 top",   0x68 }, { "VCC1V8",       0x74 },
  { "VCC0V85",      0x80 }, { "MGT0V9AVCC",   0x98 },
  { "12V PEX I in", 0xc8 }, { "12V AUX I in", 0xd4 },
  { "FPGA temp",    0xf8 }, { "fan temp",     0x104 },
  { "HBM temp",     0x260}, { "VCCINT temp",  0x2cc },
};

bool
parent_bdf(io_service_t svc, unsigned& bus, unsigned& dev, unsigned& func)
{
  io_registry_entry_t parent = IO_OBJECT_NULL;
  if (IORegistryEntryGetParentEntry(svc, kIOServicePlane, &parent) != KERN_SUCCESS)
    return false;
  bool ok = false;
  if (auto prop = IORegistryEntryCreateCFProperty(parent, CFSTR("pcidebug"), kCFAllocatorDefault, 0)) {
    char buf[64] = {0};
    if (CFGetTypeID(prop) == CFStringGetTypeID()
        && CFStringGetCString((CFStringRef)prop, buf, sizeof(buf), kCFStringEncodingUTF8))
      ok = std::sscanf(buf, "%u:%u:%u", &bus, &dev, &func) == 3;
    CFRelease(prop);
  }
  IOObjectRelease(parent);
  return ok;
}

bool
is_filler(uint32_t v)
{
  return v == 0xffffffff || v == 0xdeadfa11 || v == 0xdec0de1c;
}

} // namespace

int
main()
{
  io_iterator_t it = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it) != KERN_SUCCESS) {
    std::printf("no %s service\n", XRT_DEXT_USER_CLASS);
    return 1;
  }
  int looked = 0;
  while (io_service_t svc = IOIteratorNext(it)) {
    unsigned bus = 0, dev = 0, func = 0;
    if (parent_bdf(svc, bus, dev, func) && func == 0) {
      io_connect_t conn = IO_OBJECT_NULL;
      if (IOServiceOpen(svc, mach_task_self(), 0, &conn) == KERN_SUCCESS) {
        xrt_dext_platform plat = {};
        size_t sz = sizeof(plat);
        if (IOConnectCallStructMethod(conn, XRT_DEXT_GET_PLATFORM, nullptr, 0, &plat, &sz)
            == KERN_SUCCESS) {
          ++looked;
          const auto& b = plat.blk[XRT_BLK_CMC_REGMAP];
          if (!b.size) {
            std::printf("%02x:%02x.%x: this platform has no CMC register map\n", bus, dev, func);
          }
          else {
            mach_vm_address_t addr = 0;
            mach_vm_size_t len = 0;
            if (IOConnectMapMemory64(conn, XRT_DEXT_MAP_BAR(b.bar), mach_task_self(), &addr, &len,
                                     kIOMapAnywhere | kIOMapInhibitCache) == KERN_SUCCESS
                && b.offset + b.size <= len) {
              auto r = reinterpret_cast<volatile uint32_t*>(addr) + b.offset / 4;
              uint32_t magic = r[0];
              std::printf("%02x:%02x.%x: CMC register map at BAR%u+0x%llx\n",
                          bus, dev, func, b.bar, b.offset);
              std::printf("  magic 0x%08x %s\n", magic,
                          magic == kMagic ? "= 'test': the controller is RUNNING"
                          : is_filler(magic) ? "(filler: nothing answered)"
                                             : "(not the expected magic)");
              for (const auto& x : kRegs) {
                uint32_t v = r[x.off / 4];
                // Rails are millivolts, currents milliamps, temperatures
                // degrees C. Each is a max/average/instant triple; this reads
                // the first of each.
                const char* unit = std::strstr(x.name, "temp") ? "C"
                                 : std::strstr(x.name, "I in") ? "mA"
                                 : (x.off >= 0x20 && x.off < 0xc8) ? "mV" : "";
                if (*unit)
                  std::printf("  %-14s +0x%03x = 0x%08x  (%u %s)\n", x.name, x.off, v, v, unit);
                else
                  std::printf("  %-14s +0x%03x = 0x%08x\n", x.name, x.off, v);
              }
              // The sensor register holds the board name in ASCII.
              uint32_t nm = r[0x14 / 4];
              char name[5] = { char(nm & 0xff), char((nm >> 8) & 0xff),
                               char((nm >> 16) & 0xff), char((nm >> 24) & 0xff), 0 };
              std::printf("  board name from the sensor register: '%s'\n", name);
              IOConnectUnmapMemory64(conn, XRT_DEXT_MAP_BAR(b.bar), mach_task_self(), addr);
            }
            else
              std::printf("%02x:%02x.%x: could not map BAR%u\n", bus, dev, func, b.bar);
          }
            // And what does its mailbox say about the board itself? This is
            // where the board name and power rating come from, which is what
            // decides the aux-connection test.
            {
              xrt_dext_cmc_bdinfo info = {};
              size_t isz = sizeof(info);
              kern_return_t kr = IOConnectCallStructMethod(conn, XRT_DEXT_CMC_BOARD_INFO,
                                                          nullptr, 0, &info, &isz);
              std::printf("  board info: call 0x%x status 0x%x, %u bytes,"
                          " mailbox at 0x%x, error 0x%x\n",
                          kr, info.status, info.bytes, info.msg_offset, info.error);
              for (uint32_t i = 0; i + 2 <= info.bytes;) {
                uint8_t k = info.data[i], l = info.data[i + 1];
                i += 2;
                if (i + l > info.bytes)
                  break;
                std::printf("    key 0x%02x len %2u : ", k, l);
                bool printable = l > 0;
                for (uint32_t j = 0; j < l; ++j) {
                  uint8_t c = info.data[i + j];
                  if (c && (c < 0x20 || c > 0x7e))
                    printable = false;
                }
                if (printable)
                  std::printf("'%.*s'", l, reinterpret_cast<const char*>(info.data + i));
                else
                  for (uint32_t j = 0; j < l; ++j)
                    std::printf("%02x ", info.data[i + j]);
                std::printf("\n");
                i += l;
              }
            }

            // While we are here: what are the kernel clocks actually running
            // at? This measures with the counters the loaded xclbin brought,
            // rather than reporting what the download asked the wizards for.
            {
              static const char* kName[] = { "data", "kernel", "system" };
              for (uint64_t i = 0; i < 3; ++i) {
                uint64_t mhz = 0;
                uint32_t n = 1;
                kern_return_t kr = IOConnectCallScalarMethod(conn, XRT_DEXT_CLOCK_MEASURE,
                                                            &i, 1, &mhz, &n);
                if (kr == KERN_SUCCESS)
                  std::printf("  measured %-6s clock: %llu MHz\n", kName[i], mhz);
                else
                  std::printf("  measured %-6s clock: no counter (0x%x)\n", kName[i], kr);
              }
            }
        }
        IOServiceClose(conn);
      }
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  if (!looked)
    std::printf("no management function answered\n");
  return 0;
}
