/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include <cstdint>
#include "absl/status/statusor.h"
#include "../raw_image_source.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}  // extern "C"

#include <optional>
#include <string>

namespace android {
namespace ver {

class RawVideofileSource : public RawImageSource {
public:
    static std::unique_ptr<RawVideofileSource> Create(std::string filename);
    int Start(VerImageFormat pixel_format, int width, int height) override;
    absl::StatusOr<std::optional<RawImageToken>> UpdateImage(
            int64_t target_time_us,
            std::optional<RawImageToken> token,
            std::function<absl::Status(const RawImageBufferView*)> updater)
            override;
    int Stop() override;
    int GetBaseRotation() override;
    int64_t GetAnimationLengthUs() override;

private:
    std::string file_;
    int64_t us_per_frame_;
    int64_t seek_threshold_pts_;
    int64_t last_update_time_us_;
    int64_t last_update_time_pts_;
    bool paused_;

    int64_t GetUsFromPts(int64_t pts) const;
    int64_t GetPtsFromUs(int64_t us) const;

    struct AVFormatContextDeleter {
        void operator()(AVFormatContext* p) const {
            ::avformat_close_input(&p);
        }
    };

    using AVFormatContextPtr =
            std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;

    struct AVCodecContextDeleter {
        void operator()(AVCodecContext* p) const {
            ::avcodec_close(p);
            ::avcodec_free_context(&p);
        }
    };

    using AVCodecContextPtr =
            std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;

    struct AVFrameDeleter {
        void operator()(AVFrame* p) const { ::av_frame_free(&p); }
    };

    using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

    struct SwsContextDeleter {
        void operator()(SwsContext* p) const { ::sws_freeContext(p); }
    };

    using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

    struct VideoFile {
        AVFormatContextPtr formatCtx;
        AVCodecContextPtr codecCtx;
        unsigned videoStreamIndex;
        int baseRotation;
    };

    VideoFile mVideoFile;
    AVFramePtr mFrameCache;
    AVFramePtr mConvertedFrameCache;
    std::vector<uint8_t> mConvertedFrameBuffer;

    struct ConversionKey {
        int srcWidth;
        int srcHeight;
        AVPixelFormat srcFmt;
        int dstWidth;
        int dstHeight;
        AVPixelFormat dstFmt;

        bool operator==(const ConversionKey& rhs) const {
            return (srcWidth == rhs.srcWidth) && (srcHeight == rhs.srcHeight) &&
                   (srcFmt == rhs.srcFmt) && (dstWidth == rhs.dstWidth) &&
                   (dstHeight == rhs.dstHeight) && (dstFmt == rhs.dstFmt);
        }
    };

    explicit RawVideofileSource(VideoFile videoFile);
    int decodeNextFrame();
    int convertFrameToRGBA();
    static std::optional<VideoFile> openVideoFile(const char* filename);

    std::vector<std::pair<ConversionKey, SwsContextPtr>> mConverterCache;
    SwsContext* getSwsContext(const int srcWidth,
                              const int srcHeight,
                              const AVPixelFormat srcFmt,
                              const int dstWidth,
                              const int dstHeight,
                              const AVPixelFormat dstFmt);
};

}  // namespace ver
}  // namespace android
