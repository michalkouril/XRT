// Does the TX-stream shadow decode what it is actually handed?
//
// It drives the register sequence xspi.cpp's finalTransfer() uses, through
// XRT_DEXT_SPI_REG, and then asks the driver what it made of it. Every command here
// is a READ - 0x03 (3-byte address) and 0x13 (4-byte address) - so nothing is
// written to a flash device, not even a volatile register. That is also why both
// address widths are covered: 0x13 carries a full 32-bit address, so the
// reconstruction can be checked at a high address without setting the extended
// address register, which would be a device write.
//
// NOT covered, and it needs saying: the extended-address-register path (opcode
// 0xc5), which supplies bits 27:24 when a 3-byte-address command is used above
// 16 MiB. Exercising it means sending 0xc5 to the device.
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include "xrt_dext_abi.h"

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

static kern_return_t
wr(io_connect_t c, uint32_t off, uint32_t val)
{
  uint64_t in[3] = { off, val, 1 };
  uint64_t out = 0; uint32_t nout = 1;
  return IOConnectCallScalarMethod(c, XRT_DEXT_SPI_REG, in, 3, &out, &nout);
}

static uint32_t
rd(io_connect_t c, uint32_t off)
{
  uint64_t in[3] = { off, 0, 0 };
  uint64_t out = 0; uint32_t nout = 1;
  IOConnectCallScalarMethod(c, XRT_DEXT_SPI_REG, in, 3, &out, &nout);
  return (uint32_t)out;
}

static const uint32_t IDLE = XRT_SPI_CR_MASTER | XRT_SPI_CR_MANUAL_SS
                           | XRT_SPI_CR_INHIBIT | XRT_SPI_CR_SPE;

// One transaction, in exactly the order finalTransfer() does it: fill, select,
// clear INHIBIT to launch, wait, drain, re-inhibit.
static void
transact(io_connect_t c, uint32_t slave, const uint8_t* tx, uint32_t n)
{
  wr(c, XRT_SPI_CR, IDLE | XRT_SPI_CR_TX_RESET | XRT_SPI_CR_RX_RESET);
  wr(c, XRT_SPI_CR, IDLE);
  for (uint32_t i = 0; i < n; ++i)
    wr(c, XRT_SPI_DTR, tx[i]);
  wr(c, XRT_SPI_SSR, slave == 0 ? ~0x01u : ~0x02u);
  wr(c, XRT_SPI_CR, IDLE & ~XRT_SPI_CR_INHIBIT);          // <-- the launch
  for (int guard = 0; guard < 100000; ++guard)
    if (rd(c, XRT_SPI_SR) & XRT_SPI_SR_TX_EMPTY) break;
  wr(c, XRT_SPI_CR, IDLE);
  wr(c, XRT_SPI_SSR, 0xffffffffu);
  for (int guard = 0; guard < 600 && !(rd(c, XRT_SPI_SR) & XRT_SPI_SR_RX_EMPTY); ++guard)
    (void)rd(c, XRT_SPI_DRR);
}

static kern_return_t
set_mode(io_connect_t c, uint32_t mode, uint32_t* was)
{
  uint64_t in[1] = { mode };
  uint64_t out = 0; uint32_t nout = 1;
  kern_return_t kr = IOConnectCallScalarMethod(c, XRT_DEXT_SPI_REG_MODE, in, 1, &out, &nout);
  if (was) *was = (uint32_t)out;
  return kr;
}

static kern_return_t
audit(io_connect_t c, xrt_dext_spi_audit* a, bool reset)
{
  uint64_t in[1] = { reset ? 1ull : 0ull };
  size_t sz = sizeof(*a);
  std::memset(a, 0, sizeof(*a));
  return IOConnectCallMethod(c, XRT_DEXT_SPI_REG_AUDIT, in, 1, nullptr, 0,
                             nullptr, nullptr, a, &sz);
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
    io_connect_t c = open_flash_retry();
    if (c)
      return c;
    usleep(100000);
  }
  return 0;
}

int
main()
{
  io_connect_t c = open_flash();
  if (!c) { puts("could not open the flash client"); return 2; }
  int fail = 0;
  uint32_t was = 0;
  xrt_dext_spi_audit a{};

  audit(c, &a, true);                                     // start from a clean tally

  // --- 0. Ground truth for the RX FIFO, in AUDIT-ONLY mode, with no substitution
  // anywhere: send an N-byte read and count what actually comes back. Everything
  // else here depends on knowing this, and I had it wrong: the suppression logic
  // assumed RX_FIFO_OCY holds count-minus-one.
  set_mode(c, XRT_SPI_GUARD_OFF, &was);
  for (uint32_t nb = 1; nb <= 4; ++nb) {
    wr(c, XRT_SPI_CR, IDLE | XRT_SPI_CR_TX_RESET | XRT_SPI_CR_RX_RESET);
    wr(c, XRT_SPI_CR, IDLE);
    for (uint32_t i = 0; i < nb; ++i)
      wr(c, XRT_SPI_DTR, i ? 0x00 : 0x05);        // read status + dummies
    wr(c, XRT_SPI_SSR, ~0x01u);
    wr(c, XRT_SPI_CR, IDLE & ~XRT_SPI_CR_INHIBIT);
    for (int g = 0; g < 100000; ++g)
      if (rd(c, XRT_SPI_SR) & XRT_SPI_SR_TX_EMPTY) break;
    wr(c, XRT_SPI_CR, IDLE);
    const uint32_t sr = rd(c, XRT_SPI_SR);
    const uint32_t ocy = rd(c, XRT_SPI_RX_OCY);
    uint32_t drained = 0;
    while (!(rd(c, XRT_SPI_SR) & XRT_SPI_SR_RX_EMPTY) && drained < 32) {
      (void)rd(c, XRT_SPI_DRR); drained++;
    }
    wr(c, XRT_SPI_SSR, 0xffffffffu);
    std::printf("0. sent %u byte(s): SPISR 0x%02x, RX_OCY %u, DRR yielded %u byte(s)\n",
                nb, sr, ocy, drained);
    if (drained != nb) {
      std::printf("   MISMATCH: a real transfer returned %u for %u sent\n", drained, nb);
      fail++;
    }
  }

  audit(c, &a, true);        // check 0's own launches must not count below

  // --- 1. an unaddressed read: RDID. One launch, class READ, no address.
  const uint8_t rdid[6] = { 0x9f, 0, 0, 0, 0, 0 };
  transact(c, 0, rdid, sizeof(rdid));
  audit(c, &a, false);
  std::printf("1. RDID (0x9f): %u launch(es), read class count %u, opcode 0x%02x\n",
              a.launches, a.cls[XRT_SPI_OP_READ].count, a.cls[XRT_SPI_OP_READ].last_opcode);
  if (a.launches != 1 || a.cls[XRT_SPI_OP_READ].count != 1
      || a.cls[XRT_SPI_OP_READ].last_opcode != 0x9f) { puts("   WRONG"); fail++; }
  if (a.undecoded) { std::printf("   %u undecoded - WRONG\n", a.undecoded); fail++; }
  if (a.addr_bytes_seen) { puts("   an address was decoded for an unaddressed opcode - WRONG"); fail++; }

  // --- 2. a 3-byte-address read at a low address, so the extended address
  // register does not enter into it.
  // Set the extended address register first. It supplies address bits 27:24 and it
  // PERSISTS - across tally resets and across processes - so a test that assumes it
  // is 0 reads whatever the last tool left. That is how this check started failing
  // after flash_policy_check ran: 0x00123456 decoded as 0x01123456, correctly.
  audit(c, &a, true);
  wr(c, XRT_SPI_CR, IDLE | XRT_SPI_CR_TX_RESET | XRT_SPI_CR_RX_RESET);
  wr(c, XRT_SPI_CR, IDLE);
  wr(c, XRT_SPI_DTR, 0xc5);
  wr(c, XRT_SPI_DTR, 0x00);
  wr(c, XRT_SPI_SSR, ~0x01u);
  wr(c, XRT_SPI_CR, IDLE & ~XRT_SPI_CR_INHIBIT);
  for (int g = 0; g < 8 && !(rd(c, XRT_SPI_SR) & XRT_SPI_SR_RX_EMPTY); ++g)
    (void)rd(c, XRT_SPI_DRR);
  wr(c, XRT_SPI_CR, IDLE);
  wr(c, XRT_SPI_SSR, 0xffffffffu);

  audit(c, &a, true);
  const uint32_t want3 = 0x00123456u;
  const uint8_t r3[8] = { 0x03, (uint8_t)(want3 >> 16), (uint8_t)(want3 >> 8),
                          (uint8_t)want3, 0, 0, 0, 0 };
  transact(c, 0, r3, sizeof(r3));
  audit(c, &a, false);
  std::printf("2. READ 0x03 at 0x%08x: decoded 0x%08x..0x%08x, width %s\n",
              want3, a.cls[XRT_SPI_OP_READ].min_addr, a.cls[XRT_SPI_OP_READ].max_addr,
              (a.addr_bytes_seen & 1) ? "3-byte" : "?");
  if (a.cls[XRT_SPI_OP_READ].min_addr != want3 || !(a.addr_bytes_seen & 1)) {
    puts("   WRONG"); fail++;
  }

  // --- 3. a 4-byte-address read at a HIGH address. This is the one that proves
  // full 32-bit reconstruction without touching the extended address register.
  audit(c, &a, true);
  const uint32_t want4 = 0x01400000u;
  const uint8_t r4[9] = { 0x13, (uint8_t)(want4 >> 24), (uint8_t)(want4 >> 16),
                          (uint8_t)(want4 >> 8), (uint8_t)want4, 0, 0, 0, 0 };
  transact(c, 0, r4, sizeof(r4));
  audit(c, &a, false);
  std::printf("3. READ 0x13 at 0x%08x: decoded 0x%08x, width %s\n",
              want4, a.cls[XRT_SPI_OP_READ].min_addr,
              (a.addr_bytes_seen & 2) ? "4-byte" : "?");
  if (a.cls[XRT_SPI_OP_READ].min_addr != want4 || !(a.addr_bytes_seen & 2)) {
    puts("   WRONG"); fail++;
  }

  // --- 4. the chip the command went to, from SSR.
  audit(c, &a, true);
  transact(c, 1, rdid, sizeof(rdid));
  audit(c, &a, false);
  std::printf("4. a command to chip 1: chip_mask 0x%x\n", a.chip_mask);
  if (a.chip_mask != 0x2) { puts("   WRONG - the selected chip was not recorded"); fail++; }

  // --- 5. the address span across several commands.
  audit(c, &a, true);
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t addr = 0x00010000u * (i + 1);
    const uint8_t r[8] = { 0x03, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8),
                           (uint8_t)addr, 0, 0, 0, 0 };
    transact(c, 0, r, sizeof(r));
  }
  audit(c, &a, false);
  std::printf("5. four reads 0x10000..0x40000: span 0x%08x..0x%08x, count %u\n",
              a.cls[XRT_SPI_OP_READ].min_addr, a.cls[XRT_SPI_OP_READ].max_addr,
              a.cls[XRT_SPI_OP_READ].count);
  if (a.cls[XRT_SPI_OP_READ].min_addr != 0x00010000u
      || a.cls[XRT_SPI_OP_READ].max_addr != 0x00040000u
      || a.cls[XRT_SPI_OP_READ].count != 4) { puts("   WRONG"); fail++; }

  // --- 6. no erase or program was recorded, because none was issued. A shadow
  // that invented one would be worse than no shadow.
  if (a.cls[XRT_SPI_OP_ERASE].count || a.cls[XRT_SPI_OP_PROGRAM].count
      || a.cls[XRT_SPI_OP_BULK_ERASE].count) {
    puts("6. an erase or program was recorded but none was sent - WRONG"); fail++;
  } else {
    puts("6. no erase/program/bulk recorded, as expected - nothing was written");
  }

  // --- 7. audit mode performs everything: nothing was suppressed above.
  std::printf("7. mode %u (%s), suppressed %u\n", a.mode,
              a.mode == XRT_SPI_GUARD_DRY_RUN ? "dry run" : "audit only", a.suppressed);
  if (a.mode != XRT_SPI_GUARD_OFF || a.suppressed) {
    puts("   WRONG - audit mode must perform every command"); fail++;
  }

  // --- 8. the mode selector: ENFORCE is a real mode now, a bogus one is not.
  kern_return_t mkr = set_mode(c, XRT_SPI_GUARD_ENFORCE, &was);
  std::printf("8. setting ENFORCE: %s (0x%x)\n", mkr ? "REFUSED - BUG" : "accepted", mkr);
  if (mkr) fail++;
  if (set_mode(c, 99, &was) == kIOReturnSuccess) { puts("   mode 99 accepted - BUG"); fail++; }

  // --- 8b. The only rule ENFORCE has left: a bulk erase is a violation. Issued in
  // DRY_RUN, where the opcode is rewritten to a read before it reaches the FIFO, so it
  // cannot execute whatever the policy decides - and the policy is still evaluated and
  // counted, which is what counting violations in every mode is for.
  //
  // The address-range checks that used to live here are gone with the rule they
  // tested (see xrt_dext_abi.h). What remains is card-agnostic, so it needs no
  // per-card setup and cannot go stale on a different board.
  set_mode(c, XRT_SPI_GUARD_DRY_RUN, &was);
  auto issue = [&](uint8_t op, uint32_t addr, bool addressed) {
    wr(c, XRT_SPI_CR, IDLE | XRT_SPI_CR_TX_RESET | XRT_SPI_CR_RX_RESET);
    wr(c, XRT_SPI_CR, IDLE);
    wr(c, XRT_SPI_DTR, op);
    if (!addressed && addr)
      wr(c, XRT_SPI_DTR, addr & 0xff);
    if (addressed) {
      wr(c, XRT_SPI_DTR, (addr >> 16) & 0xff);
      wr(c, XRT_SPI_DTR, (addr >> 8) & 0xff);
      wr(c, XRT_SPI_DTR, addr & 0xff);
    }
    wr(c, XRT_SPI_SSR, ~0x01u);
    kern_return_t k = wr(c, XRT_SPI_CR, IDLE & ~XRT_SPI_CR_INHIBIT);
    for (int g = 0; g < 8 && !(rd(c, XRT_SPI_SR) & XRT_SPI_SR_RX_EMPTY); ++g)
      (void)rd(c, XRT_SPI_DRR);
    wr(c, XRT_SPI_CR, IDLE);
    wr(c, XRT_SPI_SSR, 0xffffffffu);
    return k;
  };
  puts("8b. the one remaining rule:");
  audit(c, &a, true);
  issue(0xc7, 0, false);
  audit(c, &a, false);
  std::printf("    bulk erase 0xc7:      %s\n",
              a.refused == 1 ? "violation recorded" : "NOT RECORDED - BUG");
  if (a.refused != 1) fail++;
  audit(c, &a, true);
  issue(0xd8, 0x00000000, true);
  audit(c, &a, false);
  std::printf("    sector erase at 0:    %s\n",
              a.refused == 0 ? "no violation - there is no address rule, as intended"
                             : "VIOLATION - an address rule came back");
  if (a.refused != 0) fail++;

  // --- 8c. ENFORCE really refuses, not merely records. Issued with EVERY CHIP
  // DESELECTED, so even a failed refusal has no device listening. A bulk erase is the
  // most destructive command there is, which is exactly why it is worth proving the
  // refusal on it - and why it is issued with nothing selected.
  puts("8c. ENFORCE refuses (chips deselected, so nothing can execute):");
  set_mode(c, XRT_SPI_GUARD_ENFORCE, &was);
  audit(c, &a, true);
  wr(c, XRT_SPI_CR, IDLE | XRT_SPI_CR_TX_RESET | XRT_SPI_CR_RX_RESET);
  wr(c, XRT_SPI_CR, IDLE);
  wr(c, XRT_SPI_DTR, 0xc7);
  wr(c, XRT_SPI_SSR, 0xffffffffu);
  kern_return_t ek = wr(c, XRT_SPI_CR, IDLE & ~XRT_SPI_CR_INHIBIT);
  uint32_t cr_e = rd(c, XRT_SPI_CR);
  audit(c, &a, false);
  std::printf("    bulk erase returned 0x%x %s\n", ek,
              ek == kIOReturnNotPermitted ? "(kIOReturnNotPermitted, as it should)"
                                          : "- EXPECTED kIOReturnNotPermitted");
  std::printf("    SPICR after: 0x%08x  INHIBIT %s\n", cr_e,
              (cr_e & XRT_SPI_CR_INHIBIT) ? "set - never launched, correct"
                                          : "CLEARED - it launched, BUG");
  if (ek != kIOReturnNotPermitted) fail++;
  if (!(cr_e & XRT_SPI_CR_INHIBIT)) fail++;
  if (a.refused != 1) { puts("    tally did not record it - BUG"); fail++; }
  wr(c, XRT_SPI_CR, IDLE);

  // --- 9. DRY RUN suppresses, and the substituted transfer returns the RIGHT
  // NUMBER OF BYTES. That second half is the property that actually broke three
  // designs: finalTransfer() decrements its byte count per byte RECEIVED, so a
  // suppressed transfer that comes back short hangs the caller for ever.
  //
  // The probe opcode is 0x04, WRITE DISABLE, chosen because it is harmless even if
  // the suppression failed completely - it clears the write-enable latch, which is
  // volatile and is the opposite of enabling a write. An erase opcode would be the
  // stronger probe, but this chip has no blank sector anywhere (checked against the
  // baseline dump: 64 of 64 1 MiB blocks are non-blank), so a regression in
  // suppression would erase live content. A test that can destroy what it is
  // testing is not a test worth having.
  //
  // What this therefore does NOT cover: the address-carrying path, where the
  // substitute read uses the original command's address bytes. Verifying that would
  // mean issuing a real erase. It is exercised by the dry run itself instead, where
  // the erase and program addresses show up in the tally.
  puts("9. dry run:");
  if (set_mode(c, XRT_SPI_GUARD_DRY_RUN, &was)) { puts("   could not set dry run"); fail++; }
  audit(c, &a, true);
  wr(c, XRT_SPI_CR, IDLE | XRT_SPI_CR_TX_RESET | XRT_SPI_CR_RX_RESET);
  wr(c, XRT_SPI_CR, IDLE);
  wr(c, XRT_SPI_DTR, 0x04);                        // write disable: harmless either way
  wr(c, XRT_SPI_DTR, 0x00);                        // 2 bytes, so the count is testable
  wr(c, XRT_SPI_SSR, ~0x01u);                      // chip 0 selected, as a real run does
  kern_return_t lkr = wr(c, XRT_SPI_CR, IDLE & ~XRT_SPI_CR_INHIBIT);
  uint32_t sr_after = rd(c, XRT_SPI_SR);
  audit(c, &a, false);
  std::printf("   write-disable launch returned 0x%x (success is expected: the caller\n"
              "     must believe it worked so it carries on)\n", lkr);
  std::printf("   sent %u byte(s), short_rx %u, RX %s\n",
              a.last_suppressed_sent, a.short_rx,
              (a.last_suppressed_sr & 1) ? "EMPTY - would hang" : "has data");
  std::printf("   SPISR after: 0x%08x  TX %s\n", sr_after,
              (sr_after & XRT_SPI_SR_TX_EMPTY) ? "empty - the transfer completed"
                                               : "NOT EMPTY - the caller would hang");
  if (lkr != kIOReturnSuccess) { puts("   WRONG - should report success"); fail++; }
  if (a.suppressed != 1) { puts("   WRONG - suppressed count"); fail++; }
  if (a.short_rx) { puts("   WRONG - nothing came back; a real caller would hang"); fail++; }
  // The exact count cannot be asserted from here: RX_FIFO_OCY reads 0 on this core
  // whatever the FIFO holds (check 0 above measures that), and counting by draining
  // would consume the bytes a real caller needs. What check 0 establishes is that a
  // genuine transfer returns exactly what it sent - and the substitution IS a
  // genuine transfer, which is the point of doing it that way.
  // Drain what the substituted read returned, so the next check starts clean.
  for (int g = 0; g < 8 && !(rd(c, XRT_SPI_SR) & XRT_SPI_SR_RX_EMPTY); ++g)
    (void)rd(c, XRT_SPI_DRR);
  wr(c, XRT_SPI_CR, IDLE);
  wr(c, XRT_SPI_SSR, 0xffffffffu);

  // --- 10. and a read still goes through in dry run, or the rehearsal would be a
  // rehearsal of something else.
  audit(c, &a, true);
  transact(c, 0, rdid, sizeof(rdid));
  audit(c, &a, false);
  std::printf("10. a read during dry run: read count %u, suppressed %u\n",
              a.cls[XRT_SPI_OP_READ].count, a.suppressed);
  if (a.cls[XRT_SPI_OP_READ].count != 1 || a.suppressed) {
    puts("    WRONG - reads must still be performed"); fail++;
  }

  set_mode(c, XRT_SPI_GUARD_ENFORCE, &was);        // leave the DEFAULT in place, not OFF

  wr(c, XRT_SPI_CR, 0);                                   // release STARTUPE3
  audit(c, &a, true);                                     // leave a clean tally
  IOServiceClose(c);
  std::printf("\n%d check(s) failed\n", fail);
  return fail ? 1 : 0;
}
