/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <stdint.h>
#include <string_view>

#include "android/xr-defines.h"
#include "xr_emulator_conn.pb.h"

namespace android {
namespace xr {
namespace xr_service {

struct Handle;
extern Handle* const kInvalidHandleValue;

using XrServiceCallbackFunction =
        void(void* user_data,
             const xr_emulator_proto::EmulatorResponse& response);

// Registers the QEMUD service to handle XR related communications with the
// guest.
void initializeQemudXrService();

// Registers/Unregisters a function that should be invoked when any
// EmulatorResponse messages are received from the guest
//
// Please note, currently only 3 (XR_SERVICE_MAX_CALLBACKS - 1) callbacks are
// able to be registered globally at this time, unregistering a callback does
// not make a slot available for a 4th callback at this time. If you need more
// slots, please increase the slot count (XR_SERVICE_MAX_CALLBACKS) in the
// implementation file, and update this comment. :)
//
// Also note, any callbacks registered MUST NOT call back into this API, as
// we are not using a reentrant mutex. Any calls to this API from these
// callbacks will cause a deadlock.
Handle* registerCallback(XrServiceCallbackFunction* callback, void* user_data);
void unregisterCallback(Handle* handle);

// Retrieves the last seen set of XrOptions
xr_emulator_proto::XrOptions retrieveXrOptionsCache();

// Helper functions for the various data types we want to send around the
// emulator
void sendEmulatorRequest(const xr_emulator_proto::EmulatorRequest& request);
void sendHeadRotation(float x, float y, float z);
void sendHeadMovement(float delta_x, float delta_y, float delta_z);
void sendHeadAngularVelocity(float omega_x, float omega_y, float omega_z);
void sendHeadVelocity(float x, float y, float z);
void sendHandEvent(int32_t x, int32_t y, int32_t buttons, int32_t display);
void sendEyeEvent(int32_t x, int32_t y, int32_t buttons, int32_t display);
void sendInputMode(XrInputMode mode);
void sendScreenRecenter();
void sendViewportControlMode(XrViewportControlMode mode);
void sendHandGesture(xr_emulator_proto::HandGesture gesture);
void sendPassthroughCoefficient(float value);
void sendEnvironment(xr_emulator_proto::XrOptions_Environment environment);
void sendDimmingValue(float value);
void sendXrOptions(const xr_emulator_proto::XrOptions& options);

}  // namespace xr_service
}  // namespace xr
}  // namespace android
