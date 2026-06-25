/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "android/camera/camera-virtualscene-utils.h"

#include "android/camera/camera-virtualscene.h"
#include "android/virtualscene/VirtualSceneManager.h"

#define VIRTUALSCENE_PIXEL_FORMAT V4L2_PIX_FMT_RGB32

#ifdef _WIN32
#undef ERROR
#endif

namespace android {
namespace virtualscene {

static VerImageFormat formatFromCameraFormat(uint32_t cameraPixelFormat) {
    if (cameraPixelFormat == V4L2_PIX_FMT_RGB32) {
        return VerImageFormat::RGBA8;
    }
    derror("Unsupported camera format for virtual scene views %lu",
           cameraPixelFormat);
    return VerImageFormat::RGBA8;
}

static uint32_t cameraFormatFromFormat(VerImageFormat format) {
    if (format == VerImageFormat::RGBA8) {
        return V4L2_PIX_FMT_RGB32;
    }
    derror("Unknown view format %lu", (uint32_t)format);
    return 0;
}

RenderedCameraDevice::RenderedCameraDevice(std::string_view name) {
    mHeader.opaque = this;

    mUsingEnvironmentScene = false; // set later, at capture start
    mName = name;

    LOG(INFO) << "Initialized camera with name: " << name;
}

RenderedCameraDevice::~RenderedCameraDevice() {
    stopCapturing();

    ver_destroy_scene(mOwnedScene);
    mOwnedScene = VER_INVALID_HANDLE;
}

int RenderedCameraDevice::startCapturing(uint32_t pixelFormat,
                                         int frameWidth,
                                         int frameHeight) {
    VLOG(camera) << "Start capturing at " << frameWidth << " x " << frameHeight;

    VerSceneConfig::Mode sceneMode = VerSceneConfig::Mode::Unknown;
    std::string sceneModeStr;
    std::string sceneArgument;
    const size_t sepPos =
            mName.find(camera_virtualscene_name_argument_separator());
    if (sepPos != std::string::npos) {
        sceneModeStr = mName.substr(0, sepPos);
        sceneArgument = mName.substr(sepPos + 1);
    } else {
        sceneModeStr = mName;
    }

    // "environment" means the camera is using the global environment
    // scene, defined in environment.ini file.
    // Camera name can be "environment", "environment|back" or
    // "environment|front" depending on the emulator version and camera
    // direction used.
    mUsingEnvironmentScene = (sceneModeStr == "environment");
    if (mUsingEnvironmentScene) {
        mOwnedScene = VER_INVALID_HANDLE;
        sceneMode = VirtualSceneManager::getSceneMode();

        VirtualSceneManager::setSceneControlsParameters(true);
        VirtualSceneManager::addSceneUser();
    } else {
        // Create and own the scene
        VerSceneConfig::Mode mode = VerSceneConfig::modeFromString(sceneModeStr);
        if (sceneArgument.empty()) {
            // Create with default content if a filename is not given
            sceneArgument = VerSceneConfig::defaultArgumentForMode(mode);
        }
        VerSceneConfig sceneConfig(mode, sceneArgument);
        mOwnedScene = ver_create_scene(sceneConfig);
        if (mOwnedScene == VER_INVALID_HANDLE) {
            // Use magenta/error color fallback for camera owned scenes
            LOG(ERROR)
                    << "Camera scene could not be initialized, using default configuration!";
            mOwnedScene = ver_create_scene(
                    VerSceneConfig(VerSceneConfig::Mode::Color, "#FF00FF"));
        }

        if (mOwnedScene != VER_INVALID_HANDLE) {
            sceneMode = ver_scene_get_mode(mOwnedScene);
            ver_scene_load_user_resources(mOwnedScene, [](){});
        }
    }

    if (sceneMode == VerSceneConfig::Mode::Unknown) {
        LOG(ERROR) << "Camera scene could not be not initialized!";
        stopCapturing();
        return -1;
    }

    mSceneCamera.setAspectRatio(static_cast<float>(frameWidth) / frameHeight);

    if (formatFromCameraFormat(pixelFormat) != VerImageFormat::RGBA8) {
        LOG(ERROR) << "Camera scene could not be initialized, unsupported format requested!";
        stopCapturing();
        return -1;
    }

    mActiveView = ver_create_render_view();
    ver_render_view_set_dimensions(mActiveView, frameWidth, frameHeight);

    return 0;
}

// Resets camera device after capturing.
// Since new capture request may require different frame dimensions we must
// reset camera device by reopening its handle. Otherwise attempts to set up new
// frame properties (different from the previous one) may fail.
void RenderedCameraDevice::stopCapturing() {
    if (mActiveView != VER_INVALID_HANDLE) {
        ver_destroy_render_view(mActiveView);
        mActiveView = VER_INVALID_HANDLE;
    }

    if (mUsingEnvironmentScene) {
        VirtualSceneManager::setSceneControlsParameters(false);
        VirtualSceneManager::removeSceneUser();
        mUsingEnvironmentScene = false;
    } else if (mOwnedScene != VER_INVALID_HANDLE) {
        ver_scene_unload_user_resources(mOwnedScene);
        ver_destroy_scene(mOwnedScene);
        mOwnedScene = VER_INVALID_HANDLE;
    }
}

int RenderedCameraDevice::readFrame(ClientFrame* resultFrame,
                                    float rScale,
                                    float gScale,
                                    float bScale,
                                    float expComp,
                                    const char* direction,
                                    int orientation) {

    VerSceneConfig::Mode sceneMode = VerSceneConfig::Mode::Unknown;
    if (mUsingEnvironmentScene) {
        sceneMode = VirtualSceneManager::getSceneMode();
    } else if (mOwnedScene != VER_INVALID_HANDLE) {
        sceneMode = ver_scene_get_mode(mOwnedScene);
    }

    if (sceneMode == VerSceneConfig::Mode::Unknown) {
        LOG(ERROR) << "Virtual scene is not initialized!";
        return -1;
    }
    if (!mUsingEnvironmentScene) {
        if (mOwnedScene == VER_INVALID_HANDLE) {
            LOG(ERROR) << "Virtual scene is not initialized!";
            return -1;
        }
        ver_scene_update(mOwnedScene, true);
    }

    glm::vec3 extraRotationEulerDegrees = glm::vec3();
    if (direction && strcmp(direction, "front") == 0) {
        // Rotate scene view matrix by 180 degrees for the front camera
        extraRotationEulerDegrees.y = 180.0f;
    }

    // TODO(virtualscene-perf): update the view here to avoid resizing?
    // Update camera based on physical model and set view projection accordingly
    const bool supportsPosition = (sceneMode == VerSceneConfig::Mode::Mesh3D);
    mSceneCamera.setExtraRotationEulerDegrees(extraRotationEulerDegrees);
    mSceneCamera.update(supportsPosition);

    auto viewProj = mSceneCamera.getViewProjection();
    ver_render_view_set_view_projection(mActiveView, &viewProj[0][0]);

    int conversionResult = -1;
    auto onRenderComplete = [&]() {
        const uint8_t* viewFbDataPtr = nullptr;
        uint64_t viewFbDataSize = 0;
        ver_render_view_get_framebuffer(mActiveView, &viewFbDataPtr, &viewFbDataSize);
        if (!viewFbDataPtr || viewFbDataSize == 0) {
            LOG(ERROR) << "Could not get framebuffer data for the view.";
            return;
        }

        uint32_t pixelFormat =
                cameraFormatFromFormat(VerImageFormat::RGBA8);

        int32_t viewWidth=0, viewHeight=0;
        ver_render_view_get_dimensions(mActiveView, &viewWidth, &viewHeight);

        // Do not rotate during the conversion if the view is already handling
        const bool viewHandlesRotation =
                VerSceneConfig::modeSupportsViewRotations(sceneMode);
        const char* convertDirection = direction;
        int convertOrientation = orientation;
        if (viewHandlesRotation) {
            convertDirection = "front";
            convertOrientation = 1;
        } else {
            int rotation = 0;
            if (mUsingEnvironmentScene) {
                rotation = VirtualSceneManager::getSceneBaseRotationLocked();
            } else {
                rotation = ver_scene_get_scene_rotation(mOwnedScene);
            }
            if (rotation) {
                // Apply the required base rotation to the image
                convertOrientation += rotation / 90;
                convertOrientation %= 4;
                if (convertOrientation < 0) {
                    convertOrientation += 4;
                }
            }
        }
        // Convert frame to the receiving buffers.
        conversionResult = convert_frame(
                viewFbDataPtr, pixelFormat, viewFbDataSize,
                viewWidth, viewHeight,
                resultFrame, rScale, gScale, bScale, expComp, convertDirection,
                convertOrientation);
    };

    uint64_t frameTime = 0;
    bool renderResult = false;
    if (mUsingEnvironmentScene) {
        renderResult = VirtualSceneManager::renderView(
                mActiveView, onRenderComplete, &frameTime);
    } else {
        renderResult = ver_render_view(mOwnedScene, mActiveView,
                                       onRenderComplete, &frameTime);
    }

    if (!renderResult) {
        LOG(ERROR) << "Virtual scene could not be rendered!";
        return -1;
    }

    // Set the frame time used in the render
    resultFrame->frame_time = static_cast<int64_t>(frameTime);

    return conversionResult;
}

}  // namespace virtualscene
}  // namespace android
