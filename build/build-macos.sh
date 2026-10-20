#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
#
# Build XRT for macOS (Apple silicon) and package it, together with the
# xrt_user DriverKit extension, as an installer package.
#
#   build/build-macos.sh [-clean] [-nopkg]
#
# Prerequisites: Xcode (DriverKit SDK), Homebrew packages
#   boost cmake ninja pkgconf openssl@3 rapidjson pybind11 opencl-headers ocl-icd xcodegen
# and pip3 --user pyyaml markdown jinja2 (aiebu spec generator).
#
# Result: build/Darwin-pkg/xrt_<version>_macos_arm64.pkg installing
#   /opt/xilinx/xrt                    runtime, tools, headers, pyxrt, test scripts
#   /Applications/XrtLoader.app        loader app embedding the driver extension
# and activating the extension from the postinstall script. First activation
# needs approval in System Settings > General > Login Items & Extensions.
# Until Apple grants the PCI DriverKit entitlement the extension is ad-hoc
# signed and needs SIP disabled and boot-args amfi_get_out_of_my_way=1.
set -e
export PATH=/opt/homebrew/bin:$PATH

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/build/Darwin-pkg
PREFIX=/opt/xilinx/xrt
PKGROOT=$BUILD/pkgroot
DRIVER=$ROOT/src/runtime_src/core/pcie/driver/macos
clean=0 nopkg=0
for a in "$@"; do
  case $a in
    -clean) clean=1 ;;
    -nopkg) nopkg=1 ;;
    *) echo "usage: $0 [-clean] [-nopkg]"; exit 2 ;;
  esac
done
[ $clean = 1 ] && rm -rf "$BUILD"

# 1. XRT runtime and tools, installed into a staging root
cmake -S "$ROOT/src" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DXRT_ALVEO=ON \
  -DAIEBU_BUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX=$PREFIX \
  -DCMAKE_PREFIX_PATH="/opt/homebrew;/opt/homebrew/opt/openssl@3"
ninja -C "$BUILD"
rm -rf "$PKGROOT"
DESTDIR="$PKGROOT" ninja -C "$BUILD" install > "$BUILD/install.log"

# 2. Python test cases used by 'xrt-smi validate' (the Linux packages ship
#    these under $PREFIX/test)
mkdir -p "$PKGROOT$PREFIX/test"
cp "$ROOT/tests/python/23_bandwidth/23_bandwidth.py" \
   "$ROOT/tests/python/23_bandwidth/host_mem_23_bandwidth.py" \
   "$ROOT/tests/python/utils_binding.py" "$PKGROOT$PREFIX/test/"

# 3. Driver extension and its loader app
CONFIG=Release "$DRIVER/build.sh"
mkdir -p "$PKGROOT/Applications"
ditto "$DRIVER/build/Build/Products/Release/XrtLoader.app" "$PKGROOT/Applications/XrtLoader.app"
mkdir -p "$PKGROOT$PREFIX/bin/unwrapped"
cp "$DRIVER/build/xrt_dext_probe" "$DRIVER/build/xrt_dext_load" "$DRIVER/build/xrt_dext_dma" \
   "$PKGROOT$PREFIX/bin/unwrapped/"

[ $nopkg = 1 ] && { echo "staged in $PKGROOT"; exit 0; }

# 4. Installer package
VERSION=$(grep -m1 "^XRT_VERSION_STRING:" "$BUILD/CMakeCache.txt" | cut -d= -f2)
VERSION=${VERSION:-0.0.0}
PKG="$BUILD/xrt_${VERSION}_macos_$(uname -m).pkg"
# Bundles must land exactly in /Applications, never on top of another copy
# of the same bundle elsewhere on disk.
COMPONENTS="$BUILD/components.plist"
pkgbuild --analyze --root "$PKGROOT" "$COMPONENTS"
i=0
while /usr/libexec/PlistBuddy -c "Print :$i" "$COMPONENTS" >/dev/null 2>&1; do
  /usr/libexec/PlistBuddy -c "Delete :$i:BundleIsRelocatable" "$COMPONENTS" >/dev/null 2>&1 || true
  /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$COMPONENTS"
  i=$((i + 1))
done
pkgbuild --root "$PKGROOT" --component-plist "$COMPONENTS" --identifier org.xrtmac.xrt \
  --version "$VERSION" --scripts "$ROOT/build/macos/scripts" --install-location / "$PKG"
echo "package: $PKG"
