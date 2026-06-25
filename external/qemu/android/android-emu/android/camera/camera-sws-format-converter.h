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

#pragma once

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

typedef struct CameraFrameInfoVtbl {
    int (*getWidth)(const void* data);
    int (*getHeight)(const void* data);
    int (*getAVPixelFormat)(const void* data);
    const uint8_t* const* (*getSlice)(const void* data);
    const int* (*getStride)(const void* data);
} CameraFrameInfoVtbl;

struct SwsContextDeleter {
    void operator()(SwsContext* p) const { ::sws_freeContext(p); }
};

using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

struct FrameBufferInfo {
    uint8_t* planes[3];
    int strides[3];
    AVPixelFormat format;
};

FrameBufferInfo getFrameBufferInfo(const ClientFrameBuffer& cfb);

struct SwsFormatConverter {
    explicit SwsFormatConverter(const CameraFrameInfoVtbl& ops);

    int fillCFB(const ClientFrameBuffer& cfb,
                const void* frame,
                const bool backFacing);
    void ClearConverterCache();

private:
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

    SwsContext* getSwsContext(const int srcWidth,
                              const int srcHeight,
                              const AVPixelFormat srcFmt,
                              const int dstWidth,
                              const int dstHeight,
                              const AVPixelFormat dstFmt);

    std::vector<std::pair<ConversionKey, SwsContextPtr>> mConverterCache;
    const CameraFrameInfoVtbl* mOps;
};
