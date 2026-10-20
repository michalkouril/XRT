// SPDX-License-Identifier: Apache-2.0
// Opens the xrt_user dext user client, prints device info, the feature ROM,
// and cross-checks the ROM through a read-only BAR0 mapping.
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "../XrtUser/xrt_dext_abi.h"
#include "xclfeatures.h"   // XRT's struct FeatureRomHeader

static void
print_info(const xrt_dext_device_info& info)
{
  std::printf("abi %u  %02x:%02x.%x  vendor %04x device %04x  subsys %04x:%04x  rev %u\n",
              info.abi_version, info.bus, info.dev, info.func, info.vendor, info.device,
              info.subsystem_vendor, info.subsystem_id, info.revision);
  std::printf("command 0x%04x status 0x%04x  link: Gen%u x%u (max Gen%u x%u)\n",
              info.command, info.status, info.link_status & 0xf, (info.link_status >> 4) & 0x3f,
              info.link_caps & 0xf, (info.link_caps >> 4) & 0x3f);
  for (int i = 0; i < 6; ++i)
    if (info.bars[i].present)
      std::printf("  BAR%d: memory index %u, type 0x%02x, size %llu KiB\n", i,
                  info.bars[i].memory_index, info.bars[i].type,
                  (unsigned long long)info.bars[i].size / 1024);
  if (info.func == 1)
    std::printf("DMA completion: %s, %llu batches by interrupt, %llu polled\n",
                info.irq_vectors ? "MSI-X" : "polling",
                (unsigned long long)info.dma_irq_batches, (unsigned long long)info.dma_poll_batches);
}

static void
print_rom(const FeatureRomHeader& h)
{
  char vbnv[65] = {0}, part[65] = {0};
  std::memcpy(vbnv, h.VBNVName, 64);
  std::memcpy(part, h.FPGAPartName, 64);
  std::time_t t = static_cast<std::time_t>(h.TimeSinceEpoch);
  char when[64] = "?";
  std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S UTC", std::gmtime(&t));
  std::printf("feature ROM v%u.%u\n", h.MajorVersion, h.MinorVersion);
  std::printf("  VBNV          : %s\n", vbnv);
  std::printf("  FPGA part     : %s\n", part);
  std::printf("  built         : %s (id %llu)\n", when, (unsigned long long)h.TimeSinceEpoch);
  std::printf("  Vivado build  : %u, IP build %u\n", h.VivadoBuildID, h.IPBuildID);
  std::printf("  DDR           : %u x %u GB\n", h.DDRChannelCount, h.DDRChannelSize);
  std::printf("  DR base       : 0x%llx\n", (unsigned long long)h.DRBaseAddress);
  std::printf("  feature bits  : 0x%llx\n", (unsigned long long)h.FeatureBitMap);
  std::printf("  UUID          : ");
  for (int i = 0; i < 16; ++i) std::printf("%02x", h.uuid[i]);
  std::printf("\n");
}

// SYSMON on the management BAR (XRT devices.h XOCL_RES_SYSMON) and the
// conversions from xocl subdev/sysmon.c (UltraScale+ SYSMON4).
static void
print_sysmon(volatile uint32_t* bar)
{
  constexpr uint32_t base = 0xA0000;
  auto rd = [&](uint32_t off) { return bar[(base + off) / 4]; };
  auto mdeg = [](uint32_t v) { return (int32_t)((((int64_t)v * 509314) >> 16) - 280230); };
  auto mv = [](uint32_t v) { return (uint32_t)((uint64_t)v * 1000 * 3 >> 16); };
  std::printf("SYSMON  temp %.1f C (min %.1f, max %.1f)\n",
              mdeg(rd(0x400)) / 1000.0, mdeg(rd(0x490)) / 1000.0, mdeg(rd(0x480)) / 1000.0);
  std::printf("        VCCINT %u mV (min %u, max %u)  VCCAUX %u mV  VCCBRAM %u mV\n",
              mv(rd(0x404)), mv(rd(0x494)), mv(rd(0x484)), mv(rd(0x408)), mv(rd(0x418)));
}

// Legacy clock wizards on the management BAR (XRT devices.h __RES_CLOCK_K1/K2)
// and the UltraScale frequency formula from xocl subdev/clock_wiz.c.
static unsigned
clkwiz_mhz(volatile uint32_t* bar, uint32_t base)
{
  auto rd = [&](uint32_t off) { return bar[(base + off) / 4]; };
  if ((rd(0x4) & 1) == 0)
    return 0;                                  // not locked
  uint32_t v = rd(0x200);
  uint64_t div0 = v & 0xff, mul0 = (v >> 8) & 0xff, mul_frac0 = 0;
  if (v & (1u << 26))
    mul_frac0 = (v >> 16) & 0x3ff;
  mul0 = mul0 * 1000 + mul_frac0;
  div0 *= 1000;
  v = rd(0x208);
  uint64_t div1 = v & 0xff, div_frac1 = 0;
  if (v & (1u << 18))
    div_frac1 = (v >> 8) & 0x3ff;
  div1 = div1 * 1000 + div_frac1;
  div0 *= div1;
  mul0 *= 1000;
  return div0 ? static_cast<unsigned>((100 * mul0) / div0) : 0;
}

int main(int argc, char** argv)
{
  // --dma-mode poll|irq switches DMA completion on the user function.
  int64_t set_mode = -1;
  if (argc == 3 && std::strcmp(argv[1], "--dma-mode") == 0)
    set_mode = std::strcmp(argv[2], "irq") == 0 ? XRT_DMA_MODE_IRQ : XRT_DMA_MODE_POLL;
  io_iterator_t it = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it) != KERN_SUCCESS)
    return 1;
  int found = 0;
  while (io_service_t svc = IOIteratorNext(it)) {
    ++found;
    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
      std::printf("IOServiceOpen failed: 0x%x\n", kr);
      continue;
    }

    xrt_dext_device_info info = {};
    size_t size = sizeof(info);
    kr = IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &size);
    if (kr == KERN_SUCCESS && set_mode >= 0 && info.func == 1) {
      uint64_t in = static_cast<uint64_t>(set_mode), out = 0;
      uint32_t n = 1;
      kr = IOConnectCallScalarMethod(conn, XRT_DEXT_DMA_MODE, &in, 1, &out, &n);
      std::printf("DMA_MODE -> %s (0x%x)\n", out == XRT_DMA_MODE_IRQ ? "irq" : "poll", kr);
      size = sizeof(info);
      kr = IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &size);
    }
    if (kr == KERN_SUCCESS)
      print_info(info);
    else
      std::printf("GET_DEVICE_INFO failed: 0x%x\n", kr);

    uint8_t rom[XRT_DEXT_ROM_SIZE] = {};
    size = sizeof(rom);
    kr = IOConnectCallStructMethod(conn, XRT_DEXT_READ_ROM, nullptr, 0, rom, &size);
    if (kr == KERN_SUCCESS) {
      FeatureRomHeader h;
      static_assert(sizeof(h) <= XRT_DEXT_ROM_SIZE, "ROM header larger than read size");
      std::memcpy(&h, rom, sizeof(h));
      print_rom(h);
    }
    else {
      std::printf("READ_ROM failed: 0x%x\n", kr);
    }

    // Cross-check: read the same ROM words through a read-only BAR0 mapping.
    mach_vm_address_t addr = 0;
    mach_vm_size_t len = 0;
    kr = IOConnectMapMemory64(conn, XRT_DEXT_MAP_BAR(XRT_DEXT_ROM_BAR), mach_task_self(),
                              &addr, &len, kIOMapAnywhere | kIOMapInhibitCache);
    if (kr == KERN_SUCCESS) {
      auto base = reinterpret_cast<volatile uint32_t*>(addr);
      int mismatches = 0;
      for (unsigned i = 0; i < XRT_DEXT_ROM_SIZE / 4; ++i) {
        uint32_t via_map = base[(XRT_DEXT_ROM_OFFSET / 4) + i];
        uint32_t via_dext;
        std::memcpy(&via_dext, rom + i * 4, 4);
        if (via_map != via_dext)
          ++mismatches;
      }
      std::printf("BAR0 mapped at %p, %llu KiB; ROM words via mapping vs driver: %d mismatches\n",
                  (void*)addr, (unsigned long long)len / 1024, mismatches);
      if (info.func == 0) {
        print_sysmon(base);
        std::printf("CLOCKS  wizard K1 (0x50000): %u MHz, K2 (0x51000): %u MHz\n",
                    clkwiz_mhz(base, 0x50000), clkwiz_mhz(base, 0x51000));
      }
      IOConnectUnmapMemory64(conn, XRT_DEXT_MAP_BAR(XRT_DEXT_ROM_BAR), mach_task_self(), addr);
    }
    else {
      std::printf("map BAR0 failed: 0x%x\n", kr);
    }
    IOServiceClose(conn);
  }
  IOObjectRelease(it);
  if (!found)
    std::printf("no XrtUser service found (is the dext active and matched?)\n");
  return found ? 0 : 1;
}
