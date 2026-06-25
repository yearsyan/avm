// Copyright (C) 2019 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/emulation/control/automation_agent.h"

#include <string_view>

namespace android {
namespace automation {

static void reset() {
#ifndef AEMU_CORE_ONLY
    return AutomationController::get().reset();
#endif
}

static StartResult start_recording(std::string_view filename) {
#ifdef AEMU_CORE_ONLY
    (void)filename;
    return android::base::Ok();
#else
    return AutomationController::get().startRecording(filename.data());
#endif
}

static StopResult stop_recording() {
#ifdef AEMU_CORE_ONLY
    return android::base::Ok();
#else
    return AutomationController::get().stopRecording();
#endif
}

static StartResult start_playback(std::string_view filename) {
#ifdef AEMU_CORE_ONLY
    (void)filename;
    return android::base::Ok();
#else
    return AutomationController::get().startPlayback(filename.data());
#endif
}

static StopResult stop_playback() {
#ifdef AEMU_CORE_ONLY
    return android::base::Ok();
#else
    return AutomationController::get().stopPlayback();
#endif
}

static StartResult start_playback_with_callback(std::string_view filename,
                                                void (*onStopCallback)()) {
#ifdef AEMU_CORE_ONLY
    (void)filename;
    (void)onStopCallback;
    return android::base::Ok();
#else
    return AutomationController::get().startPlaybackWithCallback(
            filename.data(), onStopCallback);
#endif
}

static void set_macro_name(std::string_view macroName,
                           std::string_view filename) {
#ifndef AEMU_CORE_ONLY
    AutomationController::get().setMacroName(macroName, filename);
#else
    (void)macroName;
    (void)filename;
#endif
}

static std::string get_macro_name(std::string_view filename) {
#ifdef AEMU_CORE_ONLY
    (void)filename;
    return {};
#else
    return AutomationController::get().getMacroName(filename);
#endif
}

static std::pair<uint64_t, uint64_t> get_metadata(std::string_view filename) {
#ifdef AEMU_CORE_ONLY
    (void)filename;
    return {0, 0};
#else
    return AutomationController::get().getMetadata(filename);
#endif
}

static const QAndroidAutomationAgent sQAndroidAutomationAgent = {
        .reset = reset,
        .startRecording = start_recording,
        .stopRecording = stop_recording,
        .startPlayback = start_playback,
        .stopPlayback = stop_playback,
        .startPlaybackWithCallback = start_playback_with_callback,
        .setMacroName = set_macro_name,
        .getMacroName = get_macro_name,
        .getMetadata = get_metadata};

extern "C" const QAndroidAutomationAgent* const gQAndroidAutomationAgent =
        &sQAndroidAutomationAgent;

}  // namespace automation
}  // namespace android
