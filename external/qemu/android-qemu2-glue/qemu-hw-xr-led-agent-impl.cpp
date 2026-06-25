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

#include "android/emulation/control/hw_xr_led_agent.h"

#include "android/hw-xrlights.h"

namespace {

void set_callbacks(void* opaque, const AndroidHwXrLedFuncs* funcs) {
    android_hw_xrlights_set(opaque, funcs);
}

const QAndroidHwXrLedAgent sQAndroidHwXrLedAgent = {
        .setCallbacks = set_callbacks,
};

}  // namespace

extern "C" const QAndroidHwXrLedAgent* const gQAndroidHwXrLedAgent =
        &sQAndroidHwXrLedAgent;