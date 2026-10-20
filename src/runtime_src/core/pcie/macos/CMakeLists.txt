# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
#
# macOS PCIe shim (libxrt_core). Phase 1: device discovery from the IOKit
# registry, no driver required.
add_library(core_pciemacos_objects OBJECT
  pci_enum.cpp
  dext_client.cpp
  fdt_macos.cpp
  bo_macos.cpp
  exec_macos.cpp
  device_macos.cpp
  system_macos.cpp
  shim.cpp
  )

target_compile_definitions(core_pciemacos_objects
  PRIVATE
  XCLHAL_MAJOR_VER=2
  XCLHAL_MINOR_VER=1
  )

target_include_directories(core_pciemacos_objects
  PRIVATE
  ${XRT_SOURCE_DIR}/runtime_src
  ${XRT_SOURCE_DIR}/runtime_src/core/pcie/driver/macos/XrtUser   # xrt_dext_abi.h
  ${XRT_BINARY_DIR}/gen
  ${XRT_BINARY_DIR}
  )

add_library(xrt_core SHARED
  $<TARGET_OBJECTS:core_pciemacos_objects>
  $<TARGET_OBJECTS:core_pciecommon_objects>
  $<TARGET_OBJECTS:core_smi_alveo_objects>
  )

set_target_properties(xrt_core PROPERTIES
  VERSION ${XRT_VERSION_STRING}
  SOVERSION ${XRT_SOVERSION}
  )

target_link_libraries(xrt_core
  PRIVATE
  xrt_coreutil
  "-framework IOKit"
  "-framework CoreFoundation"
  )

install(TARGETS xrt_core
  EXPORT xrt-targets
  LIBRARY DESTINATION ${XRT_INSTALL_LIB_DIR} COMPONENT ${XRT_COMPONENT} NAMELINK_COMPONENT ${XRT_DEV_COMPONENT}
  )
