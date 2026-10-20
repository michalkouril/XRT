#!/bin/bash
# Build, ad-hoc sign and (optionally) activate the xrt_user dext.
# Usage: [CONFIG=Release] ./build.sh [activate]
set -e
export PATH=/opt/homebrew/bin:$PATH
cd "$(dirname "$0")"
# XRT source tree (this file lives in src/runtime_src/core/pcie/driver/macos)
XRT_RUNTIME_SRC="$(cd ../../../.. && pwd)"
xcodegen generate -q
CONFIG=${CONFIG:-Debug}
xcodebuild -project XrtMac.xcodeproj -scheme XrtLoader -configuration $CONFIG \
  -derivedDataPath build CODE_SIGNING_ALLOWED=NO build > xcb.log 2>&1 \
  || { grep -E "error:" xcb.log | sort -u; exit 1; }
APP=build/Build/Products/$CONFIG/XrtLoader.app
DEXT=$APP/Contents/Library/SystemExtensions/org.xrtmac.XrtUser.dext
codesign -f -s - --entitlements XrtUser/XrtUser.entitlements "$DEXT"
for d in "$APP"/Contents/MacOS/*.dylib; do [ -e "$d" ] && codesign -f -s - "$d"; done
codesign -f -s - --entitlements Loader/Loader.entitlements "$APP"
clang++ -std=c++17 -O2 -I$XRT_RUNTIME_SRC/core/include -framework IOKit -framework CoreFoundation tools/xrt_dext_probe.cpp -o build/xrt_dext_probe
clang++ -std=c++17 -O2 -I$XRT_RUNTIME_SRC/core/include -I$XRT_RUNTIME_SRC -framework IOKit -framework CoreFoundation tools/xrt_dext_load.cpp -o build/xrt_dext_load
clang++ -std=c++17 -O2 -framework IOKit -framework CoreFoundation tools/xrt_dext_dma.cpp -o build/xrt_dext_dma
echo "built: $(lipo -archs "$DEXT/org.xrtmac.XrtUser")"
if [ "$1" = activate ]; then
  OLD=$(pgrep -f "org.xrtmac.XrtUser.dext/org.xrtmac.XrtUser" || true)
  "$APP/Contents/MacOS/XrtLoader" activate
  # A running dext can hang in "terminating for upgrade". Kill the old
  # instance so the kernel rematches the card with the new version.
  if [ -n "$OLD" ]; then
    sleep 2
    for p in $OLD; do sudo -n kill "$p" 2>/dev/null && echo "killed old dext pid $p"; done
    for i in $(seq 1 10); do
      NEW=$(pgrep -f "org.xrtmac.XrtUser.dext/org.xrtmac.XrtUser" || true)
      [ -n "$NEW" ] && [ "$NEW" != "$OLD" ] && { echo "new dext pid $NEW"; break; }
      sleep 1
    done
  fi
  systemextensionsctl list | grep org.xrtmac
fi
