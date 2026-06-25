# TODO(joshuaduong, b/231626582): Move/consolidate this file to vulkan-cereal.
prebuilt(ANGLE)
prebuilt(VULKAN)

add_definitions("-DAEMU_BUILD")

function(add_opengl_dependencies TGT)
    android_target_dependency(${TGT} windows SWIFTSHADER_DEPENDENCIES)
    # b/257077718 Linux presubmit is flaky with SwANGLE
    android_target_dependency(${TGT} linux SWIFTSHADER_DEPENDENCIES)
    android_target_dependency(${TGT} linux LLVMPIPE_DEPENDENCIES)
    android_target_dependency(${TGT} darwin ANGLE_RENDERER_DEPENDENCIES)
    android_target_dependency(${TGT} all ANGLE_DEPENDENCIES)
    android_target_dependency(${TGT} all VULKAN_DEPENDENCIES)
    android_target_properties(
        ${TGT}
        linux
        "LINK_FLAGS<=-Wl,-rpath,'$ORIGIN/lib64/gles_swiftshader' -Wl,--disable-new-dtags"
      )
    android_target_properties(
        ${TGT} darwin "${RUNTIME_OS_PROPERTIES};INSTALL_RPATH>=@loader_path/lib64/gles_angle")
endfunction()

# GFXSTREAM BACKEND
if(TRUE)
    set(GFXSTREAM_REPO_ROOT ${ANDROID_QEMU2_TOP_DIR}/../../hardware/google/gfxstream)
    set(AEMU_COMMON_REPO_ROOT ${ANDROID_QEMU2_TOP_DIR}/../../hardware/google/aemu)
    set(ANGLE_REPO_ROOT ${ANDROID_QEMU2_TOP_DIR}/../../external/angle)

    set(BUILD_STANDALONE OFF)
    # AEMU core-only: VKCEREAL tests are disabled (forced OFF).

    if(NOT LINUX_AARCH64)
        set(ASTC_CPU_DECODING ON)
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
        cmake_path(IS_PREFIX GFXSTREAM_THIRD_PARTY_ROOT ${abs_subdir} NORMALIZE IS_THIRD_PARTY)
        if(IS_THIRD_PARTY)
            return()
        endif()
        cmake_path(IS_PREFIX GFXSTREAM_REPO_ROOT ${abs_subdir} NORMALIZE IS_GFXSTREAM_SUBDIR)
        cmake_path(IS_PREFIX AEMU_COMMON_REPO_ROOT ${abs_subdir} NORMALIZE IS_AEMU_COMMON_SUBDIR)
        cmake_path(IS_PREFIX GFXSTREAM_PROTOCOLS_DIR ${abs_subdir} NORMALIZE IS_GFXSTREAM_PROTOCOLS_SUBDIR)
        if(NOT IS_GFXSTREAM_SUBDIR AND NOT IS_AEMU_COMMON_SUBDIR AND NOT IS_GFXSTREAM_PROTOCOLS_SUBDIR)
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

    android_add_library(
        TARGET aemu-vkcereal-base
        LICENSE Apache-2.0
        SRC impl/base/HealthMonitor.cpp
            impl/base/System.cpp
            impl/base/SharedLibrary.cpp)
    target_link_libraries(
        aemu-vkcereal-base
        PRIVATE
        aemu-base.headers
        android-emu
        android-emu-base-headers
        android-emu-crashreport
        android-emu-crashreport-consent-no-ui)
    # Allows us a way to include headers from both aemu's base and hardware/google/aemu base.
    target_include_directories(
        aemu-vkcereal-base
        PRIVATE
        ${ANDROID_QEMU2_TOP_DIR}/android)
    set(GFXSTREAM_BASE_LIB aemu-vkcereal-base)

    android_add_library(
        TARGET aemu-vkcereal-host-common
        LICENSE Apache-2.0
        SRC impl/host-common/globals.cpp
            impl/host-common/crash_reporter.cpp
            impl/host-common/AemuGraphicsAgents.cpp)
    target_link_libraries(
        aemu-vkcereal-host-common
        PUBLIC
        emugl_common
        android-emu
        android-emu-agents
        PRIVATE
        android-emu-base-headers
        aemu-vkcereal-base
        logging-base # TODO(joshuaduong): Replace the logging impl with our own
        )
    # We define some of vkcereal's base API in android-emu (e.g. resolveLayout), so we can't simply
    # link to android-emu-base.
    set(GFXSTREAM_HOST_COMMON_LIB aemu-vkcereal-host-common)
    add_library(gfxstream-gl-host-common INTERFACE)
    target_link_libraries(
        gfxstream-gl-host-common
        INTERFACE aemu-vkcereal-host-common)

    if (WIN32)
        target_compile_definitions(aemu-vkcereal-base PUBLIC "-DUNICODE -D_UNICODE -DNOMINMAX -DEMUGL_BUILD -DVK_USE_PLATFORM_WIN32_KHR -DSTATIC_EMUGL")
        option(RECORDER_DELEGATE_LIB "Use recorder_delegate_lib dll" OFF)
        if(RECORDER_DELEGATE_LIB)
            add_definitions(-DRECORDER_DELEGATE_LIB)
        endif()
    endif()

    option(VIRGL_RENDERER_UNSTABLE_APIS "Use unstable virglrenderer APIs" OFF)
    if(VIRGL_RENDERER_UNSTABLE_APIS)
        add_definitions(-DVIRGL_RENDERER_UNSTABLE_APIS)
    endif()

    if(UNIX AND NOT APPLE)
        set(LINUX TRUE)
    endif()

    # TODO(http://b/257068626): bring back ANGLE shader translator
    option(USE_ANGLE_SHADER_PARSER "Build with ANGLE shader parser." OFF)

    find_package(Threads)
    include(ExternalProject)

    # AEMU core-only: gfxstream unit tests are not built, so GoogleTest /
    # gtest_discover_tests are not needed.

    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_C_STANDARD 11)

    set(CONFIG_AEMU TRUE CACHE BOOL "Enable aemu-specific code in vulkan-cereal")

    if (APPLE)
        add_compile_definitions(VK_USE_PLATFORM_MACOS_MVK)
        add_compile_definitions(VK_USE_PLATFORM_METAL_EXT)
    elseif(WIN32)
        add_compile_definitions(VK_USE_PLATFORM_WIN32_KHR)
    elseif(UNIX)
        add_compile_definitions(VK_USE_PLATFORM_XCB_KHR)
    endif()
    add_compile_definitions(USE_ANGLE_SHADER_PARSER)

    add_compile_definitions(GLM_FORCE_RADIANS)
    add_compile_definitions(GLM_FORCE_DEFAULT_ALIGNED_GENTYPES)
    add_compile_definitions(VK_GFXSTREAM_STRUCTURE_TYPE_EXT)

    if (MSVC)
        # ask msvc not to warn not secure C ISO functions
        add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
        # ask msvc not to warn non C ISO POSIX functions
        add_compile_definitions(_CRT_NONSTDC_NO_DEPRECATE)
    endif()

    # Uncomment for ASAN support
    # set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
    # set (CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fno-omit-frame-pointer -fsanitize=address")

    # For empty struct size warning in C vs C++
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DGFXSTREAM_ENABLE_HOST_GLES=1 -Wno-extern-c-compat")

    if (WIN32)
        if (BUILD_ASAN_WIN32)
            set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
            # ASAN does not work with flag /MDd, replace it with /MD
            string(REPLACE "/MDd" "/MD" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
            set(CMAKE_CXX_FLAGS_DEBUG ${CMAKE_CXX_FLAGS_DEBUG})

            # ASAN linker
            # User needs to use -D ASAN_LIB_DIR:STRING=/path/to/asan_libs to add library directory
            if (NOT DEFINED ASAN_LIB_DIR)
                message(FATAL_ERROR "Please input ASAN library path with -D ASAN_LIB_DIR:STRING=/path/to/asan_lib_dir")
            endif()
            link_libraries(clang_rt.asan_dynamic-x86_64.lib clang_rt.asan_dynamic_runtime_thunk-x86_64.lib)
            message("Linking ASAN libraries from: ${ASAN_LIB_DIR}")
            link_directories(${ASAN_LIB_DIR})
        endif()
    endif()

    set(GFXSTREAM_PREFIX "gfxstream")
    set(GFXSTREAM_PROTOCOLS_DIR ${ANDROID_QEMU2_TOP_DIR}/../../hardware/google/gfxstream)

    set(EXTRA_SUBDIR_TEST_INCLUDE_FILES)
    # Third party dependencies
    add_subdirectory(${GFXSTREAM_REPO_ROOT}/third_party/stb ${GFXSTREAM_PREFIX}-third-party-stb)
    # TODO(http://b/257068626): bring back ANGLE shader translator
    # add_library(angle_shader_translator SHARED IMPORTED)
    # set_target_properties(angle_shader_translator PROPERTIES IMPORTED_LOCATION "${ANDROID_QEMU2_TOP_DIR}/../../prebuilts/android-emulator-build/common/ANGLE/${ANDROID_TARGET_OS_FLAVOR}-${ANDROID_TARGET_ARCH}/lib/libshadertranslator${CMAKE_SHARED_LIBRARY_SUFFIX}")

    # Backends######################################################################
    add_subdirectory(${GFXSTREAM_REPO_ROOT}/third_party/drm ${GFXSTREAM_PREFIX}-third-party-drm)
    add_subdirectory(${GFXSTREAM_REPO_ROOT}/third_party/opengl ${GFXSTREAM_PREFIX}-third-party-opengl)
    add_subdirectory(${GFXSTREAM_REPO_ROOT}/third_party/renderdoc ${GFXSTREAM_PREFIX}-third-party-renderdoc)
    add_subdirectory(${GFXSTREAM_REPO_ROOT}/third_party/x11 ${GFXSTREAM_PREFIX}-third-party-x11)
    add_subdirectory(${GFXSTREAM_REPO_ROOT}/third_party/vulkan ${GFXSTREAM_PREFIX}-third-party-vulkan)
    add_subdirectory(${GFXSTREAM_REPO_ROOT}/host ${GFXSTREAM_PREFIX}-host)
    target_link_libraries(
        GLES_V2_translator_static PRIVATE ANGLE::ANGLE
    )

    # Fake Android guest#########################3##################################

    if (ENABLE_VKCEREAL_TESTS)
        list(APPEND EXTRA_SUBDIR_TEST_INCLUDE_FILES ${GFXSTREAM_REPO_ROOT}/toolchain/cmake/test_properties.cmake)
    endif()

    include(${GFXSTREAM_REPO_ROOT}/toolchain/cmake/SetSubdirectorProperties.cmake)

    # AEMU core-only: gfxstream unit tests are not built.

    get_directory_property(GFXSTREAM_SUBDIRS SUBDIRECTORIES)
    foreach(GFXSTREAM_SUBDIR IN LISTS GFXSTREAM_SUBDIRS)
        discover_gfxstream_internal_targets(GFXSTREAM_INTERNAL_TARGETS ${GFXSTREAM_SUBDIR})
    endforeach()
    set(GFXSTREAM_DEPENDENCIES
        ${GFXSTREAM_INTERNAL_TARGETS}
        angle_common
        angle_shader_translator
        renderdoc
        gfxstream_common_base
        gfxstream_common_base_windows_compat
        gfxstream_drm_headers
        gfxstream_egl_headers
        gfxstream_glm_headers
        gfxstream_host_address_space
        gfxstream_host_address_space.headers
        gfxstream_host_tracing
        gfxstream_opengl_headers
        gfxstream_vulkan_headers
        gfxstream_x11_headers)
   android_license(TARGET "${GFXSTREAM_DEPENDENCIES}"
                   LIBNAME gfxstream
                   URL "https://android.googlesource.com/platform/hardware/google/gfxstream/+/refs/heads/master"
                   SPDX Apache-2.0
                   LOCAL "${ANDROID_QEMU2_TOP_DIR}/../../hardware/google/gfxstream/LICENSE"
                   LICENSE "https://android.googlesource.com/platform/hardware/google/gfxstream/+/refs/heads/master/LICENSE")
endif()
