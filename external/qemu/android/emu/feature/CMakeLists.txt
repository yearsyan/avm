android_add_library(
  TARGET android-emu-feature
  # SHARED # Used to test if you can compile this stand alone.
  LICENSE Apache-2.0
  SOURCE_DIR
  src
  SRC android/cpu_accelerator.cpp
      android/emulation/CpuAccelerator.cpp
      android/featurecontrol/feature_control.cpp
      android/featurecontrol/FeatureControl.cpp
      android/featurecontrol/FeatureControlImpl.cpp
      android/featurecontrol/FeatureControlOverride.cpp
      android/featurecontrol/HWMatching.cpp
      android/HostHwInfo.cpp
      android/opengl/gpuinfo.cpp
  WINDOWS android/opengl/NativeGpuInfo_windows.cpp android/windows_installer.cpp
  LINUX android/opengl/NativeGpuInfo_linux.cpp
  DARWIN android/opengl/macTouchOpenGL.m android/opengl/NativeGpuInfo_darwin.cpp
  DEPS absl::strings
       android-emu-agents
       android-emu-avd
       android-emu-base
       android-emu-base-headers
       android-emu-cmdline
       android-emu-crashreport
       android-emu-files
       android-emu-protos
       android-emu-studio-config
       android-emu-utils
       android-hw-config
       protobuf::libprotobuf)
target_include_directories(android-emu-feature PRIVATE src PUBLIC include)
android_target_link_libraries(android-emu-feature darwin
                              PUBLIC "-framework Metal")
target_compile_options(android-emu-feature PRIVATE "-Wno-extern-c-compat")

if(WINDOWS_MSVC_X86_64)
  # TODO(jansene): Remove once crash reporting is pulled out.
  target_include_directories(android-emu-feature PRIVATE ../../android-emu)
  target_link_libraries(android-emu-feature PRIVATE d3d9::d3d9)
endif()
