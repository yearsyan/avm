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

#include <stdbool.h>
#include <stdint.h>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "aemu/base/Log.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

/**
 * @file virtual_environment_renderer_types.h
 * @brief Type definitions for the Virtual Environment Renderer.
 */

// TODO(virtualscene-library): remove the namespace and other C++ usages
// TODO(virtualscene-library): make sure get/set pairing is complete
namespace android {
namespace ver {

/**
 * @brief Minimum allowed size for a poster in meters.
 */
static constexpr float kPosterMinimumSizeMeters = 0.2f;

/**
 * @brief Information about a poster location and default image.
 */
struct PosterInfo {
    std::string name;                      ///< Unique identifier for the poster
    glm::vec2 size = glm::vec2(1.0f, 1.0f); ///< Dimensions in meters
    glm::vec3 position = glm::vec3();      ///< 3D coordinates in the scene
    glm::quat rotation = glm::quat();      ///< Orientation in the scene
    std::string defaultFilename;           ///< Fallback image filename
};

/**
 * @brief Opaque handle to a webcam info.
 */
typedef struct VerWebcamInfoOpaque* VerWebcamHandle;

/**
 * @brief Configuration for a virtual scene.
 */
struct SceneConfig {
    /**
     * @brief Rendering mode for the scene.
     */
    enum class Mode {
        Unknown = 0,
        Mesh3D,     ///< Full 3D environment with posters
        VideoFile,  ///< Single video file rendered as a plane
        ImageFile,  ///< Single image file rendered as a plane
        Color,      ///< Uniform background color
        Image360,   ///< 360-degree panoramic image
        Webcam,     ///< Single webcam feed rendered as a plane
    };

    SceneConfig(Mode mode, std::string_view argument) {
        mSceneMode = mode;
        mArgument = argument;
    }

    Mode mSceneMode = Mode::Unknown; ///< Selected rendering mode
    std::string mArgument;           ///< Path or argument specific to the mode

    // Default filenames for different scene modes, can be used
    // when the file cannot be found or loaded, all relative to
    // the emulator's 'resources' folder
    static constexpr const char* kDefaultSceneObj = "Toren1BD.obj";
    static constexpr const char* kDefaultImageFile = "default.jpg";
    static constexpr const char* kDefaultVideoFile = "default.mp4";
    static constexpr const char* kDefaultImage360File = "default360.jpg";
    static constexpr const char* kDefaultWebcam = "";

    // A blank background
    static constexpr const char* kDefaultColor = "#000000";

    /**
     * @brief Converts a string to a SceneConfig::Mode.
     */
    static Mode modeFromString(std::string_view sceneModeStr) {
        if (sceneModeStr == "virtualscene") {
            return SceneConfig::Mode::Mesh3D;
        } else if (sceneModeStr == "mesh3d") {
            return SceneConfig::Mode::Mesh3D;
        } else if (sceneModeStr == "videofile") {
            return SceneConfig::Mode::VideoFile;
        } else if (sceneModeStr == "imagefile") {
            return SceneConfig::Mode::ImageFile;
        } else if (sceneModeStr == "color") {
            return SceneConfig::Mode::Color;
        } else if (sceneModeStr == "image360") {
            return SceneConfig::Mode::Image360;
        } else if (sceneModeStr == "webcam") {
            return SceneConfig::Mode::Webcam;
        } else {
            dwarning("Unknown scene mode requested: %s", sceneModeStr);
            return SceneConfig::Mode::Unknown;
        }
    }

    /**
     * @brief Converts a SceneConfig::Mode to its string representation.
     */
    static const char* modeToString(SceneConfig::Mode mode) {
        if (mode == SceneConfig::Mode::Mesh3D) {
            return "mesh3d";
        } else if (mode == SceneConfig::Mode::VideoFile) {
            return "videofile";
        } else if (mode == SceneConfig::Mode::ImageFile) {
            return "imagefile";
        } else if (mode == SceneConfig::Mode::Color) {
            return "color";
        } else if (mode == SceneConfig::Mode::Image360) {
            return "image360";
        } else if (mode == SceneConfig::Mode::Webcam) {
            return "webcam";
        } else {
            return "unknown";
        }
    }

    /**
     * @brief Returns the default resource path for a given mode.
     */
    static const char* defaultArgumentForMode(SceneConfig::Mode mode) {
        if (mode == SceneConfig::Mode::Mesh3D) {
            return kDefaultSceneObj;
        } else if (mode == SceneConfig::Mode::VideoFile) {
            return kDefaultVideoFile;
        } else if (mode == SceneConfig::Mode::ImageFile) {
            return kDefaultImageFile;
        } else if (mode == SceneConfig::Mode::Color) {
            return kDefaultColor;
        } else if (mode == SceneConfig::Mode::Image360) {
            return kDefaultImage360File;
        } else if (mode == SceneConfig::Mode::Webcam) {
            return kDefaultWebcam;
        } else {
            derror("%s: Invalid mode %d", __func__, (int)mode);
            return "invalid_filename";
        }
    }

    /**
     * @brief Checks if the mode requires a hardware-accelerated renderer.
     */
    static bool modeRequiresRenderer(SceneConfig::Mode mode) {
        return (mode == SceneConfig::Mode::Mesh3D) ||
               (mode == SceneConfig::Mode::Image360);
    }

    /**
     * @brief Checks if the mode supports view-space rotations.
     */
    static bool modeSupportsViewRotations(SceneConfig::Mode mode) {
        // Currently, only Mesh3D supports view rotations
        return (mode == SceneConfig::Mode::Mesh3D) ||
               (mode == SceneConfig::Mode::Image360);
    }

    /**
     * @brief Checks if the mode supports animations (e.g., video).
     */
    static bool modeSupportsAnimations(SceneConfig::Mode mode) {
        // Output of the ImageFile and Color modes won't be affected by the
        // animations
        return (mode != SceneConfig::Mode::ImageFile &&
                mode != SceneConfig::Mode::Color);
    }

    /**
     * @brief Checks if the mode supports scene-level controls (camera).
     */
    static bool modeSupportsSceneControls(SceneConfig::Mode mode) {
        // These modes should enable scene controls for movement or rotation
        return (mode == SceneConfig::Mode::Mesh3D) ||
               (mode == SceneConfig::Mode::Image360);
    }
};

inline bool operator==(const SceneConfig& lhs, const SceneConfig& rhs) {
    return (lhs.mSceneMode == rhs.mSceneMode) &&
           (lhs.mArgument == rhs.mArgument);
}

}  // namespace ver
}  // namespace android

/**
 * @brief Supported image formats for render views.
 */
enum class VerImageFormat {
    UNKNOWN,  ///< An unsupported format
    RGBA8,    ///< 32-bit RGBA (8 bits per channel)
};

/**
 * @brief Alias for scene configuration.
 */
typedef android::ver::SceneConfig VerSceneConfig;

// Opaque handle for a Scene
typedef struct VerScene VerScene;

// Opaque handle for a RendererView
typedef struct VerRenderView VerRenderView;

/**
 * @brief Invalid handle value.
 */
#define VER_INVALID_HANDLE 0

/**
 * @brief Opaque handle to a virtual scene.
 */
typedef struct VerScene* VerSceneHandle;

/**
 * @brief Opaque handle to a render view.
 */
typedef struct VerRenderView* VerRenderViewHandle;

/**
 * @brief Callback type for completion of a render operation.
 */
using VerRenderFinishCallback = std::function<void()>;
