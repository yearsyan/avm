// Copyright 2019 The Android Open Source Project
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

#include "android/utils/compiler.h"

#include <stdint.h>

ANDROID_BEGIN_HEADER

typedef struct AndroidHwXrLedEvent {
    int lightid;
    int lightcolor;
} AndroidHwXrLedEvent;

typedef void (*AndroidHwXrLedFunc)(void* opaque,
                                   const AndroidHwXrLedEvent* event);

typedef struct {
    AndroidHwXrLedFunc led_forwarder;
} AndroidHwXrLedFuncs;

typedef struct QAndroidHwXrLedAgent {
    // used to register a new hw-xrlights back-end
    void (*setCallbacks)(void* opaque, const AndroidHwXrLedFuncs* funcs);
} QAndroidHwXrLedAgent;

ANDROID_END_HEADER