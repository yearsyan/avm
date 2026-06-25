/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "Scene.h"

#include <cmath>
#include <cstring>
#include <memory>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "android/base/system/System.h"
#include "raw_image_sources/image_file/raw_image_file_source.h"
#include "raw_image_sources/raw_image_source.h"
#include "raw_image_sources/video_file/raw_video_file_source.h"
#include "raw_image_sources/webcam/webcam_source.h"

#include "MeshSceneObject.h"
#include "Renderer.h"
#include "TextureUtils.h"

using namespace android::base;
namespace fs = std::filesystem;

#define E(...) derror(__VA_ARGS__)
#define W(...) dwarning(__VA_ARGS__)
#define D(...) dprint(__VA_ARGS__)

// Function to find fullpath from a filename for the scene
// Scene filenames can be provided as fullpaths, or local
// paths based on the basePaths argument.
std::string resolveSceneFilename(const std::string& sceneFilename,
                                 const std::vector<fs::path>& basePaths) {
    // Check if it's a fullpath
    fs::path inputPath(sceneFilename);
    if (fs::exists(inputPath)) {
        return inputPath.string();
    }

    // Try search directories, e.g AVD folder, resources/ folder..etc
    for (const fs::path& basePath : basePaths) {
        fs::path searchPath = fs::path(basePath) / inputPath;
        if (fs::exists(searchPath)) {
            return searchPath.string();
        }
    }

    dwarning("Could not resolve environment scene filename '%s'",
             sceneFilename.c_str());
    return "";
}

// static_cast the value in a unique_ptr.
// After this call, the unique_ptr that the value is cast from will be removed.
template <typename To, typename From>
std::unique_ptr<To> static_unique_cast(std::unique_ptr<From>& from) {
    return std::unique_ptr<To>(static_cast<To*>(from.release()));
}

namespace android {
namespace ver {

Scene::Scene(const SceneConfig& config,
             const std::vector<std::filesystem::path>& basePaths)
    : mConfig(config), mResourceBasePaths(basePaths) {
    D("%s: creating Scene", __func__);
}

Scene::~Scene() {
    D("%s: destroying Scene", __func__);
    if (mSceneObjects.size() || mRawImageSource || mOverlayObject) {
        // releaseResources should have been called!
        E("%s: Scene resources are not released!", __func__);
    }
    mRenderer.reset();
}

std::unique_ptr<Scene> Scene::create(
        const SceneConfig& config,
        const std::vector<fs::path>& resourceBasePaths) {
    std::unique_ptr<Scene> scene;
    scene.reset(new Scene(config, resourceBasePaths));
    if (!scene->initialize()) {
        return nullptr;
    }

    return scene;
}

bool Scene::configArgumentFileExists(
        const SceneConfig& config,
        const std::vector<fs::path>& resourceBasePaths) {
    if (config.mSceneMode == SceneConfig::Mode::Color) {
        // Argument is not a file
        return true;
    }
    if (config.mSceneMode == SceneConfig::Mode::Webcam) {
        return !WebcamSource::ResolveWebcamId(config.mArgument).empty();
    }
    auto sceneFilename =
            resolveSceneFilename(config.mArgument, resourceBasePaths);
    if (sceneFilename.empty()) {
        dprint("%s: Invalid scene argument: %s", __func__,
               config.mArgument.c_str());
        return false;
    }
    return true;
}

bool Scene::initialize() {
    auto sceneMode = getSceneMode();
    const char* sceneModeStr = SceneConfig::modeToString(sceneMode);
    dprint("Initializing scene with '%s' mode, argument:%s", sceneModeStr,
           mConfig.mArgument.c_str());

    // Find and validate the input file parameter, call configArgumentFileExists
    // to avoid or handle this case earlier.
    std::string sceneFilename;
    if (sceneMode != SceneConfig::Mode::Color &&
        sceneMode != SceneConfig::Mode::Webcam) {
        sceneFilename =
                resolveSceneFilename(mConfig.mArgument, mResourceBasePaths);
        if (sceneFilename.empty()) {
            derror("%s: Invalid scene argument: %s", __func__,
                   mConfig.mArgument.c_str());
            return false;
        }
    }

    switch (sceneMode) {
        case SceneConfig::Mode::Unknown: {
            derror("%s: Unknown scene mode!", __func__);
            return false;
        } break;
        case SceneConfig::Mode::Mesh3D: {
            // Just check if the obj file is valid, the object addition is
            // handled in loadRendererResources
            // TODO(virtualscene-perf): initialize renderer early and avoid
            // loading content twice
            if (!MeshSceneObject::canLoad(sceneFilename.c_str())) {
                derror("%s: Could not load scene object: %s", __func__,
                       sceneFilename.c_str());
                return false;
            }

            // TODO(virtualscene) The virtual scene by default renders the
            // image rotated 90 degrees
            mBaseRotation = 90;
        } break;
        case SceneConfig::Mode::VideoFile: {
            mRawImageSource = RawVideofileSource::Create(sceneFilename);
            if (!mRawImageSource) {
                derror("%s: Could not load video file: '%s'", __func__,
                       mConfig.mArgument.c_str());
                return false;
            }
        } break;
        case SceneConfig::Mode::ImageFile: {
            mRawImageSource = RawImageFileSource::Create(sceneFilename);
            if (!mRawImageSource) {
                derror("%s: Could not load image file: '%s'", __func__,
                       mConfig.mArgument.c_str());
                return false;
            }
        } break;
        case SceneConfig::Mode::Color: {
            unsigned int r, g, b;
            if (sscanf(mConfig.mArgument.c_str(), "#%02x%02x%02x", &r, &g,
                       &b) != 3) {
                derror("%s: Could not parse color: %s", __func__,
                       mConfig.mArgument.c_str());
                return false;
            }

            mRawImageSource = std::make_unique<SolidColorImageSource>(
                    Color{(uint8_t)r, (uint8_t)g, (uint8_t)b});
        } break;
        case SceneConfig::Mode::Webcam: {
            std::string resolvedId =
                    WebcamSource::ResolveWebcamId(mConfig.mArgument);
            if (resolvedId.empty()) {
                derror("%s: Could not resolve webcam argument: '%s'", __func__,
                       mConfig.mArgument.c_str());
                return false;
            }
            mRawImageSource = WebcamSource::Create(resolvedId);
            if (!mRawImageSource) {
                derror("%s: Could not load webcam: '%s' (resolved: '%s')",
                       __func__, mConfig.mArgument.c_str(), resolvedId.c_str());
                return false;
            }
        } break;
        case SceneConfig::Mode::Image360: {
            // Check if the texture file is valid to be able to return error
            // back to the caller, the object addition is handled in
            // loadRendererResources
            // TODO(virtualscene-perf): initialize renderer early and avoid
            // loading content twice
            auto result = TextureUtils::load(sceneFilename.c_str());
            if (!result) {
                E("%s: Could not load texture from file '%s'", __func__,
                  sceneFilename.c_str());
                return false;
            }

            // TODO(virtualscene) The virtual scene by default renders the
            // image rotated 90 degrees
            mBaseRotation = 90;
        } break;
        default:
            dwarning("%s: Unhandled scene mode %d", __func__, (int)sceneMode);
    }

    if (mRawImageSource) {
        mBaseRotation = mRawImageSource->GetBaseRotation();

        // Set up an initial black image
        mOverlayObject = std::make_unique<SceneOverlayObject>();
        mOverlayObject->mHeight = 1;
        mOverlayObject->mWidth = 1;
        mOverlayObject->mDataRGBA = {0x00, 0x00, 0x00, 0xFF};
    }

    mStartTimeUs = System::get()->getUnixTimeUs();
    mFrameTimeUs = 0;

    mObjectsVersion++;

    return true;
}

bool Scene::loadRendererResources() {
    if (mRenderer) {
        // Already loaded
        return true;
    }

    // Only initialize a renderer for the scene if GL is required
    const auto sceneMode = getSceneMode();
    bool rendererRequired = SceneConfig::modeRequiresRenderer(sceneMode);
    if (!rendererRequired) {
        // Renderer is not required
        return true;
    }

    mRenderer = Renderer::create();
    if (!mRenderer) {
        E("VirtualSceneManager renderer failed to construct");
        return false;
    }

    // Make the renderer context current for graphics operations
    auto context = mRenderer ? mRenderer->makeCurrent() : nullptr;
    if (context && !context->isValid()) {
        E("%s: Cannot use EGL context", __FUNCTION__);
        return false;
    }

    // Find the file, in case it's given as a local path
    std::string sceneFilename;
    if (sceneMode != SceneConfig::Mode::Color) {
        sceneFilename = resolveSceneFilename(mConfig.mArgument, mResourceBasePaths);
        if (sceneFilename.empty()) {
            E("%s: Cannot find file '%s'", __FUNCTION__, mConfig.mArgument);
            return false;
        }
    }

    switch (sceneMode) {
        case SceneConfig::Mode::Mesh3D: {
            std::unique_ptr<MeshSceneObject> sceneObject =
                    MeshSceneObject::load(*mRenderer, sceneFilename.c_str());
            if (!sceneObject) {
                derror("%s: Could not load scene object: %s", __func__,
                       sceneFilename.c_str());
                return false;
            }

            mSceneObjects.push_back(std::move(sceneObject));
        } break;
        case SceneConfig::Mode::Image360: {
            std::unique_ptr<MeshSceneObject> photoSphereObject =
                    MeshSceneObject::createSphere(*mRenderer);
            Texture sceneTexture =
                    mRenderer->loadTexture(sceneFilename.c_str());
            if (sceneTexture.isValid()) {
                photoSphereObject->setTexture(0, sceneTexture);
                mRenderer->releaseTexture(sceneTexture);
            }
            mSceneObjects.push_back(std::move(photoSphereObject));
        } break;
    }

    mObjectsVersion++;

    return true;
}

bool Scene::releaseResources() {
    auto context = mRenderer ? mRenderer->makeCurrent() : nullptr;
    if (mRenderer) {
        if (!context->isValid()) {
            E("%s: Cannot use EGL context", __FUNCTION__);
            return false;
        }

        for (auto& poster : mPosters) {
            mRenderer->releaseTexture(poster.second.texture);
            mRenderer->releaseTexture(poster.second.defaultTexture);

            if (poster.second.sceneObject) {
                poster.second.sceneObject.reset();
            }
        }
    }

    mSceneObjects.clear();

    mRawImageSource.reset();
    mOverlayObject.reset();

    mObjectsVersion++;

    return true;
}

void Scene::update(bool updateTime) {
    // TODO(virtualscene-video): this should play the video in video mode
    for (auto& poster : mPosters) {
        poster.second.sceneObject->update();
    }

    if (updateTime) {
        // TODO(virtualscene): use ThreadLooper::nowNs(ClockType::kVirtual) ?
        mFrameTimeUs = System::get()->getUnixTimeUs() - mStartTimeUs;
        if (mRawImageSource) {
            int64_t animationLength = mRawImageSource->GetAnimationLengthUs();
            if (animationLength > 0 && mFrameTimeUs > animationLength) {
                mFrameTimeUs %= mRawImageSource->GetAnimationLengthUs();
            }
        }
    } else {
        // While paused, move our start time so we resume with the same value;
        mStartTimeUs = System::get()->getUnixTimeUs() - mFrameTimeUs;
    }

    if (mRawImageSource) {
        auto res = mRawImageSource->UpdateImage(
                mFrameTimeUs, mRawImageSourceToken,
                [&](const RawImageBufferView* buffer) {
                    if (buffer->pixel_format != VerImageFormat::RGBA8) {
                        return absl::InvalidArgumentError(absl::StrFormat(
                                "Unsupported pixel format from image source: %d",
                                (int)(buffer->pixel_format)));
                    }

                    if (mOverlayObject->mDataRGBA.size() <
                        buffer->buffer_size) {
                        mOverlayObject->mDataRGBA.resize(buffer->buffer_size);
                        mOverlayObject->mWidth = buffer->width;
                        mOverlayObject->mHeight = buffer->height;
                    }

                    std::memcpy(mOverlayObject->mDataRGBA.data(),
                                buffer->buffer, buffer->buffer_size);
                    return absl::OkStatus();
                });
        if (res.ok()) {
            if (res->has_value()) {
                mRawImageSourceToken = res.value();
                mObjectsVersion++;
            }
        } else {
            E("Failed to Update Image Source: %s", res.status().message());
        }
    }
}

uint64_t Scene::getVersionHashForView(
        const RendererView* /*lockedView*/) const {
    const uint64_t sceneHash = reinterpret_cast<uint64_t>(this);
    // TODO(virtualscene-perf): check if the objects inside the view frustum
    // includes any changes/animations
    return (mObjectsVersion ^ sceneHash);
}

std::vector<RenderableObject> Scene::getRenderableObjects(
        const glm::mat4& viewProjection) const {
    std::vector<RenderableObject> renderables;

    for (auto& sceneObject : mSceneObjects) {
        getRenderableObjectsFromSceneObject(viewProjection, sceneObject.get(),
                                            renderables);
    }

    for (auto& poster : mPosters) {
        if (poster.second.sceneObject) {
            getRenderableObjectsFromSceneObject(viewProjection,
                                                poster.second.sceneObject.get(),
                                                renderables);
        }
    }

    return std::move(renderables);
}

bool Scene::createPosterLocation(const PosterInfo& info) {
    if (mConfig.mSceneMode != SceneConfig::Mode::Mesh3D) {
        // Scene mode doesn't support poster locations, not an error
        return true;
    }
    if (!mRenderer) {
        return false;
    }
    PosterStorage storage;
    storage.sceneObject =
            PosterSceneObject::create(*mRenderer, info.position, info.rotation,
                                      kPosterMinimumSizeMeters, info.size);
    if (!storage.sceneObject) {
        W("%s: Failed to create poster scene object %s.", __FUNCTION__,
          info.name.c_str());
        return false;
    }

    if (!info.defaultFilename.empty()) {
        storage.defaultTexture =
                mRenderer->loadTextureAsync(info.defaultFilename.c_str());
        storage.sceneObject->setTexture(0, storage.defaultTexture);
    }

    mPosters.insert(std::make_pair(info.name, std::move(storage)));
    return true;
}

bool Scene::loadPoster(const char* posterName,
                       const char* filename,
                       float scale,
                       LoadBehavior loadBehavior) {
    auto it = mPosters.find(posterName);
    if (it == mPosters.end()) {
        W("%s: Could not find poster with name '%s'", __FUNCTION__, posterName);
        return false;
    }

    PosterStorage& poster = it->second;

    mRenderer->releaseTexture(poster.texture);
    poster.texture = Texture();

    if (filename && strlen(filename) > 0) {
        poster.texture = loadBehavior == LoadBehavior::Synchronous
                                 ? mRenderer->loadTexture(filename)
                                 : mRenderer->loadTextureAsync(filename);
    } else {
        // Always render empty posters at 100% scale.
        scale = 1.0f;
    }

    poster.sceneObject->setScale(scale);
    poster.sceneObject->setTexture(0, poster.texture.isValid()
                                              ? poster.texture
                                              : poster.defaultTexture);

    mObjectsVersion++;

    return true;
}

void Scene::updatePosterScale(const char* posterName, float scale) {
    auto it = mPosters.find(posterName);
    if (it == mPosters.end()) {
        W("%s: Could not find poster with name '%s'", __FUNCTION__, posterName);
        return;
    }

    PosterStorage& poster = it->second;
    poster.sceneObject->setScale(scale);

    mObjectsVersion++;
}

void Scene::getRenderableObjectsFromSceneObject(
        const glm::mat4& viewProjection,
        const SceneObject* sceneObject,
        std::vector<RenderableObject>& outRenderableObjects) {
    if (sceneObject->isVisible()) {
        const glm::mat4 mvp = viewProjection * sceneObject->getTransform();

        for (const Renderable& renderable : sceneObject->getRenderables()) {
            outRenderableObjects.push_back({mvp, renderable});
        }
    }
}

void Scene::loadUserResources() {
    dprint("%s", __FUNCTION__);

    if (!loadRendererResources()) {
        derror("%s: failed to create renderer resources", __func__);
        return;
    }

    if (mRawImageSource) {
        // TODO(virtualscene) Determine if we need these input values.
        // Currently they are just hints that we may use to better initialize
        // the webcam to a sensible resolution. Using 720p for now, as that's on
        // the high end of what virtualscene reports support for.
        mRawImageSource->Start(VerImageFormat::RGBA8, 1280, 720);
    }
}

void Scene::unloadUserResources() {
    dprint("%s", __FUNCTION__);

    // Intentionally not unloading rendering resources as that'll require
    // extra tracking of renderer objects

    if (mRawImageSource) {
        mRawImageSource->Stop();
    }
}

}  // namespace ver
}  // namespace android
