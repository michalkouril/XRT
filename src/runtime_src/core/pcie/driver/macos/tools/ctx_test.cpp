// SPDX-License-Identifier: Apache-2.0
// Tests the CU context arbitration the dext keeps across processes
// (XRT_DEXT_CU_CTX_OPEN / _CLOSE): an exclusive context excludes every other
// connection, a shared one excludes only exclusive ones, and a context dies
// with the connection that opened it.
//
// Contexts are bookkeeping in the dext, so this touches no hardware and needs
// no xclbin loaded.
#include <IOKit/IOKitLib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include "../XrtUser/xrt_dext_abi.h"

static int failures = 0;

static const char*
name_of(kern_return_t kr)
{
  switch (kr) {
  case kIOReturnSuccess:         return "success";
  case kIOReturnExclusiveAccess: return "refused";
  case kIOReturnNotPermitted:    return "not permitted";
  case kIOReturnNotFound:        return "not found";
  case kIOReturnBusy:            return "busy";
  case kIOReturnUnsupported:     return "unsupported";
  case kIOReturnBadArgument:     return "bad argument";
  default:                       return "other";
  }
}

static void
check(const char* what, kern_return_t got, kern_return_t want)
{
  bool ok = got == want;
  if (!ok)
    ++failures;
  std::printf("%-52s %-14s %s\n", what, name_of(got), ok ? "ok" : "FAIL");
  if (!ok)
    std::printf("%56s expected %s (0x%x), got 0x%x\n", "", name_of(want), want, got);
}

static io_connect_t
open_user()
{
  io_iterator_t it = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it))
    return 0;
  io_connect_t found = 0;
  while (io_service_t svc = IOIteratorNext(it)) {
    io_connect_t conn = 0;
    if (!found && IOServiceOpen(svc, mach_task_self(), 0, &conn) == KERN_SUCCESS) {
      xrt_dext_device_info info{};
      size_t sz = sizeof(info);
      // A discarded status leaves info zeroed, which would read as func 0.
      if (!IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &sz)
          && info.func == 1)
        found = conn;
      else
        IOServiceClose(conn);
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  return found;
}

// The CU window starts page aligned, so base + cu * page keeps every test CU
// on its own page, which is what a read-write mapping needs.
//
// The base is NOT the compile-time default. Since the platform table landed the
// dext moves the window to wherever the loaded xclbin puts its CUs - 0x1400000
// on a U50 against the XDMA 5.x default of 0x1800000 - so a test that assumes
// the constant fails on any card that moved it, which is what happened on the
// U50. Ask the dext instead, and fall back to the default if it cannot say.
static uint64_t g_cu_window_base = XRT_CU_WINDOW_BASE;
static uint64_t g_cu_window_size = XRT_CU_WINDOW_END - XRT_CU_WINDOW_BASE;

static void
learn_cu_window(io_connect_t conn)
{
  xrt_dext_platform plat = {};
  size_t sz = sizeof(plat);
  if (IOConnectCallStructMethod(conn, XRT_DEXT_GET_PLATFORM, nullptr, 0, &plat, &sz) == KERN_SUCCESS
      && plat.cu_window_size) {
    g_cu_window_base = plat.cu_window_offset;
    g_cu_window_size = plat.cu_window_size;
  }
  std::printf("CU window 0x%llx + 0x%llx, %llu page(s) of 0x%x\n",
              (unsigned long long)g_cu_window_base, (unsigned long long)g_cu_window_size,
              (unsigned long long)(g_cu_window_size / XRT_CU_RW_PAGE), XRT_CU_RW_PAGE);
}

static uint64_t
cu_base(uint32_t cu)
{
  return g_cu_window_base + uint64_t{cu} * XRT_CU_RW_PAGE;
}

static kern_return_t
ctx_open_at(io_connect_t c, uint32_t cu, uint32_t shared, uint64_t base)
{
  uint64_t in[3] = { cu, shared, base };
  return IOConnectCallScalarMethod(c, XRT_DEXT_CU_CTX_OPEN, in, 3, nullptr, nullptr);
}

static kern_return_t
ctx_open(io_connect_t c, uint32_t cu, uint32_t shared)
{
  return ctx_open_at(c, cu, shared, cu_base(cu));
}

// IOConnectMapMemory64 does not pass the driver's status through: whatever
// CopyClientMemoryForType returns, a refused mapping surfaces as
// kIOReturnBadArgument. So the mapping checks assert refused or granted, not a
// particular code.
static void
check_refused(const char* what, kern_return_t kr)
{
  bool ok = kr != KERN_SUCCESS;
  if (!ok)
    ++failures;
  std::printf("%-52s %-14s %s\n", what, ok ? "refused" : "GRANTED", ok ? "ok" : "FAIL");
}

static void
check_granted(const char* what, kern_return_t kr)
{
  bool ok = kr == KERN_SUCCESS;
  if (!ok)
    ++failures;
  std::printf("%-52s %-14s %s\n", what, ok ? "granted" : name_of(kr), ok ? "ok" : "FAIL");
}

// Try to map a CU's register page read-write. Returns the mapping result;
// unmaps again on success so the test leaves nothing behind.
static kern_return_t
try_map_rw(io_connect_t c, uint32_t cu)
{
  mach_vm_address_t addr = 0;
  mach_vm_size_t len = 0;
  kern_return_t kr = IOConnectMapMemory64(c, XRT_DEXT_MAP_CU_RW(cu), mach_task_self(),
                                          &addr, &len, kIOMapAnywhere | kIOMapInhibitCache);
  if (kr == KERN_SUCCESS)
    IOConnectUnmapMemory64(c, XRT_DEXT_MAP_CU_RW(cu), mach_task_self(), addr);
  return kr;
}

static kern_return_t
ctx_close(io_connect_t c, uint32_t cu)
{
  uint64_t in = cu;
  return IOConnectCallScalarMethod(c, XRT_DEXT_CU_CTX_CLOSE, &in, 1, nullptr, nullptr);
}

// Run a child that opens cu in the given mode, reports the result, and keeps
// the context until it is killed. Returns its pid, or -1.
static pid_t
holder_at(uint32_t cu, uint32_t shared, uint64_t base, kern_return_t* result)
{
  int fds[2];
  if (pipe(fds))
    return -1;
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    io_connect_t c = open_user();
    kern_return_t kr = c ? ctx_open_at(c, cu, shared, base) : kIOReturnNoDevice;
    ssize_t n = write(fds[1], &kr, sizeof(kr));
    (void)n;
    pause();
    _exit(0);
  }
  close(fds[1]);
  if (read(fds[0], result, sizeof(*result)) != sizeof(*result))
    *result = kIOReturnError;
  close(fds[0]);
  return pid;
}

static pid_t
holder(uint32_t cu, uint32_t shared, kern_return_t* result)
{
  return holder_at(cu, shared, cu_base(cu), result);
}

static void
reap(pid_t pid)
{
  kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
  usleep(200000);            // the client teardown is asynchronous
}

int
main()
{
  io_connect_t conn = open_user();
  if (!conn) {
    std::fprintf(stderr, "xrt_user dext not attached to the user function\n");
    return 2;
  }
  xrt_dext_device_info info{};
  size_t sz = sizeof(info);
  IOConnectCallStructMethod(conn, XRT_DEXT_GET_DEVICE_INFO, nullptr, 0, &info, &sz);
  std::printf("dext ABI %u\n", info.abi_version);
  learn_cu_window(conn);
  std::printf("\n");

  const uint32_t cu = 0, sh = XRT_CU_CTX_SHARED, ex = XRT_CU_CTX_EXCLUSIVE;

  std::printf("-- one connection --\n");
  check("open shared", ctx_open(conn, cu, sh), kIOReturnSuccess);
  check("open shared again (counted)", ctx_open(conn, cu, sh), kIOReturnSuccess);
  check("open exclusive while shared here", ctx_open(conn, cu, ex), kIOReturnNotPermitted);
  check("close once", ctx_close(conn, cu), kIOReturnSuccess);
  check("close twice", ctx_close(conn, cu), kIOReturnSuccess);
  check("close when none left", ctx_close(conn, cu), kIOReturnNotFound);
  check("open out of range CU", ctx_open(conn, XRT_CU_MAX, sh), kIOReturnBadArgument);

  std::printf("\n-- shared here, another process --\n");
  check("open shared here", ctx_open(conn, cu, sh), kIOReturnSuccess);
  kern_return_t kr = kIOReturnError;
  pid_t pid = holder(cu, sh, &kr);
  check("it opens shared too", kr, kIOReturnSuccess);
  reap(pid);
  pid = holder(cu, ex, &kr);
  check("it is refused exclusive", kr, kIOReturnExclusiveAccess);
  reap(pid);
  check("close ours", ctx_close(conn, cu), kIOReturnSuccess);

  std::printf("\n-- exclusive elsewhere --\n");
  pid = holder(cu, ex, &kr);
  check("another process opens exclusive", kr, kIOReturnSuccess);
  check("we are refused shared", ctx_open(conn, cu, sh), kIOReturnExclusiveAccess);
  check("we are refused exclusive", ctx_open(conn, cu, ex), kIOReturnExclusiveAccess);
  check("a different CU is free", ctx_open(conn, cu + 1, sh), kIOReturnSuccess);
  check("close the different CU", ctx_close(conn, cu + 1), kIOReturnSuccess);

  std::printf("\n-- the holder dies --\n");
  reap(pid);
  check("exclusive is free again", ctx_open(conn, cu, ex), kIOReturnSuccess);
  check("close it", ctx_close(conn, cu), kIOReturnSuccess);

  // A read-write mapping of a CU's page is only for a connection holding that
  // CU exclusively. Without that the dext must refuse it, or any process could
  // write another's compute unit at will.
  std::printf("\n-- read-write page mapping --\n");
  check_refused("map with no context at all", try_map_rw(conn, cu));
  check("open shared", ctx_open(conn, cu, sh), kIOReturnSuccess);
  check_refused("map with only a shared context", try_map_rw(conn, cu));
  check("close shared", ctx_close(conn, cu), kIOReturnSuccess);
  check("open exclusive", ctx_open(conn, cu, ex), kIOReturnSuccess);
  check_granted("map with an exclusive context", try_map_rw(conn, cu));
  check("close exclusive", ctx_close(conn, cu), kIOReturnSuccess);
  check_refused("a fresh map after closing the context", try_map_rw(conn, cu));

  // A page another connection has a context on must not be handed over, even
  // to the exclusive holder of a different CU in that page. CU 7 is given a
  // base inside CU 0's page to set that up.
  pid = holder_at(7, sh, cu_base(cu) + 0x40, &kr);
  check("another process holds a CU in that page", kr, kIOReturnSuccess);
  check("we hold our own CU exclusively", ctx_open(conn, cu, ex), kIOReturnSuccess);
  check_refused("map while another holds a CU in that page", try_map_rw(conn, cu));
  reap(pid);
  check_granted("granted once that process is gone", try_map_rw(conn, cu));
  check("close ours", ctx_close(conn, cu), kIOReturnSuccess);

  IOServiceClose(conn);
  std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
  return failures ? 1 : 0;
}
