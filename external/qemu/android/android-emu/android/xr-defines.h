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

enum XrInputMode {
    XR_INPUT_MODE_UNKNOWN = 0,
    XR_INPUT_MODE_MOUSE_KEYBOARD = 1,
    XR_INPUT_MODE_HAND_RAYCAST = 2,
    XR_INPUT_MODE_EYE_TRACKING = 3,
};

enum XrEnvironmentMode {
    XR_ENVIRONMENT_MODE_UNKNOWN = 0,
    XR_ENVIRONMENT_MODE_PASSTHROUGH_ON = 1,
    XR_ENVIRONMENT_MODE_PASSTHROUGH_OFF = 2,
    XR_ENVIRONMENT_MODE_LIVING_ROOM_DAY = 3,
    XR_ENVIRONMENT_MODE_LIVING_ROOM_NIGHT = 4,
};

enum XrViewportControlMode {
    VIEWPORT_CONTROL_MODE_UNKNOWN = 0,
    VIEWPORT_CONTROL_MODE_PAN = 1,
    VIEWPORT_CONTROL_MODE_ZOOM = 2,
    VIEWPORT_CONTROL_MODE_ROTATE = 3,
};
