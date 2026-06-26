cmake_minimum_required(VERSION 3.20)

project(MacMuShell LANGUAGES CXX OBJCXX)

set(MACMU_BINARY_NAME "macmu" CACHE STRING "MacMu shell executable name")

find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(METALKIT_FRAMEWORK MetalKit REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)

add_executable(macmu_shell MacMu.mm)
set_target_properties(macmu_shell PROPERTIES
    OUTPUT_NAME "${MACMU_BINARY_NAME}"
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

install(TARGETS macmu_shell
    RUNTIME DESTINATION .
)
install(FILES LICENSE
    DESTINATION .
    RENAME LICENSE.shell-MIT
)
