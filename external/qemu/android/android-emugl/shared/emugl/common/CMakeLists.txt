android_add_library(
  TARGET emugl_common
  SHARED
  LICENSE Apache-2.0
  SRC address_space_device_control_ops.cpp
      crash_reporter.cpp
      shared_library.cpp
      stringparsing.cpp
      vm_operations.cpp
      window_operations.cpp)

android_target_compile_definitions(emugl_common windows_msvc-x86_64
                                   PRIVATE "-DBUILDING_EMUGL_COMMON_SHARED")
target_compile_options(emugl_common PRIVATE -fvisibility=default)
target_link_libraries(emugl_common PRIVATE emugl_base)
target_link_libraries(emugl_common PRIVATE android-emu android-emu-base-headers
                                           qemu-host-common-headers)
target_include_directories(
  emugl_common PRIVATE ${ANDROID_EMUGL_DIR}/host/include
                       ${ANDROID_EMUGL_DIR}/shared)

android_target_properties(
  emugl_common linux
  "LINK_FLAGS<=-Wl,-rpath,'$COMMON'  -Wl,--disable-new-dtags")
android_install_shared_library(TARGET emugl_common)

# AEMU core-only: emugl_common host unit tests are not built.
