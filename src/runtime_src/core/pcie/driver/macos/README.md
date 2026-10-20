# XRT on macOS (Apple silicon)

Experimental port of XRT's PCIe (Alveo) stack to macOS. It runs Xilinx XDMA
5.x shells over Thunderbolt on Apple silicon Macs: the runtime, `xrt-smi`,
`xbmgmt`, `xclbinutil`, `pyxrt` and the native XRT C++ API.

Tested on an M1 MacBook Pro running macOS 27.0 with a VCU1525 (U200 class)
card running `xilinx_vcu1525_xdma_201830_1` in a Sonnet Breakaway Box
(PCIe Gen3 x4 over Thunderbolt 3).

## Components

| Path | What |
|---|---|
| `src/runtime_src/core/pcie/driver/macos/XrtUser` | DriverKit extension `org.xrtmac.XrtUser`. One binary binds both PCI functions of the card. |
| `src/runtime_src/core/pcie/driver/macos/Loader` | `XrtLoader.app`, activates or deactivates the extension. |
| `src/runtime_src/core/pcie/driver/macos/tools` | Low-level probes and tests that talk to the extension directly. |
| `src/runtime_src/core/pcie/macos` | The XRT shim (`libxrt_core.dylib`). |
| `build/build-macos.sh` | Builds everything and produces an installer package. |

The design keeps the extension thin and the shim thick:

* The extension opens the device and enables memory decoding and bus
  mastering. It owns device memory for all processes and frees a process's
  buffers when it exits. It maps BARs read-only into processes and runs privileged
  sequences: programming the user partition through ICAP, reprogramming
  clocks, and XDMA transfers from the caller's own pages through the DART.
  Transfer completion is polled by default, or signalled by MSI-X.
  It writes kernel registers only inside the user partition's CU window, and
  keeps the loaded xclbin's UUID and memory, IP and connectivity metadata
  for all processes. It maps the CU window of the user partition into
  itself once, so starting a compute unit is one call carrying the whole
  register map.
* The shim enumerates cards from the IOKit registry and answers device
  queries. It reads sensors (SYSMON), clocks and the feature ROM through the
  mappings. It allocates device memory and schedules compute units in user
  space, in the style of xocl with ERT disabled. It polls a running CU at an
  interval of 1/16 of the command's run time so far, between 20 us and 1 ms.

`xrt_dext_abi.h` is the single definition of the interface between the two.

## One-time machine setup

The extension claims the PCI DriverKit entitlement, which Apple grants per
vendor. Until that is granted it is ad-hoc signed, so the Mac needs:

1. Full Xcode, for the DriverKit SDK and `iig`.
2. SIP disabled: in Recovery, run `csrutil disable`.
3. `sudo nvram boot-args="amfi_get_out_of_my_way=1"`, then a reboot.
4. `sudo systemextensionsctl developer on`.
5. For Thunderbolt enclosures, approve the accessory on first connection.
6. For unattended machines, `sudo pmset -a disablesleep 1`. Sleep suspends
   the extension, and an upgrade in progress will not finish until the Mac
   wakes.

## Build and install

```
brew install boost cmake ninja pkgconf openssl@3 rapidjson pybind11 opencl-headers ocl-icd xcodegen
pip3 install --user pyyaml markdown jinja2
build/build-macos.sh
sudo installer -pkg build/Darwin-pkg/xrt_<version>_macos_arm64.pkg -target /
```

OpenCL applications link Homebrew's ICD loader, not the system
`OpenCL.framework`, which does not load vendor ICDs:

```
clang++ app.cpp -I/opt/homebrew/opt/opencl-headers/include \
  -L/opt/homebrew/opt/ocl-icd/lib -lOpenCL -Wl,-rpath,/opt/homebrew/opt/ocl-icd/lib
```

On first install, approve the extension in System Settings, under General,
Login Items & Extensions, Driver Extensions. Upgrades do not ask again.

For `xrt-smi validate`, install the shell's test xclbins where the Linux
platform package puts them, in
`/opt/xilinx/dsa/<shell>/test/{verify,bandwidth}.xclbin`.

## Status

Working on both test cards - a VCU1525 (XDMA 5.x) and a U50 (dynamic IP):

| | |
|---|---|
| `xrt-smi` / `xbmgmt examine` | identity, feature ROM or logic uuid, SYSMON and CMC sensors, measured clocks, memory topology, PCIe |
| Shell generations | **XDMA 5.x and dynamic-IP (2RP)**, chosen by the PCIe VSEC capability rather than by whether a feature ROM answers |
| Platform discovery | per-card block table built from the platform package's partition metadata, with the XDMA 5.x values as defaults; no fixed management offsets remain |
| xclbin load | ICAP download for both flows, three clock wizards and the UCS, PR isolation or the XDMA gate; re-load without power cycle |
| `xrt::bo` | zero-copy XDMA sync, about 1.87 GB/s each way over Thunderbolt 3; DDR and **HBM**, with group topology and connectivity |
| DMA completion | polling by default; MSI-X on request, one vector per XDMA engine, see below |
| `xrt::kernel` / `xrt::run` | **`ap_ctrl_hs` and `ap_ctrl_chain`**, single and concurrent CUs; one call into the extension per command, or none for an exclusive context; polled with backoff |
| `xrt-smi validate` | the whole suite passes on both cards, with the rest skipped for real reasons |
| Board controller | CMC/XMC firmware loaded to RAM and running: temperatures, rails, SC version, board info over the mailbox |
| OpenCL | through Homebrew's ICD loader (`ocl-icd`); the package registers `xilinx.icd` in `/opt/homebrew/etc/OpenCL/vendors` |
| XRT tests | `00_hello`, `22_verify`, `query`, `56_xclbin`, `perf_IOPS` (1,000,000 commands), `102_multiproc_verify` |
| Multiple processes | device memory allocated by the extension; no overlap between processes; a killed process's buffers are released. CU contexts arbitrate shared and exclusive access, and CU starts are serialised across processes |
| Reset | `sudo xbmgmt reset --device <mgmt BDF> --type hot`: PCIe hot reset plus firewall recovery, then reload the xclbin |
| Sleep and wake | DMA quiesced before sleep; on wake the xclbin record is cleared because the card can lose the user partition |

Largest measured numbers: 158 GB/s on HBM across three concurrent `ap_ctrl_chain`
CUs, 18.7 us per kernel run on an exclusive context, 1.87 GB/s bo sync.

Not supported, but nothing rules it out:

* Flash programming. No QSPI driver and no flash block in the table; the
  endpoint is known (`ep_card_flash_program_00`). The risk is unlike anything
  else here - a bad flash needs JTAG, not a hot reset.
* M2M, host-memory banks and CMA.
* `xclRegRead`/`xclRegWrite` and `xclRead`/`xclWrite`. Deliberately absent: the
  kernel control window is the only place user space can make the extension
  write.
* Runtime reclocking outside a download (`xclReClock2`).
* More than one card in a host. Enumeration is written for it, never tested.
* Signing and notarization; that needs Apple's per-vendor PCI DriverKit
  entitlement. Today the extension is ad-hoc signed with SIP off.

Not supported, and not expected to be:

* **ERT / XGQ command queues. Dropped rather than deferred**: ERT is not
  running on the reference Linux U200 either, with full xocl support and
  nothing disabling it, so the host-side scheduler here is what upstream
  actually uses.
* CU completion interrupts. On the XDMA 5.x shells they reach the host only
  through the ERT. A newer shell with an `ert_cfg_gpio` block can route them
  directly, so this is a property of these cards rather than of the port.
* P2P. It needs a resizable BAR exposing device memory to a peer, and how much
  latitude DriverKit gives over BAR resizing and over handing physical
  addresses to another driver is the likely wall - untested, but worth
  settling before costing the work.
* dma-buf export or import; there is no such thing on macOS.
* QDMA shells, AIE and Versal parts, and the emulation flows.
* Host-only and SVM buffers, refused at allocation.

One behaviour worth knowing rather than fixing: the loaded xclbin's UUID and
metadata are lost if the extension restarts, although the card keeps running
the xclbin. Linux behaves the same way when the driver reloads.

## DMA completion interrupts

The extension allocates four MSI-X vectors on the user function and routes
XDMA engines H2C0, H2C1, C2H0 and C2H1 to vectors 0 to 3, as Linux
`libxdma` does. In interrupt mode the transfer thread sleeps until the
engine signals completion. It wakes 20 to 50 us after the interrupt.

Polling stays the default. When the Mac is otherwise idle, a sleeping CPU
lets Apple silicon lower its fabric clocks, and XDMA throughput drops:

| bo_test, Thunderbolt 3 | polling | MSI-X, idle Mac | MSI-X, one core busy |
|---|---|---|---|
| 16 MiB, to / from card (MB/s) | 1796 / 1675 | 1660 / 1668 | 1727 / 1690 |
| 256 MiB, to / from card (MB/s) | 1830 / 1681 | 1802 / 1546 | 1828 / 1671 |
| 8 MiB user pointer, to / from card (MB/s) | 1679 / 1655 | 968 / 1089 | 1656 / 1649 |

Interrupt mode frees the core that polling keeps busy for the length of each
transfer. Switch modes at run time, for all processes, until the extension
restarts:

```
/opt/xilinx/xrt/bin/unwrapped/xrt_dext_probe --dma-mode irq    # or poll
/opt/xilinx/xrt/bin/unwrapped/xrt_dext_probe                   # shows mode and counters
```

## Waiting for compute units

Without CU interrupts, the XRT library polls. The interval grows with the
command's run time, so short kernels complete as fast as before and long
ones cost little CPU. Measured with `tools/cu_wait_test.cpp` and the
shell's `bandwidth.xclbin`:

| Kernel run time | CPU while waiting, fixed 20 us polling | With backoff | Added completion delay |
|---|---|---|---|
| 0.3 ms | 45% | 40% | none |
| 8 ms | 21% | 8% | about 0.45 ms |
| 75 ms | 21% | 2 to 4% | about 1 ms |

Before the first sleep the wait can spin instead: an MMIO read of a CU
register costs 3.3 us, so a loop with no sleep in it already paces itself at
that interval, and a command that finishes inside the window is noticed
within 3.3 us rather than the 32 us that `sleep_for(20 us)` really takes.

Spinning for the first 50 us of every command turned out to be a bad trade.
A kernel longer than the window never finishes inside it, so the window is
pure cost: on a 0.17 ms kernel the CPU while waiting went from 38% to 58%
with no change in run time at all. The wait therefore spins on a CU only
until that CU has shown it does not pay. The last run time of each CU is
remembered, and a CU whose last command took longer than the window is
slept on from the start:

| | No spin | Spin for the first 50 us always | Spin only on CUs that finish inside it |
|---|---|---|---|
| CPU waiting on a 0.17 ms kernel | 38.3% | 58.0% | 40.2% |
| CPU waiting on a 0.84 ms kernel | 22.6% | 26.9% | 23.0% |
| CPU waiting on a 7.9 ms kernel | 7.7% | 8.3% | 7.8% |
| CPU waiting on a 75 ms kernel | 2.7% | 2.7% | 2.8% |
| hello, 64 in flight | 28,900 runs/s | 29,900 runs/s | 30,000 runs/s |
| hello run+wait, mean of three runs | 35.5 us | 33.6 us | 34.4 us |

The gain is small and the `hello` latency figures overlap between runs, so
treat the throughput number as the real one. What spinning cannot help with
is the 14.2 us cost of the call that starts the command, which is most of
what a short command pays.

Build the test against the installed XRT:

```
clang++ -std=c++17 -O2 -I/opt/xilinx/xrt/include tools/cu_wait_test.cpp \
  -L/opt/xilinx/xrt/lib -lxrt_coreutil -Wl,-rpath,/opt/xilinx/xrt/lib -o cu_wait_test
```

## The cost of a command

Starting a compute unit means writing its argument registers and then its
`ap_start` bit. Those registers are inside the user partition, so only the
extension may write them, and the shim reaches them through an external
method. What that costs is dominated by the shape of the call, not by the
register writes. Measured with `tools/lat_parts.cpp`, which includes a
selector that does nothing (`XRT_DEXT_NOP`) so the floor can be measured
directly:

| | |
|---|---|
| A call into the extension that does nothing | 14.2 us |
| The same call with any number of scalar arguments in or out | 14.2 us |
| The same call with a structure input, 4 bytes or 68 bytes alike | 23.9 us |
| One register write on top of that, through the extension's mapping | 2.5 to 3.9 us |
| Each further register write | 0.2 us |
| An MMIO read of a CU register over the read-only BAR mapping | 3.2 us |

So a structure input costs about 10 us however small it is, while scalar
arguments are free, and `IOPCIDevice::MemoryWrite32` is within 0.2 us of a
store through a mapping the extension makes itself.

The shim used to send two calls per command, each carrying its register
values as a structure input: about 48 us of call overhead per command. It
now sends one `XRT_DEXT_CU_EXEC` call that writes the arguments and
`ap_start` together, with the argument words packed into the scalar array
(up to `XRT_CU_EXEC_INLINE_WORDS`, 28 words; longer register maps fall back
to a structure input). The extension writes them through its own mapping of
the CU window, made once in `Start`.

`tools/xrt_bench.cpp`, verify.xclbin's `hello` kernel, Thunderbolt 3:

| | Two struct calls | One scalar call |
|---|---|---|
| hello run+wait | 71.2 us | 35.5 us |
| hello, 64 in flight | 14,013 runs/s | 28,900 runs/s |
| bo sync 256 MiB to / from card | 1866 / 1845 MB/s | 1866 / 1850 MB/s |

Latency varies a little between runs: three consecutive runs gave 35.5 us,
and two earlier ones 31.5 and 33.0 us. The costs in the first table do not
vary, and they account for the difference the change made. A command used
to pay two structure calls, 23.8 us each, with about 7 us of register
writes inside them, so about 58 us before XRT's own work; it now pays one
scalar call, 14.2 us, with the same writes, about 21 us. That predicts a
saving of 37 us against the 36 us measured. What is left per command is
that 14.2 us call floor, the register writes over Thunderbolt, and the
3.3 us MMIO read that checks the CU is idle.
Removing the call floor as well would mean mapping the CU window into the
process read-write, which is only safe for a hardware context that holds
the CU exclusively, as `xrt::ip` does on Linux.

## The fixed cost of a sync

A 1 MiB sync used to reach 1.43 GB/s while 256 MiB reached 1.87, so about
170 us was fixed cost rather than transfer. Two thirds of it was not what the
plan assumed.

**The descriptor ring was a third of it.** Every transfer allocated a 16 KiB
wired buffer for the XDMA descriptors, created two `IODMACommand`s and mapped
the ring through the DART, all before any data moved, and tore them down
afterwards. None of that depends on the buffer. The extension now builds them
once, on the first transfer, and reuses them: 1 MiB went from 1.43 to
1.54 GB/s, about 52 us of the fixed cost.

**Mapping each buffer once, at allocation, made things worse.** The rest of
the fixed cost is `PrepareForDMA` on the caller's buffer, so the obvious next
step was to map a buffer when it is allocated and keep it, leaving a sync only
its descriptors to program. Measured, that was a regression:

| bo sync, to card | Mapped per sync | Mapped once at allocation |
|---|---|---|
| 1 MiB | 1540 MB/s | 920 MB/s |
| 16 MiB | 1845 MB/s | 1810 MB/s |
| 256 MiB | 1868 MB/s | 1932 MB/s |

Consistent over seven runs, and the data was correct throughout, so it is a
performance effect and not a bug in the descriptors. It is also not the idle
CPU slowing the fabric, which is the usual explanation for a 40% DMA drop
here: the 1 MiB figure does not recover with a core kept busy (831 MB/s).
Keeping the mapping helps only the 256 MiB case, where the fixed cost was
already amortised. Why holding a buffer prepared makes a small transfer slower
is not yet understood, so the per-buffer mapping was reverted and only the
descriptor ring kept.

Two things worth knowing for anyone who returns to this:

* `IODMACommand::PrepareForDMA` accepts at most 32 segments. Ask for more and
  it fails with `kIOReturnOverrun` rather than filling fewer, which is easy to
  misread as fragmentation.
* The DART coalesces aggressively: a 16 MiB buffer came back as a single
  segment, so segment counts are never the obstacle.

## Compute unit contexts

`xocl` keeps CU contexts in the kernel because the kernel schedules the CUs.
Here the scheduler is in the XRT library, one instance per process, so
nothing in the library can see another process's commands. The extension
therefore holds the contexts:

| | |
|---|---|
| exclusive context | excludes every other connection |
| shared context | excludes only exclusive contexts |

These match `xrt::hw_context::access_mode`, whose default is shared, so
several processes can still share a CU. A context belongs to the connection
that opened it and is dropped when that connection closes, and repeated
opens by one connection are counted, as XRT opens a context per kernel
naming the same CU.

Sharing a CU needs more than arbitration at open time: two processes could
both find the CU idle, both write their argument registers, and one command
would then run with the other's arguments. So `XRT_DEXT_CU_EXEC` reads the
control register and writes the arguments and `ap_start` under one lock
inside the extension, which makes the start atomic for every process on the
card. A caller that loses the race gets `kIOReturnBusy` and the scheduler
leaves the command queued for the next poll.

The call returns the control register it read, and the scheduler uses that
instead of reading the register itself. Doing both cost 3.3 us per command
for nothing (39.0 us against 35.5 us per `hello`), which is what an MMIO read
over Thunderbolt costs. Handing the value back instead makes the atomic start
slightly cheaper than the unsynchronised version it replaced, because the read
now happens once, inside the extension, rather than once in each place:

| `hello`, mean of three runs | Before CU contexts | Both sides reading | Extension returns the register |
|---|---|---|---|
| run+wait | 35.5 us | 39.0 us | 34.2 us |
| 64 in flight | 28,900 runs/s | 26,160 runs/s | 29,375 runs/s |
| CPU while waiting | 50% | 46% | 40% |

This is arbitration, not isolation: the extension does not require a context
before it will write CU registers, any more than `xocl` checks one per
command. `tools/ctx_test.cpp` covers the arbitration, which is bookkeeping
and needs no xclbin loaded.

XRT's older per-IP API, `xclOpenContext` and `xclCloseContext`, is served from
the same table. It is still how the tools take a lock on the whole xclbin,
with ip_index `UINT_MAX`, xocl's virtual CU: `XBUtilities::xclbin_lock`, which
`xrt-smi validate`'s m2m and p2p tests both construct. While it reported
ENOTSUP those two tests failed on the lock instead of reaching the checks that
would have skipped them, with "failed to open ip context: Operation not
supported", which reads as though M2M itself had been attempted. The virtual
index only checks that the caller means the xclbin that is loaded; there is
nothing to arbitrate for it, because a reload is already kept away from
processes by the partition hold. A real index goes to the CU context table.

## Starting a compute unit without calling the extension

A command costs about 14.2 us in the call into the extension alone, which is
most of what a short command pays. That call exists because CU registers are
inside the user partition and only the extension may write them. For a
hardware context that holds a CU **exclusively** the extension can instead map
that CU's register page into the process read-write, and the library starts the
CU with stores:

```
xrt::hw_context ctx{dev, uuid, xrt::hw_context::access_mode::exclusive};
```

`tools/xrt_bench.cpp --exclusive` measures it against the default shared
context, which keeps using the call:

| `hello` | Shared context | Exclusive context |
|---|---|---|
| run+wait | 33.7 us | **18.8 us** |
| 64 in flight | 29,900 runs/s | **54,200 runs/s** |
| CPU while waiting | 40% | 100% of a core |

Latency and throughput both improve about 1.8 times. The CPU rises because
nothing blocks any more: the 14.2 us the thread used to spend waiting inside
an IPC is now spent either writing registers or spinning for completion. At
54,000 commands a second one core is what driving the CU costs.

This is opt-in, as it is on Linux, where only `xrt::ip` with an exclusive
context gets CU registers mapped. The default shared context is unchanged.

What the extension checks before granting a page:

* the connection holds that CU exclusively, so no other process may use it;
* the CU's base is page aligned, or the page would reach below it;
* no other connection has a context on a CU in the same page.

It is a fast path for cooperating processes, not an isolation boundary.
DriverKit cannot revoke a mapping, so closing the context does not take an
existing one away — the library drops it, and a fresh request is refused. Any
process can already write the whole CU window through `XRT_DEXT_CU_WRITE`, so
the page mapping adds speed, not reach. `tools/ctx_test.cpp` checks every rule
above, including that a mapping is refused with no context, with a shared
context, and while another connection has a CU in the page.

## Reprogramming the partition while processes are using it

Downloading an xclbin freezes the user partition's AXI gate for several
seconds. An access that reaches the partition in that window does not fail
quietly: it trips the shell's AXI firewalls, which isolate the partition
until the card is reset. Nothing in the hardware prevents this, and the two
PCI functions are separate extension processes, so the management function
cannot see what the user function is doing.

The shim therefore takes a hold on the user function before it starts a
download or a hot reset, and releases it afterwards:

```
XRT_DEXT_PARTITION_HOLD      wait for any transfer in flight, stop the DMA
                             engines, refuse DMA and CU writes
   ... ICAP download or PCIe hot reset on the management function ...
XRT_DEXT_PARTITION_RELEASE
```

While it is held, DMA and CU register writes are refused for **every**
process with `kIOReturnBusy`, not only the one doing the download. Taking
the hold blocks until a transfer already in flight has finished, so a
download cannot start underneath one.

The hold belongs to the connection that took it. If that process dies part
way through a download, the extension drops the hold when the connection
closes, so a crash cannot leave the card refusing work. A second process
trying to take it gets `kIOReturnBusy`, which also serialises two
simultaneous downloads.

What this cannot do is stop a process *reading* the CU window through its
read-only BAR mapping; DriverKit has no way to revoke a mapping already
handed out. Such a read returns all ones while the partition is down, which
the shim's scheduler treats as a failure (see below) rather than waiting for
ever.

`tools/hold_test.cpp` checks all of this, including that killing the holder
frees the hold.

## When the user partition stops responding

If a CU control register reads `0xffffffff`, the read never reached the user
partition: reserved bits read 0 on `ap_ctrl_hs` and `ap_start`, `ap_done` and
`ap_idle` are never set together. Either the partition is gone or an AXI
firewall has tripped and isolated it. `tools/fw_peek.cpp` reads the four
firewalls on the management BAR.

The shim treats that pattern as a failure and aborts the command
(`ERT_CMD_STATE_ABORT`), so `xrt::run::wait` returns and the XRT API raises
an error. Without that it waited for ever, because `try_start` reads
`ap_start` set in the all-ones register and never starts the CU.

Recover with a hot reset, which clears the firewalls, then load the xclbin
again:

```
sudo xbmgmt reset --device <mgmt BDF> --type hot --force
```

On 2026-09-24 two firewalls tripped on the test card (`FAULT_STATUS 0x4` at
`0xe0000` and `0xf0000`) after a stray process was left holding the card
while another reloaded the xclbin, so a DMA reached the partition while the
ULP gate was frozen. The hot reset recovered it completely: firewalls clear,
CU idle, `xrt-smi validate -r dma` passing.

## Newer shells: the U50

The driver's register offsets are those of the XDMA 5.x shells the VCU1525
runs. A U50 has been fitted to the enclosure and it binds - the PCI ids were
already matched - but nothing above PCI identity works, because the U50 is a
`XOCL_DSAFLAG_DYNAMIC_IP` shell and describes its blocks through a PCIe vendor
capability and an FDT instead of putting them at fixed offsets.

Two sources for that description, and they agree:

- On the live card, `tools/vsec_peek.cpp` walks the extended capability list,
  finds vsec id `0x0020` at config offset `0xe80` on both functions, and dumps
  the table it points at (`XRT_DEXT_CFG_READ` reads one aligned dword of
  config space, bounded to 4 KiB, read-only).
- On a host with the platform package installed,
  `partition_metadata.json` carries the whole map in readable form.
  `U50_ENDPOINTS.md` next to this file is that map, all 46 endpoints, with
  the raw JSON as `u50_partition_metadata.json`.

Use the package to understand the shell and the card only to identify it: the
firmware directory is named by the logic uuid, and the UUID ROM reports that
uuid, which is what ties a particular card to a particular map. Note that the
UUID ROM holds *only* the repeated logic uuid - there is no VBNV string - so
shell identification keyed on VBNV has to be rewired, not just repointed.

## Lessons from the port

* DriverKit processes on Apple silicon must be `arm64e`. An `arm64` dext
  fails to launch with "Exec format error".
* Thunderbolt-attached devices need `IOPCITunnelCompatible` in the matching
  personality.
* Xcode refuses ad-hoc signing for DriverKit targets. Build unsigned and sign
  afterwards with `codesign --entitlements`.
* Upgrading a running dext can hang in "terminating for upgrade". Killing
  the old process makes the kernel rematch. It also drops bus mastering,
  which clears a stuck DMA engine.
* The DART maps large buffers as single segments. XDMA descriptor lengths
  are 28 bits, so segments must be split below 256 MiB.
* External-method structure inputs and outputs above 4 KiB arrive as memory
  descriptors, not `OSData`.
* A structure input costs about 10 us on an external method however small it
  is, because it becomes an `OSData`. Scalar arguments are free, and there
  are 16 of them. Anything that fits belongs in the scalar array. See
  "The cost of a command".
* macOS `aligned_alloc` returns NULL unless the size is a multiple of the
  alignment, unlike glibc.
* Sleep over Thunderbolt resets the card: the static shell and its ROM
  survive, but the XDMA engines reset and the user partition is lost. A PCIe
  hot reset, by contrast, kept the partition.
* An idle Mac with sleep enabled cycles through dark wakes about every 16
  seconds, so the power handler must be cheap and repeatable.
* A dext cannot link libSystem directly, so Mach semaphores and pthread
  condition variables are out of reach. To block until an interrupt, run a
  block on a queue created with `kIODispatchQueueReentrant` that calls
  `SleepWithDeadline`, and call `Wakeup` from the interrupt handler, which
  runs on the same queue.
* On a PCI provider the MSI-X sources start at interrupt index 0 after
  `ConfigureInterrupts`. There is no legacy entry in front of them.
* `IONewZero` zero-fills the ivars structure. Default member initializers
  in it never run.
* DMA throughput depends on CPU activity. See the table above.
* Spinning instead of sleeping only pays if the wait actually ends inside the
  spin window; otherwise the window is pure CPU cost. Gate it on what the
  hardware has been seen to do. See "Waiting for compute units".
