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
if(TARGET emulator)
  set(AEMU_RUNTIME_DEPENDENCY_TARGET emulator)
else()
  set(AEMU_RUNTIME_DEPENDENCY_TARGET aemu-headless-runtime-dependencies)
  add_library(${AEMU_RUNTIME_DEPENDENCY_TARGET} ${ANDROID_QEMU2_TOP_DIR}/dummy.c)
  android_license(TARGET ${AEMU_RUNTIME_DEPENDENCY_TARGET} LIBNAME None SPDX None
    LICENSE None LOCAL None)
endif()
android_target_dependency(${AEMU_RUNTIME_DEPENDENCY_TARGET} all EMULATOR_FEATURE_DEPENDENCIES)
android_target_dependency(${AEMU_RUNTIME_DEPENDENCY_TARGET} all EMULATOR_KEYMAP_DEPENDENCIES)
android_target_dependency(${AEMU_RUNTIME_DEPENDENCY_TARGET} all EMULATOR_BIOS_DEPENDENCIES)
android_target_dependency(${AEMU_RUNTIME_DEPENDENCY_TARGET} windows SWIFTSHADER_DEPENDENCIES)
android_target_dependency(${AEMU_RUNTIME_DEPENDENCY_TARGET} linux SWIFTSHADER_DEPENDENCIES)
android_target_dependency(${AEMU_RUNTIME_DEPENDENCY_TARGET} linux LLVMPIPE_DEPENDENCIES)
if(NOT LINUX_AARCH64)
  android_target_dependency(${AEMU_RUNTIME_DEPENDENCY_TARGET} all ANGLE_DEPENDENCIES)
  android_target_dependency(${AEMU_RUNTIME_DEPENDENCY_TARGET} all ANGLE_RENDERER_DEPENDENCIES)
endif()
android_target_dependency(${AEMU_RUNTIME_DEPENDENCY_TARGET} all RUNTIME_OS_DEPENDENCIES)
android_target_dependency(${AEMU_RUNTIME_DEPENDENCY_TARGET} all E2FSPROGS_DEPENDENCIES)
android_target_dependency(${AEMU_RUNTIME_DEPENDENCY_TARGET} all CERT_DEPEPENDENCIES)
android_target_dependency(${AEMU_RUNTIME_DEPENDENCY_TARGET} all EMULATOR_HW_DEPENDENCIES)
android_target_dependency(${AEMU_RUNTIME_DEPENDENCY_TARGET} all VULKAN_DEPENDENCIES)
android_target_dependency(${AEMU_RUNTIME_DEPENDENCY_TARGET} linux TCMALLOC_OS_DEPENDENCIES)
android_target_dependency(${AEMU_RUNTIME_DEPENDENCY_TARGET} windows_msvc-x86_64 MSVC_DEPENDENCIES)
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
if(TARGET emulator)
  android_target_properties(emulator all "${RUNTIME_OS_PROPERTIES}")
endif()

# Install files we need.
install(FILES ${ANDROID_QEMU2_TOP_DIR}/LICENSES/LICENSE.APACHE2 DESTINATION .
  RENAME LICENSE)

# Duplicate install for windows (emulator-check needs these)
if(WINDOWS_MSVC_X86_64)
  install(TARGETS android-emu-agents libprotobuf RUNTIME DESTINATION .)
endif()
