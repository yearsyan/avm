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

#pragma once

#include "host-common/opengl/emugl_config.h"
// #include "android/skin/winsys.h"
#include "android/skin/backend-defs.h"
#include "android/utils/compiler.h"

ANDROID_BEGIN_HEADER

// Convenience function used to initialize an EmuglConfig instance |config|
// with appropriate settings corresponding to an AVD startup configuration.
// |config| config to be initialized.
// |gpuOption| is the value of the '-gpu' option, if any.
// |hwGpuModePtr| hw gpu config value.
// |noWindow| is true iff the -no-window option was used.
// |uiPreferredBackend| communicates the preferred GLES backend from the UI.
// On success, initializes |config| and returns true. Return false on failure.
bool androidEmuglConfigInit(EmuglConfig* config,
                            const char* gpuOption,
                            const char* hwGpuModePtr,
                            bool noWindow,
                            enum WinsysPreferredGlesBackend uiPreferredBackend);

ANDROID_END_HEADER
