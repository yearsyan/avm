// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>
#include "android/camera/camera-common.h"
#include "raw_image_sources/fourcc_utils.h"
#include "raw_image_sources/webcam/webcam_source_virtual.h"

namespace android {
namespace ver {

class WebcamSourceTest : public ::testing::Test {
protected:
    void SetUp() override { WebcamSourceTestHelper::ClearWebcams(); }

    void TearDown() override { WebcamSourceTestHelper::ClearWebcams(); }
};

TEST_F(WebcamSourceTest, DiscoverVirtualWebcam) {
    auto info = std::make_shared<WebcamSource::WebcamInfo>();
    info->friendly_name = "Virtual Test Camera";
    info->os_name = "test_cam_0";
    info->os_alias = "TestCamera0";

    WebcamSourceTestHelper::AddVirtualWebcam(info);

    ASSERT_EQ(WebcamSource::GetWebcamCount(), 1);
    auto retrievedInfo = WebcamSource::GetWebcamInfo(0);
    ASSERT_NE(retrievedInfo, nullptr);
    EXPECT_EQ(retrievedInfo->friendly_name, "Virtual Test Camera");
    EXPECT_EQ(retrievedInfo->os_name, "test_cam_0");
    EXPECT_EQ(retrievedInfo->os_alias, "TestCamera0");
}

TEST_F(WebcamSourceTest, ResolveWebcamId) {
    auto info1 = std::make_shared<WebcamSource::WebcamInfo>();
    info1->friendly_name = "Camera 1";
    info1->os_name = "cam1";
    info1->os_alias = "alias1";
    WebcamSourceTestHelper::AddVirtualWebcam(info1);

    auto info2 = std::make_shared<WebcamSource::WebcamInfo>();
    info2->friendly_name = "Camera 2";
    info2->os_name = "cam2";
    info2->os_alias = "";  // No alias
    WebcamSourceTestHelper::AddVirtualWebcam(info2);

    // Resolve default (empty string) -> should be first camera (alias1)
    EXPECT_EQ(WebcamSource::ResolveWebcamId(""), "alias1");

    // Resolve by name/alias
    EXPECT_EQ(WebcamSource::ResolveWebcamId("alias1"), "alias1");
    EXPECT_EQ(WebcamSource::ResolveWebcamId("cam1"), "alias1");
    EXPECT_EQ(WebcamSource::ResolveWebcamId("cam2"), "cam2");

    // Resolve by index
    EXPECT_EQ(WebcamSource::ResolveWebcamId("0"), "alias1");
    EXPECT_EQ(WebcamSource::ResolveWebcamId("1"), "cam2");

    // Resolve invalid
    EXPECT_EQ(WebcamSource::ResolveWebcamId("2"), "");
    EXPECT_EQ(WebcamSource::ResolveWebcamId("invalid"), "");
}

TEST_F(WebcamSourceTest, VirtualWebcamFramePumping) {
    auto info = std::make_shared<WebcamSource::WebcamInfo>();
    info->friendly_name = "Virtual Test Camera";
    info->os_name = "test_cam_0";
    info->os_alias = "TestCamera0";

    WebcamSource::WebcamPixelFormat format;
    uint32_t rgb32_fourcc = VerImageFormatToFourcc(VerImageFormat::RGBA8);
    format.pixel_format = rgb32_fourcc;

    WebcamSource::Resolution res = {640, 480};
    WebcamSource::WebcamResolutions webRes = {res, {30.0f}};
    format.resolutions.push_back(webRes);
    info->supported_formats.push_back(format);
    info->preferred_format_index = 0;

    auto virtualImpl = std::make_shared<VirtualWebcamImpl>();
    info->shared_impl = virtualImpl;

    WebcamSourceTestHelper::AddVirtualWebcam(info);

    // Create WebcamSource
    auto source = WebcamSource::Create("TestCamera0");
    ASSERT_NE(source, nullptr);

    // Start source
    int startRes = source->Start(VerImageFormat::RGBA8, 640, 480);
    ASSERT_EQ(startRes, 0);

    // Case A: Grab frame when there isn't one (none ever pushed)
    bool updated = false;
    auto updateRes = source->UpdateImage(0, std::nullopt,
                                         [&](const RawImageBufferView* view) {
                                             updated = true;
                                             return absl::OkStatus();
                                         });
    ASSERT_TRUE(updateRes.ok());
    EXPECT_FALSE(updateRes->has_value());
    EXPECT_FALSE(updated);

    // Push a frame (Frame 1)
    std::vector<uint8_t> frameData(640 * 480 * 4, 0xff);  // White frame
    virtualImpl->PushFrame(std::move(frameData));

    // Grab Frame 1
    updated = false;
    std::optional<RawImageToken> token1;
    updateRes = source->UpdateImage(
            0, std::nullopt, [&](const RawImageBufferView* view) {
                EXPECT_EQ(view->width, 640);
                EXPECT_EQ(view->height, 480);
                EXPECT_EQ(view->pixel_format, VerImageFormat::RGBA8);
                EXPECT_EQ(view->buffer_size, 640 * 480 * 4);
                EXPECT_EQ(view->buffer[0], 0xff);
                updated = true;
                return absl::OkStatus();
            });

    ASSERT_TRUE(updateRes.ok());
    ASSERT_TRUE(updateRes->has_value());
    EXPECT_TRUE(updated);
    token1 = *updateRes;

    // Case B: Grab next frame when there isn't one (we use token1, no new frame
    // pushed)
    updated = false;
    updateRes =
            source->UpdateImage(0, token1, [&](const RawImageBufferView* view) {
                updated = true;
                return absl::OkStatus();
            });
    ASSERT_TRUE(updateRes.ok());
    EXPECT_FALSE(updateRes->has_value());
    EXPECT_FALSE(updated);

    // Case C: Grab next frame when there isn't one using an old token
    RawImageToken oldToken = {token1->token - 1};
    updated = false;
    updateRes = source->UpdateImage(
            0, oldToken, [&](const RawImageBufferView* view) {
                EXPECT_EQ(view->buffer[0],
                          0xff);  // Should still be the white frame
                updated = true;
                return absl::OkStatus();
            });
    ASSERT_TRUE(updateRes.ok());
    ASSERT_TRUE(updateRes->has_value());
    EXPECT_EQ((*updateRes)->token, token1->token);  // Should return token1
    EXPECT_TRUE(updated);

    // Stop source
    source->Stop();
}

TEST_F(WebcamSourceTest, MultipleUsersSharingSource) {
    auto info = std::make_shared<WebcamSource::WebcamInfo>();
    info->friendly_name = "Virtual Test Camera";
    info->os_name = "test_cam_0";
    info->os_alias = "TestCamera0";

    WebcamSource::WebcamPixelFormat format;
    uint32_t rgb32_fourcc = VerImageFormatToFourcc(VerImageFormat::RGBA8);
    format.pixel_format = rgb32_fourcc;

    WebcamSource::Resolution res = {640, 480};
    WebcamSource::WebcamResolutions webRes = {res, {30.0f}};
    format.resolutions.push_back(webRes);
    info->supported_formats.push_back(format);
    info->preferred_format_index = 0;

    auto virtualImpl = std::make_shared<VirtualWebcamImpl>();
    info->shared_impl = virtualImpl;

    WebcamSourceTestHelper::AddVirtualWebcam(info);

    // Create WebcamSource for User 1
    auto source1 = WebcamSource::Create("TestCamera0");
    ASSERT_NE(source1, nullptr);

    // Create WebcamSource for User 2
    auto source2 = WebcamSource::Create("TestCamera0");
    ASSERT_NE(source2, nullptr);

    // Start both
    ASSERT_EQ(source1->Start(VerImageFormat::RGBA8, 640, 480), 0);
    ASSERT_EQ(source2->Start(VerImageFormat::RGBA8, 640, 480), 0);

    // Push Frame 1
    std::vector<uint8_t> frameData1(640 * 480 * 4, 0x11);  // pattern 0x11
    virtualImpl->PushFrame(std::move(frameData1));

    // User 1 updates (nullopt) -> should fetch Frame 1
    std::optional<RawImageToken> token1;
    bool updated1 = false;
    auto res1 = source1->UpdateImage(0, std::nullopt,
                                     [&](const RawImageBufferView* view) {
                                         EXPECT_EQ(view->buffer[0], 0x11);
                                         updated1 = true;
                                         return absl::OkStatus();
                                     });
    ASSERT_TRUE(res1.ok());
    ASSERT_TRUE(res1->has_value());
    EXPECT_TRUE(updated1);
    token1 = *res1;

    // User 2 updates (nullopt) -> should get Frame 1 from cache
    std::optional<RawImageToken> token2;
    bool updated2 = false;
    auto res2 = source2->UpdateImage(0, std::nullopt,
                                     [&](const RawImageBufferView* view) {
                                         EXPECT_EQ(view->buffer[0], 0x11);
                                         updated2 = true;
                                         return absl::OkStatus();
                                     });
    ASSERT_TRUE(res2.ok());
    ASSERT_TRUE(res2->has_value());
    EXPECT_TRUE(updated2);
    token2 = *res2;

    EXPECT_EQ(token1->token, token2->token);  // Should be same token

    // Push Frame 2
    std::vector<uint8_t> frameData2(640 * 480 * 4, 0x22);  // pattern 0x22
    virtualImpl->PushFrame(std::move(frameData2));

    // User 1 updates (with token1) -> should fetch Frame 2
    updated1 = false;
    res1 = source1->UpdateImage(0, token1, [&](const RawImageBufferView* view) {
        EXPECT_EQ(view->buffer[0], 0x22);
        updated1 = true;
        return absl::OkStatus();
    });
    ASSERT_TRUE(res1.ok());
    ASSERT_TRUE(res1->has_value());
    EXPECT_TRUE(updated1);
    token1 = *res1;

    // User 2 updates (with token2, which is equal to old token1) -> should get
    // Frame 2 from cache
    updated2 = false;
    res2 = source2->UpdateImage(0, token2, [&](const RawImageBufferView* view) {
        EXPECT_EQ(view->buffer[0], 0x22);
        updated2 = true;
        return absl::OkStatus();
    });
    ASSERT_TRUE(res2.ok());
    ASSERT_TRUE(res2->has_value());
    EXPECT_TRUE(updated2);
    token2 = *res2;

    EXPECT_EQ(token1->token, token2->token);  // Should still be same token

    source1->Stop();
    source2->Stop();
}

TEST_F(WebcamSourceTest, EnumerateWebcams) {
    // Verify EnumerateWebcams runs without crashing
    auto webcams = WebcamSourceTestHelper::EnumerateWebcams();
    for (const auto& info : webcams) {
        EXPECT_FALSE(info->friendly_name.empty());
        EXPECT_FALSE(info->os_name.empty());
    }
}

TEST_F(WebcamSourceTest, FormatSelection) {
    std::vector<WebcamSource::WebcamPixelFormat> formats;

    // Test with empty formats
    EXPECT_EQ(WebcamSourceTestHelper::GetPreferredFormatIndex(formats), -1);

    uint32_t yuv_fourcc = 0x32315559;  // YU12
    uint32_t rgb_fourcc = VerImageFormatToFourcc(VerImageFormat::RGBA8);

    WebcamSource::WebcamPixelFormat formatYuv;
    formatYuv.pixel_format = yuv_fourcc;
    formats.push_back(formatYuv);

    WebcamSource::WebcamPixelFormat formatRgb;
    formatRgb.pixel_format = rgb_fourcc;
    formats.push_back(formatRgb);

    // Preferred format should be YUV420 over RGB32
    EXPECT_EQ(WebcamSourceTestHelper::GetPreferredFormatIndex(formats), 0);

    // If we only have RGB32:
    std::vector<WebcamSource::WebcamPixelFormat> formatsRgbOnly;
    formatsRgbOnly.push_back(formatRgb);
    EXPECT_EQ(WebcamSourceTestHelper::GetPreferredFormatIndex(formatsRgbOnly),
              0);

    // Test FindBestMatchForResolution
    WebcamSource::WebcamPixelFormat formatWithResolutions;
    formatWithResolutions.resolutions = {{{320, 240}, {30.0f}},
                                         {{640, 480}, {30.0f}},
                                         {{1280, 720}, {30.0f}},
                                         {{1920, 1080}, {30.0f}}};

    // Requested exactly matches
    auto match = formatWithResolutions.FindBestMatchForResolution({640, 480});
    EXPECT_EQ(match.width, 640);
    EXPECT_EQ(match.height, 480);

    // Requested is smaller than smallest, but triggers threshold (should still
    // return smallest)
    match = formatWithResolutions.FindBestMatchForResolution({160, 120});
    EXPECT_EQ(match.width, 320);
    EXPECT_EQ(match.height, 240);

    // Requested is smaller than smallest, but does not trigger threshold
    // (returns smallest)
    match = formatWithResolutions.FindBestMatchForResolution({300, 200});
    EXPECT_EQ(match.width, 320);
    EXPECT_EQ(match.height, 240);

    // Requested is between two, falls back due to threshold
    match = formatWithResolutions.FindBestMatchForResolution({800, 600});
    EXPECT_EQ(match.width, 640);
    EXPECT_EQ(match.height, 480);

    // Requested is between two, matches larger
    match = formatWithResolutions.FindBestMatchForResolution({1024, 768});
    EXPECT_EQ(match.width, 1280);
    EXPECT_EQ(match.height, 720);
}

TEST_F(WebcamSourceTest, ConvertBufferToRGB32_YUYV_to_RGB32) {
    // 2x2 Red image in YUYV format
    // Y approx 76, U approx 85, V approx 255
    const std::vector<uint8_t> yuyv_data = {76, 85, 76, 255, 76, 85, 76, 255};

    int width = 2;
    int height = 2;

    RawImageBufferViewFourCC view = {yuyv_data.data(), yuyv_data.size(),
                                     V4L2_PIX_FMT_YUYV, width, height};

    std::vector<uint8_t> conversion_storage;
    std::vector<uint8_t> staging_buffer;

    int res = ConvertBufferToRGB32(view, conversion_storage, staging_buffer);

    ASSERT_EQ(res, 0);
    ASSERT_EQ(view.pixel_format, V4L2_PIX_FMT_RGB32);
    ASSERT_EQ(view.buffer, conversion_storage.data());
    ASSERT_EQ(view.buffer_size, width * height * 4);

    for (int i = 0; i < width * height; ++i) {
        int offset = i * 4;
        // (FOURCC_ABGR output is [R, G, B, A] in memory)
        EXPECT_NEAR(view.buffer[offset + 0], 255, 5);  // R
        EXPECT_NEAR(view.buffer[offset + 1], 0, 5);    // G
        EXPECT_NEAR(view.buffer[offset + 2], 0, 5);    // B
        EXPECT_EQ(view.buffer[offset + 3], 255);       // A
    }
}

TEST_F(WebcamSourceTest, ConvertBufferToRGB32_BGR32_to_RGB32) {
    // 2x2 image in BGR32 format
    // Input bytes: [B, G, R, A]
    const std::vector<uint8_t> bgr32_data = {
            255, 0,   0,   255,  // Pixel 0: Blue
            0,   255, 0,   255,  // Pixel 1: Green
            0,   0,   255, 255,  // Pixel 2: Red
            255, 255, 255, 255   // Pixel 3: White
    };

    int width = 2;
    int height = 2;

    RawImageBufferViewFourCC view = {bgr32_data.data(), bgr32_data.size(),
                                     V4L2_PIX_FMT_BGR32, width, height};

    std::vector<uint8_t> conversion_storage;
    std::vector<uint8_t> staging_buffer;

    int res = ConvertBufferToRGB32(view, conversion_storage, staging_buffer);

    ASSERT_EQ(res, 0);
    ASSERT_EQ(view.pixel_format, V4L2_PIX_FMT_RGB32);
    ASSERT_EQ(view.buffer, conversion_storage.data());
    ASSERT_EQ(view.buffer_size, width * height * 4);

    // We expect R and B channels to be swapped.
    // Pixel 0: Input BGR Blue [255, 0, 0, 255] -> Output RGB Blue [0, 0, 255, 255]
    EXPECT_NEAR(view.buffer[0], 0, 5);
    EXPECT_NEAR(view.buffer[1], 0, 5);
    EXPECT_NEAR(view.buffer[2], 255, 5);
    EXPECT_EQ(view.buffer[3], 255);

    // Pixel 1: Input BGR Green [0, 255, 0, 255] -> Output RGB Green [0, 255, 0, 255]
    EXPECT_NEAR(view.buffer[4], 0, 5);
    EXPECT_NEAR(view.buffer[5], 255, 5);
    EXPECT_NEAR(view.buffer[6], 0, 5);
    EXPECT_EQ(view.buffer[7], 255);

    // Pixel 2: Input BGR Red [0, 0, 255, 255] -> Output RGB Red [255, 0, 0, 255]
    EXPECT_NEAR(view.buffer[8], 255, 5);
    EXPECT_NEAR(view.buffer[9], 0, 5);
    EXPECT_NEAR(view.buffer[10], 0, 5);
    EXPECT_EQ(view.buffer[11], 255);

    // Pixel 3: Input White [255, 255, 255, 255] ->
    //          Output White [255, 255, 255, 255]
    EXPECT_NEAR(view.buffer[12], 255, 5);
    EXPECT_NEAR(view.buffer[13], 255, 5);
    EXPECT_NEAR(view.buffer[14], 255, 5);
    EXPECT_EQ(view.buffer[15], 255);
}

}  // namespace ver
}  // namespace android
