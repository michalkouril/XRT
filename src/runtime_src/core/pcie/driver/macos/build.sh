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

VER=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$DEXT/Info.plist")
echo "built: $(lipo -archs "$DEXT/org.xrtmac.XrtUser")  v$VER  $CONFIG"
# A lower version than the one installed is one macOS can refuse to stage - it
# would leave the old driver running while this script reports success. Say so
# rather than let it pass silently.
INSTALLED=$(systemextensionsctl list 2>/dev/null | sed -n 's/.*org.xrtmac.XrtUser (\([^)]*\)).*/\1/p' | sed 's|.*/||' | head -1)
if [ -n "$INSTALLED" ] && [ "$INSTALLED" != "$VER" ]; then
  LOWER=$(printf '%s\n%s\n' "$INSTALLED" "$VER" | sort -V | head -1)
  if [ "$LOWER" = "$VER" ]; then
    echo "NOTE: $INSTALLED is installed and $VER is lower. macOS may refuse to"
    echo "      replace it. Bump CFBundleVersion in XrtUser/Info.plist, or check"
    echo "      'systemextensionsctl list' after activating."
  fi
fi
if [ "$1" = activate ]; then
  OLD=$(pgrep -f "org.xrtmac.XrtUser.dext/org.xrtmac.XrtUser" || true)
  "$APP/Contents/MacOS/XrtLoader" activate
  # A running dext can hang in "terminating for upgrade". Kill the old
  # instance so the kernel rematches the card with the new version.
  # macOS counts every such kill as a driver crash, and after about three in
  # one boot it panics ("has crashed too many times"). Reboot rather than
  # activate a third time; with no dext running there is nothing to kill.
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
