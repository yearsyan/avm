android_add_library(
  TARGET android-emu-studio-config
  # SHARED # Testing only..
  LICENSE Apache-2.0
  SOURCE_DIR
  src
  SRC android/emulation/control/EmulatorAdvertisement.cpp
      android/metrics/MetricsPaths.cpp android/metrics/StudioConfig.cpp
  DEPS android-emu-base android-emu-base-headers android-emu-base-process
       android-emu-files android-emu-utils LibXml2::LibXml2)
target_include_directories(android-emu-studio-config PUBLIC include PRIVATE src)
