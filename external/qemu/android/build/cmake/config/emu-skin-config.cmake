get_filename_component(
  PREBUILT_ROOT
  "${ANDROID_QEMU2_TOP_DIR}/../../prebuilts/android-emulator-build/common"
  ABSOLUTE)
set (ARCH "x86_64")
if (DARWIN_AARCH64)
set (ARCH "arm64-v8a")
endif()

set(SKIN_DEPENDENCIES
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/user/pixel_10_pro_xl/EmulationPixel10ProXLOverlay.apk>resources/skins/android-36/user/pixel_10_pro_xl/EmulationPixel10ProXLOverlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/user/pixel_10_pro_xl/SystemUIEmulationPixel10ProXLOverlay.apk>resources/skins/android-36/user/pixel_10_pro_xl/SystemUIEmulationPixel10ProXLOverlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/user/pixel_10/SystemUIEmulationPixel10Overlay.apk>resources/skins/android-36/user/pixel_10/SystemUIEmulationPixel10Overlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/user/pixel_10/EmulationPixel10Overlay.apk>resources/skins/android-36/user/pixel_10/EmulationPixel10Overlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/user/pixel_10_pro_fold/SystemUIEmulationPixel10ProFoldOverlay.apk>resources/skins/android-36/user/pixel_10_pro_fold/SystemUIEmulationPixel10ProFoldOverlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/user/pixel_10_pro_fold/EmulationPixel10ProFoldOverlay.apk>resources/skins/android-36/user/pixel_10_pro_fold/EmulationPixel10ProFoldOverlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/user/pixel_10_pro/SystemUIEmulationPixel10ProOverlay.apk>resources/skins/android-36/user/pixel_10_pro/SystemUIEmulationPixel10ProOverlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/user/pixel_10_pro/EmulationPixel10ProOverlay.apk>resources/skins/android-36/user/pixel_10_pro/EmulationPixel10ProOverlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/userdebug/pixel_10_pro_xl/EmulationPixel10ProXLOverlay.apk>resources/skins/android-36/userdebug/pixel_10_pro_xl/EmulationPixel10ProXLOverlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/userdebug/pixel_10_pro_xl/SystemUIEmulationPixel10ProXLOverlay.apk>resources/skins/android-36/userdebug/pixel_10_pro_xl/SystemUIEmulationPixel10ProXLOverlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/userdebug/pixel_10/SystemUIEmulationPixel10Overlay.apk>resources/skins/android-36/userdebug/pixel_10/SystemUIEmulationPixel10Overlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/userdebug/pixel_10/EmulationPixel10Overlay.apk>resources/skins/android-36/userdebug/pixel_10/EmulationPixel10Overlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/userdebug/pixel_10_pro_fold/SystemUIEmulationPixel10ProFoldOverlay.apk>resources/skins/android-36/userdebug/pixel_10_pro_fold/SystemUIEmulationPixel10ProFoldOverlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/userdebug/pixel_10_pro_fold/EmulationPixel10ProFoldOverlay.apk>resources/skins/android-36/userdebug/pixel_10_pro_fold/EmulationPixel10ProFoldOverlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/userdebug/pixel_10_pro/SystemUIEmulationPixel10ProOverlay.apk>resources/skins/android-36/userdebug/pixel_10_pro/SystemUIEmulationPixel10ProOverlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/userdebug/pixel_10_pro/EmulationPixel10ProOverlay.apk>resources/skins/android-36/userdebug/pixel_10_pro/EmulationPixel10ProOverlay.apk;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/data/misc/pixel_10_pro_fold/devicestate/device_state_configuration.xml>resources/skins/android-36/data/misc/pixel_10_pro_fold/devicestate/device_state_configuration.xml;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/data/misc/pixel_10_pro_fold/display_settings.xml>resources/skins/android-36/data/misc/pixel_10_pro_fold/display_settings.xml;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/data/misc/pixel_10_pro_fold/displayconfig/display_layout_configuration.xml>resources/skins/android-36/data/misc/pixel_10_pro_fold/displayconfig/display_layout_configuration.xml;
    ${PREBUILT_ROOT}/skins/${ARCH}/android-36/data/misc/pixel_10_pro_fold/extra_feature.xml>resources/skins/android-36/data/misc/pixel_10_pro_fold/extra_feature.xml;
    )
android_license(TARGET "SKIN_DEPENDENCIES" LIBNAME None SPDX None LICENSE None LOCAL None)
set(PACKAGE_EXPORT "SKIN_DEPENDENCIES")
