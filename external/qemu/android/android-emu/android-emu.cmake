# This file defines android-emu library

# This is a minimal emu-android library needed to start the emulator. - It has a
# minimal number of dependencies - It should *NEVER* rely on a shared library
# (binplace issues MSVC) - It is used by the "emulator" launcher binary to
# provide basic functionality (launch, find avds, list camera's etc.)
#
# This allows us to create shared libraries without negatively impacting the
# launcher.
set(android-emu-launch-deps
    android-emu-agents-headers
    android-emu-base
    android-emu-base-headers
    android-emu-cmdline
    android-emu-files
    android-emu-hardware
    android-emu-min-avd
    android-emu-utils
    android-hw-config
    ui::common)


set(android-emu-launch-src
    android/emulation/launcher/launcher_stub.cpp android/main-help.cpp)
set(android-emu-launch-windows-src android/emulation/USBAssist.cpp)
set(android-emu-launch-linux-src)
set(android-emu-launch-darwin-src)

  list(APPEND android-emu-launch-src android/camera/camera-list.cpp)

android_add_library(
  TARGET android-emu-launch
  LICENSE Apache-2.0
  SRC ${android-emu-launch-src}
  WINDOWS ${android-emu-launch-windows-src}
  LINUX ${android-emu-launch-linux-src}
  DARWIN ${android-emu-launch-darwin-src}
  DEPS ${android-emu-launch-deps})
target_include_directories(android-emu-launch PRIVATE .)
target_compile_options(android-emu-launch PRIVATE -Wno-extern-c-compat)
target_compile_definitions(android-emu-launch PRIVATE AEMU_MIN AEMU_LAUNCHER)
target_include_directories(
  android-emu-launch
  PRIVATE
    ${ANDROID_QEMU2_TOP_DIR}/android-qemu2-glue/config/${ANDROID_TARGET_TAG})


android_target_link_libraries(
  android-emu-launch windows PRIVATE emulator-libusb ole32::ole32
                                     setupapi::setupapi mfuuid::mfuuid)

# This is the set of sources that are common in both the shared libary and the
# archive. We currently have to split them up due to dependencies on external
# variables/functions that are implemented in other libraries.
set(android-emu-common
    ../emu/avd/src/android/avd/BugreportInfo.cpp # TODO(jansene) Move once the
                                                 # dependency has been truly
                                                 # resolved.
    android/adb-server.cpp
    android/base/async/CallbackRegistry.cpp
    android/base/LayoutResolver.cpp
    android/bootconfig.cpp
    android/car-cluster.cpp
    android/car-physics.cpp
    android/car.cpp
    android/emulation/AdbDebugPipe.cpp
    android/emulation/AdbGuestPipe.cpp
    android/emulation/AdbHostListener.cpp
    android/emulation/AdbHub.cpp
    android/emulation/AdbMessageSniffer.cpp
    android/emulation/AdbVsockPipe.cpp
    android/emulation/address_space_device.cpp
    android/emulation/address_space_graphics.cpp
    android/emulation/address_space_host_media.cpp
    android/emulation/address_space_host_memory_allocator.cpp
    android/emulation/address_space_shared_slots_host_memory_allocator.cpp
    android/emulation/AudioCaptureEngine.cpp
    android/emulation/AudioOutputEngine.cpp
    android/emulation/ComponentVersion.cpp
    android/emulation/control/AgentLogger.cpp
    android/emulation/control/ApkInstaller.cpp
    android/emulation/control/BootCompletionHandler.cpp
    android/emulation/control/FilePusher.cpp
    android/emulation/control/GooglePlayServices.cpp
    android/emulation/control/ServiceUtils.cpp
    android/emulation/DmaMap.cpp
    android/emulation/GoldfishDma.cpp
    # android/emulation/H264NaluParser.cpp
    # android/emulation/H264PingInfoParser.cpp
    # android/emulation/HevcNaluParser.cpp
    # android/emulation/HevcPingInfoParser.cpp
    android/emulation/hostdevices/HostAddressSpace.cpp
    android/emulation/hostdevices/HostGoldfishPipe.cpp
    android/emulation/HostmemIdMapping.cpp
    android/emulation/AutoDisplays.cpp
    # android/emulation/MediaFfmpegVideoHelper.cpp
    # android/emulation/MediaH264Decoder.cpp
    # android/emulation/MediaH264DecoderDefault.cpp
    # android/emulation/MediaH264DecoderGeneric.cpp
    # android/emulation/MediaHevcDecoder.cpp
    # android/emulation/MediaHevcDecoderDefault.cpp
    # android/emulation/MediaHevcDecoderGeneric.cpp
    # android/emulation/MediaHostRenderer.cpp
    # android/emulation/MediaSnapshotHelper.cpp
    # android/emulation/MediaSnapshotState.cpp
    # android/emulation/MediaTexturePool.cpp
    # android/emulation/MediaVideoHelper.cpp
    # android/emulation/MediaVpxDecoder.cpp
    # android/emulation/MediaVpxDecoderGeneric.cpp
    # android/emulation/MediaVpxVideoHelper.cpp
    android/emulation/MultiDisplay.cpp
    android/emulation/MultiDisplayPipe.cpp
    android/emulation/SetupParameters.cpp
    android/emulation/USBAssist.cpp
    # android/emulation/VpxFrameParser.cpp
    # android/emulation/VpxPingInfoParser.cpp
    android/error-messages.cpp
    android/jdwp/JdwpProxy.cpp
    android/jpeg-compress.c
    android/kernel/kernel_utils.cpp
    android/main-help.cpp
    android/main-kernel-parameters.cpp
    android/metrics/DependentMetrics.cpp
    android/metrics/PerfStatReporter.cpp
    android/multitouch-port.c
    android/multitouch-screen.c
    android/network/globals.c
    android/network/control.cpp
    android/network/wifi.cpp
    android/opengl/GLProcessPipe.cpp
    android/opengl/GpuFrameBridge.cpp
    android/opengl/OpenglEsPipe.cpp
    android/opengles.cpp
    android/process_setup.cpp
    android/qemu-tcpdump.c
    android/sensor_mock/SensorMockUtils.cpp
    android/shaper.c
    android/snaphost-android.c
    android/snapshot.c
    android/snapshot/common.cpp
    android/snapshot/Compressor.cpp
    android/snapshot/Decompressor.cpp
    android/snapshot/GapTracker.cpp
    android/snapshot/Hierarchy.cpp
    android/snapshot/IncrementalStats.cpp
    android/snapshot/interface.cpp
    android/snapshot/Loader.cpp
    android/snapshot/MemoryWatch_common.cpp
    android/snapshot/PathUtils.cpp
    android/snapshot/Quickboot.cpp
    android/snapshot/RamLoader.cpp
    android/snapshot/RamSaver.cpp
    android/snapshot/RamSnapshotTesting.cpp
    android/snapshot/Saver.cpp
    android/snapshot/Snapshot.cpp
    android/snapshot/Snapshotter.cpp
    android/snapshot/SnapshotUtils.cpp
    android/snapshot/TextureLoader.cpp
    android/snapshot/TextureSaver.cpp
    android/verified-boot/load_config.cpp)

# These are the set of sources for which we know we have dependencies. You can
# use this as a starting point to figure out what can move to a seperate library
set(android_emu_dependent_src
    android/automation/AutomationController.cpp
    android/automation/AutomationEventSink.cpp
    android/camera/camera-common.cpp
    android/camera/camera-format-converters.c
    android/camera/camera-list.cpp
    android/camera/camera-metrics.cpp
    android/camera/camera-service.cpp
    android/camera/camera-sws-format-converter.cpp
    android/camera/camera-virtualscene-utils.cpp
    android/camera/camera-virtualscene.cpp
    android/console.cpp
    android/emulation/FakeRotatingCameraSensor.cpp
    android/emulation/HostMemoryService.cpp
    android/emulation/QemuMiscPipe.cpp
    android/emulation/resizable_display_config.cpp
    android/hw-sensors.cpp
    android/userspace-boot-properties.cpp
    android/main-common.c
    android/offworld/OffworldPipe.cpp
    android/physics/AmbientEnvironment.cpp
    android/physics/BodyModel.cpp
    android/physics/FoldableModel.cpp
    android/physics/InertialModel.cpp
    android/physics/PhysicalModel.cpp
    android/qemu-setup.cpp
    android/sensors-port.c
    android/snapshot/Icebox.cpp
    android/snapshot/SnapshotAPI.cpp
    android/virtualscene/SceneCamera.cpp
    android/virtualscene/VirtualSceneManager.cpp
    android/virtualscene/WASDInputHandler.cpp
    android/xr/XrService.cpp)

  list(REMOVE_ITEM android-emu-common
       android/emulation/address_space_host_media.cpp)
  list(REMOVE_ITEM android_emu_dependent_src
       android/automation/AutomationController.cpp
       android/automation/AutomationEventSink.cpp
       android/camera/camera-common.cpp
       android/camera/camera-format-converters.c
       android/console.cpp
       android/camera/camera-metrics.cpp
       android/camera/camera-service.cpp
       android/camera/camera-sws-format-converter.cpp
       android/camera/camera-virtualscene-utils.cpp
       android/camera/camera-virtualscene.cpp
       android/emulation/FakeRotatingCameraSensor.cpp
       android/offworld/OffworldPipe.cpp
       android/virtualscene/SceneCamera.cpp
       android/virtualscene/VirtualSceneManager.cpp
       android/virtualscene/WASDInputHandler.cpp
       android/xr/XrService.cpp)
  list(APPEND android_emu_dependent_src android/console-core-stub.cpp)


# The standard archive has all the sources, including those that have external
# dependencies that we have not properly declared yet.
# TODO(jansene): Properly clean up the mutual dependencies and make sure they
# are not circular
list(APPEND android-emu_src ${android-emu-common} ${android_emu_dependent_src})

set(android-emu-windows-src
    android/camera/camera-capture-windows.cpp
    android/snapshot/MemoryWatch_windows.cpp)
set(android-emu-linux-src
    android/camera/camera-capture-linux.c
    android/snapshot/MemoryWatch_linux.cpp)
set(android-emu-darwin-src
    android/camera/camera-capture-mac.m
    android/snapshot/MacSegvHandler.cpp
    android/snapshot/MemoryWatch_darwin.cpp)

  list(REMOVE_ITEM android-emu-windows-src
       android/camera/camera-capture-windows.cpp)
  list(REMOVE_ITEM android-emu-linux-src android/camera/camera-capture-linux.c)
  list(REMOVE_ITEM android-emu-darwin-src android/camera/camera-capture-mac.m)

android_add_library(
  TARGET android-emu
  LICENSE Apache-2.0
  SRC ${android-emu_src}
  WINDOWS ${android-emu-windows-src}
  LINUX ${android-emu-linux-src}
  DARWIN ${android-emu-darwin-src})

# Note that all these dependencies will propagate to whoever relies on android-
# emu It will also setup the proper include directories, so that android-emu can
# find all the headers needed for using the library defined below. We ideally
# would like to keep this list small.
target_link_libraries(
  android-emu
  PRIVATE android-emu-base-headers qemu-host-common-headers
  PUBLIC emulator-libext4_utils
         android-emu-avd
         android-emu-files
         android-emu-feature
         android-emu-metrics
         android-emu-net
         android-emu-proxy
         android-emu-cmdline
         android-emu-base
         android-emu-studio-config
         android-emu-telnet-console-auth
         android-emu-gps
         android-emu-sockets
         android-emu-adb-interface
         android-emu-hardware
         android-emu-crashreport
         android-emu-telephony
         emulator-libsparse
         emulator-libselinux
         emulator-libjpeg
         emulator-libwebp
         emulator-libkeymaster3
         emulator-murmurhash
         picosha2
         # Protobuf dependencies
         android-emu-protos
         protobuf::libprotobuf
         qemu-host-common-headers
         # Prebuilt libraries
         android-net
         android-emu-base
         ssl
         crypto
         LibXml2::LibXml2
         lz4
         zlib
         android-hw-config
         android-emu-agents
         android-emu-utils
         absl::strings
         ui::common
         ui::window
         aemu-gl-init
         gfxstream_features
         gfxstream_glm_headers
  PRIVATE android-emu-protobuf)


# Here are the windows library and link dependencies. They are public and will
# propagate onwards to others that depend on android-emu
android_target_link_libraries(
  android-emu windows
  PUBLIC emulator-libmman-win32
         emulator-libusb
         setupapi::setupapi
         d3d9::d3d9
         mfuuid::mfuuid
         # For CoTaskMemFree used in camera-capture-windows.cpp
         ole32::ole32
         # For GetPerformanceInfo in CrashService_windows.cpp
         psapi::psapi)

# These are the libs needed for android-emu on linux.
android_target_link_libraries(android-emu linux-x86_64 PUBLIC -lrt -lc++)

# Here are the darwin library and link dependencies. They are public and will
# propagate onwards to others that depend on android-emu. You should really only
# add things that are crucial for this library to link
android_target_link_libraries(
  android-emu darwin
  PUBLIC "-framework AppKit"
         "-framework AVFoundation" # For camera-capture-mac.m
         "-framework Accelerate" # Of course, our camera needs it!
         "-framework CoreMedia" # Also for the camera.
         "-framework CoreVideo" # Also for the camera.
         "-framework IOKit"
         "-framework VideoToolbox" # For HW codec acceleration on mac
         "-framework VideoDecodeAcceleration" # For HW codec acceleration on mac
         "-weak_framework Hypervisor"
         "-framework OpenGL")

target_include_directories(
  android-emu
  PUBLIC
    # TODO(jansene): The next 2 imply a link dependendency on emugl libs, which
    # we have not yet made explicit
    ${ANDROID_QEMU2_TOP_DIR}/android/android-emugl/host/include
    ${ANDROID_QEMU2_TOP_DIR}/android/android-emugl/shared
    # TODO(jansene): We actually have a hard dependency on qemu-glue as there
    # are a lot of externs that are actually defined in qemu2-glue. this has to
    # be sorted out,
    ${ANDROID_QEMU2_TOP_DIR}/android-qemu2-glue/config/${ANDROID_TARGET_TAG}
    ${ANDROID_QEMU2_TOP_DIR}
    # If you use our library, you get access to our headers.
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${DARWINN_INCLUDE_DIRS})

target_compile_options(
  android-emu PRIVATE -Wno-invalid-constexpr -Wno-return-type-c-linkage
                      -fvisibility=default PUBLIC -Wno-return-type-c-linkage)

android_target_compile_options(
  android-emu linux-x86_64 PRIVATE -idirafter
                                   ${ANDROID_QEMU2_TOP_DIR}/linux-headers)
android_target_compile_options(
  android-emu darwin-x86_64
  PRIVATE -Wno-objc-method-access -Wno-missing-selector-name -Wno-receiver-expr
          -Wno-incomplete-implementation -Wno-incompatible-pointer-types
          -Wno-deprecated-declarations)

android_target_compile_options(
  android-emu windows_msvc-x86_64
  PRIVATE -Wno-unused-private-field -Wno-reorder -Wno-missing-braces
          -Wno-pessimizing-move -Wno-unused-lambda-capture)

android_target_compile_definitions(
  android-emu darwin PRIVATE "-D_DARWIN_C_SOURCE=1")

target_compile_definitions(android-emu PRIVATE "-D_LIBCPP_VERSION=__GLIBCPP__")

if(WEBRTC)
  target_compile_definitions(android-emu PUBLIC ANDROID_WEBRTC)
endif()

# The dependent target os specific sources, they are pretty much the same as
# above, excluding camera support, because that brings in a whole slew of
# dependencies Shared version of the library. Note that this only has the set of
# common sources, otherwise you will get a lot of linker errors.

android_add_library(
  TARGET android-emu-shared
  SHARED
  LICENSE Apache-2.0
  SRC android/base/async/CallbackRegistry.cpp
      android/emulation/address_space_device.cpp
      android/emulation/address_space_graphics.cpp
      android/emulation/address_space_host_memory_allocator.cpp
      android/emulation/address_space_shared_slots_host_memory_allocator.cpp
      android/emulation/AudioCaptureEngine.cpp
      android/emulation/AudioOutputEngine.cpp
      android/emulation/ComponentVersion.cpp
      android/emulation/control/FilePusher.cpp
      android/emulation/DmaMap.cpp
      android/emulation/GoldfishDma.cpp
      android/emulation/hostdevices/HostAddressSpace.cpp
      android/emulation/hostdevices/HostGoldfishPipe.cpp
      android/emulation/HostmemIdMapping.cpp
      android/emulation/SetupParameters.cpp
      android/error-messages.cpp
      android/kernel/kernel_utils.cpp
      android/opengl/GLProcessPipe.cpp
      android/opengl/GpuFrameBridge.cpp
      android/opengl/OpenglEsPipe.cpp
      android/opengles.cpp
      android/snaphost-android.c
      android/snapshot.c
      android/snapshot/common.cpp
      android/snapshot/Compressor.cpp
      android/snapshot/Decompressor.cpp
      android/snapshot/GapTracker.cpp
      android/snapshot/Hierarchy.cpp
      android/snapshot/IncrementalStats.cpp
      android/snapshot/interface.cpp
      android/snapshot/Loader.cpp
      android/snapshot/MemoryWatch_common.cpp
      android/snapshot/PathUtils.cpp
      android/snapshot/Quickboot.cpp
      android/snapshot/RamLoader.cpp
      android/snapshot/RamSaver.cpp
      android/snapshot/RamSnapshotTesting.cpp
      android/snapshot/Saver.cpp
      android/snapshot/Snapshot.cpp
      android/snapshot/Snapshotter.cpp
      android/snapshot/TextureLoader.cpp
      android/snapshot/TextureSaver.cpp
      stubs/stubs.cpp
  WINDOWS android/snapshot/MemoryWatch_windows.cpp
  LINUX android/snapshot/MemoryWatch_linux.cpp
  DARWIN android/snapshot/MacSegvHandler.cpp
         android/snapshot/MemoryWatch_darwin.cpp)

# Note that these are basically the same as android-emu-shared. We should clean
# this up
target_link_libraries(
  android-emu-shared
  PUBLIC android-emu-base
         android-emu
         android-emu-base-headers
         android-emu-avd
         android-emu-agents
         android-emu-cmdline
         android-emu-utils
         emulator-murmurhash
         android-emu-feature
         android-emu-files
         android-emu-metrics
         android-emu-crashreport
         android-emu-adb-interface
         android-emu-sockets
         android-emu-hardware
         # Protobuf dependencies
         android-emu-protos
         protobuf::libprotobuf
         qemu-host-common-headers
         # Prebuilt libraries
         ui::common
         ui::window
         aemu-gl-init
         lz4
         zlib
         android-hw-config
         absl::strings
  PRIVATE android-emu-protobuf)
# Here are the windows library and link dependencies. They are public and will
# propagate onwards to others that depend on android-emu-shared
android_target_link_libraries(
  android-emu-shared windows
  PRIVATE emulator-libmman-win32
          emulator-libusb
          setupapi::setupapi
          d3d9::d3d9
          # IID_IMFSourceReaderCallback
          mfuuid::mfuuid
          # For CoTaskMemFree used in camera-capture-windows.cpp
          ole32::ole32
          # For GetPerformanceInfo in CrashService_windows.cpp
          psapi::psapi)
# These are the libs needed for android-emu-shared on linux.
android_target_link_libraries(android-emu-shared linux-x86_64 PRIVATE -lrt)
# Here are the darwin library and link dependencies. They are public and will
# propagate onwards to others that depend on android-emu-shared. You should
# really only add things that are crucial for this library to link If you don't
# you might see bizarre errors. (Add opengl as a link dependency, you will have
# fun)
android_target_link_libraries(
  android-emu-shared darwin-x86_64
  PRIVATE "-framework AppKit"
          "-framework ApplicationServices" # To control icon
          "-framework AVFoundation" # For camera-capture-mac.m
          "-framework Accelerate" # Of course, our camera needs it!
          "-framework CoreMedia" # Also for the camera.
          "-framework CoreVideo" # Also for the camera.
          "-framework VideoToolbox" # For HW codec acceleration on mac
          "-framework VideoDecodeAcceleration" # For HW codec acceleration on
                                               # mac
          "-framework OpenGL"
          "-framework IOKit")

android_target_link_libraries(
  android-emu-shared darwin-aarch64
  PRIVATE "-framework AppKit"
          "-framework ApplicationServices" # To control icon
          "-framework AVFoundation" # For camera-capture-mac.m
          "-framework Accelerate" # Of course, our camera needs it!
          "-framework CoreMedia" # Also for the camera.
          "-framework CoreVideo" # Also for the camera.
          "-framework VideoToolbox" # For HW codec acceleration on mac
          "-framework VideoDecodeAcceleration" # For HW codec acceleration on
                                               # mac
          "-framework OpenGL"
          "-framework IOKit")
target_include_directories(
  android-emu-shared
  PUBLIC
    # TODO(jansene): The next 2 imply a link dependendency on emugl libs, which
    # we have not yet made explicit
    ${ANDROID_QEMU2_TOP_DIR}/android/android-emugl/host/include
    ${ANDROID_QEMU2_TOP_DIR}/android/android-emugl/shared
    # TODO(jansene): We actually have a hard dependency on qemu-glue as there
    # are a lot of externs that are actually defined in qemu2-glue. this has to
    # be sorted out,
    ${ANDROID_QEMU2_TOP_DIR}/android-qemu2-glue/config/${ANDROID_TARGET_TAG}
    # If you use our library, you get access to our headers.
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR})
android_target_compile_options(
  android-emu-shared Clang PRIVATE -Wno-extern-c-compat -Wno-invalid-constexpr
                                   -fvisibility=default)
android_target_compile_options(
  android-emu-shared Clang PUBLIC -Wno-return-type-c-linkage) # android_getOp
# enG lesRenderer
android_target_compile_options(
  android-emu-shared linux-x86_64
  PRIVATE -idirafter ${ANDROID_QEMU2_TOP_DIR}/linux-headers)
android_target_compile_options(
  android-emu-shared darwin-x86_64
  PRIVATE -Wno-error -Wno-objc-method-access -Wno-receiver-expr
          -Wno-incomplete-implementation -Wno-missing-selector-name
          -Wno-incompatible-pointer-types)
android_target_compile_options(
  android-emu-shared windows_msvc-x86_64
  PRIVATE -Wno-unused-private-field -Wno-reorder -Wno-unused-lambda-capture)
# Definitions needed to compile our deps as static
target_compile_definitions(android-emu-shared PUBLIC ${LIBXML2_DEFINITIONS})
android_target_compile_definitions(
  android-emu-shared darwin-x86_64
  PRIVATE "-D_DARWIN_C_SOURCE=1")
android_target_compile_definitions(
  android-emu-shared darwin-aarch64
  PRIVATE "-D_DARWIN_C_SOURCE=1")
target_compile_definitions(
  android-emu-shared
  PRIVATE "-DCRASHUPLOAD=${OPTION_CRASHUPLOAD}"
          "-DANDROID_SDK_TOOLS_REVISION=${OPTION_SDK_TOOLS_REVISION}"
          "-DANDROID_SDK_TOOLS_BUILD_NUMBER=${OPTION_SDK_TOOLS_BUILD_NUMBER}")
if(WEBRTC)
  target_compile_definitions(android-emu-shared PUBLIC ANDROID_WEBRTC)
  android_install_shared_library(TARGET android-emu-shared)
endif()

target_compile_definitions(android-emu-shared PUBLIC -DAEMU_MIN=1)

android_install_shared_library(TARGET android-emu-shared)
