# Copyright 2018 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# The toolchain files get processed multiple times during compile detection keep these files simple, and do not setup
# libraries or anything else. merely tags, compiler toolchain and flags should be set here.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(AOSP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.."
                       ABSOLUTE)
set(ANDROID_QEMU2_TOP_DIR "${AOSP_ROOT}/external/qemu")
include(toolchain)
include(toolchain-rust)

# First we setup all the tags.
toolchain_configure_tags("darwin-x86_64")

# Make sure runtime paths stay relative to the produced binary. Conan-provided
# shared libraries are linked from absolute package-cache paths during the build,
# but those link paths must not be copied into LC_RPATH.
set(RUNTIME_OS_PROPERTIES
    "INSTALL_RPATH>=@loader_path;INSTALL_RPATH>=@loader_path/lib64;BUILD_WITH_INSTALL_RPATH=ON;INSTALL_RPATH_USE_LINK_PATH=OFF"
)

if(NOT APPLE)
  get_osxcross_settings("darwin-x86_64")
  toolchain_generate("darwin-x86_64")
  set(CMAKE_SYSTEM_NAME "Darwin")
  set(CMAKE_SYSTEM_PROCESSOR "x86_64")

  # search for programs in the build host directories
  set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
  # for libraries and headers in the target directories
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
  # where is the target environment
  set(CMAKE_FIND_ROOT_PATH
      "${OSXCROSS_SDK}"
      "${OSXCROSS_TARGET_DIR}/macports/pkgs/opt/local")
  set(ENV{PKG_CONFIG_LIBDIR} "${OSXCROSS_TARGET_DIR}/macports/pkgs/opt/local/lib/pkgconfig")
  set(ENV{PKG_CONFIG_SYSROOT_DIR} "${OSXCROSS_TARGET_DIR}/macports/pkgs")
  set(CMAKE_OSX_SYSROOT ${OSXCROSS_SDK})
  set(CMAKE_INSTALL_DO_STRIP TRUE)
else()

  toolchain_generate("darwin-x86_64")
  # Configure how we strip executables, and set the osx sysroot.
  set(CMAKE_STRIP_CMD "${CMAKE_STRIP} -S")
  set(CMAKE_OSX_SYSROOT "${ANDROID_SYSROOT}")
endif()

# Always consider the source to be darwin.
add_definitions(-D_DARWIN_C_SOURCE=1)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-parameter -Wno-unknown-warning-option")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-parameter -Wno-unknown-warning-option")


# Next we configure rust.
get_rust_version(RUST_VER)
configure_rust(COMPILER_ROOT "${AOSP_ROOT}/prebuilts/rust/darwin-x86/${RUST_VER}/bin")

set(CMAKE_OSX_ARCHITECTURES "x86_64")
# And the asm type if we are compiling with ASM
set(ANDROID_ASM_TYPE macho64)
# No magical includes or dependencies for darwin..
