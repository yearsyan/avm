// Copyright 2026 The Android Open Source Project
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

#include "fourcc_utils.h"
#include <libyuv.h>

#include <cstddef>
#include "android/camera/camera-common.h"

namespace android {
namespace ver {

std::string FourccToString(uint32_t fourcc) {
    char buf[5];
    buf[0] = fourcc & 0xff;
    buf[1] = (fourcc >> 8) & 0xff;
    buf[2] = (fourcc >> 16) & 0xff;
    buf[3] = (fourcc >> 24) & 0xff;
    buf[4] = '\0';
    return std::string(buf);
}

VerImageFormat FourccToVerImageFormat(uint32_t fourcc) {
    if (fourcc == V4L2_PIX_FMT_RGB32) {
        return VerImageFormat::RGBA8;
    }
    return VerImageFormat::UNKNOWN;
}

uint32_t VerImageFormatToFourcc(VerImageFormat ver_image_format) {
    if (ver_image_format == VerImageFormat::RGBA8) {
        return V4L2_PIX_FMT_RGB32;
    }
    return 0;
}

RawImageBufferView RawImageBufferViewFourCCBridge(
        RawImageBufferViewFourCC* fourcc) {
    return {fourcc->buffer, fourcc->buffer_size,
            FourccToVerImageFormat(fourcc->pixel_format), fourcc->width,
            fourcc->height};
}

int ConvertBufferToRGB32(RawImageBufferViewFourCC& buffer,
                         std::vector<uint8_t>& conversion_storage,
                         std::vector<uint8_t>& staging_buffer) {
    if (conversion_storage.size() <
        static_cast<size_t>(buffer.height) * buffer.width * 4) {
        conversion_storage.resize(static_cast<size_t>(buffer.height) *
                                  buffer.width * 4);
    }

    int width = buffer.width;
    int height = buffer.height;

    int res = 0;
    if (buffer.pixel_format == V4L2_PIX_FMT_RGB32) {
        if (conversion_storage.data() != buffer.buffer) {
            memcpy(conversion_storage.data(), buffer.buffer,
                   static_cast<size_t>(width) * height * 4);
        }
        res = 0;
    } else if (buffer.pixel_format == V4L2_PIX_FMT_BGR32) {
        // Swap R and B channels: FOURCC_ARGB -> FOURCC_ABGR
        res = libyuv::ARGBToABGR(static_cast<const uint8_t*>(buffer.buffer),
                                 width * 4, conversion_storage.data(),
                                 width * 4, width, height);
    } else {
        int y_stride = (width + 15) & ~15;
        int u_stride = (y_stride / 2 + 15) & ~15;
        int v_stride = u_stride;
        size_t y_size = static_cast<size_t>(y_stride) * height;
        size_t u_size = static_cast<size_t>(u_stride) * (height / 2);
        size_t v_size = u_size;

        size_t required_staging_size = y_size + u_size + v_size;
        if (staging_buffer.size() < required_staging_size) {
            staging_buffer.resize(required_staging_size);
        }

        uint8_t* y_staging = staging_buffer.data();
        uint8_t* u_staging = y_staging + y_size;
        uint8_t* v_staging = u_staging + u_size;

        uint32_t src_format = buffer.pixel_format;
        // These formats are aliases of eachother
        if (buffer.pixel_format == V4L2_PIX_FMT_YUY2 ||
            buffer.pixel_format == V4L2_PIX_FMT_YUNV ||
            buffer.pixel_format == V4L2_PIX_FMT_V422) {
            src_format = V4L2_PIX_FMT_YUYV;
        }

        res = libyuv::ConvertToI420(static_cast<const uint8_t*>(buffer.buffer),
                                    buffer.buffer_size, y_staging, y_stride,
                                    u_staging, u_stride, v_staging, v_stride, 0,
                                    0, width, height, width, height,
                                    libyuv::kRotate0, src_format);

        if (res == 0) {
            res = libyuv::ConvertFromI420(y_staging, y_stride, u_staging,
                                          u_stride, v_staging, v_stride,
                                          conversion_storage.data(), 0, width,
                                          height, libyuv::FOURCC_ABGR);
        } else {
            res = -1;
        }
    }

    if (res != 0) {
        return res;
    }

    buffer.buffer_size = static_cast<size_t>(width) * height * 4;
    buffer.pixel_format = V4L2_PIX_FMT_RGB32;
    buffer.buffer = conversion_storage.data();
    return 0;
}

}  // namespace ver
}  // namespace android
