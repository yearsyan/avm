// Copyright 2016 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/main-emugl.h"

#include "aemu/base/memory/ScopedPtr.h"
#include "android/avd/util.h"
#include "android/console.h"
#include "android/opengl/gpuinfo.h"
#include "android/utils/debug.h"
#include "android/utils/string.h"
#include "host-common/FeatureControl.h"
#include "host-common/feature_control.h"
#include "host-common/opengles.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

using android::base::ScopedCPtr;
namespace fc = android::featurecontrol;

static const char DEFAULT_SOFTWARE_GPU_MODE[] = "lavapipe";

// Calculates gpu mode to be used based on -gpu command line option, avd config
// or the ui option
std::string gpuChoiceBasedOnGpuOptions(
        const char* gpuOption,
        const char* hwGpuModePtr,
        enum WinsysPreferredGlesBackend uiPreferredBackend) {
    // Support old style gpu parameters for backwards compatibility
    if (gpuOption && !strcmp("swiftshader_indirect", gpuOption)) {
        gpuOption = "swiftshader";
    }
    if (gpuOption && !strcmp("swangle_indirect", gpuOption)) {
        gpuOption = "swangle";
    }

    std::string gpuChoice;
    if (gpuOption) {
        // It's enforced with -gpu option
        gpuChoice = gpuOption;
    } else if (uiPreferredBackend != WINSYS_GLESBACKEND_PREFERENCE_AUTO) {
        // Use UI preference
        switch (uiPreferredBackend) {
            // Keep deprecated swangle and swiftshader UI modes for tests
            case WINSYS_GLESBACKEND_PREFERENCE_ANGLE_DEPRECATED:
                gpuChoice = "swangle";
                break;
            case WINSYS_GLESBACKEND_PREFERENCE_SWIFTSHADER_DEPRECATED:
                gpuChoice = "swiftshader";
                break;
            case WINSYS_GLESBACKEND_PREFERENCE_SOFTWARE:
                gpuChoice = "software";
                break;
            case WINSYS_GLESBACKEND_PREFERENCE_NATIVEGL_DEPRECATED:
            case WINSYS_GLESBACKEND_PREFERENCE_HARDWARE:
                gpuChoice = "host";
                break;
            default:
                gpuChoice = "auto";
        }
    } else if (hwGpuModePtr) {
        // Use hw gpu mode
        gpuChoice = hwGpuModePtr;
    } else {
        gpuChoice = "auto";
    }

    const std::vector<std::string> allowedOptions = {
            "auto", "host", "lavapipe", "swiftshader", "swangle", "software"};
    bool validOption = false;
    for (auto& option : allowedOptions) {
        if (option == gpuChoice) {
            validOption = true;
            break;
        }
    }
    if (!validOption) {
        derror("%s: Selected GPU option '%s' is not valid, switching to "
               "'auto' mode.",
               __func__, gpuChoice.c_str());
        gpuChoice = "auto";
    }

    return gpuChoice;
}

// Converts selected gpu mode string to feature flags and overrides them if
// not already overwritten.
void convertGpuOptionsToFeatureFlags(
        const char* gpuOption,
        const char* hwGpuModePtr,
        enum WinsysPreferredGlesBackend uiPreferredBackend) {
    const std::string gpuChoice = gpuChoiceBasedOnGpuOptions(
            gpuOption, hwGpuModePtr, uiPreferredBackend);

    // Lavapipe has a special feature flag, which would result usage
    // of lavapipe even when swiftshader/swange was requested. This
    // is mainly to migrate old scripts to lavapipe automatically.
    const bool force_lavapipe_on_software =
            fc::isEnabled(fc::ForceLavapipeForSoftwareRendering);
    if (gpuChoice == "host") {
        feature_set_if_not_overridden(kFeature_ForceGpuHost, true);
    } else if (gpuChoice == "software") {
        // "software" is a special term to select best software mode for the
        // platform/avd and not a real gpu backend mode.
        feature_set_if_not_overridden(kFeature_ForceGpuSoftware, true);
    } else if ((gpuChoice == "lavapipe") ||
               (force_lavapipe_on_software &&
                (gpuChoice == "swiftshader" || gpuChoice == "swangle"))) {
        feature_set_if_not_overridden(kFeature_ForceLavapipe, true);
    } else if (gpuChoice == "swiftshader") {
        feature_set_if_not_overridden(kFeature_ForceSwiftshader, true);
    } else if (gpuChoice == "swangle") {
        feature_set_if_not_overridden(kFeature_ForceANGLE, true);
    } else {
        // Auto mode, no need to set any feature flags
    }
}

bool androidEmuglConfigInit(
        EmuglConfig* config,
        const char* gpuOption,
        const char* hwGpuModePtr,
        bool noWindow,
        enum WinsysPreferredGlesBackend uiPreferredBackend) {
    // Check gpu selection control flags, if any of them is set, other options
    // will be overwritten.
    const std::vector<fc::Feature> gpuControlFeatures = {
            fc::ForceGpuHost,     fc::ForceGpuSoftware, fc::ForceLavapipe,
            fc::ForceSwiftshader, fc::ForceANGLE,
    };
    bool anyFeatureControlIsSet = false;
    for (fc::Feature feature : gpuControlFeatures) {
        if (fc::isEnabled(feature)) {
            dinfo("GPU mode control feature flag '%s' is set",
                  fc::featureToString(feature));
            anyFeatureControlIsSet = true;
        }
    }

    if (anyFeatureControlIsSet) {
        // Gpu selection will be done based on user provided feature flags
        dinfo("GPU mode selection will be done based on user provided feature "
              "flags");
    } else {
        // Set feature flags based on the options provided and only use feature
        // flags to determine the gpu mode.
        convertGpuOptionsToFeatureFlags(gpuOption, hwGpuModePtr,
                                        uiPreferredBackend);
    }

    // when set, 'force' feature flags will overwrite other options
    const bool force_host = fc::isEnabled(fc::ForceGpuHost);
    const bool force_software = fc::isEnabled(fc::ForceGpuSoftware);

    // Select GPU mode based on feature flags
    std::string gpuChoice = "auto";
    if (force_host) {
        gpuChoice = "host";
    } else if (force_software) {
        gpuChoice = DEFAULT_SOFTWARE_GPU_MODE;
    } else {
        // Finer control feature flags
        const bool force_lavapipe = fc::isEnabled(fc::ForceLavapipe);
        const bool force_swiftshader = fc::isEnabled(fc::ForceSwiftshader);
        const bool force_swangle = fc::isEnabled(fc::ForceANGLE);
        if (force_lavapipe) {
            gpuChoice = "lavapipe";
        } else if (force_swiftshader) {
            gpuChoice = "swiftshader";
        } else if (force_swangle) {
            gpuChoice = "swangle";
        }
    }

    bool hostGpuDenylisted = false;

    // Only check the blacklist for 'auto' and 'host' modes.
    const bool gpuChoiceAuto = (gpuChoice == "auto");
    const bool gpuChoiceHost = (gpuChoice == "host");

    if (gpuChoiceAuto || gpuChoiceHost) {
        bool switchToSoftware = false;

        // Decide if a switch to software is needed
        const bool onDenyList = isHostGpuBlacklisted();
        if (onDenyList) {
            if (gpuChoiceAuto) {
                // Auto switch to software if denylisted, give warning
                dwarning(
                        "Your GPU drivers may have a bug. "
                        "Switching to software rendering.");
                switchToSoftware = true;
            } else {
                // We cannot use vulkan on this device, it's highly
                // likely that it'll crash with host GPU Overwrite
                // user's 'host' setting and use software rendering
                derror("Your GPU cannot be used for hardware rendering."
                       " Consider using software rendering.");
            }
        }

        if (switchToSoftware) {
            hostGpuDenylisted = onDenyList;
            setGpuBlacklistStatus(hostGpuDenylisted);
            gpuChoice = DEFAULT_SOFTWARE_GPU_MODE;
        }
    }

    return emuglConfig_init(config, gpuChoice.c_str(), noWindow);
}
