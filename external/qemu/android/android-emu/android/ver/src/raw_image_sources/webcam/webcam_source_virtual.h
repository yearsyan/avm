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

#pragma once

#include <mutex>
#include <vector>
#include "raw_image_sources/webcam/webcam_source.h"

namespace android {
namespace ver {

class VirtualWebcamImpl : public WebcamSource::Impl {
public:
    VirtualWebcamImpl() = default;
    ~VirtualWebcamImpl() override = default;

    void PushFrame(std::vector<uint8_t> frame) {
        std::lock_guard<std::mutex> lock(mFrameMutex);
        mFrameBuffer = std::move(frame);
        mFrameUpdated = true;
    }

protected:
    int StartLocked(uint32_t pixel_format, int width, int height) override;
    int StopLocked() override { return 0; }
    absl::StatusOr<bool> FetchNextFrame(
            std::function<absl::Status(const RawImageBufferView*)> new_frame_cb)
            override;

private:
    std::mutex mFrameMutex;
    std::vector<uint8_t> mFrameBuffer;
    bool mFrameUpdated = false;
    VerImageFormat mFormat = VerImageFormat::UNKNOWN;
    int mWidth = 0;
    int mHeight = 0;
};

class WebcamSourceTestHelper {
public:
    static void AddVirtualWebcam(
            std::shared_ptr<WebcamSource::WebcamInfo> info);
    static void ClearWebcams();
    static std::vector<std::shared_ptr<WebcamSource::WebcamInfo>>
    EnumerateWebcams();
    static int GetPreferredFormatIndex(
            std::vector<WebcamSource::WebcamPixelFormat>& formats);
};

}  // namespace ver
}  // namespace android
