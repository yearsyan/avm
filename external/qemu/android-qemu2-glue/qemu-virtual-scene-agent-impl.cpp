// Copyright (C) 2018 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/emulation/control/virtual_scene_agent.h"

#ifdef AEMU_CORE_ONLY

static void setInitialPosterNoop(const char* posterName, const char* filename) {
    (void)posterName;
    (void)filename;
}

static bool loadPosterNoop(const char* posterName, const char* filename) {
    (void)posterName;
    (void)filename;
    return false;
}

static void enumeratePostersNoop(void* context,
                                 EnumeratePostersCallback callback) {
    (void)context;
    (void)callback;
}

static void setPosterScaleNoop(const char* posterName, float scale) {
    (void)posterName;
    (void)scale;
}

static void setAnimationStateNoop(bool state) {
    (void)state;
}

static bool getAnimationStateNoop() {
    return false;
}

static bool reloadEnvironmentNoop(const char* environmentData) {
    (void)environmentData;
    return false;
}

static void* getAnimationStateEventListenerNoop() {
    return nullptr;
}

static void enumerateWebcamsNoop(void* context,
                                 EnumerateWebcamsCallback callback) {
    (void)context;
    (void)callback;
}

static const QAndroidVirtualSceneAgent sQAndroidVirtualSceneAgent = {
        setInitialPosterNoop,
        loadPosterNoop,
        enumeratePostersNoop,
        setPosterScaleNoop,
        setAnimationStateNoop,
        getAnimationStateNoop,
        reloadEnvironmentNoop,
        getAnimationStateEventListenerNoop,
        enumerateWebcamsNoop,
};

#else

#include "android/virtualscene/VirtualSceneManager.h"

using android::virtualscene::VirtualSceneManager;

static const QAndroidVirtualSceneAgent sQAndroidVirtualSceneAgent = {
        VirtualSceneManager::setInitialPoster,
        VirtualSceneManager::loadPoster,
        VirtualSceneManager::enumeratePosters,
        VirtualSceneManager::setPosterScale,
        VirtualSceneManager::setAnimationState,
        VirtualSceneManager::getAnimationState,
        VirtualSceneManager::reloadEnvironment,
        VirtualSceneManager::getAnimationStateEventListener,
        VirtualSceneManager::enumerateWebcams,
};

#endif

extern "C" const QAndroidVirtualSceneAgent* const gQAndroidVirtualSceneAgent =
        &sQAndroidVirtualSceneAgent;
