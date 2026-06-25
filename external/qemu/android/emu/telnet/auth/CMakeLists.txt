android_add_library(
  TARGET android-emu-telnet-console-auth
  # SHARED # Used to test if you can compile this stand alone.
  LICENSE Apache-2.0
  SOURCE_DIR src
  SRC android/console_auth.cpp
  DEPS android-emu-base android-emu-base-headers android-emu-proxy android-emu-utils)
target_include_directories(android-emu-telnet-console-auth PRIVATE src
                           PUBLIC include)
