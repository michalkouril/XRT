// SPDX-License-Identifier: Apache-2.0
// Interface between the XRT macOS shim and the xrt_user DriverKit extension.
// Shared by the dext (DriverKit) and user space (IOKit), so plain C types only.
#ifndef XRT_DEXT_ABI_H
#define XRT_DEXT_ABI_H

#include <stdint.h>

#define XRT_DEXT_ABI_VERSION     42
#define XRT_DEXT_USER_CLASS      "XrtUser"

// External method selectors
enum xrt_dext_selector {
  XRT_DEXT_GET_DEVICE_INFO = 0,   // out: struct xrt_dext_device_info
  XRT_DEXT_READ_ROM        = 1,   // out: XRT_DEXT_ROM_SIZE bytes of the feature ROM
  XRT_DEXT_LOAD_BITSTREAM  = 2,   // mgmt only. scalar in: flags, data clock MHz,
                                  // kernel clock MHz (0 = keep current), xclbin
                                  // UUID bytes 0-7, bytes 8-15; struct in:
                                  // bitstream words in host order;
                                  // out: xrt_dext_load_result
  XRT_DEXT_GET_XCLBIN_UUID = 3,   // mgmt only. out: 16-byte UUID of the xclbin
                                  // loaded by this dext instance (zero if none)
  XRT_DEXT_DMA_SELFTEST    = 4,   // user only. scalar in: card address, length,
                                  // seed; out: xrt_dext_dma_result
  XRT_DEXT_SYNC            = 5,   // user only. scalar in: card address, length,
                                  // direction (0 host->card, 1 card->host);
                                  // H2C: struct in = host data; C2H: struct out
                                  // = host buffer. scalar out: status of engine
  XRT_DEXT_CU_WRITE        = 6,   // user only. scalar in: BAR0 offset; struct in:
                                  // u32 values written to consecutive registers.
                                  // Limited to the kernel control window.
  XRT_DEXT_SET_SECTION     = 7,   // mgmt only. scalar in: section slot; struct in:
                                  // raw xclbin section of the loaded xclbin
  XRT_DEXT_GET_SECTION     = 8,   // mgmt only. scalar in: section slot, byte offset;
                                  // scalar out: total section size; struct out:
                                  // up to XRT_SECTION_CHUNK bytes from offset
  XRT_DEXT_MEM_ALLOC       = 9,   // user only. scalar in: bank base, bank size,
                                  // bytes; scalar out: card address
  XRT_DEXT_MEM_FREE        = 10,  // user only. scalar in: card address
  XRT_DEXT_MEM_STAT        = 11,  // user only. scalar in: bank base, bank size;
                                  // scalar out: bytes used, allocation count
  XRT_DEXT_HOT_RESET       = 12,  // mgmt only. out: xrt_dext_reset_result
  XRT_DEXT_DMA_MODE        = 13,  // user only. scalar in: XRT_DMA_MODE_*;
                                  // scalar out: mode in effect afterwards
  XRT_DEXT_NOP             = 14,  // does nothing; the floor of a call into
                                  // the dext, for measurement
  XRT_DEXT_CU_EXEC         = 15,  // user only. One call per command: writes a
                                  // CU's argument registers, then ap_start.
                                  // scalar in: [0] CU base offset in BAR0,
                                  // [1] argument register offset within the CU
                                  // | (word count << 32), [2..15] the argument
                                  // words, two per scalar, low word first.
                                  // Above XRT_CU_EXEC_INLINE_WORDS the words
                                  // come in the structure input instead, which
                                  // costs about 10 us more (see the README).
                                  // scalar out: the control register as the
                                  // dext read it under its lock, so the caller
                                  // needs no read of its own. kIOReturnBusy if
                                  // the CU was not idle.
  XRT_DEXT_PARTITION_HOLD  = 16,  // user only. The user partition is about to
                                  // be reprogrammed or reset through the
                                  // management function: wait for any transfer
                                  // in flight, stop the DMA engines, and refuse
                                  // DMA and CU writes until released. Held by
                                  // the calling connection; released if it
                                  // closes. kIOReturnBusy if another connection
                                  // holds it.
  XRT_DEXT_PARTITION_RELEASE = 17, // user only. Release a hold this connection
                                  // took. Idempotent.
  XRT_DEXT_CU_CTX_OPEN     = 18,  // user only. scalar in: CU index, mode
                                  // (XRT_CU_CTX_*), the CU's base offset in
                                  // BAR0. Refused with
                                  // kIOReturnExclusiveAccess if another
                                  // connection's context conflicts. Repeated
                                  // opens by one connection are counted.
  XRT_DEXT_CU_CTX_CLOSE    = 19,  // user only. scalar in: CU index.
  XRT_DEXT_CFG_READ        = 20,  // Read one dword of PCI configuration space,
                                  // for walking the extended capability list on
                                  // platforms that describe themselves rather
                                  // than sitting at known offsets. scalar in:
                                  // byte offset, 4-aligned, below
                                  // XRT_CFG_SPACE_BYTES; scalar out: the value.
                                  // Read-only and bounded; either function.
  XRT_DEXT_SET_PLATFORM    = 21,  // Install this card's register map; struct in:
                                  // xrt_dext_platform. Refused once the map has
                                  // already been replaced or an xclbin is loaded.
  XRT_DEXT_GET_PLATFORM    = 22,  // Read the register map in force; struct out:
                                  // xrt_dext_platform.
  XRT_DEXT_SET_CU_WINDOW   = 23,  // user only. Move the CU register window to
                                  // where this xclbin's CUs actually are.
                                  // scalar in: byte offset in BAR0, length.
                                  // Refused while any CU context is open.
  XRT_DEXT_SET_CARD_RANGE  = 24,  // user only. The span of card addresses DMA
                                  // and the allocator may touch, from the
                                  // loaded xclbin's memory topology. scalar in:
                                  // first, last+1. Refused while any device
                                  // buffer is outstanding.
  XRT_DEXT_LOAD_CMC        = 28,  // mgmt only. Bring the board controller up:
                                  // hold its reset, copy the firmware image
                                  // into its RAM, release the reset, wait for
                                  // the register map to come alive. struct in:
                                  // the image. struct out: xrt_dext_cmc_result.
  XRT_DEXT_CLOCK_MEASURE   = 29,  // mgmt only. Measure a kernel clock with the
                                  // frequency counter the loaded xclbin brought
                                  // with it. scalar in: counter index 0..2;
                                  // scalar out: MHz, 0 if it did not measure.
  XRT_DEXT_ERT_START       = 30,  // mgmt only. Bring the scheduler up: hold its
                                  // reset, copy the firmware into its RAM,
                                  // release, then configure it through the
                                  // command queue and wait for the command to
                                  // come back completed - which is the only
                                  // real proof the MicroBlaze is executing.
                                  // struct in: the image. struct out:
                                  // xrt_dext_ert_result.
  XRT_DEXT_CMC_BOARD_INFO  = 31,  // mgmt only. Ask the board controller for its
                                  // board info over its mailbox and hand back
                                  // the raw key/length/value blob. struct out:
                                  // xrt_dext_cmc_bdinfo.
  // NB: XRT_DEXT_SELECTOR_COUNT takes the value after the LAST enumerator in
  // TEXTUAL order, not the highest value. A new selector written above a
  // lower-numbered one leaves the count too small and the dispatch table too
  // short, which shows up as "array designator index exceeds array bounds".
  // Add new selectors here, at the end.
  XRT_DEXT_SPI_XFER        = 32,  // mgmt only, XRT_DEXT_CLIENT_FLASH only.
                                  // One AXI Quad SPI transaction.
                                  // scalar in: slave index, bytes to return;
                                  // struct in: bytes to send; struct out: the
                                  // bytes clocked back. The driver owns every
                                  // register access - user space never names an
                                  // address - so this cannot reach anything but
                                  // the flash controller.
  XRT_DEXT_SPI_REG         = 33,  // mgmt only, XRT_DEXT_CLIENT_FLASH only.
                                  // One 32-bit read or write of one
                                  // AXI Quad SPI register. scalar in: register
                                  // offset within the flash block, value to
                                  // write, 1 to write / 0 to read. scalar out:
                                  // the value read. See the bound below.
  XRT_DEXT_SPI_REG_AUDIT   = 34,  // mgmt only, XRT_DEXT_CLIENT_FLASH only. Read
                                  // back what the flash commands driven through
                                  // SPI_REG actually were. scalar in: 1 to reset
                                  // the tally after reading; struct out:
                                  // xrt_dext_spi_audit.
  XRT_DEXT_SPI_REG_MODE    = 35,  // mgmt only, XRT_DEXT_CLIENT_FLASH only. Set
                                  // what the TX-stream shadow does with what it
                                  // decodes. scalar in: XRT_SPI_GUARD_*; scalar
                                  // out: the mode in force before the call.
  XRT_DEXT_SELECTOR_COUNT
};

// PCI configuration space a caller may read through XRT_DEXT_CFG_READ: the
// whole extended space, which is where the vendor capability that locates a
// dynamic-IP platform's blocks lives.
#define XRT_CFG_SPACE_BYTES      4096

// Feature ROM location in the user BAR for XDMA shells (XRT devices.h,
// XOCL_RES_FEATURE_ROM) and how much of it the driver returns.
// Which BAR the feature ROM is in comes from the platform map; this is only the
// default, and what the probe tools use to name BAR0.
#define XRT_DEXT_ROM_BAR         0
#define XRT_DEXT_ROM_OFFSET      0xB0000
#define XRT_DEXT_ROM_SIZE        256
#define XRT_DEXT_ROM_MAGIC       0x786e6c78   /* "xlnx" */

// IOConnectMapMemory64 memory types: the BAR number. Mapped read-only.
#define XRT_DEXT_MAP_BAR(n)      (n)

// One compute unit's register page, mapped read-WRITE, so a process can start
// that CU with stores instead of a call into the dext. Granted only to a
// connection that holds the CU exclusively, and only while no other
// connection has a context on a CU in the same page. The page is the
// machine's, 16 KiB on Apple silicon; a CU whose base is not page aligned is
// refused, because the mapping would reach below it.
//
// The grant is a fast path for cooperating processes, not an isolation
// boundary: DriverKit cannot revoke a mapping, so closing the context does not
// take it away, and every process can already write the CU window through
// XRT_DEXT_CU_WRITE. What the page mapping adds is speed, not reach.
#define XRT_DEXT_MAP_CU_RW_BASE  64
#define XRT_DEXT_MAP_CU_RW(cu)   (XRT_DEXT_MAP_CU_RW_BASE + (cu))
#define XRT_CU_RW_PAGE           16384u

// Management-function register map of the XDMA 5.x shells (XRT devices.h).
// These are the DEFAULTS of the lookup below, not the lookup itself: nothing
// in the driver should use them directly any more.
#define XRT_MGMT_ICAP            0x020000   /* AXI HWICAP */
#define XRT_MGMT_GATE_ULP        0x030000   /* axigate, PRP level */
#define XRT_MGMT_MEMCALIB        0x032000   /* bit0: MIG calibrated */
#define XRT_MGMT_CLKWIZ_K1       0x050000
#define XRT_MGMT_CLKWIZ_K2       0x051000
#define XRT_MGMT_SYSMON          0x0A0000

// ---------------------------------------------------------------------------
// Where this platform keeps the blocks the driver touches.
//
// The XDMA 5.x shells put them at the fixed offsets above, which are this
// table's defaults, so a dext that is never told otherwise behaves exactly as
// it did before the table existed. A dynamic-IP platform such as the U50
// describes its own layout, and the shim installs that description with
// XRT_DEXT_SET_PLATFORM - the dext cannot read it itself, because the
// description lives in a file on the host and because the vendor capability on
// the card names only a handful of blocks (not ICAP, the gate or the
// firewalls).
//
// A block with size 0 is ABSENT, and that is a normal answer, not an error: a
// U50 has no feature ROM, no kernel clock wizards and no SYSMON at all. Code
// must check rather than read offset 0 and believe the zeroes it gets back.
//
// The CU window is deliberately NOT here. It is not a property of the base
// platform - it comes from the loaded xclbin's ip_layout - so it would be
// describing the wrong thing. It is moved with XRT_DEXT_SET_CU_WINDOW instead,
// and reported back in this struct's cu_window_* fields (output only).
// ---------------------------------------------------------------------------
enum xrt_dext_block_id {
  XRT_BLK_FEATURE_ROM = 0,   /* mgmt: fixed feature ROM; XDMA 5.x only     */
  XRT_BLK_ICAP,              /* mgmt: AXI HWICAP                           */
  XRT_BLK_GATE_ULP,          /* mgmt: ULP AXI gate / PR isolation          */
  XRT_BLK_MEMCALIB,          /* mgmt: memory calibration status            */
  XRT_BLK_CLKWIZ_K1,         /* mgmt: kernel clock wizard 1                */
  XRT_BLK_CLKWIZ_K2,         /* mgmt: kernel clock wizard 2                */
  XRT_BLK_SYSMON,            /* mgmt: SYSMON4                              */
  XRT_BLK_UUID_ROM,          /* mgmt: logic uuid ROM; dynamic IP only      */
  XRT_BLK_CMC_REGMAP,        /* mgmt: CMC register map; dynamic IP only    */
  XRT_BLK_FIREWALL_0,        /* mgmt: AXI firewalls, contiguous ids        */
  XRT_BLK_FIREWALL_1,
  XRT_BLK_FIREWALL_2,
  XRT_BLK_FIREWALL_3,
  XRT_BLK_FIREWALL_4,
  XRT_BLK_FIREWALL_5,
  XRT_BLK_FIREWALL_6,
  XRT_BLK_FIREWALL_7,
  XRT_BLK_XDMA,              /* user: the XDMA engine registers            */
  XRT_BLK_CMC_FW_MEM,        /* mgmt: RAM the CMC runs from                 */
  XRT_BLK_CMC_RESET,         /* mgmt: the CMC's reset GPIO                 */
  XRT_BLK_CMC_MUTEX,         /* mgmt: carries the regmap-ready bit         */
  XRT_BLK_ERT_FW_MEM,        /* mgmt: the scheduler MicroBlaze's code RAM  */
  XRT_BLK_ERT_RESET,         /* mgmt: its reset GPIO                       */
  XRT_BLK_ERT_CQ,            /* the command queue; both functions see it   */
  XRT_BLK_ERT_CSR,           /* the scheduler's status registers           */
  XRT_BLK_FLASH,             /* mgmt: AXI Quad SPI, the boot flash          */
  XRT_BLK_MAX
};

/* Firewalls are addressed as XRT_BLK_FIREWALL_0 + i. XDMA 5.x has four; a U50
 * has seven, so leave headroom rather than silently dropping one. */
#define XRT_MGMT_FIREWALLS       8

struct xrt_dext_block {
  uint64_t offset;           /* within the BAR                             */
  uint64_t size;             /* 0: this platform has no such block         */
  uint8_t  bar;
  uint8_t  func;             /* 0 management, 1 user                       */
  uint8_t  pad[6];
};

#define XRT_PLATFORM_DEFAULT     0x1   /* the map is the compiled-in default */

struct xrt_dext_platform {
  uint32_t abi;              /* XRT_DEXT_ABI_VERSION of the sender          */
  uint32_t count;            /* entries that follow; <= XRT_BLK_MAX         */
  uint32_t flags;
  uint32_t pad;
  struct xrt_dext_block blk[XRT_BLK_MAX];
  uint64_t cu_window_offset; /* output only: the CU window in force          */
  uint64_t cu_window_size;   /* output only                                  */
  uint64_t card_min;         /* output only: card addresses DMA may touch     */
  uint64_t card_max;         /* output only                                  */
};

// Xilinx AXI Quad SPI v3.2, register offsets within XRT_BLK_FLASH. The
// controller is a plain SPI master: the flash command set lives in user space,
// which is where it can be tested without a card.
#define XRT_SPI_IPISR            0x1c
#define XRT_SPI_IPIER            0x28
#define XRT_SPI_SRR              0x40   /* write 0x0a to reset the core       */
#define XRT_SPI_CR               0x60
#define XRT_SPI_SR               0x64
#define XRT_SPI_DTR              0x68
#define XRT_SPI_DRR              0x6c
#define XRT_SPI_SSR              0x70
#define XRT_SPI_TX_OCY           0x74
#define XRT_SPI_RX_OCY           0x78

#define XRT_SPI_SRR_RESET        0x0000000a
#define XRT_SPI_CR_LOOP          (1u << 0)   /* local loopback: MOSI tied to MISO
                                              * internally, and the core transfers
                                              * with no slave select asserted    */
#define XRT_SPI_CR_TX_RESET      (1u << 5)
#define XRT_SPI_CR_RX_RESET      (1u << 6)
#define XRT_SPI_CR_MANUAL_SS     (1u << 7)
#define XRT_SPI_CR_INHIBIT       (1u << 8)
#define XRT_SPI_CR_MASTER        (1u << 2)
#define XRT_SPI_CR_SPE           (1u << 1)
#define XRT_SPI_SR_RX_EMPTY      (1u << 0)
#define XRT_SPI_SR_TX_EMPTY      (1u << 2)
#define XRT_SPI_SR_TX_FULL       (1u << 3)

/* One transaction is bounded by the controller's 256-byte FIFOs. Erase and
 * program are built from many of these, in user space. */
#define XRT_SPI_MAX_XFER         256

/* The `type` argument to IOServiceOpen(), which picks a user client class.
 *
 * XRT_DEXT_CLIENT_DEFAULT is everything except flash, and any process may open it
 * - the dext holds allow-any-userclient-access, which is what makes the tools here
 * usable without provisioning.
 *
 * XRT_DEXT_CLIENT_FLASH carries the two SPI selectors and nothing else, and its
 * personality in Info.plist sets IOUserClientEntitlements, so the opening process
 * must be signed with `org.xrtmac.flash-access` = true. Without it IOServiceOpen
 * fails and there is no route to the flash controller at all. The reason the
 * flash needs its own gate: XRT_DEXT_SPI_REG can drive a bulk erase (opcode 0xc7
 * needs no address), which destroys the factory image and is not recoverable
 * without JTAG. Being able to run a program should not be enough to do that.
 *
 * Splitting by client class rather than by privilege is deliberate. DriverKit
 * gives a dext no way to ask whether its caller is root, so "root only" is not
 * expressible; an entitlement is, and it is checked by the kernel before the
 * dext sees the connection.
 */
/* ---------------------------------------------------------------------------
 * The TX-stream shadow: what the driver makes of the bytes it is handed.
 *
 * SPI_REG hands the driver dword writes to a FIFO, so on its own the driver
 * cannot tell a read of the device identity from an erase of the whole part. The
 * shadow closes that gap WITHOUT changing the interface: the driver keeps the
 * first few bytes pushed to DTR, and decodes them at the one moment the command
 * becomes irreversible - the SPICR write that clears TRANS_INHIBIT, which is what
 * actually starts the transfer (xspi.cpp finalTransfer()). Nothing has left the
 * FIFO before that write, so that write is also where a refusal would go.
 *
 * What it does with what it decodes is a MODE, because the useful modes are not
 * the same thing:
 *
 *  - OFF is an audit. Every write is performed; the tally records what happened.
 *    Use it to watch a real operation. It does NOT tell you what an operation
 *    would do before it does it - a distinction worth being exact about, because
 *    getting it wrong is how a "dry run" ends up writing to a card.
 *  - DRY_RUN is the rehearsal. EVERY command that would change device state is
 *    SUPPRESSED: every slave select is deasserted immediately before the launch, so
 *    the controller clocks the command out with no device listening (a SPI flash
 *    with CS high ignores clock and data), and the caller sees a normal transfer.
 *    Deasserting rather than withholding the launch matters: SPI is symmetric, so
 *    the caller waits to receive as many bytes as it sent, and a transfer that
 *    never runs hangs it. That covers erase,
 *    program and bulk erase, and also the write-enable latch, the extended address
 *    register and the configuration registers: a rehearsal leaves the chips
 *    bit-for-bit AND register-for-register as they were. Reads and status polls are
 *    performed, which is what keeps the caller on the same path as a real run.
 *    Returning success is a lie to the caller, which is why it takes an explicit
 *    mode and why the tally counts `suppressed` separately.
 *  - ENFORCE is the DEFAULT, and all it refuses is a BULK ERASE. Opcode 0xc7 takes
 *    no address and takes out the whole part, and nothing legitimate issues one:
 *    upstream's bulkErase() is dead code, defined and never called, revertToMFG()
 *    included. So refusing it is free on every card.
 *
 *    There was also an address rule - refuse erase/program below a ceiling - and it
 *    is GONE. It was removed deliberately, and the reasons are worth keeping because
 *    they are the reasons not to add it back casually:
 *      - Linux has no equivalent. xocl and xbflash.qspi guard no addresses at all.
 *      - The layout is not universal. On the QSPI-PS parts the golden image sits HIGH
 *        (xqspips.h: GOLDEN_BASE 0x06000000) with the user image at 0, so "refuse
 *        below X" protects the wrong half.
 *      - Even on one card it is image-dependent: the Innova-2's MCS declares its
 *        bitstream guard at 0x00fff000 while upstream's built-in default is
 *        0x01002000, so a ceiling that fits one flow refuses another.
 *      - Measured on that card, a range covering the factory image would refuse
 *        xbflash --factory-reset, whose guard write lands at 0x00801000 - inside the
 *        factory bitstream. Whether that is protection or breakage depends on which
 *        operation you meant, which is exactly the judgement a driver cannot make.
 *    What remains protecting the flash is the entitlement on XrtFlashClient (item 78)
 *    and DRY_RUN, which are not layout-dependent.
 *
 *    A refusal does not pretend to succeed: the launching write is not performed, the
 *    FIFOs are emptied so the queued command cannot be launched later, and the call
 *    returns kIOReturnNotPermitted, which upstream turns into an exception and
 *    aborts. That is the opposite of DRY_RUN, where reporting success is the point.
 *
 *    Violations are counted in EVERY mode, so a DRY_RUN reports whether enforcing
 *    would have got in the way.
 *
 *    WHAT THIS IS NOT: a security boundary. Any caller that can reach these selectors
 *    can also set XRT_SPI_GUARD_OFF. It catches a wrong opcode, not malice - malice is
 *    what the entitlement on XrtFlashClient is for.
 *
 * ---------------------------------------------------------------------------
 */
#define XRT_SPI_GUARD_OFF        0   /* no guard: everything is performed. Set this
                                      * deliberately to write the factory image.   */
#define XRT_SPI_GUARD_DRY_RUN    1   /* rehearse: suppress every device write      */
#define XRT_SPI_GUARD_ENFORCE    2   /* DEFAULT: refuse bulk erase. No address rule;
                                      * see above for why there is not one.        */

/* Measured layout of the Innova-2 as flashed with the rbds image, from the baseline
 * dump and a full rehearsal. Recorded for whoever needs it - nothing in the driver
 * uses it, and the comment above says why a driver should not.
 *   0x00000000..0x00ddc000  factory bitstream (all 3,548 4K sectors hold data)
 *   0x00ddc000..0x00fff000  zero padding
 *   0x00fff000..0x01000000  bitstream guard sector (erased when the User image is valid)
 *   0x01000000..0x0192cddd  User slot, where a real flash writes
 * and note xbflash --factory-reset would write its guard at 0x00801000, inside the
 * factory bitstream. */

#define XRT_SPI_OP_OTHER         0   /* status polls, write-enable, anything else */
#define XRT_SPI_OP_READ          1   /* 0x03 0x0b 0x3b 0xbb 0x6b 0xeb 0x13 0x9f   */
#define XRT_SPI_OP_ERASE         2   /* 0x20 0x52 0xd8: addressed erase           */
#define XRT_SPI_OP_PROGRAM       3   /* 0x02 0x32 0x38: addressed page program    */
#define XRT_SPI_OP_BULK_ERASE    4   /* 0xc7: the whole part, no address at all   */
#define XRT_SPI_OP_EAR_WRITE     5   /* 0xc5: sets the top nibble of the address  */
#define XRT_SPI_OP_CLASSES       6

/* Per class: how many transactions, the last opcode seen in it, and the span of
 * addresses it reached. min_addr/max_addr are meaningless for the unaddressed
 * classes and are reported as 0 and 0. */
struct xrt_dext_spi_audit_class {
  uint32_t count;
  uint32_t last_opcode;
  uint32_t min_addr;
  uint32_t max_addr;
};

struct xrt_dext_spi_audit {
  uint32_t abi;
  uint32_t launches;         /* transactions started since the last reset        */
  uint32_t mode;             /* XRT_SPI_GUARD_* in force. See above.             */
  uint32_t undecoded;        /* launches whose first byte was never seen (a FIFO
                              * left filled across a reset, say) - if this is not
                              * 0 the tally below is incomplete, so check it      */
  struct xrt_dext_spi_audit_class cls[XRT_SPI_OP_CLASSES];
  /* The first few addressed WRITES (erase and program only), in order, so the
   * halving can be eyeballed without reading the log. Reads are counted and their
   * address span recorded in cls[] above, but they do not appear here. */
  uint32_t first_write_addr[8];
  uint32_t first_write_op[8];
  uint32_t first_write_n;
  uint32_t ear;              /* extended address register as last set by 0xc5    */
  uint32_t ear_valid;
  uint32_t chip_mask;        /* bit N: chip N was selected at least once         */
  uint32_t addr_bytes_seen;  /* bit0: a 3-byte address; bit1: a 4-byte address   */
  /* The controller's state right after the most recent suppressed transfer. A
   * suppressed command has to leave the RX FIFO as a real one would, or the caller
   * waits for bytes that never come; these two make that visible from the tally
   * rather than from a stack sample of a wedged process. */
  /* Diagnostics kept from working out how to suppress safely. They matter much less
   * now that suppression is a single opcode rewrite on the way into the FIFO: the
   * transfer is entirely the caller's own, so its byte count cannot come out wrong.
   * They stay because they are what caught three designs where it did.
   *
   * NOTE on RX_FIFO_OCY: it reads 0 on this core whatever the FIFO holds, measured
   * against known transfer sizes where DRR yielded exactly 1, 2, 3 and 4 bytes. It
   * is reported raw and must never be treated as a count - assuming it held
   * count-minus-one is what made a working design look broken. */
  uint32_t last_suppressed_sr;       /* not sampled since the rewrite; reads 0     */
  uint32_t last_suppressed_rx_ocy;   /* not sampled since the rewrite; reads 0     */
  uint32_t last_suppressed_sent;     /* bytes the caller put in the TX FIFO        */
  uint32_t drr_since_launch;         /* DRR reads since the last launch: how many
                                      * bytes the caller has drained. Still useful:
                                      * if it stalls below what was sent, the caller
                                      * is spinning.                               */
  uint32_t short_rx;                 /* not sampled since the rewrite; reads 0     */
  uint32_t refused;          /* commands that VIOLATE the policy - a bulk erase.
                              * Counted in every mode, so a DRY_RUN says whether
                              * enforcing would have got in the way; only actually
                              * refused in ENFORCE.                              */
  uint32_t last_refused_addr;/* the address of the most recent violation          */
  uint32_t last_refused_op;  /* its opcode                                        */
  uint32_t suppressed;       /* commands NOT sent because the mode suppressed them.
                              * Non-zero means the device did not see everything
                              * the caller asked for - which is the point in
                              * DRY_RUN and a bug anywhere else.                 */
};

#define XRT_DEXT_CLIENT_DEFAULT  0u
#define XRT_DEXT_CLIENT_FLASH    1u
#define XRT_DEXT_FLASH_ENTITLEMENT "org.xrtmac.flash-access"

/* What XRT_DEXT_SPI_REG can reach. Unlike XRT_DEXT_SPI_XFER, which keeps the
 * whole register sequence in the driver, this selector lets user space name an
 * offset - so the set of offsets is an explicit ALLOWLIST, not a window.
 *
 * These seven are exactly the registers upstream's xbflash.qspi touches (SRR,
 * SPICR, SPISR, DTR, DRR, SPISSR, TX_OCY); it never reads RX_OCY. A window
 * instead of a list was the first design, and it was wrong twice over: 4 KiB is
 * fifty times the core's defined aperture, and the claim that the core "does not
 * respond above its aperture" was an assumption - Xilinx AXI-Lite slaves commonly
 * decode only the low address bits, so an offset above 0x78 may well ALIAS onto a
 * real register rather than read as nothing. An allowlist needs no such
 * assumption.
 *
 * The selector exists so the upstream erase and program sequencing (xspi.cpp) can
 * be used unchanged rather than reimplemented - the one flash operation where a
 * mistake costs a card - at the price of a weaker bound than SPI_XFER's. Note
 * what that bound does NOT do: these seven registers are enough to erase the
 * flash, the Factory image included, because DTR is how opcodes reach the device.
 * The driver sees dword writes to a FIFO, not flash addresses. SPI_XFER remains
 * the interface for reads.
 *
 * Cost: one call per register, so ~14 us of IOKit floor plus ~3 us of MMIO.
 * xspi.cpp pushes one byte per DTR write, which puts a 256-byte page at about
 * 4 ms. That is the price of reusing the sequencing; if it ever matters, the
 * fix is a mapped page, not a longer list.
 */
#define XRT_SPI_REG_ALLOWED      { XRT_SPI_SRR, XRT_SPI_CR, XRT_SPI_SR, \
                                   XRT_SPI_DTR, XRT_SPI_DRR, XRT_SPI_SSR, \
                                   XRT_SPI_TX_OCY }

// The result of XRT_DEXT_SPI_XFER, so a caller can tell a refusal apart from a
// transaction that ran and returned nothing useful.
struct xrt_dext_spi_result {
  uint32_t status;           /* IOReturn                                     */
  uint32_t sr_before;        /* SPISR as found, before the transaction        */
  uint32_t sr_after;         /* SPISR when the transaction finished           */
  uint32_t rx_bytes;         /* bytes actually clocked back                   */
  uint32_t slave;            /* which slave was asserted                     */
  uint32_t waited_us;        /* time spent waiting for the FIFO to drain      */
};

// A frequency counter, one per kernel clock. Write MEASURE_START, wait for
// MEASURE_DONE in the low half, then read the count - which is already MHz. The
// result register moves if bit 16 of the status is set. After xocl
// subdev/clock_counter.c.
#define XRT_FREQ_CNT_STATUS_MASK    0xffffu
#define XRT_FREQ_CNT_MEASURE_START  0x1u
#define XRT_FREQ_CNT_MEASURE_DONE   0x2u
#define XRT_FREQ_CNT_VALUE          0x8u
#define XRT_FREQ_CNT_V5_ENABLED     0x10000u
#define XRT_FREQ_CNT_V5_VALUE       0x10u
// The count is in kHz, despite xocl calling it a frequency in MHz: on a U50 the
// three counters read 300000/500000/450000 against 300/500/450 MHz requested.
#define XRT_FREQ_CNT_PER_MHZ        1000u

// UCS control/status, on platforms that have it: channel 2 enables the kernel
// clocks once they are programmed, channel 1 reports clock shutdown/throttling.
#define XRT_UCS_CHANNEL1         0x0
#define XRT_UCS_CHANNEL2         0x8

// LOAD_BITSTREAM flags
#define XRT_DEXT_LOAD_STATE_ONLY 0x1        /* read state, write nothing */

// Kernel clocks a download may program. XDMA 5.x has two; a U50 xclbin asks for
// three, the third being the HBM clock, and HBM calibration does not complete
// without it.
#define XRT_LOAD_CLOCKS          3

// LOAD_BITSTREAM scalar inputs:
//   0      flags
//   1-2    target MHz for clocks 0 and 1
//   3-4    the xclbin uuid
//   5-7    clock wizard offsets, 0 where unused
//   8      target MHz for clock 2
//   9      UCS control/status offset, 0 if the platform has none
//   10-12  frequency counter offsets, 0 where absent. Kept for
//          XRT_DEXT_CLOCK_MEASURE, so the frequency can be measured rather
//          than assumed from what was asked for.
//
// The wizard offsets are passed in because on a dynamic-IP platform the kernel
// clocks are NOT in the base platform: they arrive with the user partition the
// xclbin brings, so only the caller - which has the xclbin and its partition
// metadata - knows where they are. Pass 0 for all of them to use the platform's
// own wizards (XRT_BLK_CLKWIZ_K1/K2), which is what an XDMA 5.x card does.
//
// The two cases need different sequences, not just different addresses. xocl
// spells both out in subdev/clock_wiz.c, with the warning that violating them
// trips firewalls at random:
//
//   base-platform clocks (legacy): freeze gate, set clocks, free gate
//   clocks in the partition (2RP): download, free gate, reset gate, set clocks,
//                                  enable UCS channel 2, wait for calibration
//
// The second cannot save and re-commit a configuration, because until the
// partition is programmed the wizards do not exist - reads from them come back
// as filler.

// LOAD_BITSTREAM steps, for failure reporting
enum xrt_dext_load_step {
  XRT_LOAD_STEP_CHECK = 0,
  XRT_LOAD_STEP_CLOCK_SAVE,
  XRT_LOAD_STEP_GATE_FREEZE,
  XRT_LOAD_STEP_ICAP_WRITE,
  XRT_LOAD_STEP_ICAP_DONE,
  XRT_LOAD_STEP_CLOCK_RESTORE,
  XRT_LOAD_STEP_GATE_FREE,
  XRT_LOAD_STEP_CALIBRATION,
  XRT_LOAD_STEP_COMPLETE,
};

struct xrt_dext_load_result {
  int32_t  status;          // kern_return_t
  uint32_t step;            // last step reached (xrt_dext_load_step)
  uint32_t words_written;
  uint32_t icap_sr_before;
  uint32_t icap_sr_after;
  uint32_t icap_wfv;        // write FIFO vacancy before download
  uint32_t gate_before;     // gate read register: 0 = frozen
  uint32_t gate_after;
  uint32_t clk_status[XRT_LOAD_CLOCKS];  // wizard status (bit0 = locked) after
  uint32_t clk_cfg0[XRT_LOAD_CLOCKS];    // CONFIG(0) saved/restored
  uint32_t clk_cfg2[XRT_LOAD_CLOCKS];    // CONFIG(2) saved/restored
  uint32_t calib;           // memcalib register after
  uint32_t calib_ms;        // time waited for calibration
  uint32_t elapsed_ms;      // ICAP streaming time
  uint32_t target_mhz[XRT_LOAD_CLOCKS]; // requested per clock (0 = keep)
  uint32_t table_mhz[XRT_LOAD_CLOCKS];  // frequency table entry programmed
  uint32_t ucs_after;       // UCS channel 1 after enabling, if there is a UCS
};

// XDMA (PG195). On the XDMA 5.x shells it is BAR1 of the user function, which
// is this block's default; a U50 puts it on BAR2. The register offsets below
// are relative to the block, which starts at offset 0 within its BAR on every
// platform seen so far - a non-zero offset is refused rather than quietly
// mishandled.
#define XRT_XDMA_BAR             1
#define XRT_XDMA_H2C(ch)         (0x0000 + (ch) * 0x100)
#define XRT_XDMA_C2H(ch)         (0x1000 + (ch) * 0x100)
#define XRT_XDMA_SGDMA           0x4000      /* SGDMA regs above each channel */
#define XRT_DMA_MODE_POLL        0           /* DMA completion by polling */
#define XRT_DMA_MODE_IRQ         1           /* by MSI-X when vectors are available */
#define XRT_DMA_MODE_QUERY       2           /* no change */
#define XRT_XDMA_IRQ             0x2000      /* IRQ block: channel enables and MSI-X vectors */
#define XRT_DMA_SELFTEST_MAX     (1024 * 1024)
// Default span of card addresses DMA and the allocator may touch: the DDR banks
// of the XDMA 5.x shells (mem_topology bank0..bank3, 16 GiB each from
// 0x4000000000). Only the default - the span in force comes from the loaded
// xclbin's memory topology (XRT_DEXT_SET_CARD_RANGE), because another shell
// puts its memory somewhere else: a U50's one HBM bank is 256 MiB at 0.
#define XRT_DMA_CARD_MIN         0x4000000000ULL
#define XRT_DMA_CARD_MAX         0x8000000000ULL

// Kernel (compute unit) control window of the user partition in user BAR0
// for the XDMA 5.x shells (OCL region, DR base 0x1800000, 8 MiB).
// Default CU register window: where the XDMA 5.x shells put their user
// partition. These are only the defaults - the window in force comes from the
// loaded xclbin's ip_layout (XRT_DEXT_SET_CU_WINDOW), because a different shell
// puts its CUs somewhere else entirely: a U50's verify kernel sits at 0x1400000,
// below this base.
#define XRT_CU_WINDOW_BASE       0x1800000u
#define XRT_CU_WINDOW_END        0x2000000u

// What one CU is allowed to occupy, for turning ip_layout's base addresses into
// a window. ip_layout gives no sizes, and xocl maps 64 KiB per CU.
#define XRT_CU_SPAN              0x10000u
#define XRT_CU_WINDOW_MAX        0x4000000u   /* refuse to map more than 64 MiB */
#define XRT_CU_WRITE_MAX_WORDS   1024
#define XRT_CU_AP_START          0x1u        /* ap_ctrl_hs control register */
#define XRT_CU_AP_DONE           0x2u
#define XRT_CU_AP_IDLE           0x4u
#define XRT_CU_AP_READY          0x8u
// ap_ctrl_chain only: a CU with this protocol asserts ap_done and then WAITS
// for the host to acknowledge by writing ap_continue. Until that happens it
// accepts no further command - so a chain CU that is never acknowledged runs
// exactly once, and the run after that sees a stale done and appears to finish
// instantly without executing.
#define XRT_CU_AP_CONTINUE       0x10u

// CU contexts, as xocl keeps them per client. An exclusive context excludes
// every other connection; a shared one excludes only exclusive ones. The
// values match xrt::hw_context::access_mode.
#define XRT_CU_CTX_EXCLUSIVE     0
#define XRT_CU_CTX_SHARED        1
#define XRT_CU_CTX_MAX           512  /* open contexts the dext tracks */
#define XRT_CU_MAX               128  /* CU indices, as XRT's cu_mask allows */
// Set in the CU_WRITE offset argument to bypass the dext's own mapping of
// the CU window and write through IOPCIDevice::MemoryWrite32 instead. For
// comparing the two paths (tools/lat_parts.cpp); the offset itself is small.
#define XRT_CU_WRITE_NO_MAP      (1ULL << 63)

// CU_EXEC: this CU uses ap_ctrl_chain, so it may be parked holding ap_done and
// waiting to be acknowledged. Acknowledging it is part of starting the next
// command rather than a call of its own: the dext already reads the control
// register inside its lock here, so writing ap_continue there costs nothing,
// where a separate CU_WRITE after each command would cost another call into the
// dext. It also means a CU left parked by a process that died between done and
// the acknowledgement is recovered by whoever starts it next, instead of
// needing a card reset.
#define XRT_CU_EXEC_CHAIN        (1ULL << 62)
// CU_EXEC argument words that fit in the 16-entry scalar array, after the
// CU base and the offset/count scalar. A structure input of any size costs
// about 10 us on top of a call; scalars are free.
#define XRT_CU_EXEC_INLINE_WORDS 28

// xclbin metadata kept by the dext for other processes (xocl keeps the
// same sections in the kernel). Slots, not axlf section kinds.
#define XRT_SECTION_MEM_TOPOLOGY 0
#define XRT_SECTION_IP_LAYOUT    1
#define XRT_SECTION_CONNECTIVITY 2
// An xclbin whose kernel arguments are backed by several banks at once carries
// a GROUP topology and connectivity as well: the same shape, with a merged
// entry appended per group. A U50 bandwidth.xclbin has 37 memory entries and a
// 38th group entry, MBG[0:31], spanning all 32 HBM banks - and that 38th index
// is what xrt::kernel::group_id() returns. Absent in an xclbin that groups
// nothing, where the plain sections are the answer.
#define XRT_SECTION_GROUP_TOPOLOGY     3
#define XRT_SECTION_GROUP_CONNECTIVITY 4
#define XRT_SECTION_SLOTS        5
#define XRT_SECTION_MAX_BYTES    (64 * 1024)
#define XRT_SECTION_CHUNK        4096        /* inline struct output limit */

// Device memory allocator kept by the dext (one per card, shared by all
// processes; freed per process when its connection closes).
#define XRT_MEM_ALLOC_ALIGN      4096
#define XRT_MEM_MAX_ALLOCS       8192

// ---------------------------------------------------------------------------
// The board controller (CMC on a U50, XMC on older shells). Its register map is
// a RAM that means nothing until the controller is running: before that every
// register reads 0, which is how to tell "not started" from "not there" (a
// block that is not there reads filler instead).
//
// Bringing it up, after xocl subdev/xmc.c load_xmc(): hold the reset GPIO at 0,
// copy the image into the controller's RAM, write 1 to release, then wait for
// the magic word and the regmap-ready bit. The image is not flashed - it is
// loaded from the host every boot - so a bad one costs a reset, not a card.
// ---------------------------------------------------------------------------
#define XRT_CMC_MAGIC            0x74736574u   /* "test" once it is running */
#define XRT_CMC_MAGIC_REG        0x00
#define XRT_CMC_VERSION_REG      0x04
#define XRT_CMC_STATUS_REG       0x08
#define XRT_CMC_ERROR_REG        0x0c
#define XRT_CMC_FEATURE_REG      0x10
#define XRT_CMC_SENSOR_REG       0x14
#define XRT_CMC_FPGA_TEMP        0xf8
#define XRT_CMC_HBM_TEMP         0x260
#define XRT_CMC_GPIO_RESET       0x0
#define XRT_CMC_GPIO_ENABLED     0x1
#define XRT_CMC_REGMAP_READY     0x2           /* in the mutex block, at +0x8 */
#define XRT_CMC_IMAGE_MAX        (128 * 1024)

enum xrt_dext_cmc_step {
  XRT_CMC_STEP_CHECK = 0,
  XRT_CMC_STEP_HOLD_RESET,
  XRT_CMC_STEP_COPY,
  XRT_CMC_STEP_RELEASE,
  XRT_CMC_STEP_WAIT_MAGIC,
  XRT_CMC_STEP_WAIT_READY,
  XRT_CMC_STEP_COMPLETE,
};

struct xrt_dext_cmc_result {
  int32_t  status;
  uint32_t step;
  uint32_t words_copied;
  uint32_t gpio_after;
  uint32_t magic;
  uint32_t version;
  uint32_t cmc_status;
  uint32_t error;
  uint32_t feature;
  uint32_t ready;            /* the mutex block's ready bits */
  uint32_t elapsed_ms;
  uint32_t was_running;      /* 1: it was already up, nothing was written */
};

// ---------------------------------------------------------------------------
// ERT, the scheduler MicroBlaze. Addresses on the XDMA 5.x shells come from
// xocl devices.h __RES_XMC and ert.h; note the command queue and the status
// registers are visible from BOTH functions at the same offsets, which is why
// bring-up needs only the management dext.
//
// An unloaded MicroBlaze sits on a cold-boot self jump at address 0 of its code
// RAM, which is how to tell "no firmware" from "running" before writing
// anything: (instruction & 0xfc00ffff) == 0xb8000000.
// ---------------------------------------------------------------------------
#define XRT_ERT_FW_MEM_DEFAULT   0x160000u   /* mgmt BAR0, 128 KiB */
#define XRT_ERT_RESET_DEFAULT    0x131000u   /* mgmt BAR0; shared with the XMC */
#define XRT_ERT_CQ_DEFAULT       0x190000u   /* 64 KiB, either function */
#define XRT_ERT_CSR_DEFAULT      0x180000u
#define XRT_ERT_SELF_JUMP(ins)   (((ins) & 0xfc00ffffu) == 0xb8000000u)
#define XRT_ERT_GPIO_RESET       0x0u
#define XRT_ERT_GPIO_ENABLED     0x1u

// CSR offsets (ert.h, relative to ERT_CSR_ADDR)
#define XRT_ERT_STATUS_REG       0x00        /* completions, clear on read */
#define XRT_ERT_CU_STATUS_REG    0x44
#define XRT_ERT_CQ_STATUS_ENABLE 0x54
#define XRT_ERT_CQ_STATUS_REG    0x58        /* the doorbell: host -> MicroBlaze */

// Command states and opcodes we use (ert.h)
#define XRT_ERT_STATE_NEW        1u
#define XRT_ERT_STATE_COMPLETED  4u
#define XRT_ERT_OP_START_CU      0u
#define XRT_ERT_OP_CONFIGURE     2u

#define XRT_ERT_SLOT_SIZE        4096u
#define XRT_ERT_IMAGE_MAX        (128 * 1024)

// XDMA 5.x board-controller offsets (xocl devices.h __RES_XMC), for the shells
// that have no partition metadata to describe them. The reset GPIO is the same
// register as the scheduler's: on these shells one bit releases both
// MicroBlazes, which is why xocl's load_xmc() always copies both images.
#define XRT_CMC_REGMAP_DEFAULT   0x120000u
#define XRT_CMC_FW_MEM_DEFAULT   0x140000u
#define XRT_CMC_RESET_DEFAULT    0x131000u

// ERT_START's structure input: both MicroBlaze images, described rather than
// guessed at, because which of them is present depends on the shell.
struct xrt_dext_ert_images {
  uint32_t sched_bytes;      /* the scheduler image, first                  */
  uint32_t cmc_bytes;        /* the board controller's, after it; may be 0  */
  /* What to write to the reset GPIO to release. xocl writes 1, which on this
   * shell releases the board controller and leaves the scheduler held - the
   * register looks like one bit per MicroBlaze. 0 here means "use 1", so the
   * old behaviour is the default. */
  uint32_t gpio_release;
  uint32_t pad;
};

enum xrt_dext_ert_step {
  XRT_ERT_STEP_CHECK = 0,
  XRT_ERT_STEP_HOLD_RESET,
  XRT_ERT_STEP_COPY,
  XRT_ERT_STEP_RELEASE,
  XRT_ERT_STEP_CONFIGURE,
  XRT_ERT_STEP_WAIT_CONFIG,
  XRT_ERT_STEP_COMPLETE,
};

struct xrt_dext_ert_result {
  int32_t  status;
  uint32_t step;
  uint32_t words_copied;
  uint32_t cmc_words_copied; /* 0 if no controller image was supplied */
  uint32_t gpio_after;
  uint32_t image_word0;      /* what the code RAM holds after the copy */
  uint32_t was_self_jump;    /* 1: no firmware was loaded before this */
  uint32_t config_header;    /* slot 0's header after the wait */
  uint32_t config_state;     /* its state field */
  uint32_t csr_status;
  uint32_t cu_status;
  uint32_t elapsed_ms;
  uint32_t config_ms;
};

// ---------------------------------------------------------------------------
// The board controller's mailbox, which is how anything beyond the plain sensor
// registers is asked for - the board's name, its maximum power level, serial
// number and so on. Offsets are within the controller's register map.
//
// The exchange (xocl subdev/xmc.c xmc_send_pkt / xmc_recv_pkt): read where the
// packet buffer lives, write the request there, set the owner bit in the control
// register, wait for the controller to clear it, then read the reply from the
// same place. The reply is a run of key/length/value entries.
//
// Note xocl also treats 0xdeadfa11 in the offset register as "not ready", which
// is the same filler value this driver checks for elsewhere.
// ---------------------------------------------------------------------------
#define XRT_CMC_CONTROL_REG      0x18
#define XRT_CMC_PKT_OWNER        (1u << 5)
#define XRT_CMC_MSG_OFFSET_REG   0x300
#define XRT_CMC_MSG_ERROR_REG    0x304
#define XRT_CMC_OP_BOARD_INFO    0x4
#define XRT_CMC_BDINFO_MAX       1024

/* key/length/value keys (xmc.c enum board_info_key) */
#define XRT_BDINFO_SN            0x21
#define XRT_BDINFO_REV           0x26
#define XRT_BDINFO_NAME          0x27
#define XRT_BDINFO_BMC_VER       0x28
#define XRT_BDINFO_MAX_PWR       0x29
#define XRT_BDINFO_FAN_PRESENCE  0x2a

struct xrt_dext_cmc_bdinfo {
  int32_t  status;
  uint32_t bytes;            /* how much of data[] the controller returned */
  uint32_t error;            /* the controller's error register */
  uint32_t msg_offset;       /* where its mailbox buffer turned out to be */
  uint8_t  data[XRT_CMC_BDINFO_MAX];
};

// AXI firewall registers. Which firewalls a platform has, and where, is in the
// block table above (XRT_BLK_FIREWALL_0 + i); XDMA 5.x has four and the U50 six.
#define XRT_FW_FAULT_STATUS      0x0
#define XRT_FW_UNBLOCK_CTRL      0x8
#define XRT_FW_SI_FAULT_STATUS   0x100
#define XRT_FW_SI_UNBLOCK_CTRL   0x108
#define XRT_FW_BUSY_MASK         0x10001     /* READ/WRITE_RESPONSE_BUSY */

struct xrt_dext_reset_result {
  int32_t  status;                       // overall kern_return_t
  int32_t  reset_status;                 // IOPCIDevice::Reset return
  uint32_t fw_before[XRT_MGMT_FIREWALLS]; // FAULT_STATUS | SI_FAULT_STATUS
  uint32_t fw_after[XRT_MGMT_FIREWALLS];
  uint32_t clear_rounds;                 // unblock rounds needed
  uint32_t rom_magic_after;
  uint32_t gate_after;
  uint32_t clk_status_after[2];
  uint16_t command_after;
  uint16_t reserved;
  uint32_t elapsed_ms;
};

#define XRT_DMA_TO_CARD          0
#define XRT_DMA_TO_HOST          1

struct xrt_dext_dma_result {
  int32_t  status;          // kern_return_t
  uint32_t step;            // 0 setup, 1 H2C, 2 C2H, 3 compare, 4 complete
  uint64_t card_addr;
  uint32_t length;
  uint32_t segments;        // IOVA segments of the host buffer
  uint64_t host_iova;       // first segment
  uint64_t desc_iova;
  uint32_t h2c_status;      // engine status after the transfer
  uint32_t h2c_done;        // completed descriptor count
  uint32_t h2c_us;
  uint32_t c2h_status;
  uint32_t c2h_done;
  uint32_t c2h_us;
  uint32_t mismatches;      // 32-bit words that differ
  uint32_t first_bad_index;
  uint32_t first_bad_expected;
  uint32_t first_bad_actual;
  uint16_t pci_command;     // after enabling bus master
  uint16_t reserved;
};

struct xrt_dext_bar {
  uint8_t  present;      // 1 if GetBARInfo succeeded
  uint8_t  memory_index; // index for MemoryRead/Write and mapping
  uint8_t  type;         // IOPCIBARType
  uint8_t  reserved;
  uint32_t reserved2;
  uint64_t size;         // bytes
};

struct xrt_dext_device_info {
  uint32_t abi_version;
  uint16_t vendor;
  uint16_t device;
  uint16_t subsystem_vendor;
  uint16_t subsystem_id;
  uint8_t  revision;
  uint8_t  bus;
  uint8_t  dev;
  uint8_t  func;
  uint16_t command;         // PCI command register after Open()
  uint16_t status;          // PCI status register
  uint16_t link_status;     // PCIe Link Status (0xffff if no PCIe capability)
  uint16_t reserved;
  uint32_t link_caps;       // PCIe Link Capabilities
  struct xrt_dext_bar bars[6];
  // ABI 13: DMA completion. irq_vectors is the number of MSI-X vectors
  // serving the XDMA engines, 0 when the dext polls (the default; see
  // XRT_DEXT_DMA_MODE). Counters are per function
  // since the dext started: batches whose completion was signalled by an
  // interrupt, and batches that finished without one (polled).
  uint32_t irq_vectors;
  uint32_t reserved2;
  uint64_t dma_irq_batches;
  uint64_t dma_poll_batches;
};

#endif
