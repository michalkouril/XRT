#!/bin/sh
# Build xbflash.qspi for macOS.
#
# The upstream tool is not part of the macOS package build: it is here because
# its flash programming path (xspi.cpp) is what we want to reuse rather than
# reimplement, reached through pcidev_macos.cpp and the dext's bounded register
# accessor. When that settles it should move into the CMake build.
#
# xspi.cpp needs C++14 semantics, not 17 (see HANDOFF item on std::ptr_fun).
set -e
src=$(cd "$(dirname "$0")/../../../tools/xbflash.qspi" && pwd)
dext=$(cd "$(dirname "$0")/../XrtUser" && pwd)
out=${1:-$HOME/xrt-tests/xbflash.qspi}
# Build to a temporary name first: clang truncates its output on failure, so
# writing straight to $out destroys a working binary when the build breaks.
clang++ -std=c++14 -O2 -Wall -o "$out.new" \
  "$src/main.cpp" "$src/xspi.cpp" "$src/xqspips.cpp" "$src/firmware_image.cpp" \
  "$src/pcidev_macos.cpp" "$src/pcidev.cpp" \
  -I"$src" -I"$dext" -framework IOKit -framework CoreFoundation
# It opens the entitlement-gated flash user client, so it must carry
# org.xrtmac.flash-access or IOServiceOpen fails with kIOReturnNotPermitted.
codesign --force --sign - \
  --entitlements "$(dirname "$0")/flash_client.entitlements" "$out.new"
mv "$out.new" "$out"
echo "built $out"
