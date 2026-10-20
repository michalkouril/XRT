// SPDX-License-Identifier: Apache-2.0
// macOS system information for xbutil examine --report host.
#ifndef core_common_detail_darwin_sysinfo_h
#define core_common_detail_darwin_sysinfo_h

#include "core/common/error.h"
#include <boost/property_tree/ptree.hpp>
#include <boost/format.hpp>
#include <cerrno>
#include <string>
#include <thread>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace {

static std::string
sysctl_string(const char* name)
{
  size_t len = 0;
  if (::sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0)
    return "unknown";
  std::string val(len, '\0');
  if (::sysctlbyname(name, val.data(), &len, nullptr, 0) != 0)
    return "unknown";
  while (!val.empty() && (val.back() == '\0' || val.back() == '\n'))
    val.pop_back();
  return val;
}

static uint64_t
sysctl_u64(const char* name)
{
  uint64_t val = 0;
  size_t len = sizeof(val);
  if (::sysctlbyname(name, &val, &len, nullptr, 0) != 0)
    return 0;
  return val;
}

} // namespace

namespace xrt_core::sysinfo::detail {

void
get_os_info(boost::property_tree::ptree& pt)
{
  struct utsname sysinfo {};
  if (!uname(&sysinfo)) {
    pt.put("sysname",   sysinfo.sysname);
    pt.put("release",   sysinfo.release);
    pt.put("version",   sysinfo.version);
    pt.put("machine",   sysinfo.machine);
  }
  pt.put("distribution", "macOS " + sysctl_string("kern.osproductversion"));
  pt.put("bios_vendor", "Apple");
  pt.put("bios_version", sysctl_string("kern.osversion"));
  pt.put("model", sysctl_string("hw.model"));
  pt.put("cores", std::thread::hardware_concurrency());
  pt.put("memory_bytes", (boost::format("0x%llx") % sysctl_u64("hw.memsize")).str());
  char hostname[256] = {0};
  gethostname(hostname, sizeof(hostname));
  pt.put("hostname", std::string(hostname));
  pt.put("processor", sysctl_string("machdep.cpu.brand_string"));
}

std::pair<unsigned int, unsigned int>
get_terminal_size()
{
  struct winsize ws {};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 || ws.ws_col == 0)
    throw xrt_core::error(errno, "Unable to determine terminal size");
  return {static_cast<unsigned int>(ws.ws_row), static_cast<unsigned int>(ws.ws_col)};
}

bool
is_advanced()
{
  return false;
}

} // xrt_core::sysinfo::detail

#endif
