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

#include "android/camera/camera-sws-format-converter.h"

#include <optional>
#include <string>
#include <vector>
#include "aemu/base/logging/Log.h"
#include "android/camera/camera-common.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}  // extern "C"

#include "android/utils/debug.h"

FrameBufferInfo getFrameBufferInfo(const ClientFrameBuffer& cfb) {
    const size_t w = cfb.width;
    const size_t h = cfb.height;

    FrameBufferInfo fbi = {};
    fbi.planes[0] = static_cast<uint8_t*>(cfb.framebuffer);

    switch (cfb.pixel_format) {
        case V4L2_PIX_FMT_RGB32:
            fbi.strides[0] = w * 4U;
            fbi.format = AV_PIX_FMT_RGBA;
            break;

        case V4L2_PIX_FMT_YUV420:
            fbi.planes[1] = fbi.planes[0] + (w * h);
            fbi.planes[2] = fbi.planes[1] + (w * h) / 4;
            fbi.strides[0] = w;
            fbi.strides[1] = w / 2;
            fbi.strides[2] = w / 2;
            fbi.format = AV_PIX_FMT_YUV420P;
            break;

        case V4L2_PIX_FMT_NV12:
            fbi.planes[1] = fbi.planes[0] + (w * h);
            fbi.strides[0] = w;
            fbi.strides[1] = w;
            fbi.format = AV_PIX_FMT_NV12;
            break;

        default:
            fbi.format = AV_PIX_FMT_NONE;
            break;
    }

    return fbi;
}

SwsFormatConverter::SwsFormatConverter(const CameraFrameInfoVtbl& ops) : mOps(&ops) {}

int SwsFormatConverter::fillCFB(const ClientFrameBuffer& cfb,
                                const void* frame,
                                const bool backFacing) {
    const FrameBufferInfo fbi = getFrameBufferInfo(cfb);
    if (fbi.format == AV_PIX_FMT_NONE) {
        const uint32_t fmt = cfb.pixel_format;
        derror("Unexpected format: %c%c%c%c", (fmt & 0xFF), ((fmt >> 8) & 0xFF),
               ((fmt >> 16) & 0xFF), ((fmt >> 24) & 0xFF));
        return -1;
    }

    SwsContext* swsCtx = getSwsContext(
            mOps->getWidth(frame), mOps->getHeight(frame),
            static_cast<AVPixelFormat>(mOps->getAVPixelFormat(frame)),
            cfb.width, cfb.height, fbi.format);
    if (swsCtx) {
        ::sws_scale(swsCtx, mOps->getSlice(frame), mOps->getStride(frame), 0,
                    mOps->getHeight(frame), fbi.planes, fbi.strides);
        return 0;
    } else {
        derror("Could not allocate SwsContext for src={ %dx%d, fmt=%d }, "
               "dst={ %dx%d, fmt=%d }",
               mOps->getWidth(frame), mOps->getHeight(frame),
               mOps->getAVPixelFormat(frame), cfb.width, cfb.height,
               fbi.format);
        return -1;
    }
}

SwsContext* SwsFormatConverter::getSwsContext(const int srcWidth,
                                              const int srcHeight,
                                              const AVPixelFormat srcFmt,
                                              const int dstWidth,
                                              const int dstHeight,
                                              const AVPixelFormat dstFmt) {
    ConversionKey conv = {
            .srcWidth = srcWidth,
            .srcHeight = srcHeight,
            .srcFmt = srcFmt,
            .dstWidth = dstWidth,
            .dstHeight = dstHeight,
            .dstFmt = dstFmt,
    };

    const auto i = std::find_if(
            mConverterCache.begin(), mConverterCache.end(),
            [&conv](const std::pair<ConversionKey, SwsContextPtr>& kv) {
                return conv == kv.first;
            });
    if (i != mConverterCache.end()) {
        return i->second.get();
    }

    SwsContext* ctx = ::sws_getContext(srcWidth, srcHeight, srcFmt, dstWidth,
                                       dstHeight, dstFmt, SWS_FAST_BILINEAR,
                                       nullptr, nullptr, nullptr);
    if (ctx) {
        mConverterCache.push_back({std::move(conv), SwsContextPtr(ctx)});
    }

    return ctx;
}

void SwsFormatConverter::ClearConverterCache() {
    mConverterCache.clear();
}
