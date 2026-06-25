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
#include "raw_image_file_source.h"

#include <csetjmp>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include "absl/status/status.h"

// jpeglib.h needs to be included. It's a C library.
extern "C" {
#include <jpeglib.h>
}

#include "aemu/base/Log.h"
#include "aemu/base/files/PathUtils.h"
#include "aemu/base/files/ScopedStdioFile.h"
#include "android/utils/file_io.h"

using android::base::PathUtils;
using android::base::ScopedStdioFile;

namespace android {
namespace ver {

std::optional<ImageData> loadPNGImage(std::string& filename) {
    derror("PNG loading is disabled in this build: %s", filename.c_str());
    return std::nullopt;
}

struct JpegErrorManager {
    struct jpeg_error_mgr pub;  // "public" fields
    jmp_buf setjmp_buffer;      // for return to caller
};

static void jpeg_error_exit(j_common_ptr cinfoPtr) {
    JpegErrorManager* myerr =
            reinterpret_cast<JpegErrorManager*>(cinfoPtr->err);
    longjmp(myerr->setjmp_buffer, 1);
}

std::optional<ImageData> loadJPEGImage(std::string& filename) {
    struct jpeg_decompress_struct cinfo;
    JpegErrorManager jerr;

    // All cpp classes must be declared before the setjmp to ensure thier
    // deconstructors are called.
    ImageData img;
    ScopedStdioFile fp(android_fopen(filename.c_str(), "rb"));
    if (!fp) {
        derror("Failed to open file %s", filename.c_str());
        return std::nullopt;
    }

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    std::unique_ptr<jpeg_decompress_struct, decltype(&jpeg_destroy_decompress)>
            cinfo_guard(&cinfo, &jpeg_destroy_decompress);

    if (setjmp(jerr.setjmp_buffer)) {
        char buffer[JMSG_LENGTH_MAX];
        (*cinfo.err->format_message)(
                reinterpret_cast<jpeg_common_struct*>(&cinfo), buffer);
        derror("JPEG library error for %s: %s", filename.c_str(), buffer);
        return std::nullopt;
    }

    jpeg_create_decompress(&cinfo);

    jpeg_stdio_src(&cinfo, fp.get());
    (void)jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGBA_8888;  // Force RGBA format.
    (void)jpeg_start_decompress(&cinfo);

    img.width = cinfo.output_width;
    img.height = cinfo.output_height;
    img.num_components = cinfo.output_components;
    img.line_size = img.width * img.num_components;

    img.data.resize(img.width * img.height * img.num_components);

    while (cinfo.output_scanline < cinfo.output_height) {
        // buffer is a pointer to the start of the current row in our vector
        unsigned char* buffer =
                &img.data[cinfo.output_scanline * img.line_size];
        (void)jpeg_read_scanlines(&cinfo, &buffer, 1);
    }

    (void)jpeg_finish_decompress(&cinfo);
    img.data_ptr = &img.data[0];
    return img;
}

std::optional<ImageData> loadImageFromFile(std::string& filename) {
    const std::string filename_str{filename};
    const std::string_view extension{PathUtils::extension(filename_str)};

    if (strncasecmp(extension.data(), ".png", extension.size()) == 0) {
        return loadPNGImage(filename);
    } else if (strncasecmp(extension.data(), ".jpg", extension.size()) == 0 ||
               strncasecmp(extension.data(), ".jpeg", extension.size()) == 0) {
        return loadJPEGImage(filename);
    } else {
        derror("Unsupported file format %s",
               android::base::c_str(extension).get());
        return std::nullopt;
    }
}

std::unique_ptr<RawImageFileSource> RawImageFileSource::Create(
        std::string filename) {
    std::optional<ImageData> maybeImage = loadImageFromFile(filename);
    if (maybeImage) {
        return std::unique_ptr<RawImageFileSource>(
                new RawImageFileSource(filename, std::move(maybeImage.value())));
    }
    return nullptr;
}

RawImageFileSource::RawImageFileSource(std::string filename, ImageData&& image)
    : file_(std::move(filename)), image_(std::move(image)) {}

int RawImageFileSource::Start(VerImageFormat pixel_format,
                                     int width,
                                     int height) {
    return 0;
}

absl::StatusOr<std::optional<RawImageToken>> RawImageFileSource::UpdateImage(
        int64_t target_time_us,
        std::optional<RawImageToken> token,
        std::function<absl::Status(const RawImageBufferView*)> updater) {
    if (token.has_value() && token.value().token == 1) {
        return std::nullopt;
    }
    size_t buffer_size;
    VerImageFormat pixel_format;
    int width;
    int height;
    struct RawImageBufferView im = {
            image_.data_ptr,
            static_cast<size_t>(image_.line_size) * image_.height,
            VerImageFormat::RGBA8, image_.width, image_.height};
    absl::Status ret = updater(&im);
    if (ret.ok()) {
        return RawImageToken{1};
    } else {
        return ret;
    }
}

int RawImageFileSource::Stop() {
    return 0;
}

}  // namespace ver
}  // namespace android
