// Copyright 2026 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../fourcc_utils.h"
#include "../raw_image_source.h"

#include "android/utils/compiler.h"
#include "ver/virtual_environment_renderer_types.h"

namespace android {
namespace ver {

/*
 * Contains declarations for webcam video capturing API that is used by the
 * camera emulator.
 */

class WebcamSource : public RawImageSource {
public:
    class Impl;  // Forward declaration

    struct Resolution {
        unsigned int width;
        unsigned int height;
    };
    struct WebcamResolutions {
        Resolution resolution;
        std::vector<float> framerates;
    };
    struct WebcamPixelFormat {
        uint32_t pixel_format;
        bool compressed;
        std::vector<WebcamResolutions> resolutions;

        Resolution FindBestMatchForResolution(Resolution requested);
    };

    struct WebcamInfo {
        // These fields are set once when the webcam list is enumerated, and
        // will not be modified after
        std::string friendly_name;
        std::string os_name;   // Fixed name used by the operating system
        std::string os_alias;  // Persistently fixed name used by the operating
                               // system
        std::vector<WebcamPixelFormat> supported_formats;
        int preferred_format_index = -1;  // -1 if no format is supported

        std::weak_ptr<Impl> shared_impl;
    };

    static size_t GetWebcamCount();
    static std::shared_ptr<WebcamInfo> GetWebcamInfo(size_t index);
    static std::string ResolveWebcamId(std::string_view camera_arg);

    static std::unique_ptr<WebcamSource> Create(std::string_view camera_arg);
    ~WebcamSource() override;
    int Start(VerImageFormat ver_image_format, int width, int height) override;
    absl::StatusOr<std::optional<RawImageToken>> UpdateImage(
            int64_t target_time_us,
            std::optional<RawImageToken> token,
            std::function<absl::Status(const RawImageBufferView*)> updater)
            override;
    int Stop() override;

    class Impl {
    public:
        virtual ~Impl() = default;
        int Start(uint32_t pixel_format, int width, int height);
        int Stop();
        absl::StatusOr<std::optional<RawImageToken>> UpdateImage(
                int64_t target_time_us,
                std::optional<RawImageToken> token,
                std::function<absl::Status(const RawImageBufferView*)> updater);

    protected:
        virtual int StartLocked(uint32_t pixel_format,
                                int width,
                                int height) = 0;
        virtual int StopLocked() = 0;
        virtual absl::StatusOr<bool> FetchNextFrame(
                std::function<absl::Status(const RawImageBufferView*)>
                        new_frame_cb) = 0;

    private:
        std::mutex mMutex;
        int mStartCount = 0;
        int mStartStatus = 0;
        uint32_t mPixelFormat = 0;
        int mWidth = 0;
        int mHeight = 0;

        std::optional<RawImageToken> mLatestToken;
        std::vector<uint8_t> mCacheBuffer;
        VerImageFormat mLatestFormat = VerImageFormat::UNKNOWN;
        int mLatestWidth = 0;
        int mLatestHeight = 0;
        int64_t mLatestTimeUs = -1;
        int64_t mTokenCounter = 0;
    };

private:
    friend class WebcamSourceTestHelper;
    explicit WebcamSource(std::shared_ptr<WebcamInfo> info);
    // The list of webcams available on the system. This is a shared ptr so we
    // can refresh it safely without distrupting webcams which are using data
    // from the list;
    static std::vector<std::shared_ptr<WebcamInfo>> EnumerateWebcams();
    static int GetPreferredFormatIndex(std::vector<WebcamPixelFormat>& formats);

    // Lock to be held when accessing or modifying the webcam list
    static std::mutex s_webcam_info_list_lock_;
    static std::optional<std::vector<std::shared_ptr<WebcamInfo>>>
            s_webcam_info_list_;
    std::shared_ptr<WebcamInfo> webcam_info_;
    std::shared_ptr<Impl> impl_;
};

std::unique_ptr<WebcamSource::Impl> CreatePlatformWebcamImpl(
        std::shared_ptr<const WebcamSource::WebcamInfo> info);

}  // namespace ver
}  // namespace android
