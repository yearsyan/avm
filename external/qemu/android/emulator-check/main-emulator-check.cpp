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
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#include <dismapi.h>
#endif

#include "aemu/base/Optional.h"
#include "android/cpu_accelerator.h"
#include "android/emulation/CpuAccelerator.h"
#include "android/cmdline-definitions.h"
#include "android/avd/info.h"
#include "android/emulation/control/AndroidAgentFactory.h"
#include "android/emulation/control/globals_agent.h"
#include "android/emulator-check/PlatformInfo.h"

using CommandReturn = std::pair<int, std::string>;
// using  android::emulation::injectConsoleAgents;

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
class MockAndroidConsoleFactory : public AndroidConsoleFactory {
public:
    const QAndroidGlobalVarsAgent* android_get_QAndroidGlobalVarsAgent()
            const override {
        return &globalVarsAgent;
    };
};
}  // namespace android::emulation

static const int kGenericError = 100;

static CommandReturn help();

// check ability to launch kvm accelerated VM and exit
// designed for use by Android Studio
static CommandReturn checkCpuAcceleration() {
    char* status = nullptr;
    AndroidCpuAcceleration capability =
            androidCpuAcceleration_getStatus(&status);
    std::string message = status ? status : "";
    free(status);
    return std::make_pair(capability, std::move(message));
}

static CommandReturn checkHyperV() {
    return android::GetHyperVStatus();
}

static CommandReturn getCpuInfo() {
    auto pair = android::GetCpuInfo();
    // GetCpuInfo() returns a set of lines, we need to turn it into a
    // single line.
    std::replace(pair.second.begin(), pair.second.end(), '\n', '|');
    return pair;
}

static CommandReturn getWindowManager() {
    const std::string name = android::getWindowManagerName();
    return std::make_pair(name.empty() ? kGenericError : 0, name);
}

static CommandReturn getDesktopEnv() {
    const std::string name = android::getDesktopEnvironmentName();
    return std::make_pair(name.empty() ? kGenericError : 0, name);
}

#ifdef _WIN32
static CommandReturn dismOpen(DismSession *session) {
    HRESULT hr;

    hr = DismInitialize(DismLogErrorsWarningsInfo, NULL, NULL);
    if (hr == __HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED))
        return std::make_pair(hr, "emulator-check whpx commands require administrator privilege.");
    else if (hr != S_OK)
        return std::make_pair(hr, "Could not initialize Dism API.");

    *session = DISM_SESSION_DEFAULT;
    hr = DismOpenSession(DISM_ONLINE_IMAGE, NULL, NULL, session);
    if (hr != S_OK) {
        DismShutdown();
        return std::make_pair(hr, "Could not open Dism session.");
    }
    return std::make_pair(0, "dismOpen succeeded.");
}

static void dismClose(DismSession *session) {
    if (!session)
        return;

    DismCloseSession(*session);
    DismShutdown();
}

static char *dism_feature_state[] = {
    "NotPresent",
    "UninstallPending",
    "Staged",
    "Removed",
    "Installed",
    "InstallPending",
    "Superseded",
    "PartiallyInstalled"
};

static CommandReturn checkWHPX() {
    HRESULT hr;
    CommandReturn ret;
    DismSession Session = DISM_SESSION_DEFAULT;

    ret = dismOpen(&Session);
    if (ret.first != 0)
        return ret;

    DismFeatureInfo *FeatureInfo;
    hr = DismGetFeatureInfo(Session, L"HypervisorPlatform", NULL, DismPackageNone, &FeatureInfo);

    dismClose(&Session);

    if (hr == S_OK) {
        std::stringstream message;
	message << "Feature state of Windows Hypervisor Platform: " << FeatureInfo->FeatureState
                << "(" << dism_feature_state[FeatureInfo->FeatureState] << ").";
        return std::make_pair(0, message.str());
    }
    else
        return std::make_pair(hr, "DismGetFeatureInfo failed.");
}

static CommandReturn enableWHPX() {
    HRESULT hr;
    CommandReturn ret;
    DismSession Session = DISM_SESSION_DEFAULT;

    ret = dismOpen(&Session);
    if (ret.first != 0)
        return ret;

    hr = DismEnableFeature(Session, L"HypervisorPlatform", NULL, DismPackageNone, FALSE, NULL, 0, FALSE, NULL, NULL, NULL);

    dismClose(&Session);

    if (hr == S_OK || hr == (HRESULT)ERROR_SUCCESS_REBOOT_REQUIRED)
        return std::make_pair(0, "Windows Hypervisor Platform is enabled in Windows Features. Reboot is required to take effect.");
    else
        return std::make_pair(hr, "DismEnableFeature Failed.");
}

static CommandReturn disableWHPX() {
    HRESULT hr;
    CommandReturn ret;
    DismSession Session = DISM_SESSION_DEFAULT;

    ret = dismOpen(&Session);
    if (ret.first != 0)
        return ret;

    hr = DismDisableFeature(Session, L"HypervisorPlatform", NULL, TRUE, NULL, NULL, NULL);

    dismClose(&Session);

    if (hr == S_OK || hr == (HRESULT)ERROR_SUCCESS_REBOOT_REQUIRED)
        return std::make_pair(0, "Windows Hypervisor Platform is disabled in Windows Features. Reboot is required to take effect.");
    else
        return std::make_pair(hr, "DismDisableFeature Failed.");
}
#endif

constexpr struct Option {
    const char* arg;
    const char* help;
    CommandReturn (*handler)();
    bool printRawAndStop;
} options[] = {
        {"-h", "\tShow this help message", &help, true},
        {"-help", "Show this help message", &help, true},
        {"--help", "Show this help message", &help, true},
        {"accel", "Check the CPU acceleration support", &checkCpuAcceleration},
        {"hyper-v", "Check if hyper-v is installed and running (Windows)",
         &checkHyperV},
        {"cpu-info", "Return the CPU model information", &getCpuInfo},
        {"window-mgr", "Return the current window manager name",
         &getWindowManager},
        {"desktop-env", "Return the current desktop environment name",
         &getDesktopEnv},
#ifdef _WIN32
        {"whpx", "Check if WHPX is installed and running (Windows)",
         &checkWHPX},
        {"enable-whpx", "Enable Windows Hypervisor Platform in Windows Features (Windows)",
         &enableWHPX},
        {"disable-whpx", "Disable Windows Hypervisor Platform in Windows Features (Windows)",
         &disableWHPX},
#endif
};

static std::string usage() {
    std::ostringstream str;
    str <<
            R"(Usage: emulator-check <argument1> [<argument2>...]

Performs the set of checks requested in <argumentX> and returns the result in
the following format:
<argument1>:
<return code for <argument1>>
<a single line of text information returned for <argument1>>
<argument1>
<argument2>:
<return code for <argument2>>
<a single line of text information returned for <argument2>>
<argument2>
...

<argumentX> is any of:

)";

    for (const auto& option : options) {
        str << "    " << option.arg << "\t\t" << option.help << '\n';
    }

    str << '\n';

    return str.str();
}

static CommandReturn help() {
    return std::make_pair(0, usage());
}

static int error(const char* format, const char* arg = nullptr) {
    if (format) {
        fprintf(stderr, format, arg);
        fprintf(stderr, "\n\n");
    }
    fprintf(stderr, "%s\n", usage().c_str());
    return kGenericError;
}

static int processArguments(int argc, const char* const* argv) {
    android::base::Optional<int> retCode;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto opt = std::find_if(
                std::begin(options), std::end(options),
                [arg](const Option& opt) { return arg == opt.arg; });
        if (opt == std::end(options)) {
            printf("%s:\n%d\nUnknown argument\n%s\n", arg.data(), kGenericError,
                   arg.data());
            continue;
        }

        auto handlerRes = opt->handler();
        if (!retCode) {
            // Remember the first return value for the compatibility with the
            // previous version.
            retCode = handlerRes.first;
        }

        if (opt->printRawAndStop) {
            printf("%s\n", handlerRes.second.c_str());
            break;
        }

        printf("%s:\n%d\n%s\n%s\n", arg.data(), handlerRes.first,
               handlerRes.second.c_str(), arg.data());
    }

    return retCode.valueOr(kGenericError);
}

int main(int argc, const char* const* argv) {
    if (argc < 2) {
        return error("Missing a required argument(s)");
    }

    android::emulation::injectConsoleAgents(
            android::emulation::MockAndroidConsoleFactory());
    return processArguments(argc, argv);
}
