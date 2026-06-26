# Copyright 2018 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the
# specific language governing permissions and limitations under the License.

get_filename_component(
  PREBUILT_ROOT
  "${ANDROID_QEMU2_TOP_DIR}/../../prebuilts/android-emulator-build/common/vulkan/${ANDROID_TARGET_TAG}"
  ABSOLUTE)

set(VULKAN_FOUND TRUE)

if(LINUX_X86_64)
  set(VULKAN_DEPENDENCIES
      # Loader (for real)
      "${PREBUILT_ROOT}/libvulkan.so>lib64/vulkan/libvulkan.so"
      "${PREBUILT_ROOT}/libvulkan.so>lib64/vulkan/libvulkan.so.1"
      # Swiftshader
      "${PREBUILT_ROOT}/icds/libvk_swiftshader.so>lib64/vulkan/libvk_swiftshader.so"
      "${PREBUILT_ROOT}/icds/vk_swiftshader_icd.json>lib64/vulkan/vk_swiftshader_icd.json"
      # Lavapipe
      "${PREBUILT_ROOT}/icds/libvulkan_lvp.so>lib64/vulkan/libvulkan_lvp.so"
      "${PREBUILT_ROOT}/icds/lvp_icd.json>lib64/vulkan/lvp_icd.json"
      "${PREBUILT_ROOT}/icds/libxcb-aemu.so>lib64/vulkan/libxcb-aemu.so"
      "${PREBUILT_ROOT}/icds/libxml2.so.2>lib64/vulkan/libxml2.so.2"
      "${PREBUILT_ROOT}/icds/libedit.so.0>lib64/vulkan/libedit.so.0"
      "${PREBUILT_ROOT}/icds/libLLVM.so>lib64/vulkan/libLLVM.so"
      "${PREBUILT_ROOT}/icds/libncurses.so.6>lib64/vulkan/libncurses.so.6"
      # for translating shaders to SPIRV
      "${PREBUILT_ROOT}/glslangValidator>lib64/vulkan/glslangValidator"
      ${VULKAN_COMMON_DEPENDENCIES})
  set(VULKAN_TEST_DEPENDENCIES
      # Loader (for testing)
      "${PREBUILT_ROOT}/libvulkan.so>testlib64/libvulkan.so"
      # Debug / validation layers
      "${PREBUILT_ROOT}/layers/libVkLayer_api_dump.so>testlib64/layers/libVkLayer_api_dump.so"
      "${PREBUILT_ROOT}/layers/libVkLayer_khronos_validation.so>testlib64/layers/libVkLayer_khronos_validation.so"
      "${PREBUILT_ROOT}/layers/libVkLayer_monitor.so>testlib64/layers/libVkLayer_monitor.so"
      "${PREBUILT_ROOT}/layers/libVkLayer_screenshot.so>testlib64/layers/libVkLayer_screenshot.so"
      "${PREBUILT_ROOT}/layers/VkLayer_api_dump.json>testlib64/layers/VkLayer_api_dump.json"
      "${PREBUILT_ROOT}/layers/VkLayer_khronos_validation.json>testlib64/layers/VkLayer_khronos_validation.json"
      "${PREBUILT_ROOT}/layers/VkLayer_monitor.json>testlib64/layers/VkLayer_monitor.json"
      "${PREBUILT_ROOT}/layers/VkLayer_screenshot.json>testlib64/layers/VkLayer_screenshot.json"
      # Shaders
      ${VULKAN_COMMON_DEPENDENCIES})
elseif(DARWIN_X86_64 OR DARWIN_AARCH64)
  # AEMU core-only / macOS: host GPU acceleration via MoltenVK. The software
  # Vulkan ICDs (lavapipe/libLLVM, swiftshader, kosmickrisp) and the host-side
  # SPIR-V tooling (glslangValidator, libz3, libzstd) are intentionally not
  # installed: -gpu host never loads them, and macOS forces swiftshader/llvmpipe
  # requests to swangle anyway (see emugl_config.cpp). Only the Vulkan loader
  # and MoltenVK (the Apple-GPU bridge) are needed for the host path.
  set(AEMU_MOLTENVK_DYLIB "${PREBUILT_ROOT}/icds/libMoltenVK.dylib")
  if(DARWIN_AARCH64)
    find_program(AEMU_LIPO lipo)
    if(NOT AEMU_LIPO)
      message(FATAL_ERROR "lipo is required to thin MoltenVK for macOS arm64")
    endif()

    set(AEMU_THIN_MOLTENVK_DYLIB
        "${CMAKE_BINARY_DIR}/thin-prebuilts/vulkan/${ANDROID_TARGET_TAG}/icds/libMoltenVK.dylib")
    file(MAKE_DIRECTORY
         "${CMAKE_BINARY_DIR}/thin-prebuilts/vulkan/${ANDROID_TARGET_TAG}/icds")
    execute_process(
      COMMAND "${AEMU_LIPO}" -info "${AEMU_MOLTENVK_DYLIB}"
      OUTPUT_VARIABLE AEMU_MOLTENVK_LIPO_INFO
      ERROR_VARIABLE AEMU_MOLTENVK_LIPO_INFO
      RESULT_VARIABLE AEMU_MOLTENVK_LIPO_INFO_RESULT)
    if(NOT AEMU_MOLTENVK_LIPO_INFO_RESULT EQUAL 0)
      message(
        FATAL_ERROR
          "Unable to inspect MoltenVK architecture slices: ${AEMU_MOLTENVK_LIPO_INFO}"
      )
    endif()
    if(AEMU_MOLTENVK_LIPO_INFO MATCHES "Non-fat file:.*arm64")
      configure_file("${AEMU_MOLTENVK_DYLIB}" "${AEMU_THIN_MOLTENVK_DYLIB}"
                     COPYONLY)
    elseif(AEMU_MOLTENVK_LIPO_INFO MATCHES
           "Architectures in the fat file:.*arm64")
      execute_process(
        COMMAND "${AEMU_LIPO}" -thin arm64 "${AEMU_MOLTENVK_DYLIB}" -output
                "${AEMU_THIN_MOLTENVK_DYLIB}"
        ERROR_VARIABLE AEMU_MOLTENVK_THIN_ERROR
        RESULT_VARIABLE AEMU_MOLTENVK_THIN_RESULT)
      if(NOT AEMU_MOLTENVK_THIN_RESULT EQUAL 0)
        message(
          FATAL_ERROR
            "Unable to thin MoltenVK to arm64: ${AEMU_MOLTENVK_THIN_ERROR}")
      endif()
    else()
      message(
        FATAL_ERROR
          "MoltenVK does not contain an arm64 slice: ${AEMU_MOLTENVK_LIPO_INFO}"
      )
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E touch
                            "${AEMU_THIN_MOLTENVK_DYLIB}")
    set(AEMU_MOLTENVK_DYLIB "${AEMU_THIN_MOLTENVK_DYLIB}")
  endif()
  set(VULKAN_DEPENDENCIES
      # Vulkan Loader
      "${PREBUILT_ROOT}/libvulkan.dylib>lib64/vulkan/libvulkan.dylib"
      # MoltenVK (Vulkan -> Metal, exposes the Apple GPU as a Vulkan device)
      "${AEMU_MOLTENVK_DYLIB}>lib64/vulkan/libMoltenVK.dylib"
      "${PREBUILT_ROOT}/icds/MoltenVK_icd.json>lib64/vulkan/MoltenVK_icd.json"
      # Shaders
      ${VULKAN_COMMON_DEPENDENCIES})
  set(VULKAN_TEST_DEPENDENCIES
      # Loader (for testing)
      "${PREBUILT_ROOT}/libvulkan.dylib>testlib64/libvulkan.dylib"
      # Debug / validation layers
      "${PREBUILT_ROOT}/layers/libVkLayer_api_dump.dylib>testlib64/layers/libVkLayer_api_dump.dylib"
      "${PREBUILT_ROOT}/layers/libVkLayer_khronos_validation.dylib>testlib64/layers/libVkLayer_khronos_validation.dylib"
      "${PREBUILT_ROOT}/layers/VkLayer_api_dump.json>testlib64/layers/VkLayer_api_dump.json"
      "${PREBUILT_ROOT}/layers/VkLayer_khronos_validation.json>testlib64/layers/VkLayer_khronos_validation.json"
      # shaders
      ${VULKAN_COMMON_DEPENDENCIES})
elseif(WINDOWS)
  get_filename_component(
    PREBUILT_ROOT
    "${ANDROID_QEMU2_TOP_DIR}/../../prebuilts/android-emulator-build/common/vulkan/windows-x86_64"
    ABSOLUTE)
  set(VULKAN_DEPENDENCIES
      "${PREBUILT_ROOT}/vulkan-1.dll>lib64/vulkan/vulkan-1.dll"
      # Lavapipe
      "${PREBUILT_ROOT}/icds/libvulkan_lvp.dll>lib64/vulkan/libvulkan_lvp.dll"
      "${PREBUILT_ROOT}/icds/lvp_icd.json>lib64/vulkan/lvp_icd.json"
      # Swiftshader
      "${PREBUILT_ROOT}/icds/vk_swiftshader.dll>lib64/vulkan/vk_swiftshader.dll"
      "${PREBUILT_ROOT}/icds/vk_swiftshader_icd.json>lib64/vulkan/vk_swiftshader_icd.json"
      # for translating shaders to SPIRV
      "${PREBUILT_ROOT}/glslangValidator.exe>lib64/vulkan/glslangValidator.exe"
      ${VULKAN_COMMON_DEPENDENCIES})
  set(VULKAN_TEST_DEPENDENCIES
      # Loader (for testing) - Use the unsafe variant to allow ICD changes with admin mode. ref: b/449967039
      "${PREBUILT_ROOT}/vulkan-1-unsafe.dll>testlib64/vulkan-1.dll"
      # Debug / validation layers
      "${PREBUILT_ROOT}/layers/VkLayer_api_dump.dll>testlib64/layers/VkLayer_api_dump.dll"
      "${PREBUILT_ROOT}/layers/VkLayer_api_dump.json>testlib64/layers/VkLayer_api_dump.json"
      "${PREBUILT_ROOT}/layers/VkLayer_gfxreconstruct.dll>testlib64/layers/VkLayer_gfxreconstruct.dll"
      "${PREBUILT_ROOT}/layers/VkLayer_gfxreconstruct.json>testlib64/layers/VkLayer_gfxreconstruct.json"
      "${PREBUILT_ROOT}/layers/VkLayer_khronos_validation.dll>testlib64/layers/VkLayer_khronos_validation.dll"
      "${PREBUILT_ROOT}/layers/VkLayer_khronos_validation.json>testlib64/layers/VkLayer_khronos_validation.json"
      "${PREBUILT_ROOT}/layers/VkLayer_monitor.dll>testlib64/layers/VkLayer_monitor.dll"
      "${PREBUILT_ROOT}/layers/VkLayer_monitor.json>testlib64/layers/VkLayer_monitor.json"
      "${PREBUILT_ROOT}/layers/VkLayer_screenshot.dll>testlib64/layers/VkLayer_screenshot.dll"
      "${PREBUILT_ROOT}/layers/VkLayer_screenshot.json>testlib64/layers/VkLayer_screenshot.json"
      # Shaders
      ${VULKAN_COMMON_DEPENDENCIES})
endif()
set(PACKAGE_EXPORT "VULKAN_DEPENDENCIES;VULKAN_TEST_DEPENDENCIES;VULKAN_FOUND")
android_license(
  TARGET VULKAN_DEPENDENCIES
  LIBNAME "vulkan-sdk"
  URL "https://vulkan.lunarg.com/sdk/home"
  SPDX "Apache-2.0"
  LICENSE
    "https://vulkan.lunarg.com/software/license/vulkan-1.3.290.0-linux-license-summary.txt"
  LOCAL "${ANDROID_QEMU2_TOP_DIR}/LICENSES/LICENSE.VULKAN")
