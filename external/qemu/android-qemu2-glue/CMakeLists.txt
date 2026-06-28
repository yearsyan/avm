cmake_minimum_required(VERSION 3.10)

# Generate the VirtioGpuTimelines flatbuffers header
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/VirtioGpuTimelines_generated.h
  COMMAND flatc ARGS --cpp --gen-object-api --scoped-enums -o
          ${CMAKE_CURRENT_BINARY_DIR} emulation/VirtioGpuTimelines.fbs
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS flatc emulation/VirtioGpuTimelines.fbs)
add_custom_target(
  virtio_gpu_timelines_fbs_header ALL
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/VirtioGpuTimelines_generated.h)

# AEMU core-only: no netsim/telephony/wifi-passthrough sources.
set(android-qemu2-glue_src
    android_qemud.cpp
    audio-capturer.cpp
    audio-output.cpp
    base/async/CpuLooper.cpp
    base/async/Looper.cpp
    base/files/QemuFileStream.cpp
    base/files/KernelCmdLineLoader.cpp
    display.cpp
    drive-share.cpp
    dtb.cpp
    emulation/CharSerialLine.cpp
    emulation/DmaMap.cpp
    emulation/VmLock.cpp
    emulation/android_address_space_device.cpp
    emulation/android_pipe_device.cpp
    emulation/charpipe.c
    emulation/goldfish_sync.cpp
    emulation/serial_line.cpp
    emulation/virtio-input-multi-touch.c
    emulation/virtio-input-rotary.c
    emulation/virtio_vsock_device.cpp
    emulation/virtio_vsock_transport.cpp
    emulation/virtio-wifi.cpp
    emulation/VirtioGpuTimelines.cpp
    emulation/VirtioWifiForwarder.cpp
    emulation/WifiService.cpp
    looper-qemu.cpp
    macmu-input-receiver.cpp
    net-android.cpp
    proxy/slirp_proxy.cpp
    qemu-automation-agent-impl.cpp
    qemu-battery-agent-impl.cpp
    qemu-cellular-agent-impl.c
    qemu-console-factory.cpp
    qemu-display-agent-impl.cpp
    qemu-globals-agent-impl.cpp
    qemu-http-proxy-agent-impl.c
    qemu-hw-control-agent-impl.cpp
    qemu-location-agent-impl.c
    qemu-multi-display-agent-impl.cpp
    qemu-net-agent-impl.c
    qemu-record-screen-agent-impl.c
    qemu-sensors-agent-impl.cpp
    qemu-setup-dns-servers.cpp
    qemu-setup.cpp
    qemu-telephony-agent-impl.c
    qemu-user-event-agent-impl.c
    snapshot_compression.cpp
    utils/register_crash_info.cpp
    utils/stream.cpp)

set(android-qemu2-virtio_src emulation/virtio-gpu-gfxstream-renderer.cpp)
set(android-qemu2-virtio_hdr gfxstream_backend_headers)

android_add_library(TARGET libqemu2-glue LICENSE Apache-2.0
                    SRC ${android-qemu2-glue_src} ${android-qemu2-virtio_src})

target_include_directories(
  libqemu2-glue
  PRIVATE ..
          ../include
          ${ANDROID_AUTOGEN}
          ../slirp
          ${VIRGLRENDERER_INCLUDE_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../../shell
          ${ANDROID_QEMU2_TOP_DIR}/android/third_party/libdtb/include/
          ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(libqemu2-glue PRIVATE ui::gl-bridge
                                            ${android-qemu2-virtio_hdr})

target_link_libraries(libqemu2-glue PRIVATE protobuf::libprotobuf)

target_link_libraries(
  libqemu2-glue
  PRIVATE android-emu
          hostapd
          glib2
          PIXMAN::PIXMAN
          emulator-libdtb
          android-emu-gps
          ssl
          flatbuffers
          qemu-host-common-headers
          protobuf::libprotobuf
  PUBLIC android-emu-check)

add_dependencies(libqemu2-glue virtio_gpu_timelines_fbs_header)

target_compile_definitions(libqemu2-glue PRIVATE "-D_LIBCPP_VERSION=__GLIBCPP__")
target_compile_definitions(libqemu2-glue INTERFACE "-DCONFIG_VIRGL")
android_add_library(
  TARGET libqemu2-glue-vm-operations LICENSE Apache-2.0
  SRC # cmake-format: sortable
      ${ANDROID_HW_CONFIG_H} qemu-vm-operations-impl.cpp)
target_include_directories(libqemu2-glue-vm-operations
                           PRIVATE .. ../include ${ANDROID_AUTOGEN})
target_link_libraries(
  libqemu2-glue-vm-operations
  PRIVATE android-emu android-emu-base-headers glib2 android-hw-config
          nlohmann_json::nlohmann_json PIXMAN::PIXMAN)
