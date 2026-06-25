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

#pragma once

/*
 * The Scene container for the Virtual Scene.
 */

#include "PosterSceneObject.h"
#include "raw_image_sources/raw_image_source.h"
#include "ver/virtual_environment_renderer_types.h"

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace android {
namespace ver {

// Forward declarations.
class SceneObject;
class Renderer;

// TODO(virtualscene-perf): temporary object type to support 2d rendering modes,
// will be removed once the 2d quad objects are used directly instead
struct SceneOverlayObject {
    uint32_t mWidth;
    uint32_t mHeight;
    std::vector<uint8_t> mDataRGBA;

    bool isValid() const {
        return (mWidth > 0) && (mHeight > 0) &&
               (mDataRGBA.size() == (mWidth * mHeight * 4));
    }
};

class Scene {
    Scene(const Scene& other) = delete;
    Scene& operator=(const Scene& other) = delete;

public:
    enum class LoadBehavior { Default, Synchronous };

    ~Scene();

    // Creates a scene instance if the scene was successfully created or
    // null if there was an error.
    static std::unique_ptr<Scene> create(
            const SceneConfig& config,
            const std::vector<std::filesystem::path>& resourceBasePaths);

    // Check if the argument file for the scene config exists
    static bool configArgumentFileExists(
            const SceneConfig& config,
            const std::vector<std::filesystem::path>& resourceBasePaths);

    // Before teardown, release all Renderer resources and SceneObjects.
    bool releaseResources();

    const SceneConfig::Mode getSceneMode() const { return mConfig.mSceneMode; }
    const SceneConfig& getSceneConfig() const { return mConfig; }

    int getSceneRotation() { return mBaseRotation; }

    // Update the scene for the next frame.
    // updateTime: Some animations are controlled by the global renderTime, use
    //             this argument to disallow frame time updates, so such
    //             animations would keep working as expected when animations are
    //             paused.
    void update(bool updateTime = true);

    // Returns a hash value based on the scene contents and animations. Can be
    // used to cache the results of a view and check if anything has changed.
    uint64_t getVersionHashForView(const RendererView* lockedView) const;

    // Get the list of RenderableObjects for the current frame.
    std::vector<RenderableObject> getRenderableObjects(
            const glm::mat4& viewProjection) const;

    // Create a new poster location.
    //
    // |info| - Poster information.
    //
    // Returns true if the poster was successfully created.
    bool createPosterLocation(const PosterInfo& info);

    // Load a poster into the scene from a file.
    //
    // |posterName| - Name of the poster position, such as "wall" or "table".
    // |filename| - Path to an image file, either PNG or JPEG.
    // |scale| - The default poster scale, between 0 and 1, which will
    //           automatically be clamped.
    // |loadBehavior| - Loading behavior, if this is LoadBehavior::Default the
    //                  texture will be loaded asynchronously.
    //
    // Returns true on success.
    bool loadPoster(const char* posterName,
                    const char* filename,
                    float scale,
                    LoadBehavior loadBehavior);

    // Update a given poster's scale.  If the poster does not exist, this has
    // no effect.
    //
    // |posterName| - Name of the poster position, such as "wall" or "table".
    // |scale| - Poster scale, between 0 and 1, which will be automatically
    //           clamped.
    void updatePosterScale(const char* posterName, float scale);

    const SceneOverlayObject* getOverlayObject() const {
        return mOverlayObject.get();
    }

    Renderer* getRenderer() { return mRenderer.get(); }

    void loadUserResources();
    void unloadUserResources();

    uint64_t getFrameTimeUs() const { return mFrameTimeUs; }

private:
    // Private constructor, use Scene::create to create an instance.
    Scene(const SceneConfig& config,
          const std::vector<std::filesystem::path>& basePaths);

    // Load the scene and create SceneObjects.
    //
    // Returns true on success.
    bool initialize();

    // Load renderer related resources, separated from the initialize call
    // function to be able to defer the renderer and related GPU resource
    // creations.
    bool loadRendererResources();

    // Gets RenderableObjects from a SceneObject.
    static void getRenderableObjectsFromSceneObject(
            const glm::mat4& viewProjection,
            const SceneObject* sceneObject,
            std::vector<RenderableObject>& outRenderableObjects);

    struct PosterStorage {
        std::unique_ptr<PosterSceneObject> sceneObject;
        Texture texture;
        Texture defaultTexture;
    };

    const SceneConfig mConfig;
    const std::vector<std::filesystem::path> mResourceBasePaths;
    std::unique_ptr<Renderer> mRenderer;

    std::vector<std::unique_ptr<SceneObject>> mSceneObjects;
    std::unordered_map<std::string, PosterStorage> mPosters;
    std::unique_ptr<RawImageSource> mRawImageSource;
    std::optional<RawImageToken> mRawImageSourceToken;
    std::unique_ptr<SceneOverlayObject> mOverlayObject;
    uint64_t mObjectsVersion = 0;
    uint64_t mFrameTimeUs = 0;
    uint64_t mStartTimeUs = 0;
    int mBaseRotation = 0;
};

}  // namespace ver
}  // namespace android
