android_add_library(
  TARGET android-emu-utils
  # SHARED Used to test if you can compile this stand alone.
  LICENSE Apache-2.0
  SOURCE_DIR
  src
  SRC android/emulation/ParameterList.cpp
      android/skin/rect.c
      android/utils/assert.c
      android/utils/async.cpp
      android/utils/cbuffer.c
      android/utils/dns.cpp
      android/utils/exec.cpp
      android/utils/format.cpp
      android/utils/host_bitness.cpp
      android/utils/http_utils.cpp
      android/utils/intmap.c
      android/utils/iolooper.cpp
      android/utils/ipaddr.cpp
      android/utils/lineinput.c
      android/utils/lock.cpp
      android/utils/Random.cpp
      android/utils/reflist.c
      android/utils/refset.c
      android/utils/system_wrapper.cpp
      android/utils/timezone.cpp
      android/utils/uri.cpp
      android/utils/utf8_utils.cpp
      android/utils/vector.c
  DEPS android-emu-base android-emu-base-headers)
target_include_directories(android-emu-utils PRIVATE src PUBLIC include)
target_compile_definitions(
  android-emu-utils
  PUBLIC "ANDROID_SDK_TOOLS_REVISION=${OPTION_SDK_TOOLS_REVISION}"
         "ANDROID_SDK_TOOLS_BUILD_NUMBER=${OPTION_SDK_TOOLS_BUILD_NUMBER}")
if(WINDOWS_MSVC_X86_64)
  target_link_libraries(android-emu-utils PRIVATE ws2_32::ws2_32
                                                  iphlpapi::iphlpapi)
endif()
