#!/bin/sh
# Rebuild every diagnostic tool in this directory into ~/xrt-tests.
#
# This exists because xrt_dext_abi.h changes often, and a tool built against an
# older ABI does not fail - it silently reads the wrong selector or the wrong
# struct offsets. A stale fw_peek once reported all four firewalls tripped on a
# healthy card. So: after any ABI change, run this.
#
# Each tool is built to a temporary name and moved into place only on success:
# clang truncates its output on failure, so compiling straight over a working
# binary destroys it when the build breaks.
#
# cl_hello is excluded: it needs the OpenCL ICD headers and is built separately.
# pcidev_probe needs xbflash.qspi's pcidev_macos.cpp linked in.
set -e
here=$(cd "$(dirname "$0")" && pwd)
dext=$(cd "$here/../XrtUser" && pwd)
inc=$(cd "$here/../../../../include" && pwd)
qspi=$(cd "$here/../../../tools/xbflash.qspi" && pwd)
out=${1:-$HOME/xrt-tests}
mkdir -p "$out"

CXXFLAGS="-std=c++17 -O2 -Wall -I$dext -I$inc -I/opt/xilinx/xrt/include"
# Tools that reach the boot flash open the entitlement-gated flash user client, so
# they have to be signed with org.xrtmac.flash-access or IOServiceOpen fails. The
# signature is ad-hoc, which is all this development setup uses.
FLASH_ENTS="$here/flash_client.entitlements"
sign_flash() {
  codesign --force --sign - --entitlements "$FLASH_ENTS" "$1" 2>/dev/null \
    || { echo "FAILED to sign $1 with the flash entitlement"; return 1; }
}
LDFLAGS="-framework IOKit -framework CoreFoundation"
# Some tools drive the card through the XRT C++ API rather than the dext
# directly, so they need the installed runtime. Rather than keep two lists that
# drift, build without it and retry with it: the ones that do not need it link
# either way.
XRTLIBS="-L/opt/xilinx/xrt/lib -lxrt_coreutil"

ok=0; bad=0
for src in "$here"/*.cpp; do
  name=$(basename "$src" .cpp)
  case $name in
    cl_hello) echo "skip $name (needs OpenCL ICD headers)"; continue ;;
    pcidev_probe) extra="$qspi/pcidev_macos.cpp"; extra_inc="-I$qspi" ;;
    *) extra=""; extra_inc="" ;;
  esac
  # shellcheck disable=SC2086
  if clang++ $CXXFLAGS $extra_inc -o "$out/$name.new" "$src" $extra $LDFLAGS 2>"$out/$name.buildlog" \
     || clang++ $CXXFLAGS $extra_inc -o "$out/$name.new" "$src" $extra $LDFLAGS $XRTLIBS 2>"$out/$name.buildlog"; then
    # Sign before the move, so a signing failure leaves the old binary in place.
    # $extra matters as much as $src: pcidev_probe's flash open lives in the
    # xbflash.qspi source it links, not in its own file.
    if grep -q 'XRT_DEXT_CLIENT_FLASH' "$src" $extra; then
      sign_flash "$out/$name.new" || { rm -f "$out/$name.new"; bad=$((bad+1)); continue; }
    fi
    mv "$out/$name.new" "$out/$name"
    rm -f "$out/$name.buildlog"
    ok=$((ok+1))
  else
    rm -f "$out/$name.new"
    echo "FAILED $name (see $out/$name.buildlog)"
    bad=$((bad+1))
  fi
done
"$here/build_xbflash.sh" "$out/xbflash.qspi" >/dev/null && ok=$((ok+1)) || bad=$((bad+1))
echo "$ok built, $bad failed"
[ "$bad" -eq 0 ]
