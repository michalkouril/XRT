// SPDX-License-Identifier: Apache-2.0
// Start the board controller, so the card has sensors.
//
// The controller runs from RAM loaded by the host, so this is needed once per
// power cycle. Nothing is flashed: a bad image costs a reset, not the card.
//
// usage: cmc_load [image.bin]
//   default image: the one in the U50 platform package
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../XrtUser/xrt_dext_abi.h"

namespace {

const char* kDefaultImage =
  "/opt/xilinx/firmware/u50/gen3x16-xdma/base/firmware/cmc-u50/cmc-u50-1.0.40.bin";

const char*
step_name(uint32_t s)
{
  switch (s) {
  case XRT_CMC_STEP_CHECK:      return "check";
  case XRT_CMC_STEP_HOLD_RESET: return "hold reset";
  case XRT_CMC_STEP_COPY:       return "copy image";
  case XRT_CMC_STEP_RELEASE:    return "release reset";
  case XRT_CMC_STEP_WAIT_MAGIC: return "wait for the register map";
  case XRT_CMC_STEP_WAIT_READY: return "wait for sensors";
  case XRT_CMC_STEP_COMPLETE:   return "complete";
  default:                      return "?";
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
  const char* path = argc > 1 ? argv[1] : kDefaultImage;

  std::vector<char> image;
  if (FILE* f = std::fopen(path, "rb")) {
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
      image.insert(image.end(), buf, buf + n);
    std::fclose(f);
  }
  if (image.empty()) {
    std::fprintf(stderr, "cannot read %s\n", path);
    return 2;
  }
  image.resize((image.size() + 3) & ~size_t(3), 0);
  std::printf("image %s, %zu bytes\n", path, image.size());

  io_iterator_t it = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching(XRT_DEXT_USER_CLASS), &it) != KERN_SUCCESS) {
    std::fprintf(stderr, "no %s service\n", XRT_DEXT_USER_CLASS);
    return 1;
  }
  int rc = 1;
  while (io_service_t svc = IOIteratorNext(it)) {
    unsigned bus = 0, dev = 0, func = 0;
    if (parent_bdf(svc, bus, dev, func) && func == 0) {
      io_connect_t conn = IO_OBJECT_NULL;
      if (IOServiceOpen(svc, mach_task_self(), 0, &conn) == KERN_SUCCESS) {
        xrt_dext_cmc_result res = {};
        size_t rsz = sizeof(res);
        kern_return_t kr = IOConnectCallStructMethod(conn, XRT_DEXT_LOAD_CMC,
                                                    image.data(), image.size(), &res, &rsz);
        std::printf("%02x:%02x.%x call 0x%x, result 0x%x, reached '%s'\n",
                    bus, dev, func, kr, res.status, step_name(res.step));
        if (res.was_running)
          std::printf("  it was already running; nothing was written\n");
        std::printf("  copied %u words, reset GPIO after 0x%x, %u ms\n",
                    res.words_copied, res.gpio_after, res.elapsed_ms);
        std::printf("  magic 0x%08x  version 0x%08x  status 0x%08x\n",
                    res.magic, res.version, res.cmc_status);
        std::printf("  error 0x%08x  feature 0x%08x  regmap ready 0x%08x\n",
                    res.error, res.feature, res.ready);
        rc = (kr == KERN_SUCCESS && res.status == 0) ? 0 : 1;
        IOServiceClose(conn);
      }
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  return rc;
}
