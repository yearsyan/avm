# AEMU core-only / no-Qt: no QT5/VIRTUALSCENE/SKIN/MACROS prebuilts.
prebuilt(E2FSPROGS)
prebuilt(MSVC)
prebuilt(VULKAN)

function(aemu_core_only_install_prebuilt_files DEPENDENCIES_TARGET)
  foreach(DEP ${${DEPENDENCIES_TARGET}})
    if(DEP MATCHES ".*>>.*" OR DEP MATCHES ".*>~>.*")
      message(
        FATAL_ERROR
          "AEMU core-only install only supports file dependencies: ${DEP}")
    endif()
    string(REPLACE ">" ";" SRC_DST ${DEP})
    list(GET SRC_DST 0 SRC)
    list(GET SRC_DST 1 DST)
    get_filename_component(DST_DIR "${DST}" DIRECTORY)
    internal_android_install_file("${SRC}" "${DST_DIR}" "${DEPENDENCIES_TARGET}")
  endforeach()
endfunction()

android_add_executable(
  TARGET emulator INSTALL .
  LICENSE Apache-2.0
  SRC main-emulator.cpp
  DEPS aemu-gl-init
  android-emu-agents-headers
  android-emu-avd
  android-emu-base
  android-emu-base-headers
  android-emu-crash-initializer
  android-emu-crashreport
  android-emu-crashreport-consent-never
  android-emu-files
  android-emu-launch
  android-emu-utils
  android-hw-config
  qemu-host-common-headers
  ui::common
  ui::window)


add_resource(TARGET emulator RESOURCE ../images/emulator_icon.rc)
target_include_directories(
  emulator # This is not right!, an implicit dependency..
  PRIVATE ${ANDROID_QEMU2_TOP_DIR}/android/android-emu)
target_compile_definitions(
  emulator
  PRIVATE "-DCRASHUPLOAD=${OPTION_CRASHUPLOAD}"
  "-DANDROID_SDK_TOOLS_REVISION=${OPTION_SDK_TOOLS_REVISION}"
  "-DANDROID_SDK_TOOLS_BUILD_NUMBER=${OPTION_SDK_TOOLS_BUILD_NUMBER}")
if(NOT WINDOWS)
  target_link_libraries(emulator PRIVATE android-emu-feature)
endif()
# Make sure we have the qemu dependencies available.
# AEMU core-only / macOS-aarch64: only the aarch64 headless engine is built.
if(BUILDING_FOR_AARCH64)
  add_dependencies(emulator qemu-system-aarch64-headless)
else()
  add_dependencies(emulator qemu-system-x86_64-headless
                          qemu-system-aarch64-headless)
endif()

set(PC_BIOS "${ANDROID_QEMU2_TOP_DIR}/pc-bios")
set(EMULATOR_BIOS_DEPENDENCIES
  "${PC_BIOS}/bios.bin>lib/pc-bios/bios.bin"
  "${PC_BIOS}/vgabios-cirrus.bin>lib/pc-bios/vgabios-cirrus.bin"
  "${PC_BIOS}/vgabios-stdvga.bin>lib/pc-bios/vgabios-stdvga.bin"
  "${PC_BIOS}/vgabios-virtio.bin>lib/pc-bios/vgabios-virtio.bin"
  "${PC_BIOS}/bios-256k.bin>lib/pc-bios/bios-256k.bin"
  "${PC_BIOS}/efi-virtio.rom>lib/pc-bios/efi-virtio.rom"
  "${PC_BIOS}/efi-e1000.rom>lib/pc-bios/efi-e1000.rom"
  "${PC_BIOS}/efi-e1000e.rom>lib/pc-bios/efi-e1000e.rom"
  "${PC_BIOS}/kvmvapic.bin>lib/pc-bios/kvmvapic.bin"
  "${PC_BIOS}/linuxboot.bin>lib/pc-bios/linuxboot.bin"
  "${PC_BIOS}/multiboot.bin>lib/pc-bios/multiboot.bin"
  "${PC_BIOS}/linuxboot_dma.bin>lib/pc-bios/linuxboot_dma.bin")
android_license(
  TARGET EMULATOR_BIOS_DEPENDENCIES
  LIBNAME bios
  URL "https://android.googlesource.com/platform/external/qemu-pc-bios/+/refs/heads/master/"
  SPDX "LGPL-2.0-or-later"
  LICENSE
  "https://android.googlesource.com/platform/external/qemu-pc-bios/+/refs/heads/master/README"
  LOCAL "${ANDROID_QEMU2_TOP_DIR}/LICENSES/LICENSE.BIOS")

set(EMULATOR_KEYMAP_DEPENDENCIES
  "${ANDROID_QEMU2_TOP_DIR}/pc-bios/keymaps/*>>lib/pc-bios/keymaps")
set(CERT_DEPEPENDENCIES
  "${ANDROID_QEMU2_TOP_DIR}/android/data/ca-bundle.pem>lib/ca-bundle.pem")
set(EMULATOR_HW_DEPENDENCIES "${HW_PROPERTIES_INI}>lib/hardware-properties.ini")

# Install all the depencies in the right place during build.
# AEMU core-only / no-Qt: no QT5/VIRTUAL_SCENE/SKIN/MACROS dependencies.
android_target_dependency(emulator all EMULATOR_FEATURE_DEPENDENCIES)
android_target_dependency(emulator all EMULATOR_KEYMAP_DEPENDENCIES)
android_target_dependency(emulator all EMULATOR_BIOS_DEPENDENCIES)
android_target_dependency(emulator windows SWIFTSHADER_DEPENDENCIES)
android_target_dependency(emulator linux SWIFTSHADER_DEPENDENCIES)
android_target_dependency(emulator linux LLVMPIPE_DEPENDENCIES)
if(NOT LINUX_AARCH64)
  android_target_dependency(emulator all ANGLE_DEPENDENCIES)
  android_target_dependency(emulator all ANGLE_RENDERER_DEPENDENCIES)
endif()
android_target_dependency(emulator all RUNTIME_OS_DEPENDENCIES)
android_target_dependency(emulator all E2FSPROGS_DEPENDENCIES)
android_target_dependency(emulator all CERT_DEPEPENDENCIES)
android_target_dependency(emulator all EMULATOR_HW_DEPENDENCIES)
android_target_dependency(emulator all VULKAN_DEPENDENCIES)
android_target_dependency(emulator linux TCMALLOC_OS_DEPENDENCIES)
android_target_dependency(emulator windows_msvc-x86_64 MSVC_DEPENDENCIES)
# And install them for a release.
android_install_dependency(all EMULATOR_FEATURE_DEPENDENCIES)
android_install_dependency(all EMULATOR_KEYMAP_DEPENDENCIES)
android_install_dependency(all EMULATOR_BIOS_DEPENDENCIES)
android_install_dependency(windows SWIFTSHADER_DEPENDENCIES)
android_install_dependency(linux SWIFTSHADER_DEPENDENCIES)
android_install_dependency(linux LLVMPIPE_DEPENDENCIES)
if(NOT LINUX_AARCH64)
  android_install_dependency(all ANGLE_DEPENDENCIES)
  android_install_dependency(all ANGLE_RENDERER_DEPENDENCIES)
endif()

android_install_dependency(all RUNTIME_OS_DEPENDENCIES)
if(TARGET RUNTIME_OS_DEPENDENCIES)
  install(TARGETS RUNTIME_OS_DEPENDENCIES LIBRARY DESTINATION lib64
          RUNTIME DESTINATION lib64)
endif()
android_install_dependency_force_exec(all E2FSPROGS_DEPENDENCIES)
android_install_dependency(all CERT_DEPEPENDENCIES)
android_install_dependency(all EMULATOR_HW_DEPENDENCIES)
# AEMU core-only: Vulkan prebuilts are installed file-by-file.
aemu_core_only_install_prebuilt_files(VULKAN_DEPENDENCIES)
android_install_dependency(linux TCMALLOC_OS_DEPENDENCIES)
android_install_dependency(windows_msvc-x86_64 ANGLE_RENDERER_DEPENDENCIES)
android_install_dependency(windows_msvc-x86_64 MSVC_DEPENDENCIES)

# Text files have no licenses.
android_license(TARGET "SKIN_DEPENDENCIES" LIBNAME None SPDX None
  LICENSE None LOCAL None)
android_license(TARGET "EMULATOR_KEYMAP_DEPENDENCIES" LIBNAME None SPDX None
  LICENSE None LOCAL None)
android_license(TARGET "CERT_DEPEPENDENCIES" LIBNAME None SPDX None LICENSE None
  LOCAL None)
android_license(TARGET "EMULATOR_HW_DEPENDENCIES" LIBNAME None SPDX None
  LICENSE None LOCAL None)
android_license(TARGET "EMULATOR_FEATURE_DEPENDENCIES" LIBNAME None SPDX None
  LICENSE None LOCAL None)

# And set the os specific properties
android_target_properties(emulator all "${RUNTIME_OS_PROPERTIES}")

# Install files we need.
install(FILES ${ANDROID_QEMU2_TOP_DIR}/LICENSES/LICENSE.APACHE2 DESTINATION .
  RENAME LICENSE)

# Duplicate install for windows (emulator-check needs these)
if(WINDOWS_MSVC_X86_64)
  install(TARGETS android-emu-agents libprotobuf RUNTIME DESTINATION .)
endif()
