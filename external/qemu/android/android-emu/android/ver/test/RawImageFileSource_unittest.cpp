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

#include "aemu/base/files/PathUtils.h"
#include "aemu/base/files/ScopedStdioFile.h"
#include "android/base/system/System.h"
#include "android/base/testing/TestTempDir.h"
#include "android/utils/file_io.h"
#include "android/ver/src/raw_image_sources/image_file/raw_image_file_source.h"

#include <gtest/gtest.h>
#include <fstream>
#include <iomanip>

using android::base::PathUtils;
using android::base::ScopedStdioFile;
using android::base::System;
using android::base::TestTempDir;

namespace android {
namespace ver {

// Allowed average squared difference
static constexpr double kCompareThreshold = 0.125;
static constexpr size_t kMaxVectorOutput = 128;

struct ImageTestParam {
    std::string filename;
    std::string goldenFilename;

    ImageTestParam(const char* filename, const char* goldenFilename)
        : filename(filename), goldenFilename(goldenFilename) {}
};

static void PrintTo(const ImageTestParam& param, std::ostream* os) {
    *os << "ImageTestParam(file=" << param.filename
        << ", golden=" << param.goldenFilename << ")";
}

struct ImageResult {
    std::vector<uint8_t> buffer;
    uint32_t width = 0;
    uint32_t height = 0;
};

static void PrintTo(const std::vector<uint8_t>& vec, std::ostream* os) {
    std::ios::fmtflags origFlags(os->flags());

    *os << '{';
    size_t count = 0;
    for (uint8_t val : vec) {
        if (count > 0) {
            *os << ", ";
        }

        if (count == kMaxVectorOutput) {
            *os << "... ";
            break;
        }

        if ((count % 16) == 0) {
            *os << "\n";
        }

        if (val == 0) {
            *os << "    ";
        } else {
            *os << "0x" << std::hex << std::uppercase << std::setw(2)
                << std::setfill('0') << static_cast<int>(val) << std::dec;
        }
    }

    *os << '}';

    os->flags(origFlags);
}

class RawImageFileSourceTest : public ::testing::Test {
protected:
    void SetUp() override {
        mTempDir.reset(new TestTempDir("raw_image_file_source_test"));
    }

    void createTempFile(const std::string& name,
                        const std::vector<uint8_t>& contents,
                        std::string* path) {
        *path = PathUtils::join(mTempDir->path(), name);
        std::ofstream ofs(*path, std::ios::binary);
        ASSERT_TRUE(ofs.is_open()) << "Failed to create temp file: " << *path;
        ofs.write(reinterpret_cast<const char*>(contents.data()),
                  contents.size());
        ofs.close();
    }

    std::unique_ptr<TestTempDir> mTempDir;
};

TEST_F(RawImageFileSourceTest, NonExistentFile) {
    std::string path = PathUtils::join(mTempDir->path(), "non_existent.png");
    auto source = RawImageFileSource::Create(path);
    EXPECT_EQ(source, nullptr);
}

TEST_F(RawImageFileSourceTest, EmptyPNGFile) {
    std::string path;
    createTempFile("empty.png", {}, &path);
    auto source = RawImageFileSource::Create(path);
    EXPECT_EQ(source, nullptr);
}

TEST_F(RawImageFileSourceTest, TruncatedPNGFile) {
    // PNG signature but nothing else
    std::string path;
    createTempFile("truncated.png",
                   {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}, &path);
    auto source = RawImageFileSource::Create(path);
    EXPECT_EQ(source, nullptr);
}

TEST_F(RawImageFileSourceTest, CorruptPNGFile) {
    // Correct signature but garbage data
    std::vector<uint8_t> data = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    for (int i = 0; i < 100; ++i)
        data.push_back(static_cast<uint8_t>(i));
    std::string path;
    createTempFile("corrupt.png", data, &path);
    auto source = RawImageFileSource::Create(path);
    EXPECT_EQ(source, nullptr);
}

TEST_F(RawImageFileSourceTest, EmptyJPEGFile) {
    std::string path;
    createTempFile("empty.jpg", {}, &path);
    auto source = RawImageFileSource::Create(path);
    EXPECT_EQ(source, nullptr);
}

TEST_F(RawImageFileSourceTest, TruncatedJPEGFile) {
    // JPEG SOI marker (FF D8) but nothing else
    std::string path;
    createTempFile("truncated.jpg", {0xFF, 0xD8}, &path);
    auto source = RawImageFileSource::Create(path);
    EXPECT_EQ(source, nullptr);
}

TEST_F(RawImageFileSourceTest, CorruptJPEGFile) {
    // JPEG SOI marker followed by garbage
    std::vector<uint8_t> data = {0xFF, 0xD8};
    for (int i = 0; i < 100; ++i)
        data.push_back(static_cast<uint8_t>(i));
    std::string path;
    createTempFile("corrupt.jpg", data, &path);
    auto source = RawImageFileSource::Create(path);
    EXPECT_EQ(source, nullptr);
}

TEST_F(RawImageFileSourceTest, UnsupportedExtension) {
    std::string path;
    createTempFile("test.txt", {'a', 'b', 'c'}, &path);
    auto source = RawImageFileSource::Create(path);
    EXPECT_EQ(source, nullptr);
}

TEST_F(RawImageFileSourceTest, NonImageWithValidExtension) {
    // It has a .jpg extension but is just a text file
    std::string path;
    createTempFile("not_a_jpeg.jpg", {'h', 'e', 'l', 'l', 'o'}, &path);
    auto source = RawImageFileSource::Create(path);
    EXPECT_EQ(source, nullptr);
}

static uint32_t readUint32LE(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

static uint16_t readUint16LE(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

static int32_t readInt32LE(const uint8_t* data) {
    return static_cast<int32_t>(readUint32LE(data));
}

static void loadGoldenBmp(std::string_view filename, ImageResult* result) {
    const std::string path = PathUtils::join(
            System::get()->getProgramDirectory(), "testdata", filename.data());

    constexpr size_t kBmpHeaderSize = 54;

    ScopedStdioFile fp(android_fopen(path.c_str(), "rb"));
    ASSERT_TRUE(fp) << "Failed to open golden image: " << path;

    uint8_t header[kBmpHeaderSize];
    ASSERT_EQ(kBmpHeaderSize, fread(header, 1, sizeof(header), fp.get()));
    ASSERT_EQ('B', header[0]);
    ASSERT_EQ('M', header[1]);

    uint32_t dataPos = readUint32LE(&header[0x0A]);
    uint32_t imageSize = readUint32LE(&header[0x22]);
    const uint16_t bitsPerPixel = readUint16LE(&header[0x1C]);
    int32_t width = readInt32LE(&header[0x12]);
    int32_t height = readInt32LE(&header[0x16]);

    if (height < 0) {
        height = -height;
    }

    if (imageSize == 0) {
        imageSize = width * height * (bitsPerPixel / 8);
    }

    if (dataPos < kBmpHeaderSize) {
        dataPos = kBmpHeaderSize;
    }

    ASSERT_TRUE(bitsPerPixel == 24 || bitsPerPixel == 32)
            << "Invalid bits per pixel: " << bitsPerPixel << " for "
            << filename;

    result->width = width;
    result->height = height;

    std::vector<uint8_t> rawData(imageSize);
    ASSERT_EQ(0, fseek(fp.get(), dataPos, SEEK_SET));
    ASSERT_EQ(imageSize, fread(rawData.data(), 1, imageSize, fp.get()));
    fp.reset();

    const size_t bytesPerPixel = bitsPerPixel / 8;
    const size_t stride = (width * bytesPerPixel + 3) / 4 * 4;

    result->buffer.resize(width * height * 4);

    // BMP is bottom-up, but RawImageFileSource is top-down.
    // Swizzle from BGR(A) to RGBA and flip vertically.
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = rawData.data() + (height - 1 - y) * stride;
        uint8_t* dstRow = result->buffer.data() + y * width * 4;

        for (int x = 0; x < width; ++x) {
            const uint8_t* srcPixel = srcRow + x * bytesPerPixel;
            uint8_t* dstPixel = dstRow + x * 4;

            if (bitsPerPixel == 32) {
                // The 32-bit BMPs in this project appear to be stored as ABGR.
                // Swizzle ABGR to RGBA.
                dstPixel[0] = srcPixel[3];  // R
                dstPixel[1] = srcPixel[2];  // G
                dstPixel[2] = srcPixel[1];  // B
                dstPixel[3] = srcPixel[0];  // A
            } else {
                // BGR to RGBA
                dstPixel[0] = srcPixel[2];  // R
                dstPixel[1] = srcPixel[1];  // G
                dstPixel[2] = srcPixel[0];  // B
                dstPixel[3] = 0xFF;         // A
            }
        }
    }
}

static void compareSumOfSquaredDifferences(const std::vector<uint8_t>& image,
                                           const std::vector<uint8_t>& golden,
                                           double threshold) {
    ASSERT_EQ(golden.size(), image.size());

    double sum = 0.0;
    for (size_t i = 0; i < image.size(); i += 4) {
        for (size_t j = 0; j < 3; ++j) {
            // Premultiply by alpha to ignore differences in transparent pixels.
            double val =
                    static_cast<double>(image[i + j]) * image[i + 3] / 255.0;
            double goldVal =
                    static_cast<double>(golden[i + j]) * golden[i + 3] / 255.0;
            double diff = val - goldVal;
            sum += diff * diff;
        }
        // Also compare alpha itself.
        double alphaDiff = static_cast<double>(image[i + 3]) - golden[i + 3];
        sum += alphaDiff * alphaDiff;
    }

    if (sum > threshold * image.size()) {
        EXPECT_LE(sum, threshold * image.size());
        // Fall back to standard EXPECT_EQ for better error output.
        ASSERT_EQ(golden, image);
    }
}

class RawImageFileSourceCompareTest
    : public ::testing::TestWithParam<ImageTestParam> {
protected:
    static std::string testdataPathToAbsolute(std::string_view filename) {
        return PathUtils::join(System::get()->getProgramDirectory(), "testdata",
                               filename.data());
    }
};

TEST_P(RawImageFileSourceCompareTest, Compare) {
    const ImageTestParam param = GetParam();
    std::string path = testdataPathToAbsolute(param.filename);
    auto source = RawImageFileSource::Create(path);
    ASSERT_NE(source, nullptr) << "Failed to load image: " << path;

    EXPECT_EQ(0, source->Start(VerImageFormat::RGBA8, 0, 0));

    std::vector<uint8_t> buffer;
    uint32_t image_width = 0;
    uint32_t image_height = 0;
    auto updater = [&](const RawImageBufferView* view) {
        buffer.assign(view->buffer, view->buffer + view->buffer_size);
        image_width = view->width;
        image_height = view->height;
        return absl::OkStatus();
    };

    auto tokenOrError = source->UpdateImage(0, std::nullopt, updater);
    ASSERT_TRUE(tokenOrError.ok());
    ASSERT_TRUE(tokenOrError.value().has_value());

    EXPECT_EQ(0, source->Stop());

    ImageResult golden;
    loadGoldenBmp(param.goldenFilename, &golden);

    ASSERT_EQ(golden.width, image_width);
    ASSERT_EQ(golden.height, image_height);

    compareSumOfSquaredDifferences(buffer, golden.buffer, kCompareThreshold);
}

INSTANTIATE_TEST_SUITE_P(
        RawImageFileSource,
        RawImageFileSourceCompareTest,
        ::testing::Values(
                ImageTestParam("gray_alpha.png", "gray_alpha_golden.bmp"),
                ImageTestParam("gray.png", "gray_golden.bmp"),
                ImageTestParam("indexed_alpha.png", "indexed_alpha_golden.bmp"),
                ImageTestParam("indexed.png", "indexed_golden.bmp"),
                ImageTestParam("interlaced.png", "interlaced_golden.bmp"),
                ImageTestParam("jpeg_gray.jpg", "jpeg_gray_golden.bmp"),
                ImageTestParam("jpeg_gray_progressive.jpg",
                               "jpeg_gray_progressive_golden.bmp"),
                ImageTestParam("jpeg_rgb24.jpg", "jpeg_rgb24_golden.bmp"),
                ImageTestParam("jpeg_rgb24_progressive.jpg",
                               "jpeg_rgb24_progressive_golden.bmp"),
                ImageTestParam("rgb24_31px.png", "rgb24_31px_golden.bmp"),
                ImageTestParam("rgba32.png", "rgba32_golden.bmp")));

}  // namespace ver
}  // namespace android
