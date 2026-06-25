android_add_library(
  TARGET android-emu-base-process # SHARED # Testing only
  LICENSE Apache-2.0
  SOURCE_DIR
  src
  SRC android/base/process/Command.cpp
  LINUX android/base/process/Command_Posix.cpp
  DARWIN android/base/process/Command_Posix.cpp
  WINDOWS android/base/process/Command_Windows.cpp
  DEPS android-emu-base android-emu-base-headers android-emu-base-logging
       android-emu-base-streams android-emu-utils)

target_include_directories(android-emu-base-process PRIVATE src)
target_link_libraries(android-emu-base-process PRIVATE android-emu-base-headers)
