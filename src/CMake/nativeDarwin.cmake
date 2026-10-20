# SPDX-License-Identifier: Apache-2.0
# macOS native build. Mirrors nativeLnx.cmake minus Linux-only pieces:
# no libdrm, no /etc/os-release, no DKMS, no cpack/deb/rpm, no ICD.
# The kernel side on macOS is a DriverKit extension built separately.

option(XRT_ENABLE_DKMS "Enable DKMS by default" OFF)
set(XRT_ENABLE_DKMS OFF CACHE BOOL "No DKMS on macOS" FORCE)
set(XRT_ENABLE_EMULATION OFF CACHE BOOL "Emulation shims are x86_64 Linux only" FORCE)

find_package(OpenCL)
if (OpenCL_FOUND OR OPENCL_FOUND)
  message(STATUS "Looking for OPENCL - found ${OpenCL_INCLUDE_DIRS}${OPENCL_INCLUDEDIR}")
  include_directories(${OpenCL_INCLUDE_DIRS} ${OPENCL_INCLUDEDIR})
else()
  message(FATAL_ERROR "Looking for OPENCL - not found (brew install opencl-headers)")
endif()

# The OpenCL layer (xocl) needs the Khronos headers and ocl_icd.h; Homebrew
# keeps both keg-only. The system OpenCL.framework does not load ICDs, so an
# ICD loader (brew ocl-icd or opencl-icd-loader) is needed at run time.
find_path(XRT_MACOS_CL_INCLUDE CL/cl.h
  HINTS /opt/homebrew/opt/opencl-headers/include /usr/local/opt/opencl-headers/include
  NO_DEFAULT_PATH)
find_path(XRT_MACOS_OCL_ICD_INCLUDE ocl_icd.h
  HINTS /opt/homebrew/opt/ocl-icd/include /usr/local/opt/ocl-icd/include
  NO_DEFAULT_PATH)
if (XRT_MACOS_CL_INCLUDE AND XRT_MACOS_OCL_ICD_INCLUDE)
  set(XRT_MACOS_OPENCL ON)
  message(STATUS "OpenCL layer enabled: ${XRT_MACOS_CL_INCLUDE} ${XRT_MACOS_OCL_ICD_INCLUDE}")
else()
  set(XRT_MACOS_OPENCL OFF)
  message(STATUS "OpenCL layer disabled (brew install opencl-headers ocl-icd to enable)")
endif()

set(LINUX_FLAVOR "darwin")
execute_process(COMMAND sw_vers -productVersion
  OUTPUT_VARIABLE LINUX_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND uname -r
  OUTPUT_VARIABLE LINUX_KERNEL_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)

option(XRT_STATIC_BUILD "Enable static building of XRT" OFF)

include(CMake/components.cmake)
include(CMake/boostUtil.cmake)
include_directories(${Boost_INCLUDE_DIRS})
add_compile_options("-DBOOST_LOCALE_HIDE_AUTO_PTR")
include(CMake/xrtVariables.cmake)

# macOS has no ldconfig, so binaries always need an rpath, even for a
# /usr/local "system" install.
set(CMAKE_INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR};@loader_path/../..;@loader_path/../../${CMAKE_INSTALL_LIBDIR}")

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE
  DESTINATION ${XRT_INSTALL_DIR}/license
  COMPONENT ${XRT_BASE_COMPONENT})

include(CMake/version.cmake)
include(CMake/ccache.cmake)
message("-- ${CMAKE_SYSTEM_INFO_FILE} (macOS ${LINUX_VERSION}) (Darwin ${LINUX_KERNEL_VERSION})")
message("-- Compiler: ${CMAKE_CXX_COMPILER} ${CMAKE_C_COMPILER}")
include(CMake/lint.cmake)

xrt_add_subdirectory(runtime_src)
xrt_add_subdirectory(python)
message("-- XRT version: ${XRT_VERSION_STRING}")
if (XRT_MACOS_OPENCL)
  include(CMake/icd.cmake)
endif()
message("-- macOS build: skipping cpack, DKMS and pkgconfig")
include(CMake/changelog.cmake)
include(CMake/findpackage.cmake)
