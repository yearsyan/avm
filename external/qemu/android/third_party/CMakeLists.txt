# Other targets depend on these
# First enable unit testing
# AEMU core-only: googletest removed; tests are not built.

set(AEMU_CONAN_DEPS_DIR
    ""
    CACHE PATH "Directory containing Conan CMakeDeps config files")
if(AEMU_CONAN_DEPS_DIR)
  list(PREPEND CMAKE_PREFIX_PATH "${AEMU_CONAN_DEPS_DIR}")
elseif(EXISTS "${CMAKE_BINARY_DIR}/conan")
  list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}/conan")
elseif(EXISTS "${CMAKE_BINARY_DIR}/generators")
  list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}/generators")
endif()
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE
      Release
      CACHE STRING "Build type" FORCE)
endif()

get_filename_component(AEMU_REPO_ROOT "${ANDROID_QEMU2_TOP_DIR}/../.."
                       ABSOLUTE)
include("${AEMU_REPO_ROOT}/cmake/aemu-conan-deps.cmake")
android_license(
  TARGET "abseil-cpp;abseil::abseil"
  LIBNAME abseil
  URL "https://github.com/abseil/abseil-cpp/tree/20240116.2"
  SPDX "Apache-2.0"
  LICENSE "https://github.com/abseil/abseil-cpp/blob/20240116.2/LICENSE"
  LOCAL "${ANDROID_QEMU2_TOP_DIR}/LICENSES/LICENSE.APACHE2")
android_license(
  TARGET "lz4;lz4::lz4;LZ4::lz4_static"
  LIBNAME lz4
  URL "https://github.com/lz4/lz4/tree/v1.10.0"
  SPDX "BSD-2-Clause"
  LICENSE "https://github.com/lz4/lz4/blob/v1.10.0/lib/LICENSE"
  LOCAL "${ANDROID_QEMU2_TOP_DIR}/LICENSES/LICENSE.BSD-2-CLAUSE-LZ4")
android_license(
  TARGET "zlib;ZLIB::ZLIB"
  LIBNAME zlib
  URL "https://github.com/madler/zlib/tree/v1.3.1"
  SPDX "Zlib"
  LICENSE "https://github.com/madler/zlib/blob/v1.3.1/LICENSE"
  LOCAL "${ANDROID_QEMU2_TOP_DIR}/LICENSES/LICENSE.ZLIB")
android_license(
  TARGET "protobuf::protobuf;protobuf::libprotobuf;protobuf::libprotoc;protobuf::protoc"
  LIBNAME protobuf
  URL "https://github.com/protocolbuffers/protobuf/tree/v28.3"
  SPDX "BSD-3-Clause"
  LICENSE "https://github.com/protocolbuffers/protobuf/blob/v28.3/LICENSE"
  LOCAL "${ANDROID_QEMU2_TOP_DIR}/LICENSES/LICENSE.BSD-3-CLAUSE-PROTOBUF")

add_library(aemu-absl-log-message-compat STATIC absl-log-message-compat.cc)
target_link_libraries(aemu-absl-log-message-compat
                      PRIVATE absl::log_internal_message)
set_target_properties(aemu-absl-log-message-compat
                      PROPERTIES CXX_VISIBILITY_PRESET default
                                 VISIBILITY_INLINES_HIDDEN OFF)
android_license(
  TARGET aemu-absl-log-message-compat
  LIBNAME abseil
  URL "https://github.com/abseil/abseil-cpp/tree/20240116.2"
  SPDX "Apache-2.0"
  LICENSE "https://github.com/abseil/abseil-cpp/blob/20240116.2/LICENSE"
  LOCAL "${ANDROID_QEMU2_TOP_DIR}/LICENSES/LICENSE.APACHE2")

foreach(_protobuf_target protobuf::libprotobuf protobuf::libprotoc
                         protobuf::protobuf)
  if(TARGET "${_protobuf_target}")
    target_link_libraries("${_protobuf_target}"
                          INTERFACE aemu-absl-log-message-compat)
  endif()
endforeach()

# We cannot run the the tests when we are cross compiling
if(CROSSCOMPILE)
  set(OPTION_ASTC_TESTS OFF CACHE BOOL "No tests astc when cross-compiling"
                                  FORCE)
endif()

add_subdirectory(${ANDROID_QEMU2_TOP_DIR}/../glib/glib glib)
# Include astc-codec as a subproject.

# astc-codec defines CMAKE_CXX_STANDARD=11, which won't work with msvc. We did
# the same for googletest.
add_subdirectory(${ANDROID_QEMU2_TOP_DIR}/../astc-codec/src/base
                 astc-codec-base)
add_subdirectory(${ANDROID_QEMU2_TOP_DIR}/../astc-codec/src/decoder
                 astc-codec-decoder)
android_license(
  TARGET astc-codec astc_utils footprint base
  LIBNAME astc-codec
  SPDX Apache-2.0
  URL https://android.googlesource.com/platform/external/astc-codec/+/refs/heads/master
  LICENSE
    https://android.googlesource.com/platform/external/astc-codec/+/refs/heads/master/LICENSE
  LOCAL ${ANDROID_QEMU2_TOP_DIR}/../astc-codec/LICENSE)

# Our magical toolchain will likely not create runnable executables due to
# libc++ dependencies. So we must make sure we set the runtime_os_properties.
#
# TODO(jansene): Once we are using cmake > 3.7 we can replace these lines with
# android_add_subdirectory as this will do proper target discovery.
if(OPTION_ASTC_TESTS)
  set(DECLARED_TESTS
      base_test
      physical_astc_block_test
      partition_test
      integer_sequence_codec_test
      intermediate_astc_block_test
      quantization_test
      weight_infill_test
      endpoint_codec_test
      logical_astc_block_test
      codec_test
      footprint_test)
  foreach(TST ${DECLARED_TESTS})
    android_target_properties(${TST} all "${RUNTIME_OS_PROPERTIES}")
  endforeach()
endif()

add_subdirectory(dtc)
if(NOT TARGET emulator-libusb)
  add_subdirectory(libusb)
endif()
if(NOT TARGET protobuf::libprotobuf)
  add_subdirectory(protobuf)
endif()
add_subdirectory(jpeg-6b)
add_subdirectory(libdtb)
add_subdirectory(libselinux)
add_subdirectory(libslirp)
add_subdirectory(libsparse)
add_subdirectory(libwebp)
add_subdirectory(murmurhash)
add_subdirectory(libkeymaster3)
add_subdirectory(ext4_utils)
add_subdirectory(mksdcard)
add_subdirectory(boringssl)
add_subdirectory(hostapd)

add_library(picosha2 INTERFACE)
target_include_directories(picosha2 INTERFACE picosha2)

android_license(
  TARGET picosha2
  LIBNAME picosha2
  URL "https://android.googlesource.com/platform/external/qemu/+/refs/heads/emu-master-dev/android/third_party/picosha2/"
  SPDX "MIT"
  LICENSE
    "https://android.googlesource.com/platform/external/qemu/+/refs/heads/emu-master-dev/android/third_party/picosha2/LICENSE"
  LOCAL "${ANDROID_QEMU2_TOP_DIR}/android/third_party/picosha2/LICENSE")

if(WINDOWS)
  add_subdirectory(mman-win32)
  add_subdirectory(regex-win32)
  if(WINDOWS_MSVC_X86_64)
    add_subdirectory(dirent-win32)
  endif()
  add_subdirectory(usbpassthrough-prebuilt-win32)
endif()

android_license(
  TARGET emulator-libusb libusb::libusb
  LIBNAME libusb
  URL "https://github.com/libusb/libusb/tree/v1.0.29"
  SPDX "LGPL-2.1"
  LICENSE "https://github.com/libusb/libusb/blob/v1.0.29/COPYING"
  LOCAL "${ANDROID_QEMU2_TOP_DIR}/LICENSES/LICENSE.LGPLv21")

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
add_subdirectory(json)

# (AEMU core-only / macOS-only: libevent was needed only by bluetooth/webrtc,
#  which have been removed.)

if(NOT TARGET flatc)
  include(ExternalProject)
  set(FLATC_BINARY_DIR)
  cmake_path(APPEND CMAKE_CURRENT_BINARY_DIR flatbuffers-flatc OUTPUT_VARIABLE
             FLATC_BINARY_DIR)
  set(FLATC_BINARY_PATH)
  cmake_path(APPEND FLATC_BINARY_DIR flatc OUTPUT_VARIABLE FLATC_BINARY_PATH)
  if(LINUX_X86_64 OR LINUX_AARCH64)
    # We will use the system toolchain as we require special libc++ bindings for the linux toolchain.
    set(EXTRA_FLATC_ARGS "")
  else()
    set(EXTRA_FLATC_ARGS "-DCMAKE_TOOLCHAIN_FILE=${ANDROID_QEMU2_TOP_DIR}/android/build/cmake/toolchain-${ANDROID_HOST_TAG}.cmake")
  endif()
  externalproject_add(
    flatbuffers-flatc
    SOURCE_DIR ${AOSP_ROOT}/external/flatbuffers
    BINARY_DIR ${FLATC_BINARY_DIR}
    CMAKE_ARGS -DFLATBUFFERS_BUILD_TESTS=OFF -DFLATBUFFERS_BUILD_CPP17=ON
               -DFLATBUFFERS_BUILD_FLAT_LIB=OFF -DFLATBUFFERS_BUILD_FLATC=ON
               -DFLATBUFFERS_STATIC_FLATC=OFF -DFLATBUFFERS_BUILD_FLATHASH=OFF
               ${EXTRA_FLATC_ARGS}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${FLATC_BINARY_PATH})
  add_executable(flatc IMPORTED GLOBAL)
  set_property(TARGET flatc PROPERTY IMPORTED_LOCATION ${FLATC_BINARY_PATH})
  add_dependencies(flatc flatbuffers-flatc)
endif()

if(NOT TARGET flatbuffers)
  option(FLATBUFFERS_BUILD_TESTS "" OFF)
  option(FLATBUFFERS_BUILD_CPP17 "" ON)
  option(FLATBUFFERS_BUILD_FLAT_LIB "" ON)
  option(FLATBUFFERS_BUILD_FLATC "" OFF)
  option(FLATBUFFERS_STATIC_FLATC "" OFF)
  option(FLATBUFFERS_BUILD_FLATHASH "" OFF)
  add_subdirectory(${AOSP_ROOT}/external/flatbuffers flatbuffers-flatlib)
endif()

android_license(
  LIBNAME flatbuffers URL "https://github.com/google/flatbuffers"
  TARGET flatc flatbuffers SPDX Apache-2.0
  LOCAL ${ANDROID_QEMU2_TOP_DIR}/LICENSES/LICENSE.APACHE2 LICENSE Apache-2.0)

# (AEMU core-only: grpc, mynewt-nimble and netsim are removed.)

# Duplicate glib dll for Windows
# The long term solution should be to figure out linking dll within lib64
if(WINDOWS_MSVC_X86_64)
  install(TARGETS glib2_${ANDROID_TARGET_TAG} RUNTIME DESTINATION .
          LIBRARY DESTINATION .)
endif()

# gfxstream, aemu-common modules
set(GFXSTREAM_REPO_ROOT
    ${ANDROID_QEMU2_TOP_DIR}/../../hardware/google/gfxstream)
if(NOT LINUX_AARCH64 AND NOT OPTION_AEMU_CORE_ONLY)
  set(ENABLE_VKCEREAL_TESTS ON)
endif()
function(discover_gfxstream_internal_targets result subdir)
  if(CMAKE_VERSION VERSION_LESS "3.7.0")
    message(
      FATAL_ERROR
        "This function cannot be used by older cmake versions (${CMAKE_VERSION}<3.7.0)"
    )
  endif()
  message("gfxstream_internal_targets result=${result} subdir=${subdir}")
  get_filename_component(abs_subdir ${subdir} ABSOLUTE)
  set(GFXSTREAM_THIRD_PARTY_ROOT ${GFXSTREAM_REPO_ROOT})
  cmake_path(APPEND GFXSTREAM_THIRD_PARTY_ROOT "third_party")
  cmake_path(IS_PREFIX GFXSTREAM_THIRD_PARTY_ROOT ${abs_subdir} NORMALIZE
             IS_THIRD_PARTY)
  if(IS_THIRD_PARTY)
    return()
  endif()
  cmake_path(IS_PREFIX GFXSTREAM_REPO_ROOT ${abs_subdir} NORMALIZE
             IS_GFXSTREAM_SUBDIR)
  cmake_path(IS_PREFIX AEMU_COMMON_REPO_ROOT ${abs_subdir} NORMALIZE
             IS_AEMU_COMMON_SUBDIR)
  cmake_path(IS_PREFIX GFXSTREAM_PROTOCOLS_DIR ${abs_subdir} NORMALIZE
             IS_GFXSTREAM_PROTOCOLS_SUBDIR)
  if(NOT IS_GFXSTREAM_SUBDIR AND NOT IS_AEMU_COMMON_SUBDIR
     AND NOT IS_GFXSTREAM_PROTOCOLS_SUBDIR)
    return()
  endif()

  get_directory_property(subdirs DIRECTORY "${abs_subdir}" SUBDIRECTORIES)
  foreach(subdir IN LISTS subdirs)
    discover_gfxstream_internal_targets(${result} "${subdir}")
  endforeach()

  get_property(targets_in_dir DIRECTORY "${subdir}"
               PROPERTY BUILDSYSTEM_TARGETS)
  set(${result} ${${result}} ${targets_in_dir} PARENT_SCOPE)
endfunction()
set(GFXSTREAM_PREFIX "gfxstream")
set(ANGLE_REPO_ROOT ${ANDROID_QEMU2_TOP_DIR}/../../external/angle)
add_library(gfxstream_egl_headers INTERFACE)
target_include_directories(gfxstream_egl_headers
                           INTERFACE ${ANGLE_REPO_ROOT}/include)
add_library(gfxstream_glm_headers INTERFACE)
target_include_directories(gfxstream_glm_headers
                           INTERFACE ${GFXSTREAM_REPO_ROOT}/third_party/glm/include)
target_compile_definitions(gfxstream_glm_headers INTERFACE GLM_FORCE_CXX14 GLM_ENABLE_EXPERIMENTAL)
add_subdirectory(${GFXSTREAM_REPO_ROOT}/host/common
                 ${GFXSTREAM_PREFIX}-host-common)
add_subdirectory(${GFXSTREAM_REPO_ROOT}/host/tracing
                 ${GFXSTREAM_PREFIX}-host-tracing)
add_subdirectory(${GFXSTREAM_REPO_ROOT}/host/features
                 ${GFXSTREAM_PREFIX}-host-features)
add_subdirectory(${GFXSTREAM_REPO_ROOT}/common
                 ${GFXSTREAM_PREFIX}-common)
add_subdirectory(${GFXSTREAM_REPO_ROOT}/third_party/stb
                 ${GFXSTREAM_PREFIX}-third-party-stb)
add_subdirectory(${GFXSTREAM_REPO_ROOT}/third_party/xcb
                 ${GFXSTREAM_PREFIX}-third-party-xcb)
# Include hardware/google/aemu project
set(AEMU_COMMON_BUILD_CONFIG "qemu-android" CACHE STRING "")
set(GFXSTREAM_REPO_ROOT
    ${ANDROID_QEMU2_TOP_DIR}/../../hardware/google/gfxstream)
set(GFXSTREAM_PROTOCOLS_DIR
    ${ANDROID_QEMU2_TOP_DIR}/../../hardware/google/gfxstream)
set(AEMU_COMMON_REPO_ROOT ${ANDROID_QEMU2_TOP_DIR}/../../hardware/google/aemu)
add_subdirectory(${ANDROID_QEMU2_TOP_DIR}/../../hardware/google/aemu
                 aemu_common)
if(NOT LINUX_AARCH64)
  set(ASTC_CPU_DECODING ON)
  set(DECOMPRESSOR ON)
  set(CLI OFF)
  if(DARWIN_AARCH64)
    set(ASTCDEC_LIB_NAME astcdec-native-static)
  else()
    set(ISA_AVX2 ON)
    set(ASTCDEC_LIB_NAME astcdec-avx2-static)
  endif()
  add_subdirectory(${GFXSTREAM_REPO_ROOT}/third_party/astc-encoder
                   ${ASTCDEC_LIB_NAME})
endif()

if(ENABLE_VKCEREAL_TESTS)
  set(AEMU_COMMON_TESTS aemu-base_unittests aemu-host-common_unittests)
  foreach(TST ${AEMU_COMMON_TESTS})
    android_target_properties(${TST} all "${RUNTIME_OS_PROPERTIES}")
  endforeach()
endif()

# Figure out all gfxstream dependencies
include(${GFXSTREAM_REPO_ROOT}/toolchain/cmake/SetSubdirectorProperties.cmake)
get_directory_property(GFXSTREAM_SUBDIRS SUBDIRECTORIES)
foreach(GFXSTREAM_SUBDIR IN LISTS GFXSTREAM_SUBDIRS)
  discover_gfxstream_internal_targets(GFXSTREAM_INTERNAL_TARGETS
                                      ${GFXSTREAM_SUBDIR})
endforeach()
set(GFXSTREAM_DEPENDENCIES
    ${GFXSTREAM_INTERNAL_TARGETS} angle_common angle_shader_translator
    renderdoc ${ASTCDEC_LIB_NAME})
android_license(
  TARGET "${GFXSTREAM_DEPENDENCIES}"
  LIBNAME gfxstream
  URL "https://android.googlesource.com/platform/hardware/google/gfxstream/+/refs/heads/master"
  SPDX Apache-2.0
  LOCAL "${ANDROID_QEMU2_TOP_DIR}/../../hardware/google/gfxstream/LICENSE"
  LICENSE
    "https://android.googlesource.com/platform/hardware/google/gfxstream/+/refs/heads/master/LICENSE"
)
# Used by gfxstream but has different license.
android_license(TARGET "libdrm_headers" LIBNAME None SPDX MIT LICENSE "MIT"
                LOCAL "${ANDROID_QEMU2_TOP_DIR}/../libdrm/LICENSE")
android_license(TARGET "gfxstream_xcb_headers" LIBNAME None SPDX X11 LICENSE "X11"
                LOCAL "${GFXSTREAM_REPO_ROOT}/third_party/xcb/LICENSE")
