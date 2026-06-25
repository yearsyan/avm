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

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include "aemu/base/synchronization/Lock.h"
#include "ver/virtual_environment_renderer_types.h"

namespace android {
namespace ver {

class Scene;
class RendererView;

/**
 * @brief Singleton-like manager for all virtual scenes and views.
 */
class ScenesManager {
public:
    static void setSearchPaths(
            const std::vector<std::filesystem::path>& resourceSearchPaths);

    static bool configArgumentFileExists(const SceneConfig& config);

    static Scene* createScene(const SceneConfig& config);

    static bool renderView(Scene* scene,
                           RendererView* view,
                           std::function<void()> finishCallback,
                           uint64_t* outFrameTime);

    static bool removeScene(Scene* scene);

    static RendererView* createView();
    static bool removeView(RendererView* viewPtr);

    static bool removeAll();

    static const void* getEglDispatch() { return mEglDispatch; }
    static const void* getGles2Dispatch() { return mGles2Dispatch; }

    static void setDispatch(const void* egl, const void* gles2) {
        mEglDispatch = egl;
        mGles2Dispatch = gles2;
    }

private:
    static android::base::StaticLock mScenesLock;
    static std::vector<std::unique_ptr<Scene>> mScenes;
    static std::vector<std::filesystem::path> mResourceSearchPaths;

    static android::base::StaticLock mViewsLock;
    static std::vector<std::unique_ptr<RendererView>> mViews;

    static const void* mEglDispatch;
    static const void* mGles2Dispatch;
};

}  // namespace ver
}  // namespace android
