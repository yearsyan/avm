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

#include <stddef.h>
#include <cmath>
#include <fstream>
#include <memory>
#include <vector>

#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"

#include "Renderer.h"
#include "Scene.h"
#include "ScenesManager.h"
#include "TextureUtils.h"

#include "raw_image_sources/webcam/webcam_source.h"
#include "ver/virtual_environment_renderer.h"

#include <glm/gtc/matrix_transform.hpp>

namespace android {
namespace ver {

// Static initialization
android::base::StaticLock ScenesManager::mScenesLock;
std::vector<std::unique_ptr<Scene>> ScenesManager::mScenes;
std::vector<std::filesystem::path> ScenesManager::mResourceSearchPaths;

android::base::StaticLock ScenesManager::mViewsLock;
std::vector<std::unique_ptr<RendererView>> ScenesManager::mViews;

const void* ScenesManager::mEglDispatch = nullptr;
const void* ScenesManager::mGles2Dispatch = nullptr;

void ScenesManager::setSearchPaths(
        const std::vector<std::filesystem::path>& resourceSearchPaths) {
    android::base::AutoLock lock(mScenesLock);
    mResourceSearchPaths = resourceSearchPaths;
}

bool ScenesManager::configArgumentFileExists(const SceneConfig& config) {
    android::base::AutoLock lock(mScenesLock);
    return Scene::configArgumentFileExists(config, mResourceSearchPaths);
}

Scene* ScenesManager::createScene(const SceneConfig& config) {
    if (config.mSceneMode == SceneConfig::Mode::Unknown) {
        derror("%s: invalid config", __func__);
        return nullptr;
    }

    dinfo("Initializing a scene with mode: %s, argument: %s",
          SceneConfig::modeToString(config.mSceneMode),
          config.mArgument.c_str());

    std::unique_ptr<Scene> scene = Scene::create(config, mResourceSearchPaths);
    if (!scene) {
        derror("%s: scene failed to load", __func__);
        return nullptr;
    }

    Scene* rawPtr = scene.get();
    android::base::AutoLock lock(mScenesLock);
    mScenes.push_back(std::move(scene));

    return rawPtr;
}

bool ScenesManager::renderView(Scene* scene,
                               RendererView* view,
                               std::function<void()> finishCallback,
                               uint64_t* outFrameTime) {
    // TODO(virtualscene-perf): do not create different renderers for each scene
    if (!scene || !view) {
        derror("%s: invalid parameters", __func__);
        return false;
    }

    const uint64_t frameTime = scene->getFrameTimeUs();
    if (outFrameTime) {
        *outFrameTime = frameTime;
    }

    std::lock_guard lock(view->mLock);

    const uint64_t sceneHash = scene->getVersionHashForView(view);
    if (view->mCache.isValidFor(sceneHash, frameTime)) {
        // We still need to call finish callback to let caller use the existing
        // view cache. viewCacheRequiresUpdate should be used when a final
        // copy/conversion is not needed.
        finishCallback();
        return true;
    }

    // Prepare view for rendering
    view->mCache.mSceneHash = sceneHash;
    view->mCache.mFrameTime = frameTime;

    const SceneConfig::Mode mode = scene->getSceneMode();
    Renderer* renderer = nullptr;
    if (SceneConfig::modeRequiresRenderer(mode)) {
        renderer = scene->getRenderer();

        // This mode requires renderer
        if (!renderer) {
            derror("%s: invalid scene renderer in mode %s", __func__,
                   SceneConfig::modeToString(mode));
            return false;
        }
    }

    // Make the renderer context current for graphics operations
    const float renderTime = frameTime / 1000000.0f;
    auto context = renderer ? renderer->makeCurrent() : nullptr;
    if (renderer && !context->isValid()) {
        derror("%s: Cannot use graphics context", __func__);
        return false;
    }

    view->preRenderLocked();

    switch (mode) {
        case SceneConfig::Mode::Mesh3D:
        case SceneConfig::Mode::Image360: {
            const auto renderables =
                    scene->getRenderableObjects(view->mViewProjection);
            if (!renderer || !renderer->render(view, renderables, renderTime)) {
                derror("Scene rendering failed");
                return false;
            }
        } break;
        case SceneConfig::Mode::ImageFile:
        case SceneConfig::Mode::VideoFile:
        case SceneConfig::Mode::Color:
        case SceneConfig::Mode::Webcam: {
            const SceneOverlayObject* overlay = scene->getOverlayObject();
            if (!overlay || !overlay->isValid()) {
                derror("Scene rendering failed");
                return false;
            }
            std::vector<uint8_t>& fbData = view->getFramebufferLocked();

            ImageScaler scaler(view->getWidthLocked(), view->getHeightLocked(),
                               fbData.data());
            auto mode = ImageScaler::ScaleMode::AspectFitZoom;
            // AspectFitZoom requires a minimum size.
            // For a single color image, just use ScaleToFill
            if (overlay->mWidth == 1 && overlay->mHeight == 1) {
                mode = ImageScaler::ScaleMode::ScaleToFill;
            }
            if (!scaler.updateImage(overlay->mWidth, overlay->mHeight,
                                    overlay->mDataRGBA.data(), mode)) {
                derror("%s: Failed to resize the framebuffer for the view",
                       __FUNCTION__);
                return false;
            }
        } break;
        default: {
            derror("%s: Unknown scene mode: %d", __FUNCTION__,
                   static_cast<int>(mode));
        }
    }

    view->postRenderLocked();

    // This needs to be called inside the lock
    finishCallback();

    return true;
}

bool ScenesManager::removeScene(Scene* scene) {
    if (!scene) {
        return false;
    }

    dinfo("%s: scene=%p", __func__, scene);

    android::base::AutoLock lock(mScenesLock);
    scene->releaseResources();

    auto it = std::find_if(
            mScenes.begin(), mScenes.end(),
            [scene](const auto& entry) { return entry.get() == scene; });

    if (it != mScenes.end()) {
        mScenes.erase(it);
        return true;
    }

    derror("%s: could not find scene %p", __func__, scene);
    return false;
}

RendererView* ScenesManager::createView() {
    dinfo("%s", __func__);
    std::unique_ptr<RendererView> view = std::make_unique<RendererView>();

    android::base::AutoLock lock(mViewsLock);
    RendererView* rawPtr = view.get();
    mViews.push_back(std::move(view));
    return rawPtr;
}

bool ScenesManager::removeView(RendererView* view) {
    if (!view) {
        return false;
    }

    dinfo("%s: view=%p", __func__, view);

    android::base::AutoLock lock(mViewsLock);
    auto it = std::find_if(
            mViews.begin(), mViews.end(),
            [view](const auto& entry) { return entry.get() == view; });

    if (it != mViews.end()) {
        mViews.erase(it);
        return true;
    }

    derror("%s: could not find view %p", __func__, view);
    return false;
}

bool ScenesManager::removeAll() {
    dinfo("%s", __func__);

    // Release all scenes
    {
        android::base::AutoLock lock(mScenesLock);
        for (auto& scene : mScenes) {
            scene->releaseResources();
        }

        mScenes.clear();
    }

    // Release all views
    {
        android::base::AutoLock lock(mViewsLock);
        mViews.clear();
    }

    return true;
}

}  // namespace ver
}  // namespace android

////////////////////////////////////////////////////////////////////////////////
// C-API Implementation (Forwarding to ScenesManager)
////////////////////////////////////////////////////////////////////////////////

using android::ver::Renderer;
using android::ver::RendererView;
using android::ver::Scene;
using android::ver::ScenesManager;

void ver_initialize(const std::vector<std::filesystem::path>& resourceBasePaths,
                    const void* eglDispatch,
                    const void* gles2Dispatch) {
    dprint("%s: Setting search paths with %d entries", __func__,
           static_cast<int>(resourceBasePaths.size()));
    ScenesManager::setSearchPaths(resourceBasePaths);
    ScenesManager::setDispatch(eglDispatch, gles2Dispatch);
}

bool ver_scene_config_file_exists(const VerSceneConfig& config) {
    return ScenesManager::configArgumentFileExists(config);
}

VerSceneHandle ver_create_scene(const VerSceneConfig& config) {
    return reinterpret_cast<VerSceneHandle>(ScenesManager::createScene(config));
}

bool ver_render_view(VerSceneHandle scene,
                     VerRenderViewHandle view,
                     VerRenderFinishCallback finish_cb,
                     uint64_t* out_frame_time) {
    auto* scenePtr = reinterpret_cast<Scene*>(scene);
    auto* viewPtr = reinterpret_cast<RendererView*>(view);

    return ScenesManager::renderView(scenePtr, viewPtr, finish_cb,
                                     out_frame_time);
}

void ver_destroy_scene(VerSceneHandle scene) {
    ScenesManager::removeScene(reinterpret_cast<Scene*>(scene));
}

bool ver_cleanup(void) {
    return ScenesManager::removeAll();
}

void ver_scene_load_user_resources(
        VerSceneHandle scene,
        std::function<void()> loadRendererResourcesCallback) {
    auto* scenePtr = reinterpret_cast<Scene*>(scene);
    if (!scenePtr) {
        return;
    }

    const bool alreadyHasRenderer = scenePtr->getRenderer() != nullptr;
    scenePtr->loadUserResources();

    // If a renderer was just created during loadUserResources, we might need
    // to trigger the callback while the context is current.
    if (!alreadyHasRenderer) {
        if (Renderer* renderer = scenePtr->getRenderer()) {
            auto context = renderer->makeCurrent();
            loadRendererResourcesCallback();
        }
    }
}

void ver_scene_unload_user_resources(VerSceneHandle scene) {
    auto* scenePtr = reinterpret_cast<Scene*>(scene);
    if (scenePtr) {
        scenePtr->unloadUserResources();
    }
}

const VerSceneConfig* ver_scene_get_config(VerSceneHandle scene) {
    auto* scenePtr = reinterpret_cast<Scene*>(scene);
    return scenePtr ? &scenePtr->getSceneConfig() : nullptr;
}

VerSceneConfig::Mode ver_scene_get_mode(VerSceneHandle scene) {
    auto* scenePtr = reinterpret_cast<Scene*>(scene);
    return scenePtr ? scenePtr->getSceneMode() : VerSceneConfig::Mode::Unknown;
}

void ver_scene_update(VerSceneHandle scene, bool update_time) {
    auto* scenePtr = reinterpret_cast<Scene*>(scene);
    if (scenePtr) {
        scenePtr->update(update_time);
    }
}

int ver_scene_get_scene_rotation(VerSceneHandle scene) {
    auto* scenePtr = reinterpret_cast<Scene*>(scene);
    return scenePtr ? scenePtr->getSceneRotation() : 0;
}

uint64_t ver_scene_get_version_hash_for_view(VerSceneHandle scene,
                                             VerRenderViewHandle view) {
    auto* scenePtr = reinterpret_cast<Scene*>(scene);
    auto* viewPtr = reinterpret_cast<RendererView*>(view);
    return (scenePtr && viewPtr) ? scenePtr->getVersionHashForView(viewPtr) : 0;
}

uint64_t ver_scene_get_frame_time_us(VerSceneHandle scene) {
    auto* scenePtr = reinterpret_cast<Scene*>(scene);
    return scenePtr ? scenePtr->getFrameTimeUs() : 0;
}

void ver_scene_update_poster_scale(VerSceneHandle scene,
                                   const char* posterName,
                                   float scale) {
    auto* scenePtr = reinterpret_cast<Scene*>(scene);
    if (scenePtr) {
        scenePtr->updatePosterScale(posterName, scale);
    }
}

uint64_t ver_scene_load_poster(VerSceneHandle scene,
                               const char* posterName,
                               const char* filename,
                               float scale) {
    auto* scenePtr = reinterpret_cast<Scene*>(scene);
    return scenePtr ? scenePtr->loadPoster(posterName, filename, scale,
                                           Scene::LoadBehavior::Default)
                    : 0;
}

bool ver_scene_create_poster_location(
        VerSceneHandle scene,
        const android::ver::PosterInfo& info) {
    auto* scenePtr = reinterpret_cast<Scene*>(scene);
    return scenePtr ? scenePtr->createPosterLocation(info) : false;
}

VerRenderViewHandle ver_create_render_view() {
    return reinterpret_cast<VerRenderViewHandle>(ScenesManager::createView());
}

void ver_destroy_render_view(VerRenderViewHandle view) {
    ScenesManager::removeView(reinterpret_cast<RendererView*>(view));
}

void ver_render_view_set_dimensions(VerRenderViewHandle view,
                                    int32_t frameWidth,
                                    int32_t frameHeight) {
    auto* viewPtr = reinterpret_cast<RendererView*>(view);
    if (viewPtr) {
        viewPtr->updateTarget(VerImageFormat::RGBA8, frameWidth, frameHeight);
    }
}

void ver_render_view_set_view_projection(VerRenderViewHandle view,
                                         const float* viewProjMatrix) {
    auto* viewPtr = reinterpret_cast<RendererView*>(view);
    if (viewPtr && viewProjMatrix) {
        const auto* viewProj =
                reinterpret_cast<const glm::mat4*>(viewProjMatrix);
        viewPtr->updateViewProjection(*viewProj);
    }
}

void ver_render_view_set_blur_factor(VerRenderViewHandle view, float factor) {
    auto* viewPtr = reinterpret_cast<RendererView*>(view);
    if (viewPtr) {
        viewPtr->setBlurFactor(factor);
    }
}

void ver_render_view_get_dimensions(VerRenderViewHandle view,
                                    int32_t* frameWidth,
                                    int32_t* frameHeight) {
    auto* viewPtr = reinterpret_cast<RendererView*>(view);
    if (viewPtr && frameWidth && frameHeight) {
        *frameWidth = viewPtr->getWidthLocked();
        *frameHeight = viewPtr->getHeightLocked();
    }
}

void ver_render_view_get_framebuffer(VerRenderViewHandle view,
                                     const uint8_t** out_fb_data_ptr,
                                     uint64_t* out_fb_data_size) {
    auto* viewPtr = reinterpret_cast<RendererView*>(view);
    if (viewPtr && out_fb_data_ptr && out_fb_data_size) {
        const std::vector<uint8_t>& fbData = viewPtr->getFramebufferLocked();
        *out_fb_data_ptr = fbData.data();
        *out_fb_data_size = static_cast<uint64_t>(fbData.size());
    }
}

bool ver_render_view_cache_is_valid_for(VerRenderViewHandle view,
                                        uint64_t sceneHash,
                                        uint64_t frameTime) {
    auto* viewPtr = reinterpret_cast<RendererView*>(view);
    return viewPtr ? viewPtr->isCacheValidFor(sceneHash, frameTime) : false;
}

// TODO(virtualscene-library): Legacy function to keep the same behavior.
bool ver_texture_utils_load_png(const char* filename,
                                int* outWidth,
                                int* outHeight,
                                int* outFormatBpp,
                                std::vector<uint8_t>* outBuffer) {
    if (!outWidth || !outHeight || !outFormatBpp || !outBuffer) {
        return false;
    }

    auto result = android::ver::TextureUtils::loadPNG(
            filename, android::ver::TextureUtils::Orientation::Qt);
    if (!result) {
        return false;
    }

    *outBuffer = std::move(result->mBuffer);
    *outWidth = result->mWidth;
    *outHeight = result->mHeight;
    *outFormatBpp = (result->mFormat ==
                     android::ver::TextureUtils::Format::RGB24)
                            ? 3
                            : 4;
    return true;
}

struct VerWebcamInfoOpaque {
    std::shared_ptr<android::ver::WebcamSource::WebcamInfo> info;
};

extern "C" {

uint32_t ver_get_webcam_count() {
    return static_cast<uint32_t>(android::ver::WebcamSource::GetWebcamCount());
}

VerWebcamHandle ver_get_webcam_info(uint32_t index) {
    auto info = android::ver::WebcamSource::GetWebcamInfo(index);
    if (!info) {
        return nullptr;
    }
    auto handle = new VerWebcamInfoOpaque{info};
    return reinterpret_cast<VerWebcamHandle>(handle);
}

void ver_free_webcam_info(VerWebcamHandle handle) {
    if (handle) {
        auto opaque = reinterpret_cast<VerWebcamInfoOpaque*>(handle);
        delete opaque;
    }
}

const char* ver_webcam_info_get_user_facing_name(VerWebcamHandle handle) {
    if (!handle)
        return nullptr;
    auto opaque = reinterpret_cast<VerWebcamInfoOpaque*>(handle);
    return opaque->info->friendly_name.c_str();
}

const char* ver_webcam_info_get_id(VerWebcamHandle handle) {
    if (!handle)
        return nullptr;
    auto opaque = reinterpret_cast<VerWebcamInfoOpaque*>(handle);
    return opaque->info->os_alias.c_str();
}

int ver_webcam_info_get_preferred_format_index(VerWebcamHandle handle) {
    if (!handle)
        return -1;
    auto opaque = reinterpret_cast<VerWebcamInfoOpaque*>(handle);
    return opaque->info->preferred_format_index;
}

uint32_t ver_webcam_info_get_format_count(VerWebcamHandle handle) {
    if (!handle)
        return 0;
    auto opaque = reinterpret_cast<VerWebcamInfoOpaque*>(handle);
    return static_cast<uint32_t>(opaque->info->supported_formats.size());
}

uint32_t ver_webcam_info_get_pixel_format_fourcc(VerWebcamHandle handle,
                                                 uint32_t format_index) {
    if (!handle)
        return 0;
    auto opaque = reinterpret_cast<VerWebcamInfoOpaque*>(handle);
    if (static_cast<size_t>(format_index) >=
        opaque->info->supported_formats.size()) {
        return 0;
    }
    return opaque->info->supported_formats[format_index].pixel_format;
}

uint32_t ver_webcam_info_get_format_resolution_count(VerWebcamHandle handle,
                                                     uint32_t format_index) {
    if (!handle)
        return 0;
    auto opaque = reinterpret_cast<VerWebcamInfoOpaque*>(handle);
    if (static_cast<size_t>(format_index) >=
        opaque->info->supported_formats.size()) {
        return 0;
    }
    return static_cast<uint32_t>(
            opaque->info->supported_formats[format_index].resolutions.size());
}

bool ver_webcam_info_get_format_resolution(VerWebcamHandle handle,
                                           uint32_t format_index,
                                           uint32_t res_index,
                                           int* out_width,
                                           int* out_height) {
    if (!handle || !out_width || !out_height)
        return false;
    auto opaque = reinterpret_cast<VerWebcamInfoOpaque*>(handle);
    if (static_cast<size_t>(format_index) >=
        opaque->info->supported_formats.size()) {
        return false;
    }
    const auto& format = opaque->info->supported_formats[format_index];
    if (static_cast<size_t>(res_index) >= format.resolutions.size()) {
        return false;
    }
    *out_width = format.resolutions[res_index].resolution.width;
    *out_height = format.resolutions[res_index].resolution.height;
    return true;
}
}
