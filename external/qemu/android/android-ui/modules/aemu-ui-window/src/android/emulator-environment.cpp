/* Copyright (C) 2026 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

#include "android/emulator-window.h"

#include <memory>
#include <string>
#include <vector>

#include "aemu/base/logging/CLog.h"
#include "android/android.h"
#include "android/avd/info.h"
#include "android/avd/keys.h"
#include "android/avd/util.h"
#include "android/base/system/System.h"
#include "android/console.h"
#include "android/hw-sensors.h"
#include "android/network/globals.h"
#include "android/ui-emu-agent.h"
#include "android/utils/debug.h"
#include "android/utils/path.h"
#include "host-common/display_agent.h"
#include "host-common/hw-config-helper.h"
#include "host-common/opengles.h"
#include "host-common/vm_operations.h"
#include "host-common/window_agent.h"

#include "android/virtualscene/VirtualSceneManager.h"

using android::virtualscene::VirtualSceneManager;

static bool emulatorSetupEnvironment() {
    if (!getConsoleAgents() || !getConsoleAgents()->settings) {
        derror("%s: Console agents are not available!", __func__);
        return false;
    }
    AndroidHwConfig* hwCfg = getConsoleAgents()->settings->hw();
    const AvdInfo* avdInfo = getConsoleAgents()->settings->avdInfo();

    if (!hwCfg || !avdInfo) {
        derror("%s: Invalid AVD config", __func__);
        return false;
    }

    std::vector<std::filesystem::path> resourceBasePaths;
    {
        // If it's not a usable full path, try AVD local
        const char* avdBasePath = avdInfo_getContentPath(avdInfo);
        if (avdBasePath) {
            resourceBasePaths.push_back(std::filesystem::path(avdBasePath));
        }

        // If not in AVD folder, check 'resources' folder
        std::filesystem::path resourcesBasePath =
                std::filesystem::path(
                        android::base::System::get()->getLauncherDirectory()) /
                "resources";
        resourceBasePaths.push_back(resourcesBasePath);
    }
    ver_initialize(resourceBasePaths, android_getEGLDispatch(),
                   android_getGLESv2Dispatch());

    int envWidth, envHeight;
    androidHwConfig_getScreenDimensions(hwCfg, &envWidth, &envHeight);
    int hwLcdWidth, hwLcdHeight;
    androidHwConfig_getLcdDimensions(hwCfg, &hwLcdWidth, &hwLcdHeight);

    // Send layout parameters to the compositor when display position and size
    // should be adjusted, note that this should be done even when there are
    // errors with the environment scene setup
    if (hwLcdWidth < envWidth && hwLcdHeight < envHeight) {
        dprint("%s: Setting up display layout at env:%dx%d, lcd:%dx%d",
                __func__, envWidth, envHeight, hwLcdWidth, hwLcdHeight);
        // Center the display at it's original size
        int displayPosX = (envWidth - hwLcdWidth) / 2;
        int displayPosY = (envHeight - hwLcdHeight) / 2;
        android_setOpenglesDisplayLayout(envWidth, envHeight, displayPosX,
                                            displayPosY, hwLcdWidth,
                                            hwLcdHeight);
    }

    // Check if the camera is set to 'environment' or 'virtualscene'
    const std::string hwCameraBack = hwCfg->hw_camera_back;
    const std::string hwCameraFront = hwCfg->hw_camera_front;
    const bool transparentDisplay = hwCfg->hw_lcd_transparent;
    const bool cameraUsesEnvironment = (hwCameraBack == "environment") ||
                                       (hwCameraFront == "environment") ||
                                       (hwCameraBack == "virtualscene");
    const bool backgroundUsesEnvironment = transparentDisplay;
    const bool environmentRequired =
            cameraUsesEnvironment || backgroundUsesEnvironment;

    if (!environmentRequired) {
        dinfo("%s: Environment scene is not required", __func__);
        return true;
    }

    // Initialize virtual scene and background view
    if (!VirtualSceneManager::initialize(backgroundUsesEnvironment,
                                         transparentDisplay)) {
        derror("%s: Cannot initialize virtual scene for the environment",
               __func__);
        return false;
    }

    return true;
}

extern "C" {

bool emulator_window_load_environment() {
    return emulatorSetupEnvironment();
}
}
