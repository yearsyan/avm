// Copyright 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "android/skin/event.h"
#include "android/utils/compiler.h"

/**
 * @file virtio-input-multi-touch.h
 * @brief Bridge between host-side input events and virtio multi-touch emulation.
 *
 * This header defines functions for bridging host-side input events (mouse,
 * pen, touch) to the virtio multi-touch emulation. These functions handle the
 * translation of host event coordinates and states into the specific format
 * and coordinate space required by the guest's virtio multi-touch device, and
 * then send them to the device.
 */

ANDROID_BEGIN_HEADER

/* Maximum number of virtio input devices*/
#define VIRTIO_INPUT_MAX_NUM 11

/**
 * Sends a raw Linux input event to the virtio multi-touch device.
 *
 * @param type The Linux input event type (e.g., EV_ABS, EV_KEY).
 * @param code The event code (e.g., ABS_MT_POSITION_X, BTN_TOUCH).
 * @param value The event value.
 * @param displayId The ID of the display the event is targeted at.
 * @param touchpad Whether the event is for the touchpad device.
 * @return 1 if the event was sent successfully, 0 otherwise.
 */
extern int android_virtio_send_event_as_mt(int type,
                                           int code,
                                           int value,
                                           int displayId,
                                           bool touchpad);

/**
 * Translates and sends a host mouse event as a virtio multi-touch event.
 *
 * @param dx The relative X movement or absolute X coordinate.
 * @param dy The relative Y movement or absolute Y coordinate.
 * @param dz The mouse wheel movement.
 * @param buttonsState The current state of mouse buttons.
 * @param displayId The ID of the display the mouse is on.
 */
extern void android_virtio_send_mouse_as_mt(int dx,
                                            int dy,
                                            int dz,
                                            int buttonsState,
                                            int displayId);

/**
 * Translates and sends a host pen event as a virtio multi-touch event.
 *
 * @param dx The scaled X coordinate.
 * @param dy The scaled Y coordinate.
 * @param ev The host skin event containing pen details.
 * @param buttonsState The current state of pen buttons.
 * @param displayId The ID of the display the pen is on.
 */
extern void android_virtio_send_pen_as_mt(int dx,
                                          int dy,
                                          const SkinEvent* ev,
                                          int buttonsState,
                                          int displayId);

/**
 * Translates and sends a host touch event as a virtio multi-touch event.
 *
 * @param data The host skin event containing touch details.
 * @param displayId The ID of the display where the touch occurred.
 */
extern void android_virtio_send_touch_as_mt(const SkinEvent* const data,
                                            int displayId);

/**
 * Translates and sends a host touchpad event as a virtio multi-touch event.
 *
 * @param data The host skin event containing touchpad details.
 * @param touchpadId The ID of the touchpad device.
 */
extern void android_virtio_send_touchpad_as_mt(const SkinEvent* const data,
                                               int touchpadId);
ANDROID_END_HEADER
