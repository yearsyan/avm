cmake_minimum_required(VERSION 3.20)

project(MacMuShell LANGUAGES CXX OBJCXX)

# Emit compile_commands.json so clangd / VSCode intellisense can resolve the
# shell's flags (includes, -fobjc-arc on .mm, etc.) without a manual config.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(MACMU_BINARY_NAME "macmu" CACHE STRING "MacMu shell executable name")

find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(METALKIT_FRAMEWORK MetalKit REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)

add_executable(macmu_shell
    protocol/viewport.cpp
    core/frame_consumer.cpp
    core/guest_input_sender.cpp
    core/input_sender.cpp
    core/machine_manager.cpp
    core/qemu_launcher.cpp
    core/shell_options.cpp
    ui/macmu_input_view.mm
    ui/macmu_surface_renderer.mm
    ui/macmu_shell.mm
)
set_target_properties(macmu_shell PROPERTIES
    OUTPUT_NAME "${MACMU_BINARY_NAME}"
)
# All shell sources use flat #include "foo.h" regardless of which subdirectory
# the header lives in; expose the three layer directories so the includes
# resolve without rewriting a single #include line.
target_include_directories(macmu_shell PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol"
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui"
)
target_compile_features(macmu_shell PRIVATE cxx_std_17)
target_compile_options(macmu_shell PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)
target_link_libraries(macmu_shell PRIVATE
    "${APPKIT_FRAMEWORK}"
    "${IOSURFACE_FRAMEWORK}"
    "${METAL_FRAMEWORK}"
    "${METALKIT_FRAMEWORK}"
    "${QUARTZCORE_FRAMEWORK}"
)

set(MACMU_ANDROID_SDK_ROOT "$ENV{ANDROID_SDK_ROOT}" CACHE PATH "Android SDK root for MacMu guest helpers")
if(NOT MACMU_ANDROID_SDK_ROOT AND DEFINED ENV{ANDROID_HOME})
    set(MACMU_ANDROID_SDK_ROOT "$ENV{ANDROID_HOME}")
endif()
find_program(MACMU_JAVAC javac)
find_program(MACMU_JAR jar)
find_program(MACMU_D8 d8
    HINTS
        "${MACMU_ANDROID_SDK_ROOT}/build-tools/35.0.1"
        "${MACMU_ANDROID_SDK_ROOT}/build-tools/35.0.0"
)
find_program(MACMU_MKE2FS mke2fs
    HINTS
        "${MACMU_ANDROID_SDK_ROOT}/platform-tools"
)
find_program(MACMU_MAKE_EXT4FS emulator_make_ext4fs
    HINTS
        "${CMAKE_BINARY_DIR}/.."
        "${CMAKE_SOURCE_DIR}/../build/cmake"
)
find_program(MACMU_CPIO cpio)
find_program(MACMU_LZ4 lz4)
find_program(MACMU_PYTHON3 python3)
set(MACMU_ANDROID_JAR "${MACMU_ANDROID_SDK_ROOT}/platforms/android-35/android.jar")
set(MACMU_BASE_RAMDISK "$ENV{MACMU_BASE_RAMDISK}" CACHE FILEPATH
    "Base Android ramdisk.img used to build the MacMu guest ramdisk")
if(NOT MACMU_BASE_RAMDISK AND MACMU_ANDROID_SDK_ROOT)
    set(MACMU_BASE_RAMDISK
        "${MACMU_ANDROID_SDK_ROOT}/system-images/android-35/default/arm64-v8a/ramdisk.img")
endif()
set(MACMU_GUEST_AGENT_JAR "${CMAKE_CURRENT_BINARY_DIR}/macmu-agent.jar")
set(MACMU_GUEST_AGENT_IMAGE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/macmu-agent-image-root")
set(MACMU_GUEST_AGENT_EXT4 "${CMAKE_CURRENT_BINARY_DIR}/macmu-agent.ext4")
set(MACMU_GUEST_AGENT_IMAGE "${CMAKE_CURRENT_BINARY_DIR}/macmu-agent.img")
set(MACMU_GUEST_OVERLAY_DIR "${CMAKE_CURRENT_BINARY_DIR}/macmu-guest-overlay")
set(MACMU_GUEST_RAMDISK_OVERLAY "${CMAKE_CURRENT_BINARY_DIR}/macmu-guest-overlay.cpio")
set(MACMU_GUEST_RAMDISK_BASE_CPIO "${CMAKE_CURRENT_BINARY_DIR}/macmu-base-ramdisk.cpio")
set(MACMU_GUEST_RAMDISK_COMBINED_CPIO "${CMAKE_CURRENT_BINARY_DIR}/macmu-ramdisk.cpio")
set(MACMU_GUEST_RAMDISK "${CMAKE_CURRENT_BINARY_DIR}/macmu-ramdisk.img")

if(MACMU_JAVAC AND MACMU_JAR AND MACMU_D8 AND EXISTS "${MACMU_ANDROID_JAR}")
    set(MACMU_GUEST_AGENT_CLASSES "${CMAKE_CURRENT_BINARY_DIR}/guest-agent-classes")
    set(MACMU_GUEST_AGENT_CLASSES_JAR "${CMAKE_CURRENT_BINARY_DIR}/macmu-agent-classes.jar")
    add_custom_command(
        OUTPUT "${MACMU_GUEST_AGENT_JAR}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${MACMU_GUEST_AGENT_CLASSES}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${MACMU_GUEST_AGENT_CLASSES_JAR}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${MACMU_GUEST_AGENT_CLASSES}"
        COMMAND "${MACMU_JAVAC}" --release 17 -classpath "${MACMU_ANDROID_JAR}"
                -d "${MACMU_GUEST_AGENT_CLASSES}"
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_agent/MacMuAgent.java"
        COMMAND "${MACMU_JAR}" --create --file "${MACMU_GUEST_AGENT_CLASSES_JAR}"
                -C "${MACMU_GUEST_AGENT_CLASSES}" .
        COMMAND "${MACMU_D8}" --min-api 23 --lib "${MACMU_ANDROID_JAR}"
                --output "${MACMU_GUEST_AGENT_JAR}"
                "${MACMU_GUEST_AGENT_CLASSES_JAR}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/guest_agent/MacMuAgent.java"
        VERBATIM
    )
    add_custom_target(macmu_guest_agent ALL DEPENDS "${MACMU_GUEST_AGENT_JAR}")
    add_dependencies(macmu_shell macmu_guest_agent)
    install(FILES "${MACMU_GUEST_AGENT_JAR}" DESTINATION lib)

    if(MACMU_MAKE_EXT4FS AND MACMU_PYTHON3)
        add_custom_command(
            OUTPUT "${MACMU_GUEST_AGENT_IMAGE}"
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${MACMU_GUEST_AGENT_IMAGE_ROOT}"
            COMMAND "${CMAKE_COMMAND}" -E rm -f
                    "${MACMU_GUEST_AGENT_EXT4}"
                    "${MACMU_GUEST_AGENT_IMAGE}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${MACMU_GUEST_AGENT_IMAGE_ROOT}"
            COMMAND "${CMAKE_COMMAND}" -E copy
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_agent/macmu-init.rc"
                    "${MACMU_GUEST_AGENT_IMAGE_ROOT}/init.rc"
            COMMAND "${CMAKE_COMMAND}" -E copy
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_agent/macmu-agent.rc"
                    "${MACMU_GUEST_AGENT_IMAGE_ROOT}/macmu-agent.rc"
            COMMAND "${CMAKE_COMMAND}" -E copy
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_agent/init.ranchu.rc"
                    "${MACMU_GUEST_AGENT_IMAGE_ROOT}/init.ranchu.rc"
            COMMAND "${CMAKE_COMMAND}" -E copy
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_agent/fstab.ranchu"
                    "${MACMU_GUEST_AGENT_IMAGE_ROOT}/fstab.ranchu"
            COMMAND "${CMAKE_COMMAND}" -E copy
                    "${MACMU_GUEST_AGENT_JAR}"
                    "${MACMU_GUEST_AGENT_IMAGE_ROOT}/macmu-agent.jar"
            COMMAND /bin/chmod -R u=rwX,go=rX "${MACMU_GUEST_AGENT_IMAGE_ROOT}"
            COMMAND "${MACMU_MAKE_EXT4FS}" -l 8M -L macmu -S
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_agent/file_contexts"
                    "${MACMU_GUEST_AGENT_EXT4}"
                    "${MACMU_GUEST_AGENT_IMAGE_ROOT}"
            COMMAND "${MACMU_PYTHON3}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/wrap_ext4_gpt.py"
                    "${MACMU_GUEST_AGENT_EXT4}"
                    "${MACMU_GUEST_AGENT_IMAGE}"
                    macmu
            DEPENDS
                    "${MACMU_GUEST_AGENT_JAR}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_agent/macmu-init.rc"
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_agent/macmu-agent.rc"
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_agent/init.ranchu.rc"
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_agent/fstab.ranchu"
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_agent/file_contexts"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/wrap_ext4_gpt.py"
            VERBATIM
        )
        add_custom_target(macmu_guest_agent_image ALL
            DEPENDS "${MACMU_GUEST_AGENT_IMAGE}")
        add_dependencies(macmu_shell macmu_guest_agent_image)
        install(FILES "${MACMU_GUEST_AGENT_IMAGE}" DESTINATION lib)
    else()
        message(WARNING
            "MacMu guest agent image will not be built; emulator_make_ext4fs and python3 are required.")
    endif()

    if(MACMU_CPIO)
        add_custom_command(
            OUTPUT "${MACMU_GUEST_RAMDISK_OVERLAY}"
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${MACMU_GUEST_OVERLAY_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${MACMU_GUEST_OVERLAY_DIR}/system/etc"
            COMMAND "${CMAKE_COMMAND}" -E copy
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_agent/fstab.ranchu"
                    "${MACMU_GUEST_OVERLAY_DIR}/system/etc/fstab.ranchu"
            COMMAND /bin/chmod -R u=rwX,go=rX "${MACMU_GUEST_OVERLAY_DIR}"
            COMMAND /bin/sh -c
                    "cd '${MACMU_GUEST_OVERLAY_DIR}' && find . | sort | '${MACMU_CPIO}' -o -H newc > '${MACMU_GUEST_RAMDISK_OVERLAY}'"
            DEPENDS
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest_agent/fstab.ranchu"
            VERBATIM
        )
        add_custom_target(macmu_guest_ramdisk_overlay ALL
            DEPENDS "${MACMU_GUEST_RAMDISK_OVERLAY}")
        add_dependencies(macmu_shell macmu_guest_ramdisk_overlay)
        install(FILES "${MACMU_GUEST_RAMDISK_OVERLAY}" DESTINATION lib)

        if(MACMU_LZ4 AND MACMU_PYTHON3 AND EXISTS "${MACMU_BASE_RAMDISK}")
            add_custom_command(
                OUTPUT "${MACMU_GUEST_RAMDISK}"
                COMMAND /bin/sh -c
                        "'${MACMU_LZ4}' -dc '${MACMU_BASE_RAMDISK}' > '${MACMU_GUEST_RAMDISK_BASE_CPIO}'"
                COMMAND "${MACMU_PYTHON3}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/merge_newc.py"
                        "${MACMU_GUEST_RAMDISK_BASE_CPIO}"
                        "${MACMU_GUEST_RAMDISK_OVERLAY}"
                        "${MACMU_GUEST_RAMDISK_COMBINED_CPIO}"
                COMMAND "${MACMU_LZ4}" -l -f
                        "${MACMU_GUEST_RAMDISK_COMBINED_CPIO}"
                        "${MACMU_GUEST_RAMDISK}"
                DEPENDS
                        "${MACMU_BASE_RAMDISK}"
                        "${MACMU_GUEST_RAMDISK_OVERLAY}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/merge_newc.py"
                VERBATIM
            )
            add_custom_target(macmu_guest_ramdisk ALL
                DEPENDS "${MACMU_GUEST_RAMDISK}")
            add_dependencies(macmu_shell macmu_guest_ramdisk)
            install(FILES "${MACMU_GUEST_RAMDISK}" DESTINATION lib)
        else()
            message(WARNING
                "MacMu full guest ramdisk will not be built; set MACMU_BASE_RAMDISK and install lz4/python3.")
        endif()
    else()
        message(WARNING "MacMu guest ramdisk overlay will not be built; cpio is required.")
    endif()
else()
    message(WARNING "MacMu guest RPC agent will not be built; set ANDROID_SDK_ROOT and install javac/d8.")
endif()

install(TARGETS macmu_shell
    RUNTIME DESTINATION .
)
install(FILES LICENSE
    DESTINATION .
    RENAME LICENSE.shell-MIT
)
