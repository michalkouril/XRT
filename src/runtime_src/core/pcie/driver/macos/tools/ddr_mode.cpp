// SPDX-License-Identifier: Apache-2.0
// DDR mode of an Innova-2 card running the rbdm shell: the macOS counterpart of
// xilinx_shells/rebuild/innova2_ddr_mode.py.
//
//   ddr_mode [status]        what the gate is doing now
//   ddr_mode dump            the raw dwords of both register windows
//   ddr_mode on | off | emu  ask for a mode and wait for it to take
//
//   on   real DDR. On a card whose DDR does not calibrate, XRT refuses DDR and
//        PLRAM xclbins. This is what the register resets to at power-up.
//   off  DDR fenced, MIG held in reset. XRT is told "calibrated", so PLRAM-only
//        xclbins load on any card; DDR reads come back 0xBADCA1B0.
//   emu  the bottom 1 MB of bank 0 is served from on-chip UltraRAM; the rest of
//        the bank is fenced as in off.
//
// The three registers live in the management function's BAR0. Reading them
// needs nothing special - the whole BAR maps read-only - but writing the mode
// goes through XRT_DEXT_REG_WRITE, which only exists in a dext built with
// XRT_MACOS_INNOVA2=1. On a generic driver the write answers
// kIOReturnUnsupported and this tool says so rather than appearing to work.
#include <IOKit/IOKitLib.h>
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include "xrt_dext_abi.h"

// mgmt BAR0. 0x32000 bit 0 is the calibration bit XRT reads; 0x32008 bits 1:0
// are the requested mode; 0x36000 is mgmt_status.
static const uint32_t MEMCALIB = 0x32000, MODE_REG = 0x32008, STATUS = 0x36000;
static const char* NAMES[4] = {"on", "off", "emu", "off"};

struct state {
  uint32_t calib, mode, status;
  unsigned requested, in_force, mig_held, gate_open, fenced_seen;
};

static state
read_state(volatile uint32_t* bar)
{
  state s{};
  s.calib  = bar[MEMCALIB / 4];
  s.mode   = bar[MODE_REG / 4];
  s.status = bar[STATUS / 4];
  s.requested   = s.mode & 3;
  s.in_force    = (s.status >> 29) & 3;
  s.mig_held    = (s.status >> 31) & 1;
  s.gate_open   = (s.status >> 28) & 1;
  s.fenced_seen = (s.status >> 27) & 1;
  return s;
}

static void
show(const state& s)
{
  std::printf("DDR mode requested : %s\n", NAMES[s.requested]);
  std::printf("DDR mode in force  : %s%s\n", NAMES[s.in_force],
              s.in_force == s.requested ? ""
                : "   (change pending: waits for DDR traffic to drain)");
  std::printf("MIG                : %s\n",
              s.mig_held ? "held in reset (DDR powered down)" : "running");
  if (s.in_force == 0)                    // real DDR
    std::printf("DDR                : %s\n", s.gate_open
                ? "calibrated, gate open"
                : "NOT calibrated - DDR fenced; XRT will refuse DDR/PLRAM xclbins");
  else
    std::printf("DDR                : not in use (real DDR state unknown while the MIG is held)\n");
  std::printf("XRT calibration bit: %u\n", s.calib & 1);
  std::printf("fenced access seen : %s\n", s.fenced_seen ? "yes (sticky)" : "no");
  std::printf("mgmt_status raw    : 0x%08x\n", s.status);
}

int
main(int argc, char** argv)
{
  const char* what = argc > 1 ? argv[1] : "status";
  int want = -1;
  const bool dump = !std::strcmp(what, "dump");
  if (!std::strcmp(what, "on"))       want = 0;
  else if (!std::strcmp(what, "off")) want = 1;
  else if (!std::strcmp(what, "emu")) want = 2;
  else if (!dump && std::strcmp(what, "status")) {
    std::fprintf(stderr, "usage: %s [status|dump|on|off|emu]\n", argv[0]);
    return 2;
  }

  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it)) {
    std::fprintf(stderr, "no %s service - is the dext loaded?\n", XRT_DEXT_USER_CLASS);
    return 1;
  }
  int rc = 1;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_connect_t conn;
    if (IOServiceOpen(svc, mach_task_self(), 0, &conn))
      continue;
    xrt_dext_device_info info{};
    size_t sz = sizeof(info);
    // A discarded status leaves info zeroed, which would read as func 0.
    if (IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &sz)
        || info.func != 0) {
      IOServiceClose(conn);
      continue;
    }

    // The gate only exists on an Innova-2 rbdm shell. Say which shell this is,
    // because every register below reads as something on any card.
    uint8_t rom[XRT_DEXT_ROM_SIZE] = {};
    size_t rsz = sizeof(rom);
    char vbnv[65] = {};
    if (IOConnectCallStructMethod(conn, XRT_DEXT_READ_ROM, nullptr, 0, rom, &rsz) == KERN_SUCCESS)
      std::memcpy(vbnv, rom + 88, 64);   // FeatureRomHeader::VBNVName
    std::printf("%02x:%02x.%u  %s\n", info.bus, info.dev, info.func,
                vbnv[0] ? vbnv : "(feature ROM unreadable)");
    if (std::strncmp(vbnv, XRT_DEXT_REG_WRITE_VBNV_PREFIX,
                     sizeof(XRT_DEXT_REG_WRITE_VBNV_PREFIX) - 1) != 0)
      std::printf("  warning: not an Innova-2 shell; the registers below mean nothing here\n");

    mach_vm_address_t a = 0;
    mach_vm_size_t l = 0;
    if (IOConnectMapMemory64(conn, XRT_DEXT_MAP_BAR(0), mach_task_self(), &a, &l,
                             kIOMapAnywhere | kIOMapInhibitCache)) {
      std::printf("mapping BAR0 failed\n");
      IOServiceClose(conn);
      continue;
    }
    auto bar = reinterpret_cast<volatile uint32_t*>(a);
    state s = read_state(bar);
    if (s.status == 0xffffffffu) {
      std::printf("*** mgmt BAR reads all-ones: card not responding\n");
      IOConnectUnmapMemory64(conn, XRT_DEXT_MAP_BAR(0), mach_task_self(), a);
      IOServiceClose(conn);
      continue;
    }

    if (dump) {
      // The windows XRT_DEXT_REG_WRITE can reach, as they read. Filler
      // (0xffffffff, 0xdec0de1c) means nothing is decoded there, which is how
      // a shell without the gate looks.
      for (uint32_t base : {XRT_DEXT_REG_WRITE_WIN0_BASE, XRT_DEXT_REG_WRITE_WIN1_BASE}) {
        for (uint32_t off = 0; off < 0x40; off += 0x10) {
          std::printf("  0x%05x:", base + off);
          for (uint32_t w = 0; w < 4; ++w)
            std::printf(" %08x", bar[(base + off) / 4 + w]);
          std::printf("\n");
        }
      }
      rc = 0;
    } else if (want < 0) {
      show(s);
      rc = 0;
    } else {
      uint64_t in[2] = { MODE_REG, static_cast<uint64_t>(want) };
      uint64_t back = 0;
      uint32_t nout = 1;
      kern_return_t kr = IOConnectCallScalarMethod(conn, XRT_DEXT_REG_WRITE, in, 2, &back, &nout);
      if (kr == kIOReturnUnsupported) {
        std::printf("*** the running driver has no register-write path.\n"
                    "    This is the generic XRT build. Rebuild and activate the\n"
                    "    Innova-2 one, which adds it:\n"
                    "      CONFIG=Release XRT_MACOS_INNOVA2=1 ./build.sh activate\n");
        rc = 2;
      } else if (kr) {
        std::printf("*** register write refused: 0x%x\n", kr);
        rc = 2;
      } else if ((back & 3) != static_cast<uint64_t>(want)) {
        std::printf("*** mode register did not take the write (read back %llu)"
                    " - not an rbdm shell?\n", (unsigned long long)(back & 3));
        rc = 2;
      } else {
        // The gate applies a mode change only once DDR traffic has drained, so
        // the requested mode and the mode in force can differ for a while.
        bool applied = false;
        for (int i = 0; i < 100 && !applied; ++i) {       // 5 s
          s = read_state(bar);
          applied = s.in_force == static_cast<unsigned>(want);
          if (!applied)
            usleep(50000);
        }
        if (applied && want == 0)
          for (int i = 0; i < 100 && !read_state(bar).gate_open; ++i)
            usleep(100000);                               // 10 s for calibration
        s = read_state(bar);
        show(s);
        if (!applied) {
          std::printf("*** mode not applied within 5 s (DDR traffic still in flight?)\n");
          rc = 2;
        } else {
          rc = 0;
        }
      }
    }
    IOConnectUnmapMemory64(conn, XRT_DEXT_MAP_BAR(0), mach_task_self(), a);
    IOServiceClose(conn);
  }
  IOObjectRelease(it);
  if (rc == 1)
    std::fprintf(stderr, "no management function found\n");
  return rc;
}
