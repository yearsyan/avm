// Copyright 2015 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "host-common/opengl/emugl_config.h"
#include "host-common/feature_control.h"
#include "android/opengl/EmuglBackendList.h"
#include "android/opengl/gpuinfo.h"
#include "aemu/base/Optional.h"
#include "android/cmdline-definitions.h"
#include "android/avd/info.h"
#include "android/emulation/control/AndroidAgentFactory.h"

#include "android/base/testing/TestSystem.h"
#include "android/base/testing/TestTempDir.h"

#include "android/main-emugl.h"
#include "host-common/globals.h"

#include <gtest/gtest.h>

namespace android {
namespace base {


#if defined(_WIN32)
#  define LIB_NAME(x)  x ".dll"
#  define LAVAPIPE_RESULT "lavapipe"
#  define HOST_VULKAN_RESULT "host"
#  define SWIFTSHADER_RESULT "swiftshader"
#  define SWANGLE_RESULT "swiftshader" // Windows redirects swangle to swiftshader
#  define AUTO_GLES_RESULT "host"
#elif defined(__APPLE__)
#  define LIB_NAME(x)  "lib" x ".dylib"
#if defined(__arm64__)
#  define LAVAPIPE_RESULT "lavapipe"
#  define HOST_VULKAN_RESULT "host"
#else
#  define LAVAPIPE_RESULT "swiftshader" // TODO(b/462005807)
#  define HOST_VULKAN_RESULT "swiftshader"
#endif
#  define SWIFTSHADER_RESULT "swangle" // Mac redirects swiftshader to swangle
#  define SWANGLE_RESULT "swangle"
#  define AUTO_GLES_RESULT "host" //TODO: depends on memory usage, avoid possible flakes
#else
#  define LIB_NAME(x)  "lib" x ".so"
#  define LAVAPIPE_RESULT "lavapipe"
#  define HOST_VULKAN_RESULT "host"
#  define SWIFTSHADER_RESULT "swiftshader"
#  define SWANGLE_RESULT "swangle"
#  define AUTO_GLES_RESULT "host"
#endif

static std::string makeLibSubPath(const char* name) {
    return StringFormat("%s/%s/%s",
                        System::get()->getLauncherDirectory().c_str(),
                        System::kLibSubDir, name);
}

static void makeLibSubDir(TestTempDir* dir, const char* name) {
    dir->makeSubDir(makeLibSubPath(name).c_str());
}

static void makeLibSubFile(TestTempDir* dir, const char* name) {
    dir->makeSubFile(makeLibSubPath(name).c_str());
}

static void makeRenderBackendSubDirAndFiles(TestTempDir* dir,
        const char* backendName) {
    makeLibSubDir(dir, backendName);
    std::string backendStr(backendName);
    makeLibSubFile(dir, (backendStr + "/libEGL.so").c_str());
    makeLibSubFile(dir, (backendStr + "/libGLES_CM.so").c_str());
    makeLibSubFile(dir, (backendStr + "/libGLESv2.so").c_str());
}

static void makeSwiftshaderSubDirAndFiles(TestTempDir* dir) {
    makeRenderBackendSubDirAndFiles(dir, "gles_swiftshader");
}

static void makeSwAngleSubDirAndFiles(TestTempDir* dir) {
    makeRenderBackendSubDirAndFiles(dir, "gles_angle");
}

AndroidOptions emptyOptions{};
AndroidOptions* sAndroid_cmdLineOptions = &emptyOptions;
AvdInfo* sAndroid_avdInfo = nullptr;
AndroidHwConfig s_hwConfig = {0};
AvdInfoParams sAndroid_avdInfoParams = {0};
std::string sCmdlLine;
LanguageSettings s_languageSettings = {0};
AUserConfig* s_userConfig = nullptr;
bool sKeyCodeForwarding = false;
bool sEnforceKeyCodeForwarding = false;

// /* this indicates that guest has mounted data partition */
int s_guest_data_partition_mounted = 0;
// /* this indicates that guest has boot completed */
bool s_guest_boot_completed = 0;
bool s_arm_snapshot_save_completed = 0;
bool s_host_emulator_is_headless = 0;
// /* are we using the emulator in the android mode or plain qemu? */
bool s_android_qemu_mode = true;
// /* are we using android-emu libraries for a minimal configuration? */
// Min config mode and fuchsia mode are equivalent, at least for now.
bool s_min_config_qemu_mode = false;
// /* is android-emu running Fuchsia? */
int s_android_snapshot_update_timer = 0;

static const QAndroidGlobalVarsAgent globalVarsAgent = {
        .avdParams = []() { return &sAndroid_avdInfoParams; },
        .avdInfo =
                []() {
                    // Do not access the info before it is injected!
                    return sAndroid_avdInfo;
                },
        .hw = []() { return &s_hwConfig; },
        // /* this indicates that guest has mounted data partition */
        .guest_data_partition_mounted =
                []() { return s_guest_data_partition_mounted; },

        // /* this indicates that guest has boot completed */
        .guest_boot_completed = []() { return s_guest_boot_completed; },

        .arm_snapshot_save_completed =
                []() { return s_arm_snapshot_save_completed; },

        .host_emulator_is_headless =
                []() { return s_host_emulator_is_headless; },

        // /* are we using the emulator in the android mode or plain qemu? */
        .android_qemu_mode = []() { return s_android_qemu_mode; },

        // /* are we using android-emu libraries for a minimal configuration? */
        .min_config_qemu_mode = []() { return s_min_config_qemu_mode; },

        // /* is android-emu running Fuchsia? */
        .is_fuchsia = []() { return s_min_config_qemu_mode; },

        .android_snapshot_update_timer =
                []() { return s_android_snapshot_update_timer; },
        .language = []() { return &s_languageSettings; },
        .use_keycode_forwarding =
                []() {
                    return sEnforceKeyCodeForwarding || sKeyCodeForwarding;
                },
        .userConfig = []() { return s_userConfig; },
        .android_cmdLineOptions = []() { return sAndroid_cmdLineOptions; },
        .inject_cmdLineOptions =
                [](AndroidOptions* opts) { sAndroid_cmdLineOptions = opts; },
        .has_cmdLineOptions =
                []() {
                    return globalVarsAgent.android_cmdLineOptions() != nullptr;
                },
        .android_cmdLine = []() { return (const char*)sCmdlLine.c_str(); },
        .inject_android_cmdLine =
                [](const char* cmdline) { sCmdlLine = cmdline; },
        .inject_language =
                [](char* language, char* country, char* locale) {
                    s_languageSettings.language = language;
                    s_languageSettings.country = country;
                    s_languageSettings.locale = locale;
                    s_languageSettings.changing_language_country_locale =
                            language || country || locale;
                },
        .inject_userConfig = [](AUserConfig* config) { s_userConfig = config; },
        .set_keycode_forwarding =
                [](bool enabled) { sKeyCodeForwarding = enabled; },
        .set_enforce_keycode_forwarding =
                [](bool enabled) { sEnforceKeyCodeForwarding = enabled; },
        .inject_AvdInfo = [](AvdInfo* avd) { sAndroid_avdInfo = avd; },

        // /* this indicates that guest has mounted data partition */
        .set_guest_data_partition_mounted =
                [](int guest_data_partition_mounted) {
                    s_guest_data_partition_mounted =
                            guest_data_partition_mounted;
                },

        // /* this indicates that guest has boot completed */
        .set_guest_boot_completed =
                [](bool guest_boot_completed) {
                    s_guest_boot_completed = guest_boot_completed;
                },

        .set_arm_snapshot_save_completed =
                [](bool arm_snapshot_save_completed) {
                    s_arm_snapshot_save_completed = arm_snapshot_save_completed;
                },

        .set_host_emulator_is_headless =
                [](bool host_emulator_is_headless) {
                    s_host_emulator_is_headless = host_emulator_is_headless;
                },

        // /* are we using the emulator in the android mode or plain qemu? */
        .set_android_qemu_mode =
                [](bool android_qemu_mode) {
                    s_android_qemu_mode = android_qemu_mode;
                },

        // /* are we using android-emu libraries for a minimal configuration? */
        .set_min_config_qemu_mode =
                [](bool min_config_qemu_mode) {
                    s_min_config_qemu_mode = min_config_qemu_mode;
                },

        // /* is android-emu running Fuchsia? */
        .set_is_fuchsia =
                [](bool is_fuchsia) { s_min_config_qemu_mode = is_fuchsia; },

        .set_android_snapshot_update_timer =
                [](int android_snapshot_update_timer) {
                    s_android_snapshot_update_timer =
                            android_snapshot_update_timer;
                }

};

namespace android::emulation {
class MockAndroidConsoleFactory : public ::android::emulation::AndroidConsoleFactory {
public:
    const QAndroidGlobalVarsAgent* android_get_QAndroidGlobalVarsAgent()
            const override {
        return &globalVarsAgent;
    };
};
}  // namespace android::emulation

TEST(EmuglConfig, init) {
    TestSystem testSys("foo", System::kProgramBitness, "/");
    TestTempDir* myDir = testSys.getTempRoot();
    myDir->makeSubDir(System::get()->getLauncherDirectory().c_str());
    makeLibSubDir(myDir, "");

    makeLibSubDir(myDir, "gles_vendor");
    makeLibSubFile(myDir, "gles_vendor/" LIB_NAME("EGL"));
    makeLibSubFile(myDir, "gles_vendor/" LIB_NAME("GLESv2"));

    ::android::emulation::injectConsoleAgents(
        android::emulation::MockAndroidConsoleFactory());

    {
        EmuglConfig config;
        EXPECT_TRUE(emuglConfig_init(
                    &config, "host", false));
        EXPECT_STREQ(HOST_VULKAN_RESULT, config.vulkan_backend);
        EXPECT_STREQ("host", config.gles_backend);
    }

    // Check that "host" mode is available with -no-window if explicitly
    // specified on command line.
    {
        EmuglConfig config;
        EXPECT_TRUE(emuglConfig_init(
                    &config, "host", true));
        EXPECT_STREQ(HOST_VULKAN_RESULT, config.vulkan_backend);
        EXPECT_STREQ("host", config.gles_backend);
    }
}

TEST(EmuglConfig, initFromUISetting) {
    TestSystem testSys("foo", System::kProgramBitness, "/");
    TestTempDir* myDir = testSys.getTempRoot();
    myDir->makeSubDir(System::get()->getLauncherDirectory().c_str());
    makeLibSubDir(myDir, "");

    makeLibSubDir(myDir, "gles_angle");
    makeLibSubFile(myDir, "gles_angle/" LIB_NAME("EGL"));
    makeLibSubFile(myDir, "gles_angle/" LIB_NAME("GLESv2"));

    makeLibSubDir(myDir, "gles_swiftshader");
    makeLibSubFile(myDir, "gles_swiftshader/" LIB_NAME("EGL"));
    makeLibSubFile(myDir, "gles_swiftshader/" LIB_NAME("GLESv2"));

    const std::vector<const char*> UiOptionToGpuOption = {
        "auto",         // WINSYS_GLESBACKEND_PREFERENCE_AUTO = 0,
        "swangle",      // WINSYS_GLESBACKEND_PREFERENCE_ANGLE_DEPRECATED = 1,
        "auto",         // WINSYS_GLESBACKEND_PREFERENCE_ANGLE9_DEPRECATED = 2,
        "swiftshader",  // WINSYS_GLESBACKEND_PREFERENCE_SWIFTSHADER_DEPRECATED = 3,
        "host",         // WINSYS_GLESBACKEND_PREFERENCE_NATIVEGL_DEPRECATED = 4,
        "lavapipe",     // WINSYS_GLESBACKEND_PREFERENCE_SOFTWARE = 5,
        "host",         // WINSYS_GLESBACKEND_PREFERENCE_HARDWARE = 6,
    };

    for (int i = 1; i < WINSYS_GLESBACKEND_PREFERENCE_NUM; i++) {
        EmuglConfig config;
        EXPECT_TRUE(emuglConfig_init(
                    &config, UiOptionToGpuOption[i], false));

        emuglConfig_setupEnv(&config);

        switch (i) {
        case WINSYS_GLESBACKEND_PREFERENCE_AUTO:
            EXPECT_STREQ(AUTO_GLES_RESULT, config.gles_backend);
#ifdef __APPLE__
            // When host gpu is not enforced, swiftshader will be used on macOS
            // for vulkan
            EXPECT_STREQ("swiftshader", config.vulkan_backend);
#else
            EXPECT_STREQ("host", config.vulkan_backend);
#endif
            break;
        case WINSYS_GLESBACKEND_PREFERENCE_ANGLE_DEPRECATED:
            // Deprecated, used to test swangle here
            EXPECT_STREQ(SWANGLE_RESULT, config.gles_backend);
            EXPECT_STREQ("swiftshader", config.vulkan_backend);
            EXPECT_STREQ("swiftshader",
                         System::get()->envGet("ANDROID_EMU_VK_ICD").c_str());
            break;
        case WINSYS_GLESBACKEND_PREFERENCE_ANGLE9_DEPRECATED:
            // Deprecated, same as auto
            // EXPECT_STREQ("host", config.gles_backend);
            // EXPECT_STREQ("host", config.vulkan_backend);
            break;
        case WINSYS_GLESBACKEND_PREFERENCE_SWIFTSHADER_DEPRECATED:
            EXPECT_STREQ(SWIFTSHADER_RESULT, config.gles_backend);
            EXPECT_STREQ("swiftshader", config.vulkan_backend);
            break;
        case WINSYS_GLESBACKEND_PREFERENCE_NATIVEGL_DEPRECATED:
        case WINSYS_GLESBACKEND_PREFERENCE_HARDWARE:
            EXPECT_STREQ("host", config.gles_backend);
            EXPECT_STREQ(HOST_VULKAN_RESULT, config.vulkan_backend);
            break;
        case WINSYS_GLESBACKEND_PREFERENCE_SOFTWARE:
            EXPECT_STREQ(SWANGLE_RESULT, config.gles_backend);
            EXPECT_STREQ(LAVAPIPE_RESULT, config.vulkan_backend);
            break;
        default:
            break;
        }

        // Check if ANDROID_EMU_VK_ICD is set correctly based on the vulkan mode
        if (strcmp(config.vulkan_backend, "swiftshader") == 0) {
            EXPECT_STREQ("swiftshader",
                         System::get()->envGet("ANDROID_EMU_VK_ICD").c_str());
        } else if (strcmp(config.vulkan_backend, "lavapipe") == 0) {
            EXPECT_STREQ("lavapipe",
                         System::get()->envGet("ANDROID_EMU_VK_ICD").c_str());
        } else if (strcmp(config.vulkan_backend, "host") == 0) {
#if defined(__APPLE__) && defined(__arm64__)
            EXPECT_STREQ("moltenvk",
                         System::get()->envGet("ANDROID_EMU_VK_ICD").c_str());
#else
            EXPECT_STREQ("",
                         System::get()->envGet("ANDROID_EMU_VK_ICD").c_str());
#endif
        }
    }
}

// Tests to cover usages of higher level function 'androidEmuglConfigInit'
TEST(EmuglConfig, initWithEmuglConfigInit) {
    TestSystem testSys("foo", System::kProgramBitness, "/");
    TestTempDir* myDir = testSys.getTempRoot();
    myDir->makeSubDir(System::get()->getLauncherDirectory().c_str());
    makeLibSubDir(myDir, "");

    makeSwAngleSubDirAndFiles(myDir);
    makeSwiftshaderSubDirAndFiles(myDir);

    {
        // Reset feature flags
        feature_reset();

        // with valid values
        EmuglConfig config;
        EXPECT_TRUE(androidEmuglConfigInit(&config, "host", "host", false,
                                           WINSYS_GLESBACKEND_PREFERENCE_AUTO));
        EXPECT_STREQ("host", config.gles_backend);
        EXPECT_STREQ(HOST_VULKAN_RESULT, config.vulkan_backend);
    }

    {
        // Reset feature flags
        feature_reset();

        // with '-gpu software' value
        EmuglConfig config;
        EXPECT_TRUE(androidEmuglConfigInit(&config, "software", "auto", false,
                                           WINSYS_GLESBACKEND_PREFERENCE_AUTO));
        EXPECT_STREQ(LAVAPIPE_RESULT, config.vulkan_backend);
    }

    {
        // Reset feature flags
        feature_reset();

        // with null options
        EmuglConfig config;
        bool initRes = androidEmuglConfigInit(&config, nullptr, nullptr, false,
                                           WINSYS_GLESBACKEND_PREFERENCE_AUTO);
        const bool onDenyList = isHostGpuBlacklisted();
        if (onDenyList) {
            EXPECT_TRUE(initRes);
            EXPECT_STREQ(LAVAPIPE_RESULT, config.vulkan_backend);
        }else {
            EXPECT_TRUE(initRes);
            EXPECT_STREQ(AUTO_GLES_RESULT, config.gles_backend);
        }
    }

    {
        // Reset feature flags
        feature_reset();

        // invalid values should fallback to 'auto' and work fine
        EmuglConfig config;
        bool initRes = androidEmuglConfigInit(&config, "invalid", "unknown", false,
                                           WINSYS_GLESBACKEND_PREFERENCE_AUTO);
        const bool onDenyList = isHostGpuBlacklisted();
        if (onDenyList) {
            EXPECT_TRUE(initRes);
            EXPECT_STREQ(LAVAPIPE_RESULT, config.vulkan_backend);
        }else {
            EXPECT_TRUE(initRes);
            EXPECT_STREQ(AUTO_GLES_RESULT, config.gles_backend);
        }
    }
}

TEST(EmuglConfig, initNoWindowWithAuto) {
    TestSystem testSys("foo", System::kProgramBitness, "/");
    TestTempDir* myDir = testSys.getTempRoot();
    myDir->makeSubDir(System::get()->getLauncherDirectory().c_str());
    makeLibSubDir(myDir, "");

    makeSwAngleSubDirAndFiles(myDir);
    makeSwiftshaderSubDirAndFiles(myDir);

    EmuglConfig config;
    EXPECT_TRUE(emuglConfig_init(
                &config, "auto", true));
    EXPECT_STREQ(LAVAPIPE_RESULT, config.vulkan_backend);
    EXPECT_STREQ(SWANGLE_RESULT, config.gles_backend);
}

TEST(EmuglConfig, initNoWindowWithLavapipe) {
    TestSystem testSys("foo", System::kProgramBitness, "/");
    TestTempDir* myDir = testSys.getTempRoot();
    myDir->makeSubDir(System::get()->getLauncherDirectory().c_str());
    makeLibSubDir(myDir, "");

    makeSwAngleSubDirAndFiles(myDir);
    makeSwiftshaderSubDirAndFiles(myDir);

    EmuglConfig config;
    EXPECT_TRUE(emuglConfig_init(
                &config, "lavapipe", true));
    EXPECT_STREQ(LAVAPIPE_RESULT, config.vulkan_backend);
    EXPECT_STREQ(SWANGLE_RESULT, config.gles_backend);
}

TEST(EmuglConfig, setupEnv) {
}

TEST(EmuglConfig, hostGpuProps) {
    TestSystem testSys("/usr", 32);
    testSys.setLiveUnixTime(true);
    GpuInfoList* gpulist = const_cast<GpuInfoList*>(&globalGpuInfoList());
    gpulist->clear();
    EXPECT_TRUE(gpulist->infos.size() == 0);
    gpulist->addGpu();
    gpulist->currGpu().make = "TEST GPU0 MAKE";
    gpulist->currGpu().model = "TEST GPU0 MODEL";
    gpulist->currGpu().device_id = "TEST GPU0 DEVICEID";
    gpulist->currGpu().revision_id = "TEST GPU0 REVISIONID";
    gpulist->currGpu().version = "TEST GPU0 VERSION";
    gpulist->currGpu().renderer = "TEST GPU0 RENDERER";
    gpulist->addGpu();
    gpulist->currGpu().make = "TEST GPU1 MAKE";
    gpulist->currGpu().model = "TEST GPU1 MODEL";
    gpulist->currGpu().device_id = "TEST GPU1 DEVICEID";
    gpulist->currGpu().revision_id = "TEST GPU1 REVISIONID";
    gpulist->currGpu().version = "TEST GPU1 VERSION";
    gpulist->currGpu().renderer = "TEST GPU1 RENDERER";
    gpulist->addGpu();
    gpulist->currGpu().make = "TEST GPU2 MAKE";
    gpulist->currGpu().model = "TEST GPU2 MODEL";
    gpulist->currGpu().device_id = "TEST GPU2 DEVICEID";
    gpulist->currGpu().revision_id = "TEST GPU2 REVISIONID";
    gpulist->currGpu().version = "TEST GPU2 VERSION";
    gpulist->currGpu().renderer = "TEST GPU2 RENDERER";
    gpulist->addGpu();
    gpulist->currGpu().make = "TEST GPU3 MAKE";
    gpulist->currGpu().model = "TEST GPU3 MODEL";
    gpulist->currGpu().device_id = "TEST GPU3 DEVICEID";
    gpulist->currGpu().revision_id = "TEST GPU3 REVISIONID";
    gpulist->currGpu().version = "TEST GPU3 VERSION";
    gpulist->currGpu().renderer = "TEST GPU3 RENDERER";

    emugl_host_gpu_prop_list gpu_props = emuglConfig_get_host_gpu_props();
    EXPECT_TRUE(gpu_props.num_gpus == 4);

    EXPECT_STREQ("TEST GPU0 MAKE", gpu_props.props[0].make);
    EXPECT_STREQ("TEST GPU1 MAKE", gpu_props.props[1].make);
    EXPECT_STREQ("TEST GPU2 MAKE", gpu_props.props[2].make);
    EXPECT_STREQ("TEST GPU3 MAKE", gpu_props.props[3].make);

    EXPECT_STREQ("TEST GPU0 MODEL", gpu_props.props[0].model);
    EXPECT_STREQ("TEST GPU1 MODEL", gpu_props.props[1].model);
    EXPECT_STREQ("TEST GPU2 MODEL", gpu_props.props[2].model);
    EXPECT_STREQ("TEST GPU3 MODEL", gpu_props.props[3].model);

    EXPECT_STREQ("TEST GPU0 DEVICEID", gpu_props.props[0].device_id);
    EXPECT_STREQ("TEST GPU1 DEVICEID", gpu_props.props[1].device_id);
    EXPECT_STREQ("TEST GPU2 DEVICEID", gpu_props.props[2].device_id);
    EXPECT_STREQ("TEST GPU3 DEVICEID", gpu_props.props[3].device_id);

    EXPECT_STREQ("TEST GPU0 REVISIONID", gpu_props.props[0].revision_id);
    EXPECT_STREQ("TEST GPU1 REVISIONID", gpu_props.props[1].revision_id);
    EXPECT_STREQ("TEST GPU2 REVISIONID", gpu_props.props[2].revision_id);
    EXPECT_STREQ("TEST GPU3 REVISIONID", gpu_props.props[3].revision_id);

    EXPECT_STREQ("TEST GPU0 VERSION", gpu_props.props[0].version);
    EXPECT_STREQ("TEST GPU1 VERSION", gpu_props.props[1].version);
    EXPECT_STREQ("TEST GPU2 VERSION", gpu_props.props[2].version);
    EXPECT_STREQ("TEST GPU3 VERSION", gpu_props.props[3].version);

    EXPECT_STREQ("TEST GPU0 RENDERER", gpu_props.props[0].renderer);
    EXPECT_STREQ("TEST GPU1 RENDERER", gpu_props.props[1].renderer);
    EXPECT_STREQ("TEST GPU2 RENDERER", gpu_props.props[2].renderer);
    EXPECT_STREQ("TEST GPU3 RENDERER", gpu_props.props[3].renderer);

    free_emugl_host_gpu_props(gpu_props);
}

TEST(EmuglConfig, hostGpuProps_empty) {
    TestSystem testSys("/usr", 32);
    testSys.setLiveUnixTime(true);
    GpuInfoList* gpulist = const_cast<GpuInfoList*>(&globalGpuInfoList());
    gpulist->clear();
    EXPECT_TRUE(gpulist->infos.size() == 0);

    emugl_host_gpu_prop_list gpu_props = emuglConfig_get_host_gpu_props();
    EXPECT_TRUE(gpu_props.num_gpus == 0);
    free_emugl_host_gpu_props(gpu_props);
}

}  // namespace base
}  // namespace android
