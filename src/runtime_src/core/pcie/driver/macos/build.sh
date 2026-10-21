#!/bin/bash
# Build, ad-hoc sign and (optionally) activate the xrt_user dext.
#
# THREE BUILD CONFIGURATIONS, selected by environment variables. The default is
# the generic XRT driver, and it is what you want unless you know otherwise.
#
#   (default)             Generic XRT. Matches Xilinx/AMD PCI functions only
#                         (the nine ids in XrtUser/Info.plist). No Innova-2
#                         registers, no ConnectX code compiled in at all.
#
#   XRT_MACOS_INNOVA2=1   Adds XRT_DEXT_REG_WRITE: one bounded 32-bit write to
#                         two FPGA shell registers on an Innova-2 shell - the
#                         DDR mode select at 0x32000/0x32008 and mgmt_status at
#                         0x36000, both on the management function's own BAR0.
#                         This is FPGA-side AXI-Lite only. It does NOT touch the
#                         ConnectX and does not change what the dext matches.
#
#   XRT_MACOS_CX5=1       Also claims the Innova-2's ConnectX-5 functions, to
#                         reach the ICMD gateway that reports which flash image
#                         the FPGA booted.
#
#                         *** THIS DISPLACES APPLE'S ETHERNET DRIVER. ***
#                         com.apple.DriverKit-AppleEthernetMLX5 matches every
#                         Mellanox Ethernet function generically; this personality
#                         outranks it with IOProbeScore. The FPGA and the ConnectX
#                         share PCIe bridges on an Innova-2, so taking the
#                         ConnectX puts the dext in the path the FPGA is reached
#                         through. Adding this personality once already made the
#                         card undetectable (HANDOFF 93). Do not use it on a card
#                         you cannot physically reach, or on a host whose network
#                         runs over it.
#
# Usage: [CONFIG=Release] [XRT_MACOS_INNOVA2=1] [XRT_MACOS_CX5=1] ./build.sh [activate]
set -e
export PATH=/opt/homebrew/bin:$PATH
cd "$(dirname "$0")"
# XRT source tree (this file lives in src/runtime_src/core/pcie/driver/macos)
XRT_RUNTIME_SRC="$(cd ../../../.. && pwd)"

INNOVA2=${XRT_MACOS_INNOVA2:-0}
CX5=${XRT_MACOS_CX5:-0}
# Claiming the ConnectX is only useful with the Innova-2 register path alongside
# it, and a CX5 build is already the Innova-2-specific one by definition.
[ "$CX5" = 1 ] && INNOVA2=1

# The spec, the Info.plist and the entitlements are all generated, so that the
# tracked files stay the generic configuration and a CX5 build cannot happen by
# accident. They are generated in this directory, because xcodegen resolves a
# spec's relative paths against the spec's own location.
# They sit here in the macos directory, NOT in XrtUser/: that directory is a
# source glob, so a generated plist inside it is also picked up as a bundle
# resource and xcodebuild then refuses with "Multiple commands produce
# .../Info.plist".
SPEC=XrtMac.gen.yml
PLIST=Info.gen.plist
ENTS=XrtUser.gen.entitlements
cp project.yml "$SPEC"
cp XrtUser/Info.plist "$PLIST"
cp XrtUser/XrtUser.entitlements "$ENTS"

# Feature flags reach the dext sources as preprocessor defines. xrt_dext_abi.h
# keeps every selector number unconditionally - the numbering must never shift -
# so what these switch off is the implementation, not the ABI. A selector with no
# dispatch entry answers kIOReturnUnsupported, which is exactly what an older
# dext does, so a caller needs no new error path.
DEFS="XRT_MACOS_ENABLE_INNOVA2=$INNOVA2 XRT_MACOS_ENABLE_CX5=$CX5"
if [ "$CX5" = 1 ]; then CFG=cx5; IDX=2
elif [ "$INNOVA2" = 1 ]; then CFG=innova2; IDX=1
else CFG=generic; IDX=0
fi
python3 - "$SPEC" "$PLIST" "$ENTS" "$DEFS" "$CX5" "$CFG" "$IDX" <<'PY'
import sys, plistlib, re
spec, plist, ents, defs, cx5, cfg, idx = sys.argv[1:8]
s = open(spec).read()

# Feature defines on the dext target, under its existing settings.base block.
anchor = "        PRODUCT_NAME: org.xrtmac.XrtUser\n"
assert anchor in s, "project.yml's dext target no longer starts with PRODUCT_NAME"
quoted = " ".join(defs.split())
s = s.replace(anchor, anchor +
              "        GCC_PREPROCESSOR_DEFINITIONS: \"$(inherited) %s\"\n" % quoted, 1)

# Point the target at the generated plist and entitlements.
s = s.replace("INFOPLIST_FILE: XrtUser/Info.plist",
              "INFOPLIST_FILE: %s" % plist, 1)
s = s.replace("CODE_SIGN_ENTITLEMENTS: XrtUser/XrtUser.entitlements",
              "CODE_SIGN_ENTITLEMENTS: %s" % ents, 1)

# The glob must not pick up the files that are build INPUT rather than bundle
# content. xcodegen drops the plist named by INFOPLIST_FILE automatically, but
# that is now the generated one at the top level, so the tracked
# XrtUser/Info.plist would be copied in as a resource and collide with it:
# "Multiple commands produce .../Info.plist".
excl = "          - \"cx5/**\"\n"
assert excl in s, "project.yml's cx5 exclusion is not where build.sh expects it"
keep = "          - \"cx5/*.plist\"\n" if cx5 == "1" else excl
s = s.replace(excl, keep + "          - \"Info.plist\"\n"
                         "          - \"*.entitlements\"\n", 1)

# Record the configuration in the bundle, and make it part of CFBundleVersion.
# The version has to differ: macOS identifies a staged system extension by
# version, so three drivers sharing one version string is how you end up running
# a different driver than you just built. Base 102 becomes 102.0 / 102.1 / 102.2.
p = plistlib.load(open(plist, 'rb'))
p['XrtBuildConfig'] = cfg
p['CFBundleVersion'] = "%s.%s" % (p['CFBundleVersion'], idx)
plistlib.dump(p, open(plist, 'wb'))

if cx5 == "1":

    # Merge the ConnectX personality into IOKitPersonalities.
    p = plistlib.load(open(plist, 'rb'))
    frag = plistlib.load(open('XrtUser/cx5/XrtCx5.personality.plist', 'rb'))
    p['IOKitPersonalities'].update(frag)
    plistlib.dump(p, open(plist, 'wb'))

    # And add the ConnectX id to the PCI transport entitlement. A device id must
    # appear in BOTH IOPCIMatch and IOPCIPrimaryMatch; with only the first, the
    # dext is refused with "provider entitlements check failed".
    e = plistlib.load(open(ents, 'rb'))
    arr = e['com.apple.developer.driverkit.transport.pci']
    arr[0]['IOPCIPrimaryMatch'] = arr[0]['IOPCIPrimaryMatch'] + " 0x101715b3"
    plistlib.dump(e, open(ents, 'wb'))

open(spec, 'w').write(s)
PY

xcodegen generate -q --spec "$SPEC"
CONFIG=${CONFIG:-Debug}
xcodebuild -project XrtMac.xcodeproj -scheme XrtLoader -configuration $CONFIG \
  -derivedDataPath build CODE_SIGNING_ALLOWED=NO build > xcb.log 2>&1 \
  || { grep -E "error:" xcb.log | sort -u; exit 1; }
APP=build/Build/Products/$CONFIG/XrtLoader.app
DEXT=$APP/Contents/Library/SystemExtensions/org.xrtmac.XrtUser.dext
codesign -f -s - --entitlements "$ENTS" "$DEXT"
for d in "$APP"/Contents/MacOS/*.dylib; do [ -e "$d" ] && codesign -f -s - "$d"; done
codesign -f -s - --entitlements Loader/Loader.entitlements "$APP"
clang++ -std=c++17 -O2 -I$XRT_RUNTIME_SRC/core/include -framework IOKit -framework CoreFoundation tools/xrt_dext_probe.cpp -o build/xrt_dext_probe
clang++ -std=c++17 -O2 -I$XRT_RUNTIME_SRC/core/include -I$XRT_RUNTIME_SRC -framework IOKit -framework CoreFoundation tools/xrt_dext_load.cpp -o build/xrt_dext_load
clang++ -std=c++17 -O2 -framework IOKit -framework CoreFoundation tools/xrt_dext_dma.cpp -o build/xrt_dext_dma

# Say which driver this is, every time. The version string alone cannot tell two
# configurations apart, and an installed dext whose contents were guessed from its
# CFBundleVersion has already cost a day (HANDOFF 95).
VER=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$DEXT/Info.plist")
if [ "$CX5" = 1 ]; then
  WHAT="XRT + Innova-2 + ConnectX-5 (DISPLACES AppleEthernetMLX5)"
elif [ "$INNOVA2" = 1 ]; then
  WHAT="XRT + Innova-2 shell registers"
else
  WHAT="generic XRT (Xilinx functions only)"
fi
echo "built: $(lipo -archs "$DEXT/org.xrtmac.XrtUser")  v$VER  $CONFIG"
echo "config: $WHAT"
if [ "$CX5" = 1 ]; then
  echo "WARNING: this build claims 0x101715b3 and outranks Apple's Ethernet driver."
  echo "         The FPGA and the ConnectX share PCIe bridges - expect to lose both"
  echo "         if it goes wrong, and have physical access to the card first."
fi
# Going from cx5 (102.2) back to generic (102.0) is a version DECREASE, which
# macOS can refuse to stage - it would leave the old driver running while this
# script reports success. Say so rather than let it pass silently.
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
