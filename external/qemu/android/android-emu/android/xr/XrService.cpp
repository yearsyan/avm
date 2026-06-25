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

#include "XrService.h"
#include "aemu/base/async/ThreadLooper.h"
#include "aemu/base/logging/Log.h"
#include "android/emulation/android_qemud.h"

using xr_emulator_proto::EmulatorRequest;
using xr_emulator_proto::EnvironmentMode;
using xr_emulator_proto::InputMode;
using xr_emulator_proto::MsgType;
using xr_emulator_proto::ViewportControlMode;

namespace android {
namespace xr {
namespace xr_service {

#define XR_SERVICE_NAME "xr_service"
// NOTE: This is an arbitrary value that was picked to be "big enough" for our
// current needs, feel free to increase the number if more are necessary.
// Remember that this number should be N+1, where N is the number of callbacks
// needing to be registered. The +1 is so that the 0 index can be used as a
// invalid handle.
#define XR_SERVICE_MAX_CALLBACKS 4

struct Handle {
    uint64_t u64[1];
};
Handle* const kInvalidHandleValue = static_cast<Handle*>(nullptr);

struct XrServiceCallbackData {
    XrServiceCallbackFunction* callback;
    void* user_data;
};

struct XrServiceInternalState {
    QemudService* service;
    QemudClient* client;
    uint32_t registeredCallbacksCount;
    XrServiceCallbackData registeredCallbacks[XR_SERVICE_MAX_CALLBACKS];
};
static absl::Mutex sXrServiceMutex;
static XrServiceInternalState sXrServiceState ABSL_GUARDED_BY(sXrServiceMutex);

static absl::Mutex sXrOptionsCacheMutex;
static xr_emulator_proto::XrOptions sXrOptionsCache
        ABSL_GUARDED_BY(sXrOptionsCacheMutex);

static Handle* handleFromCallbackIndex(uint64_t index) {
    Handle result = {index};
    return (Handle*)(void*)result.u64[0];
}
static uint64_t callbackIndexFromHandle(Handle* handle) {
    uint64_t result = {(uint64_t)(void*)handle};
    return result;
}

static void clientReceive(void* /*userContext*/,
                          uint8_t* message,
                          int length,
                          QemudClient* /*client*/) {
    // NOTE: We hold this mutex during the execution of all the callbacks,
    // so any callback that calls back into this API will cause a deadlock.
    // None of our current callbacks will do this, and so this is fine for now.
    absl::MutexLock lock(&sXrServiceMutex);

    uint32_t callbacksCount = sXrServiceState.registeredCallbacksCount;
    XrServiceCallbackData* callbacks = sXrServiceState.registeredCallbacks;

    xr_emulator_proto::EmulatorResponse response;
    if (response.ParseFromArray(message, length)) {
        for (uint32_t index = 0; index < callbacksCount; ++index) {
            XrServiceCallbackFunction* callback = callbacks[index].callback;
            void* user_data = callbacks[index].user_data;

            if (callback) {
                callback(user_data, response);
            }
        }
    } else {
        derror("Unable to parse message from guest!");
    }
}

static void clientClose(void* userContext) {
    (void)userContext;
    absl::MutexLock lock(&sXrServiceMutex);
    sXrServiceState.client = nullptr;
}

static QemudClient* clientConnect(void* userContext,
                                  QemudService* service,
                                  int channel,
                                  const char* clientArgsCString) {
    (void)userContext;
    QemudClient* result = nullptr;
    absl::MutexLock lock(&sXrServiceMutex);

    // NOTE: Currently we only supprt a single registered client. If one is
    // already connected we drop the new one on the floor
    if (!sXrServiceState.client) {
        result = qemud_client_new(service, channel, clientArgsCString, nullptr,
                                  clientReceive, clientClose, nullptr, nullptr);
        qemud_client_set_framing(result, 1);

        sXrServiceState.client = result;
    } else {
        derror("The XR Service received a second qemud client connection, only one connection is supported right now. Please ensure only one qemud client is connecting to this service.");
        assert(false);
    }

    return result;
}

static xr_emulator_proto::XrOptions updateXrOptionsCache(
        const xr_emulator_proto::XrOptions& new_options) {
    absl::MutexLock lock(&sXrOptionsCacheMutex);

    if (new_options.has_passthrough_coefficient()) {
        sXrOptionsCache.set_passthrough_coefficient(
                new_options.passthrough_coefficient());
    }
    if (new_options.has_environment()) {
        sXrOptionsCache.set_environment(new_options.environment());
    }
    if (new_options.has_dimming_value()) {
        sXrOptionsCache.set_dimming_value(new_options.dimming_value());
    }

    return sXrOptionsCache;
}

static void handleXrOptionsEventCacheUpdate(
        void* /*user_data*/,
        const xr_emulator_proto::EmulatorResponse& response) {
    if (response.response_case() ==
        xr_emulator_proto::EmulatorResponse::kXrOptions) {
        xr_service::updateXrOptionsCache(response.xr_options());
    }
}

static Handle* registerCallbackLocked(XrServiceCallbackFunction* callback,
                                      void* user_data) {
    Handle* result = nullptr;
    if (sXrServiceState.registeredCallbacksCount < XR_SERVICE_MAX_CALLBACKS) {
        XrServiceCallbackData& registered =
                sXrServiceState.registeredCallbacks
                        [sXrServiceState.registeredCallbacksCount];
        registered.callback = callback;
        registered.user_data = user_data;

        result = handleFromCallbackIndex(
                sXrServiceState.registeredCallbacksCount);

        sXrServiceState.registeredCallbacksCount++;
    } else {
        derror("Failed to register callback with the XR Service. Currently only %d callbacks can be registered globally at this time. Please note, whichever service attempted this might now act strangely due to not receiveing XR Events",
               XR_SERVICE_MAX_CALLBACKS - 1);
        assert(false);
    }

    return result;
}

void initializeQemudXrService() {
    android::base::ThreadLooper::runOnMainLooperAndWaitForCompletion([]() {
        absl::MutexLock lock(&sXrServiceMutex);
        if (!sXrServiceState.service) {
            // Our count should start at 1, so the 0 index is a valid thing to
            // unregister later if it is encountered.
            sXrServiceState.registeredCallbacksCount = 1;

            // Register the callback responsible for keeping the cache updated
            // so that none of the other callbacks need to worry about it
            registerCallbackLocked(handleXrOptionsEventCacheUpdate, nullptr);

            sXrServiceState.service =
                    qemud_service_register(XR_SERVICE_NAME, 0, nullptr,
                                           clientConnect, nullptr, nullptr);
        } else {
            derror("XR Service was already initialized!");
            assert(false);
        }
    });
}

Handle* registerCallback(XrServiceCallbackFunction* callback, void* user_data) {
    absl::MutexLock lock(&sXrServiceMutex);
    return registerCallbackLocked(callback, user_data);
}

void unregisterCallback(Handle* handle) {
    uint64_t index = callbackIndexFromHandle(handle);
    absl::MutexLock lock(&sXrServiceMutex);
    // Zero'ing out the callback data will cause this entry to be skipped on the
    // next loop through, we dont have enough callbacks right now to justify
    // needing to do the full work of freeing up this space again for new
    // callbacks, we could potentially make a free list of them as they become
    // available, but then we need a way to invalidate the old handles we gave
    // out.
    if (index < XR_SERVICE_MAX_CALLBACKS) {
        memset(sXrServiceState.registeredCallbacks + index, 0,
               sizeof(*sXrServiceState.registeredCallbacks));
    }
}

xr_emulator_proto::XrOptions retrieveXrOptionsCache() {
    absl::MutexLock lock(&sXrOptionsCacheMutex);
    return sXrOptionsCache;
}

static void sendBytes(const uint8_t* bytes, uint64_t size) {
    // NOTE: We use the blocking call to avoid copying the payload.
    //       Idk if this is worth it, we might not want to block
    //       whatever thread is calling this...
    android::base::ThreadLooper::runOnMainLooperAndWaitForCompletion([&]() {
        absl::MutexLock lock(&sXrServiceMutex);
        if (sXrServiceState.client) {
            qemud_client_send(sXrServiceState.client, bytes, size);
        } else {
            dwarning(
                    "Attempt to send a message to qemud XR client without an active connection! Please ensure the XR Service is initialized and the guest has connected before sending any messages.");
        }
    });
}

static void sendString(const std::string_view str) {
    sendBytes((const uint8_t*)str.data(), str.size());
}

void sendEmulatorRequest(const xr_emulator_proto::EmulatorRequest& request) {
    std::string serialized_request;
    request.SerializeToString(&serialized_request);

    sendString(serialized_request);
}

void sendHeadRotation(float x, float y, float z) {
    EmulatorRequest request;
    request.set_msg_type(MsgType::MSG_TYPE_SET_HEAD_ROTATION);
    auto rotation_param = request.mutable_xr_head_rotation_event();
    rotation_param->set_x(x);
    rotation_param->set_y(y);
    rotation_param->set_z(z);

    sendEmulatorRequest(request);
}

void sendHeadMovement(float delta_x, float delta_y, float delta_z) {
    EmulatorRequest request;
    request.set_msg_type(MsgType::MSG_TYPE_SET_HEAD_MOVEMENT);
    auto translation_param = request.mutable_xr_head_movement_event();
    translation_param->set_delta_x(delta_x);
    translation_param->set_delta_y(delta_y);
    translation_param->set_delta_z(delta_z);

    sendEmulatorRequest(request);
}

void sendHeadAngularVelocity(float omega_x, float omega_y, float omega_z) {
    EmulatorRequest request;
    request.set_msg_type(MsgType::MSG_TYPE_SET_HEAD_ANGULAR_VELOCITY);
    auto angular_velocity_param =
            request.mutable_xr_head_angular_velocity_event();
    angular_velocity_param->set_omega_x(omega_x);
    angular_velocity_param->set_omega_y(omega_y);
    angular_velocity_param->set_omega_z(omega_z);

    sendEmulatorRequest(request);
}

void sendHeadVelocity(float x, float y, float z) {
    EmulatorRequest request;
    request.set_msg_type(MsgType::MSG_TYPE_SET_HEAD_VELOCITY);
    auto velocity_param = request.mutable_xr_head_velocity_event();
    velocity_param->set_x(x);
    velocity_param->set_y(y);
    velocity_param->set_z(z);

    sendEmulatorRequest(request);
}

void sendHandEvent(int32_t x, int32_t y, int32_t buttons, int32_t display) {
    EmulatorRequest request;
    request.set_msg_type(MsgType::MSG_TYPE_SET_HAND_EVENT);
    auto mouseEvent = request.mutable_xr_hand_event();
    mouseEvent->set_x(x);
    mouseEvent->set_y(y);
    mouseEvent->set_buttons(buttons);
    mouseEvent->set_display(display);

    sendEmulatorRequest(request);
}

void sendEyeEvent(int32_t x, int32_t y, int32_t buttons, int32_t display) {
    EmulatorRequest request;
    request.set_msg_type(MsgType::MSG_TYPE_SET_EYE_EVENT);
    auto mouseEvent = request.mutable_xr_eye_event();
    mouseEvent->set_x(x);
    mouseEvent->set_y(y);
    mouseEvent->set_buttons(buttons);
    mouseEvent->set_display(display);

    sendEmulatorRequest(request);
}

void sendInputMode(XrInputMode mode) {
    EmulatorRequest request;
    request.set_msg_type(MsgType::MSG_TYPE_SET_INPUT_MODE);
    auto proto_mode = InputMode::INPUT_MODE_MOUSE_UNKNOWN;
    switch (mode) {
        case XR_INPUT_MODE_MOUSE_KEYBOARD:
            proto_mode = InputMode::INPUT_MODE_MOUSE_KEYBOARD;
            break;
        case XR_INPUT_MODE_HAND_RAYCAST:
            proto_mode = InputMode::INPUT_MODE_HAND_RAYCAST;
            break;
        case XR_INPUT_MODE_EYE_TRACKING:
            proto_mode = InputMode::INPUT_MODE_EYE_TRACKING;
            break;
        default:
            dwarning("Unknown XR input mode requested: %d\n", mode);
            break;
    }
    request.set_input_mode(proto_mode);

    sendEmulatorRequest(request);
}

void sendScreenRecenter() {
    EmulatorRequest request;
    request.set_msg_type(MsgType::MSG_TYPE_RECENTER_SCREEN);

    sendEmulatorRequest(request);
}

void sendViewportControlMode(XrViewportControlMode mode) {
    EmulatorRequest request;
    request.set_msg_type(MsgType::MSG_TYPE_SET_VIEWPORT_CONTROL);
    ViewportControlMode proto_mode =
            ViewportControlMode::VIEWPORT_CONTROL_MODE_UNKNOWN;
    switch (mode) {
        case VIEWPORT_CONTROL_MODE_PAN:
            proto_mode = ViewportControlMode::VIEWPORT_CONTROL_MODE_PAN;
            break;
        case VIEWPORT_CONTROL_MODE_ZOOM:
            proto_mode = ViewportControlMode::VIEWPORT_CONTROL_MODE_ZOOM;
            break;
        case VIEWPORT_CONTROL_MODE_ROTATE:
            proto_mode = ViewportControlMode::VIEWPORT_CONTROL_MODE_ROTATE;
            break;
        default:
            dwarning("Unknown XR viewport mode requested: %d\n", mode);
            break;
    }
    request.set_viewport_control_mode(proto_mode);

    sendEmulatorRequest(request);
}

void sendHandGesture(xr_emulator_proto::HandGesture gesture) {
    EmulatorRequest request;
    request.set_msg_type(MsgType::MSG_TYPE_SET_HAND_GESTURE);
    request.set_hand_gesture(gesture);

    sendEmulatorRequest(request);
}

void sendPassthroughCoefficient(float value) {
    EmulatorRequest request;
    request.set_msg_type(MsgType::MSG_TYPE_SET_OPTIONS);
    xr_emulator_proto::XrOptions* options = request.mutable_xr_options();
    options->set_passthrough_coefficient(value);

    xr_service::updateXrOptionsCache(*options);
    sendEmulatorRequest(request);
}

void sendEnvironment(xr_emulator_proto::XrOptions_Environment environment) {
    EmulatorRequest request;
    request.set_msg_type(MsgType::MSG_TYPE_SET_OPTIONS);
    xr_emulator_proto::XrOptions* options = request.mutable_xr_options();
    options->set_environment(environment);

    xr_service::updateXrOptionsCache(*options);
    sendEmulatorRequest(request);
}

void sendDimmingValue(float value) {
    EmulatorRequest request;
    request.set_msg_type(MsgType::MSG_TYPE_SET_OPTIONS);
    xr_emulator_proto::XrOptions* options = request.mutable_xr_options();
    options->set_dimming_value(value);

    xr_service::updateXrOptionsCache(*options);
    sendEmulatorRequest(request);
}

void sendXrOptions(const xr_emulator_proto::XrOptions& options) {
    EmulatorRequest request;
    request.set_msg_type(MsgType::MSG_TYPE_SET_OPTIONS);
    xr_emulator_proto::XrOptions* requestOptions = request.mutable_xr_options();
    *requestOptions = options;

    xr_service::updateXrOptionsCache(options);
    sendEmulatorRequest(request);
}

}  // namespace xr_service
}  // namespace xr
}  // namespace android
