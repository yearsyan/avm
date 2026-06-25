/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "TextureUtils.h"
#include <string>
#include <string_view>
#include <csetjmp>
#include <cstdint>
#include "aemu/base/Log.h"
#include "aemu/base/files/PathUtils.h"
#include "aemu/base/files/ScopedStdioFile.h"
#include "android/utils/file_io.h"

#ifdef _MSC_VER
#include "msvc-posix.h"
#endif

extern "C" {
#include <jpeglib.h>
}

#define E(...) derror(__VA_ARGS__)
#define W(...) dwarning(__VA_ARGS__)
#define D(...) dprint(__VA_ARGS__)

using android::base::PathUtils;
using android::base::ScopedStdioFile;

namespace android {
namespace ver {

static constexpr uint32_t kPlaceholderWidth = 1;
static constexpr uint32_t kPlaceholderHeight = 1;

template <typename T>
static inline T alignRowBytes(T value) {
    return (value + 3) / 4 * 4;
}

TextureUtils::Result TextureUtils::createEmpty(uint32_t width,
                                               uint32_t height) {
    Result result;
    result.mWidth = width;
    result.mHeight = height;
    result.mFormat = Format::RGBA32;
    return result;
}

TextureUtils::Result TextureUtils::createPlaceholder() {
    Result result;
    result.mBuffer.resize(alignRowBytes(kPlaceholderWidth * 4) *
                          kPlaceholderHeight);
    result.mWidth = kPlaceholderWidth;
    result.mHeight = kPlaceholderHeight;
    result.mFormat = Format::RGBA32;
    return result;
}

std::optional<TextureUtils::Result> TextureUtils::load(const char* filename,
                                                  Orientation orientation) {
    const std::string filename_str{filename};
    const std::string_view extension{PathUtils::extension(filename_str)};

    if (strncasecmp(extension.data(), ".png", extension.size()) == 0) {
        return loadPNG(filename, orientation);
    } else if (strncasecmp(extension.data(), ".jpg", extension.size()) == 0 ||
               strncasecmp(extension.data(), ".jpeg", extension.size()) == 0) {
        return loadJPEG(filename, orientation);
    } else {
        E("%s: Unsupported file format %s", __FUNCTION__,
          base::c_str(extension).get());
        return {};
    }
}

std::optional<TextureUtils::Result> TextureUtils::loadPNG(const char* filename,
                                                     Orientation orientation) {
    (void)filename;
    (void)orientation;
    E("%s: PNG loading is disabled in this build", __FUNCTION__);
    return {};
}

struct ErrorManager {
    struct jpeg_error_mgr pub;  // Public fields.
    jmp_buf setjmp_buffer;
};

std::optional<TextureUtils::Result> TextureUtils::loadJPEG(const char* filename,
                                                      Orientation orientation) {
    ScopedStdioFile fp(android_fopen(filename, "rb"));
    if (!fp) {
        E("%s: Failed to open file %s", __FUNCTION__, filename);
        return {};
    }

    jpeg_decompress_struct cinfo;
    ErrorManager jerr;
    std::vector<uint8_t> data;

    // RAII wrapper for cinfo.
    std::unique_ptr<jpeg_decompress_struct, decltype(&jpeg_destroy_decompress)>
            cinfo_guard(&cinfo, &jpeg_destroy_decompress);

    // Set up normal error routines, then override error_exit to avoid exit()
    // on failure.
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = [](j_common_ptr cinfoPtr) {
        ErrorManager* err = reinterpret_cast<ErrorManager*>(cinfoPtr->err);
        longjmp(err->setjmp_buffer, 1);
    };

    if (setjmp(jerr.setjmp_buffer)) {
        E("%s: JPEG library error", __FUNCTION__);
        // Prints the message.
        (cinfo.err->output_message)(
                reinterpret_cast<jpeg_common_struct*>(&cinfo));
        return {};
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp.get());

    // We can safely ignore the return value since we're using a stdio source
    // and passing require_image as true.
    (void)jpeg_read_header(&cinfo, /* require_image */ TRUE);
    cinfo.out_color_space = JCS_RGB;  // Force RGB format.
    (void)jpeg_start_decompress(&cinfo);

    const uint32_t width = cinfo.output_width;
    const uint32_t height = cinfo.output_height;
    D("%s: Loaded JPEG %s, %dx%d", __FUNCTION__, filename, width, height);

    if (cinfo.output_components != 3) {
        E("%s: Unsupported output_components %d, should be 3.", __FUNCTION__,
          cinfo.output_components);
        return {};
    }

    const size_t rowBytes = width * cinfo.output_components;
    const size_t stride = alignRowBytes(rowBytes);
    data.resize(stride * height);

    while (cinfo.output_scanline < height) {
        // libjpeg can read multiple scanlines at a time, but on high-quality
        // decompression it is typically one at a time.  Only read one at a time
        // for simplicity.
        uint8_t* rowPtrs[1];
        if (orientation == Orientation::BottomUp) {
            rowPtrs[0] =
                    data.data() + (height - cinfo.output_scanline - 1) * stride;
        } else {
            rowPtrs[0] = data.data() + cinfo.output_scanline * stride;
        }
        (void)jpeg_read_scanlines(&cinfo, rowPtrs, 1);
    }

    jpeg_finish_decompress(&cinfo);

    Result result;
    result.mBuffer = std::move(data);
    result.mWidth = width;
    result.mHeight = height;
    result.mFormat = Format::RGB24;
    return result;
}

}  // namespace ver
}  // namespace android
