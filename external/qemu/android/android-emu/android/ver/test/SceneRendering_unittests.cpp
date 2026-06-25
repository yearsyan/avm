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

#include <gtest/gtest.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "OpenGLESDispatch/OpenGLDispatchLoader.h"
#include "aemu/base/Debug.h"
#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"
#include "ver/virtual_environment_renderer.h"

using namespace android::base;
using namespace gfxstream::host::gl;  // For LazyLoadedGLESv2Dispatch

namespace {

// Threshold for golden image comparison (Sum of Squared Differences per pixel)
static constexpr double kCompareThreshold = 512.0;

class SceneRenderingTest : public ::testing::TestWithParam<std::string> {
protected:
    void SetUp() override {
        std::vector<std::filesystem::path> resourcePaths;
        std::string resourcesDir = PathUtils::join(
                System::get()->getProgramDirectory(), "resources");
        resourcePaths.push_back(resourcesDir);

        // Initialize with lazy-loaded GL dispatch
        ver_initialize(resourcePaths, (const void*)LazyLoadedEGLDispatch::get(),
                       (const void*)LazyLoadedGLESv2Dispatch::get());
    }

    void TearDown() override { ver_cleanup(); }

    bool compareWithGolden(const uint8_t* actualData,
                           int width,
                           int height,
                           const std::string& goldenPath) {
        int goldenWidth, goldenHeight, goldenBpp;
        std::vector<uint8_t> goldenBuffer;

        if (!ver_texture_utils_load_png(goldenPath.c_str(), &goldenWidth,
                                        &goldenHeight, &goldenBpp,
                                        &goldenBuffer)) {
            fprintf(stderr, "Failed to load golden image: %s",
                    goldenPath.c_str());
            return false;
        }

        if (width != goldenWidth || height != goldenHeight) {
            fprintf(stderr,
                    "Dimensions mismatch: actual(%dx%d) vs golden(%dx%d)",
                    width, height, goldenWidth, goldenHeight);
            return false;
        }

        // VER framebuffer is RGBA8 (4 bytes per pixel)
        // TextureUtils load might return RGB24 or RGBA32.
        size_t actualSize = width * height * 4;
        size_t goldenSize = goldenBuffer.size();

        if (goldenBpp == 3) {
            // Convert golden to RGBA for easier comparison
            std::vector<uint8_t> convertedGolden(actualSize);
            for (int i = 0; i < width * height; ++i) {
                convertedGolden[i * 4 + 0] = goldenBuffer[i * 3 + 0];
                convertedGolden[i * 4 + 1] = goldenBuffer[i * 3 + 1];
                convertedGolden[i * 4 + 2] = goldenBuffer[i * 3 + 2];
                convertedGolden[i * 4 + 3] = 255;
            }
            return calculateSSD(actualData, convertedGolden.data(),
                                actualSize) <= kCompareThreshold * actualSize;
        } else {
            return calculateSSD(actualData, goldenBuffer.data(), actualSize) <=
                   kCompareThreshold * actualSize;
        }
    }

    double calculateSSD(const uint8_t* data1,
                        const uint8_t* data2,
                        size_t size) {
        double sum = 0.0;
        for (size_t i = 0; i < size; ++i) {
            double diff = static_cast<double>(data1[i]) - data2[i];
            sum += diff * diff;
        }
        return sum;
    }
};

TEST_P(SceneRenderingTest, RenderSceneMode) {
    std::string modeName = GetParam();
    VerSceneConfig::Mode mode = VerSceneConfig::modeFromString(modeName);

    // Fail on invalid modes
    ASSERT_NE(mode, VerSceneConfig::Mode::Unknown);

#ifndef __APPLE__
    // TODO(virtualscene-library): Fix software GLES initialization on linux&windows
    if (VerSceneConfig::modeRequiresRenderer(mode)) {
        GTEST_SKIP()
                << "GLES renderer is currently not supported on this platform for testing.";
        return;
    }
#endif
    VerSceneConfig config(mode, VerSceneConfig::defaultArgumentForMode(mode));
    VerSceneHandle scene = ver_create_scene(config);
    ASSERT_NE(scene, (VerSceneHandle)VER_INVALID_HANDLE)
            << "Failed to create scene for mode: " << modeName;

    ver_scene_load_user_resources(scene, []() {});
    ver_scene_update(scene, true);

    VerRenderViewHandle view = ver_create_render_view();
    ASSERT_NE(view, (VerRenderViewHandle)VER_INVALID_HANDLE);

    int width = 640;
    int height = 480;
    ver_render_view_set_dimensions(view, width, height);

    // View projection from the initial SceneCamera values
    const float viewProj[16] = {1.500f,  0.000f,  0.000f,  0.000f,   //
                                0.000f,  -1.993f, -0.083f, -0.083f,  //
                                -0.000f, 0.166f,  -0.999f, -0.997f,  //
                                -0.090f, -0.060f, -0.202f, -0.002f};

    ver_render_view_set_view_projection(view, viewProj);

    bool rendered = ver_render_view(scene, view, []() {}, nullptr);
    if (!rendered) {
        // Some modes might fail if GL is not available or if resources are
        // missing. We log it and fail the test.
        ver_destroy_render_view(view);
        ver_destroy_scene(scene);
        FAIL() << "Rendering failed for mode: " << modeName;
    }

    const uint8_t* fbData = nullptr;
    uint64_t fbSize = 0;
    ver_render_view_get_framebuffer(view, &fbData, &fbSize);
    ASSERT_NE(fbData, nullptr);
    ASSERT_EQ(fbSize, (uint64_t)width * height * 4);

    std::string goldenPath =
            PathUtils::join(System::get()->getProgramDirectory(), "testdata",
                            "scene_" + modeName + "_golden.png");

    // If golden image doesn't exist, we might want to skip or fail.
    // Here we fail to ensure all modes have goldens.
    if (!std::filesystem::exists(goldenPath)) {
        FAIL() << "Golden image not found at: " << goldenPath;
    }

    bool match = compareWithGolden(fbData, width, height, goldenPath);

    if (!match) {
        FAIL() << "Golden image comparison failed for mode: " << modeName;
    }

    ver_destroy_render_view(view);
    ver_destroy_scene(scene);
}

INSTANTIATE_TEST_SUITE_P(SceneModes,
                         SceneRenderingTest,
                         ::testing::Values("mesh3d",
                                           "videofile",
                                           "imagefile",
                                           "color",
                                           "image360"));

}  // namespace
