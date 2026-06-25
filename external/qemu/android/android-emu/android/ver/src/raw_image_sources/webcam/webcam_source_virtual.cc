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

#include "raw_image_sources/webcam/webcam_source_virtual.h"
#include "../fourcc_utils.h"

namespace android {
namespace ver {

int VirtualWebcamImpl::StartLocked(uint32_t pixel_format,
                                   int width,
                                   int height) {
    mFormat = FourccToVerImageFormat(pixel_format);
    mWidth = width;
    mHeight = height;
    return 0;
}

absl::StatusOr<bool> VirtualWebcamImpl::FetchNextFrame(
        std::function<absl::Status(const RawImageBufferView*)> new_frame_cb) {
    std::lock_guard<std::mutex> lock(mFrameMutex);
    if (mFrameBuffer.empty() || !mFrameUpdated) {
        return false;
    }

    RawImageBufferView view = {mFrameBuffer.data(), mFrameBuffer.size(),
                               mFormat, mWidth, mHeight};
    absl::Status status = new_frame_cb(&view);
    if (!status.ok()) {
        return status;
    }
    mFrameUpdated = false;
    return true;
}

void WebcamSourceTestHelper::AddVirtualWebcam(
        std::shared_ptr<WebcamSource::WebcamInfo> info) {
    std::lock_guard<std::mutex> lock(WebcamSource::s_webcam_info_list_lock_);
    if (!WebcamSource::s_webcam_info_list_) {
        WebcamSource::s_webcam_info_list_ =
                std::vector<std::shared_ptr<WebcamSource::WebcamInfo>>();
    }
    WebcamSource::s_webcam_info_list_->push_back(info);
}

void WebcamSourceTestHelper::ClearWebcams() {
    std::lock_guard<std::mutex> lock(WebcamSource::s_webcam_info_list_lock_);
    WebcamSource::s_webcam_info_list_ =
            std::vector<std::shared_ptr<WebcamSource::WebcamInfo>>();
}

std::vector<std::shared_ptr<WebcamSource::WebcamInfo>>
WebcamSourceTestHelper::EnumerateWebcams() {
    std::lock_guard<std::mutex> lock(WebcamSource::s_webcam_info_list_lock_);
    return WebcamSource::EnumerateWebcams();
}

int WebcamSourceTestHelper::GetPreferredFormatIndex(
        std::vector<WebcamSource::WebcamPixelFormat>& formats) {
    return WebcamSource::GetPreferredFormatIndex(formats);
}

}  // namespace ver
}  // namespace android
