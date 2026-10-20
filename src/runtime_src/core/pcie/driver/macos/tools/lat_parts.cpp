// SPDX-License-Identifier: Apache-2.0
// Times the pieces of one CU command on macOS:
//  - an external method that does nothing: the floor of a call into the dext
//  - CU_WRITE through IOPCIDevice::MemoryWrite32 (one kernel call per word)
//  - CU_WRITE through the dext's own mapping of the CU window
//  - an MMIO read of a CU register over the read-only BAR mapping
//  - std::this_thread::sleep_for(20 us)
// Every CU_WRITE writes back the value already in the hello_1 CU's argument
// registers, so the card state is unchanged.
#include <IOKit/IOKitLib.h>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include "../XrtUser/xrt_dext_abi.h"

using clk = std::chrono::steady_clock;

static double
per_call(int n, void (^body)())
{
  auto t0 = clk::now();
  for (int i = 0; i < n; ++i)
    body();
  return std::chrono::duration<double, std::micro>(clk::now() - t0).count() / n;
}

int
main()
{
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it))
    return 0;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_connect_t conn;
    if (IOServiceOpen(svc, mach_task_self(), 0, &conn))
      continue;
    xrt_dext_device_info info{};
    size_t sz = sizeof(info);
    // A discarded status leaves info zeroed, which would read as func 0.
    if (IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &sz)
        || info.func != 1) {
      IOServiceClose(conn);
      continue;
    }
    std::printf("dext ABI %u\n", info.abi_version);
    mach_vm_address_t a = 0;
    mach_vm_size_t l = 0;
    if (IOConnectMapMemory64(conn, 0, mach_task_self(), &a, &l, kIOMapAnywhere | kIOMapInhibitCache))
      return 1;
    const uint64_t cu = XRT_CU_WINDOW_BASE;
    auto reg = reinterpret_cast<volatile uint32_t*>(a + cu);
    const int n = 20000;

    double rd = per_call(n, ^{ (void)reg[0]; });

    // The 17 words a hello command carries: read them back so the writes
    // below put back exactly what is there now.
    const int nargs = 17;
    std::vector<uint32_t> args(nargs);
    for (int i = 0; i < nargs; ++i)
      args[i] = reg[0x10 / 4 + i];
    uint64_t off = cu + 0x10;
    uint64_t off_nomap = off | XRT_CU_WRITE_NO_MAP;

    std::vector<uint64_t> scalar_vec(16, 0);
    uint64_t* scalars = scalar_vec.data();
    std::vector<uint64_t> sout_vec(1, 0);
    uint64_t* sout = sout_vec.data();
    double nop = per_call(n, ^{
      IOConnectCallScalarMethod(conn, XRT_DEXT_NOP, nullptr, 0, nullptr, nullptr);
    });
    double nop_s11 = per_call(n, ^{
      IOConnectCallScalarMethod(conn, XRT_DEXT_NOP, scalars, 11, nullptr, nullptr);
    });
    double nop_s16 = per_call(n, ^{
      IOConnectCallScalarMethod(conn, XRT_DEXT_NOP, scalars, 16, nullptr, nullptr);
    });
    double nop_sout = per_call(n, ^{
      uint32_t c = 1;
      IOConnectCallScalarMethod(conn, XRT_DEXT_NOP, nullptr, 0, sout, &c);
    });
    double nop_in4 = per_call(n, ^{
      IOConnectCallMethod(conn, XRT_DEXT_NOP, nullptr, 0, args.data(), 4,
                          nullptr, nullptr, nullptr, nullptr);
    });
    double nop_in68 = per_call(n, ^{
      IOConnectCallMethod(conn, XRT_DEXT_NOP, nullptr, 0, args.data(), nargs * 4,
                          nullptr, nullptr, nullptr, nullptr);
    });
    double w1_mmio = per_call(n, ^{
      IOConnectCallMethod(conn, XRT_DEXT_CU_WRITE, &off_nomap, 1, args.data(), 4,
                          nullptr, nullptr, nullptr, nullptr);
    });
    double wn_mmio = per_call(n, ^{
      IOConnectCallMethod(conn, XRT_DEXT_CU_WRITE, &off_nomap, 1, args.data(), nargs * 4,
                          nullptr, nullptr, nullptr, nullptr);
    });
    double w1_map = per_call(n, ^{
      IOConnectCallMethod(conn, XRT_DEXT_CU_WRITE, &off, 1, args.data(), 4,
                          nullptr, nullptr, nullptr, nullptr);
    });
    double wn_map = per_call(n, ^{
      IOConnectCallMethod(conn, XRT_DEXT_CU_WRITE, &off, 1, args.data(), nargs * 4,
                          nullptr, nullptr, nullptr, nullptr);
    });
    double sl = per_call(2000, ^{ std::this_thread::sleep_for(std::chrono::microseconds(20)); });
    // What a requested sleep actually costs. The CU wait's poll_min is 20 us,
    // and if the real floor is above that the constant is decorative - the
    // scheduler cannot poll faster than this however it is set.
    const int sleep_us[6] = { 1, 5, 10, 20, 50, 100 };
    double slept[6] = { 0 };
    for (int i = 0; i < 6; ++i) {
      const int us = sleep_us[i];
      slept[i] = per_call(1000, ^{ std::this_thread::sleep_for(std::chrono::microseconds(us)); });
    }

    // The CU window is the only place user space can make the dext write.
    // Offsets outside it must be refused, by both entry points.
    auto refused = [&](const char* what, kern_return_t kr) {
      std::printf("%-33s: %s\n", what, kr ? "refused" : "ACCEPTED - BUG");
    };
    uint64_t bad_lo[2] = { 0, 0 };
    uint64_t bad_hi[2] = { XRT_CU_WINDOW_END, 0 };
    // Four argument words starting at the last register: past the end.
    uint64_t bad_end[4] = { XRT_CU_WINDOW_END - 4, uint64_t(4) << 32, 0, 0 };
    uint64_t bad_wrap[2] = { ~3ULL, 0 };
    uint64_t bad_write = XRT_CU_WINDOW_END - 4;

    // What a struct input actually costs as it grows. The fixed part is
    // size-independent, but past the point where IOKit stops copying and hands
    // the dext a memory descriptor the curve changes - and that is the number
    // that decides whether passing a DMA buffer by handle is worth an ABI
    // change. Measured on NOP, so nothing but the marshalling is timed.
    struct { const char* name; size_t bytes; } sizes[] = {
      { "4 KiB",   4u << 10 },
      { "8 KiB",   8u << 10 },
      { "16 KiB",  16u << 10 },
      { "32 KiB",  32u << 10 },
      { "64 KiB",  64u << 10 },
      { "1 MiB",   1u << 20 },
      { "16 MiB",  16u << 20 },
    };
    std::vector<uint8_t> big(16u << 20, 0);
    uint8_t* bigp = big.data();   // a block captures big.data() as const
    double in_cost[7] = { 0 };
    double out_cost[7] = { 0 };
    for (int i = 0; i < 7; ++i) {
      size_t bytes = sizes[i].bytes;
      // Big buffers are slow enough that a thousand rounds is pointless.
      int rounds = bytes >= (1u << 20) ? 200 : n;
      in_cost[i] = per_call(rounds, ^{
        IOConnectCallMethod(conn, XRT_DEXT_NOP, nullptr, 0, bigp, bytes,
                            nullptr, nullptr, nullptr, nullptr);
      });
      out_cost[i] = per_call(rounds, ^{
        size_t osz = bytes;
        IOConnectCallMethod(conn, XRT_DEXT_NOP, nullptr, 0, nullptr, 0,
                            nullptr, nullptr, bigp, &osz);
      });
    }

    std::printf("empty call, no arguments         : %6.2f us\n", nop);
    std::printf("empty call, 11 scalars in        : %6.2f us\n", nop_s11);
    std::printf("empty call, 16 scalars in        : %6.2f us\n", nop_s16);
    std::printf("empty call, 1 scalar out         : %6.2f us\n", nop_sout);
    std::printf("empty call, 4-byte struct in     : %6.2f us\n", nop_in4);
    std::printf("empty call, %d-byte struct in    : %6.2f us\n", nargs * 4, nop_in68);
    for (int i = 0; i < 7; ++i)
      std::printf("empty call, %-6s struct in       : %6.2f us   (out: %6.2f us)\n",
                  sizes[i].name, in_cost[i], out_cost[i]);
    std::printf("CU_WRITE  1 word,  MemoryWrite32 : %6.2f us\n", w1_mmio);
    std::printf("CU_WRITE %2d words, MemoryWrite32 : %6.2f us\n", nargs, wn_mmio);
    std::printf("CU_WRITE  1 word,  dext mapping  : %6.2f us\n", w1_map);
    std::printf("CU_WRITE %2d words, dext mapping  : %6.2f us\n", nargs, wn_map);
    std::printf("MMIO read of CU register         : %6.2f us\n", rd);
    std::printf("sleep_for(20 us) actually        : %6.2f us\n", sl);
    for (int i = 0; i < 6; ++i)
      std::printf("sleep_for(%3d us) actually       : %6.2f us\n", sleep_us[i], slept[i]);

    refused("CU_EXEC below the CU window",
            IOConnectCallScalarMethod(conn, XRT_DEXT_CU_EXEC, bad_lo, 2, nullptr, nullptr));
    refused("CU_EXEC at the end of the window",
            IOConnectCallScalarMethod(conn, XRT_DEXT_CU_EXEC, bad_hi, 2, nullptr, nullptr));
    refused("CU_EXEC with args past the end",
            IOConnectCallScalarMethod(conn, XRT_DEXT_CU_EXEC, bad_end, 4, nullptr, nullptr));
    refused("CU_EXEC with a wrapping base",
            IOConnectCallScalarMethod(conn, XRT_DEXT_CU_EXEC, bad_wrap, 2, nullptr, nullptr));
    refused("CU_WRITE past the end of the window",
            IOConnectCallMethod(conn, XRT_DEXT_CU_WRITE, &bad_write, 1, args.data(), nargs * 4,
                                nullptr, nullptr, nullptr, nullptr));

    IOConnectUnmapMemory64(conn, 0, mach_task_self(), a);
    IOServiceClose(conn);
  }
  return 0;
}
