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

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include "raw_image_source.h"
#include "ver/virtual_environment_renderer_types.h"

namespace android {
namespace ver {

struct RawImageBufferViewFourCC {
    const uint8_t* buffer;
    size_t buffer_size;
    uint32_t pixel_format;
    int width;
    int height;
};

std::string FourccToString(uint32_t fourcc);
VerImageFormat FourccToVerImageFormat(uint32_t fourcc);
uint32_t VerImageFormatToFourcc(VerImageFormat ver_image_format);
RawImageBufferView RawImageBufferViewFourCCBridge(
        RawImageBufferViewFourCC* fourcc);

// Converts the provided buffer to RGB32
// If conversion is successful, buffer will be updated to point at
// conversion_storage. conversion_storage and staging_buffer will
// be resized if necessary.
int ConvertBufferToRGB32(RawImageBufferViewFourCC& buffer,
                         std::vector<uint8_t>& conversion_storage,
                         std::vector<uint8_t>& staging_buffer);

}  // namespace ver
}  // namespace android
