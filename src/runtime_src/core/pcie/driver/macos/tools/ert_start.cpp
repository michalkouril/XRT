// SPDX-License-Identifier: Apache-2.0
// Bring the scheduler MicroBlaze up and prove it is executing.
//
// Loading the image only shows the code RAM took the writes. The proof is the
// configure command: written into queue slot 0, doorbell rung, and read back
// with its state set to COMPLETED by the MicroBlaze. Nothing is flashed - the
// image lives in RAM, so a bad one costs a reset.
//
// usage: ert_start [sched.bin] [cmc.bin] [gpio-release-value]
//
// The release value defaults to 1, which is what xocl writes. On this shell that
// releases the board controller and apparently leaves the scheduler held, so the
// register looks like one bit per MicroBlaze - try 3.
//
// Both MicroBlazes share one reset on these shells, so xocl copies both images
// before releasing it. Passing the controller image too tests whether releasing
// it onto whatever its RAM holds is what stops the scheduler.
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>
#include "../XrtUser/xrt_dext_abi.h"

namespace {

const char* kDefault = "/opt/xilinx/xrt/share/fw/sched.bin";
const char* kDefaultCmc = "/opt/xilinx/xrt/share/fw/cmc.bin";

std::vector<char>
read_file(const char* path)
{
  std::vector<char> out;
  if (FILE* f = std::fopen(path, "rb")) {
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
      out.insert(out.end(), buf, buf + n);
    std::fclose(f);
  }
  out.resize((out.size() + 3) & ~size_t(3), 0);
  return out;
}

const char*
step_name(uint32_t s)
{
  switch (s) {
  case XRT_ERT_STEP_CHECK:       return "check";
  case XRT_ERT_STEP_HOLD_RESET:  return "hold reset";
  case XRT_ERT_STEP_COPY:        return "copy image";
  case XRT_ERT_STEP_RELEASE:     return "release reset";
  case XRT_ERT_STEP_CONFIGURE:   return "write configure";
  case XRT_ERT_STEP_WAIT_CONFIG: return "wait for configure";
  case XRT_ERT_STEP_COMPLETE:    return "complete";
  default:                       return "?";
  }
}

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

} // namespace

int
main(int argc, char** argv)
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char* path = argc > 1 ? argv[1] : kDefault;
  const char* cmc_path = argc > 2 ? argv[2] : kDefaultCmc;
  auto sched = read_file(path);
  auto cmc = read_file(cmc_path);
  if (sched.empty()) {
    std::fprintf(stderr, "cannot read %s\n", path);
    return 2;
  }
  std::printf("scheduler %s, %zu bytes\n", path, sched.size());
  if (cmc.empty())
    std::printf("no controller image at %s: the scheduler goes up alone\n", cmc_path);
  else
    std::printf("controller %s, %zu bytes\n", cmc_path, cmc.size());

  // header, then the scheduler image, then the controller's
  uint32_t gpio = argc > 3 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0)) : 0;
  if (gpio)
    std::printf("releasing the reset with 0x%x instead of 1\n", gpio);
  xrt_dext_ert_images h{ static_cast<uint32_t>(sched.size()),
                         static_cast<uint32_t>(cmc.size()), gpio, 0 };
  std::vector<char> image(sizeof(h));
  std::memcpy(image.data(), &h, sizeof(h));
  image.insert(image.end(), sched.begin(), sched.end());
  image.insert(image.end(), cmc.begin(), cmc.end());

  io_iterator_t it = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it) != KERN_SUCCESS)
    return 1;
  int rc = 1;
  while (io_service_t svc = IOIteratorNext(it)) {
    unsigned bus = 0, dev = 0, func = 0;
    if (parent_bdf(svc, bus, dev, func) && func == 0) {
      io_connect_t conn = IO_OBJECT_NULL;
      if (IOServiceOpen(svc, mach_task_self(), 0, &conn) == KERN_SUCCESS) {
        xrt_dext_ert_result res = {};
        size_t rsz = sizeof(res);
        kern_return_t kr = IOConnectCallStructMethod(conn, XRT_DEXT_ERT_START,
                                                    image.data(), image.size(), &res, &rsz);
        std::printf("%02x:%02x.%x call 0x%x, result 0x%x, reached '%s'\n",
                    bus, dev, func, kr, res.status, step_name(res.step));
        std::printf("  before: %s\n", res.was_self_jump
                    ? "code RAM held the cold-boot self jump, so no firmware was running"
                    : "code RAM held something other than a self jump");
        std::printf("  copied %u scheduler words and %u controller words;"
                    " code RAM word 0 now 0x%08x, reset GPIO 0x%x\n",
                    res.words_copied, res.cmc_words_copied, res.image_word0, res.gpio_after);
        std::printf("  configure: slot 0 header 0x%08x, state %u %s, %u ms\n",
                    res.config_header, res.config_state,
                    res.config_state == XRT_ERT_STATE_COMPLETED
                      ? "= COMPLETED: the MicroBlaze is executing" : "(not completed)",
                    res.config_ms);
        std::printf("  CSR status 0x%08x, CU status 0x%08x, %u ms total\n",
                    res.csr_status, res.cu_status, res.elapsed_ms);
        rc = (kr == KERN_SUCCESS && res.status == 0) ? 0 : 1;
        IOServiceClose(conn);
      }
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  return rc;
}
