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
set(MACMU_ANDROID_JAR "${MACMU_ANDROID_SDK_ROOT}/platforms/android-35/android.jar")
set(MACMU_INPUT_SERVER_JAR "${CMAKE_CURRENT_BINARY_DIR}/macmu-input-server.jar")

if(MACMU_JAVAC AND MACMU_JAR AND MACMU_D8 AND EXISTS "${MACMU_ANDROID_JAR}")
    set(MACMU_INPUT_SERVER_CLASSES "${CMAKE_CURRENT_BINARY_DIR}/input-server-classes")
    set(MACMU_INPUT_SERVER_CLASSES_JAR "${CMAKE_CURRENT_BINARY_DIR}/macmu-input-server-classes.jar")
    add_custom_command(
        OUTPUT "${MACMU_INPUT_SERVER_JAR}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${MACMU_INPUT_SERVER_CLASSES}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${MACMU_INPUT_SERVER_CLASSES_JAR}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${MACMU_INPUT_SERVER_CLASSES}"
        COMMAND "${MACMU_JAVAC}" --release 17 -classpath "${MACMU_ANDROID_JAR}"
                -d "${MACMU_INPUT_SERVER_CLASSES}"
                "${CMAKE_CURRENT_SOURCE_DIR}/guest_input/MacMuInputServer.java"
        COMMAND "${MACMU_JAR}" --create --file "${MACMU_INPUT_SERVER_CLASSES_JAR}"
                -C "${MACMU_INPUT_SERVER_CLASSES}" .
        COMMAND "${MACMU_D8}" --min-api 23 --lib "${MACMU_ANDROID_JAR}"
                --output "${MACMU_INPUT_SERVER_JAR}"
                "${MACMU_INPUT_SERVER_CLASSES_JAR}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/guest_input/MacMuInputServer.java"
        VERBATIM
    )
    add_custom_target(macmu_input_server ALL DEPENDS "${MACMU_INPUT_SERVER_JAR}")
    add_dependencies(macmu_shell macmu_input_server)
    install(FILES "${MACMU_INPUT_SERVER_JAR}" DESTINATION lib)
else()
    message(WARNING "MacMu guest input server will not be built; set ANDROID_SDK_ROOT and install javac/d8.")
endif()

install(TARGETS macmu_shell
    RUNTIME DESTINATION .
)
install(FILES LICENSE
    DESTINATION .
    RENAME LICENSE.shell-MIT
)
