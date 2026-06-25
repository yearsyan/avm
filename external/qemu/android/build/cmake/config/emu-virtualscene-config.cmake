get_filename_component(
  PREBUILT_ROOT
  "${ANDROID_QEMU2_TOP_DIR}/../../prebuilts/android-emulator-build/common"
  ABSOLUTE)
set(VIRTUAL_SCENE_DEPENDENCIES
    ${PREBUILT_ROOT}/virtualscene/Toren1BD/Toren1BD.mtl>resources/Toren1BD.mtl;
    ${PREBUILT_ROOT}/virtualscene/Toren1BD/Toren1BD.obj>resources/Toren1BD.obj;
    ${PREBUILT_ROOT}/virtualscene/Toren1BD/Toren1BD.posters>resources/Toren1BD.posters;
    ${PREBUILT_ROOT}/virtualscene/Toren1BD/Toren1BD_Decor.png>resources/Toren1BD_Decor.png;
    ${PREBUILT_ROOT}/virtualscene/Toren1BD/Toren1BD_Main.png>resources/Toren1BD_Main.png;
    ${PREBUILT_ROOT}/virtualscene/default/default360.jpg>resources/default360.jpg;
    ${PREBUILT_ROOT}/virtualscene/default/default.jpg>resources/default.jpg;
    ${PREBUILT_ROOT}/virtualscene/default/default.mp4>resources/default.mp4;
    ${PREBUILT_ROOT}/virtualscene/Toren1BD/poster.png>resources/poster.png;)
android_license(TARGET "VIRTUAL_SCENE_DEPENDENCIES" LIBNAME None SPDX None
                LICENSE None LOCAL None)
set(PACKAGE_EXPORT "VIRTUAL_SCENE_DEPENDENCIES")
