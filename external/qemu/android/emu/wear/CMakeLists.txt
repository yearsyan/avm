# A library that contains harware related things
android_add_library(
  TARGET android-emu-wear
  # SHARED # Used to test if you can compile this stand alone.
  LICENSE Apache-2.0
  SOURCE_DIR
  src
  SRC android/wear-agent/android_wear_agent.cpp
      android/wear-agent/PairUpWearPhone.cpp android/wear-agent/WearAgent.cpp
  DEPS android-emu-agents android-emu-base android-emu-base-headers android-emu-hardware
       android-emu-utils)
target_include_directories(android-emu-wear PRIVATE src PUBLIC include)

  
target_include_directories(android-emu-wear_unittests PRIVATE src test)
