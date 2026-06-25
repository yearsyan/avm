# AEMU core-only: no Qt UI, so no crash-consent Qt dialog.
android_add_library(
  TARGET android-emu-crash-initializer
  # SHARED # Used to test if you can compile this stand alone.
  LICENSE Apache-2.0
  SOURCE_DIR
  src
  SRC android/crashreport/CrashSytemInit.cpp
  DEPS android-emu-base
       android-emu-base-headers
       android-emu-crashreport
       android-emu-files
       android-emu-utils)
target_link_libraries(android-emu-crash-initializer
                      PUBLIC android-emu-base-headers)
target_include_directories(android-emu-crash-initializer PRIVATE src
                           PUBLIC include)
target_compile_options(android-emu-crash-initializer
                       PRIVATE -Wno-extern-c-compat PUBLIC -Wno-macro-redefined)

android_add_library(
  TARGET android-emu-crashreport
  # SHARED # Used to test if you can compile this stand alone.
  LICENSE Apache-2.0
  SOURCE_DIR
  src
  SRC android/crashreport/Breadcrumb.cpp
      android/crashreport/CrashReporter.cpp
      android/crashreport/detectors/CrashDetectors.cpp
      android/crashreport/detectors/TimeoutMonitor.cpp
      android/crashreport/HangDetector.cpp
  DEPS absl::strings
       android-emu-agents
       android-emu-base-logging
       android-emu-cmdline
       android-emu-files
       android-emu-utils
       android-hw-config
       protobuf::libprotobuf)
target_include_directories(android-emu-crashreport PRIVATE src PUBLIC include)
target_link_libraries(
  android-emu-crashreport PUBLIC android-emu-base android-emu-base-headers)
target_compile_options(android-emu-crashreport PRIVATE -Wno-extern-c-compat
                       PUBLIC -Wno-macro-redefined)

# AEMU core-only: the Qt crash-consent dialog (android-emu-crashreport-consent-ui)
# has been removed. The headless consent target below is what the core links.

android_add_library(
  TARGET android-emu-crashreport-consent-no-ui
  # SHARED # Used to test if you can compile this stand alone.
  LICENSE Apache-2.0
  SOURCE_DIR
  src
  SRC android/crashreport/CrashConsentNoUi.cpp
  DEPS android-emu-agents android-emu-base android-emu-base-headers
       android-emu-cmdline android-emu-utils)
target_compile_definitions(android-emu-crashreport-consent-no-ui
                           PRIVATE AEMU_CRASH_EXPORTS PUBLIC AEMU_CRASH_SHARED)
target_include_directories(android-emu-crashreport-consent-no-ui PRIVATE src
                           PUBLIC include)
target_compile_options(android-emu-crashreport-consent-no-ui
                       PRIVATE -Wno-extern-c-compat)

# This one can be used for unit tests.
android_add_library(
  TARGET android-emu-crashreport-consent-never
  # SHARED # Used to test if you can compile this stand alone.
  LICENSE Apache-2.0
  SOURCE_DIR
  src
  SRC android/crashreport/CrashConsentNever.cpp
  DEPS android-emu-base-headers)
target_compile_definitions(android-emu-crashreport-consent-never
                           PRIVATE AEMU_CRASH_EXPORTS PUBLIC AEMU_CRASH_SHARED)
target_include_directories(android-emu-crashreport-consent-never PRIVATE src
                           PUBLIC include)
