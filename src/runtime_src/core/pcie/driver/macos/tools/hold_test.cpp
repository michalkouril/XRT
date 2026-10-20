// SPDX-License-Identifier: Apache-2.0
// Tests the partition hold that keeps processes off the user partition while
// it is reprogrammed (XRT_DEXT_PARTITION_HOLD).
//
// Nothing here changes the card: the only write attempted is a CU_WRITE that
// puts back the value already in the hello_1 CU's first argument register,
// and while the hold is on it is refused before reaching the hardware.
#include <IOKit/IOKitLib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include "../XrtUser/xrt_dext_abi.h"

static int failures = 0;

static const char*
name_of(kern_return_t kr)
{
  switch (kr) {
  case kIOReturnSuccess:       return "success";
  case kIOReturnBusy:          return "busy";
  case kIOReturnNotPermitted:  return "not permitted";
  case kIOReturnOffline:       return "offline";
  case kIOReturnBadArgument:   return "bad argument";
  default:                     return "other";
  }
}

static void
check(const char* what, kern_return_t got, kern_return_t want)
{
  bool ok = got == want;
  if (!ok)
    ++failures;
  std::printf("%-46s %-14s %s\n", what, name_of(got), ok ? "ok" : "FAIL");
  if (!ok)
    std::printf("%50s expected %s (0x%x), got 0x%x\n", "", name_of(want), want, got);
}

// Open the user function's dext. Returns 0 if it is not there.
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

static kern_return_t
hold(io_connect_t c)    { return IOConnectCallScalarMethod(c, XRT_DEXT_PARTITION_HOLD, nullptr, 0, nullptr, nullptr); }
static kern_return_t
release(io_connect_t c) { return IOConnectCallScalarMethod(c, XRT_DEXT_PARTITION_RELEASE, nullptr, 0, nullptr, nullptr); }

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
  std::printf("dext ABI %u\n\n", info.abi_version);

  // Read the CU's first argument register so every write below puts back the
  // value that is already there.
  mach_vm_address_t addr = 0;
  mach_vm_size_t len = 0;
  if (IOConnectMapMemory64(conn, 0, mach_task_self(), &addr, &len, kIOMapAnywhere | kIOMapInhibitCache)) {
    std::fprintf(stderr, "cannot map user BAR0\n");
    return 2;
  }
  // Not the compile-time base: since the platform table landed the dext moves
  // the CU window to wherever the loaded xclbin puts its CUs, so writing at the
  // XDMA 5.x default is refused on any card that moved it - a U50 puts it at
  // 0x1400000. Ask the dext where it is.
  uint64_t cu_win = XRT_CU_WINDOW_BASE;
  {
    xrt_dext_platform plat = {};
    size_t psz = sizeof(plat);
    if (IOConnectCallStructMethod(conn, XRT_DEXT_GET_PLATFORM, nullptr, 0, &plat, &psz) == KERN_SUCCESS
        && plat.cu_window_size)
      cu_win = plat.cu_window_offset;
    std::printf("CU window 0x%llx\n", (unsigned long long)cu_win);
  }
  const uint64_t arg_off = cu_win + 0x10;
  uint32_t arg = reinterpret_cast<volatile uint32_t*>(addr + arg_off)[0];
  auto cu_write = [&]() {
    uint64_t off = arg_off;
    return IOConnectCallMethod(conn, XRT_DEXT_CU_WRITE, &off, 1, &arg, sizeof(arg),
                               nullptr, nullptr, nullptr, nullptr);
  };
  // The self-test reports its status inside the result struct. It is only
  // ever called here while the hold is on, so it is refused before touching
  // the engines and no data moves.
  auto dma_status = [&]() {
    uint64_t in[3] = { XRT_DMA_CARD_MIN, 4096, 1 };
    xrt_dext_dma_result res{};
    size_t rsz = sizeof(res);
    if (IOConnectCallMethod(conn, XRT_DEXT_DMA_SELFTEST, in, 3, nullptr, 0,
                            nullptr, nullptr, &res, &rsz) != KERN_SUCCESS)
      return kIOReturnError;
    return static_cast<kern_return_t>(res.status);
  };

  check("CU write, partition free", cu_write(), kIOReturnSuccess);
  check("take the hold", hold(conn), kIOReturnSuccess);
  check("take it again, same connection", hold(conn), kIOReturnSuccess);
  check("CU write while held", cu_write(), kIOReturnBusy);
  check("DMA self-test while held", dma_status(), kIOReturnBusy);
  check("release the hold", release(conn), kIOReturnSuccess);
  check("release again", release(conn), kIOReturnSuccess);
  check("CU write after release", cu_write(), kIOReturnSuccess);

  // A second process holding it locks this one out.
  int pipefd[2];
  if (pipe(pipefd)) { std::perror("pipe"); return 2; }
  pid_t child = fork();
  if (child == 0) {
    close(pipefd[0]);
    io_connect_t c = open_user();
    kern_return_t kr = c ? hold(c) : kIOReturnNoDevice;
    ssize_t n = write(pipefd[1], &kr, sizeof(kr));
    (void)n;
    pause();                       // hold it until killed, never releasing
    _exit(0);
  }
  close(pipefd[1]);
  kern_return_t child_kr = kIOReturnError;
  if (read(pipefd[0], &child_kr, sizeof(child_kr)) != sizeof(child_kr))
    child_kr = kIOReturnError;
  check("another process takes the hold", child_kr, kIOReturnSuccess);
  check("our hold refused while it holds", hold(conn), kIOReturnBusy);
  check("CU write refused while it holds", cu_write(), kIOReturnBusy);

  // Killing it without a release must drop the hold, or a process that dies
  // part way through a download would leave the card refusing work.
  kill(child, SIGKILL);
  int status = 0;
  waitpid(child, &status, 0);
  for (int i = 0; i < 100 && cu_write() == kIOReturnBusy; ++i)
    usleep(10000);                 // the client teardown is asynchronous
  check("CU write after the holder was killed", cu_write(), kIOReturnSuccess);
  check("hold available again", hold(conn), kIOReturnSuccess);
  check("final release", release(conn), kIOReturnSuccess);

  close(pipefd[0]);
  IOConnectUnmapMemory64(conn, 0, mach_task_self(), addr);
  IOServiceClose(conn);
  std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
  return failures ? 1 : 0;
}
