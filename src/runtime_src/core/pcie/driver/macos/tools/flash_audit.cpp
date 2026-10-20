// Read back the TX-stream shadow's tally of the flash commands driven through
// XRT_DEXT_SPI_REG. This is the dry run's output: upstream's xbflash offers no
// read-only verb, so the way to find out what a program run touches is to let the
// shadow watch it and then read this.
//
// Purely a readout. `--reset` clears the tally, which writes nothing to flash.
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <cstdlib>
#include "xrt_dext_abi.h"

static const char* kClass[XRT_SPI_OP_CLASSES] = {
  "other (status, write-enable, ...)", "read", "erase", "program",
  "BULK ERASE", "extended address write",
};

static io_connect_t
open_flash()
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
      if (!IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &sz)
          && info.func == 0) { found = conn; IOObjectRelease(svc); break; }
      IOServiceClose(conn);
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  return found;
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
    io_connect_t c = open_flash();
    if (c)
      return c;
    usleep(100000);
  }
  return 0;
}

int
main(int argc, char** argv)
{
  // Every flag is looked for in every position: --reset used to be tested only at
  // argv[1], so "--dry-run --reset" silently did not reset.
  bool reset = false;
  int set_mode = -1;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--reset")) reset = true;
    if (!std::strcmp(argv[i], "--dry-run")) set_mode = XRT_SPI_GUARD_DRY_RUN;
    if (!std::strcmp(argv[i], "--audit-only")) set_mode = XRT_SPI_GUARD_OFF;
    if (!std::strcmp(argv[i], "--enforce")) set_mode = XRT_SPI_GUARD_ENFORCE;
  }

  io_connect_t conn = open_flash_retry();
  if (!conn) {
    puts("could not open the flash client - is this binary signed with "
         XRT_DEXT_FLASH_ENTITLEMENT ", and is the flash free?");
    return 2;
  }

  if (set_mode >= 0) {
    uint64_t min[1] = { (uint64_t)set_mode };
    uint64_t mout = 0; uint32_t mn = 1;
    kern_return_t mkr = IOConnectCallScalarMethod(conn, XRT_DEXT_SPI_REG_MODE, min, 1, &mout, &mn);
    if (mkr) { std::printf("setting the mode failed 0x%x\n", mkr); IOServiceClose(conn); return 1; }
    auto name = [](uint32_t m) {
      return m == XRT_SPI_GUARD_DRY_RUN ? "dry run"
           : m == XRT_SPI_GUARD_ENFORCE ? "enforce" : "no guard";
    };
    std::printf("shadow mode: %s -> %s\n", name((uint32_t)mout), name((uint32_t)set_mode));
    if (set_mode == XRT_SPI_GUARD_DRY_RUN)
      puts("  every device write will now be SUPPRESSED: none will reach a device,\n"
           "  and the caller will be told it succeeded. Nothing will be written to\n"
           "  flash while this is in force.");
    else if (set_mode == XRT_SPI_GUARD_OFF)
      puts("  WARNING: the guard is now OFF. Bulk erase is permitted - it takes out\n"
           "  the whole part, golden image included, and that needs JTAG to recover.");
  }

  uint64_t in[1] = { reset ? 1ull : 0ull };
  xrt_dext_spi_audit a{};
  size_t sz = sizeof(a);
  kern_return_t kr = IOConnectCallMethod(conn, XRT_DEXT_SPI_REG_AUDIT, in, 1,
                                         nullptr, 0, nullptr, nullptr, &a, &sz);
  IOServiceClose(conn);
  if (kr) { std::printf("audit read failed 0x%x\n", kr); return 1; }

  std::printf("dext ABI %u, mode: %s\n", a.abi,
              a.mode == XRT_SPI_GUARD_DRY_RUN
                ? "DRY RUN - every device write suppressed, nothing reaches the device"
              : a.mode == XRT_SPI_GUARD_ENFORCE
                ? "ENFORCE - bulk erase refused (no address rule; see xrt_dext_abi.h)"
                : "NO GUARD - everything is performed, the factory image is writable");
  std::printf("%u transaction(s) launched", a.launches);
  if (a.undecoded)
    std::printf(", %u NOT DECODED (the tally below is incomplete)", a.undecoded);
  std::puts("");

  if (!a.launches) {
    puts("\nnothing recorded yet. Run a flash operation, then read this again.");
    return 0;
  }

  std::puts("\nby command class:");
  for (uint32_t i = 0; i < XRT_SPI_OP_CLASSES; ++i) {
    const auto& c = a.cls[i];
    if (!c.count)
      continue;
    std::printf("  %-34s %6u  last opcode 0x%02x", kClass[i], c.count, c.last_opcode);
    if (i == XRT_SPI_OP_ERASE || i == XRT_SPI_OP_PROGRAM)
      std::printf("  chip-local 0x%08x..0x%08x", c.min_addr, c.max_addr);
    std::puts("");
  }

  std::printf("\nextended address register: %s",
              a.ear_valid ? "" : "never set\n");
  if (a.ear_valid)
    std::printf("0x%x (address bits 27:24)\n", a.ear);
  std::printf("chips selected: %s%s%s\n",
              (a.chip_mask & 1) ? "0 " : "", (a.chip_mask & 2) ? "1" : "",
              a.chip_mask ? "" : "none");
  std::printf("address width seen: %s%s%s\n",
              (a.addr_bytes_seen & 1) ? "3-byte " : "",
              (a.addr_bytes_seen & 2) ? "4-byte" : "",
              a.addr_bytes_seen ? "" : "no addressed command");

  if (a.first_write_n) {
    std::puts("\nfirst addressed writes, in order:");
    for (uint32_t i = 0; i < a.first_write_n && i < 8; ++i)
      std::printf("  opcode 0x%02x  chip-local 0x%08x\n",
                  a.first_write_op[i], a.first_write_addr[i]);
    // This used to print a "logical = chip-local x 2" column, on the assumption that
    // dual-quad mode halves the address because each chip holds every other byte.
    // The dry run disproved that: the addresses on the wire are exactly the ones the
    // MCS declares (guard at 0x00fff000, payload from 0x01000000). The striping is in
    // the DATA - xbflash splits it between the primary and secondary MCS with
    // stripe_data() - and both chips are written at the SAME address. So there is no
    // doubling to apply, and printing one invited a wrong conclusion.
    std::puts("  (these are the addresses as they go on the wire; compare them with\n"
              "   the MCS's own declared range, which they should match exactly)");
  }

  std::printf("\npolicy violations: %u", a.refused);
  if (a.refused)
    std::printf(" - last opcode 0x%02x at 0x%08x, %s\n", a.last_refused_op,
                a.last_refused_addr,
                a.mode == XRT_SPI_GUARD_ENFORCE ? "REFUSED" : "would be refused if enforcing");
  else
    std::puts(" - no bulk erase was issued");

  if (a.suppressed) {
    std::printf("\n%u command(s) SUPPRESSED - they never reached a device.\n", a.suppressed);
    std::printf("  the last one queued %u byte(s)\n", a.last_suppressed_sent);
    std::printf("  the caller drained %u byte(s) since the last launch\n",
                a.drr_since_launch);
    // Suppression is now a single opcode rewrite on the way into the FIFO, so the
    // transfer is the caller's own and its byte count cannot be short. The counter
    // is left in place because it is what caught the three designs where it could.
    if (a.short_rx)
      std::printf("  *** %u transfer(s) came back short - unexpected now that only the\n"
                  "      opcode is rewritten. Investigate. ***\n", a.short_rx);

  }
  if (a.cls[XRT_SPI_OP_BULK_ERASE].count)
    std::printf("\n*** A BULK ERASE WAS ISSUED (%s). It erases the whole part,\n"
                "    factory image included. ***\n",
                a.mode == XRT_SPI_GUARD_DRY_RUN ? "suppressed, so it did not happen"
                                                : "and it was NOT suppressed");
  if (reset)
    std::puts("\ntally reset.");
  return 0;
}
