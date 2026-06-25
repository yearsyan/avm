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
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "../raw_image_source.h"

namespace android {
namespace ver {

struct ImageData {
    unsigned int width;
    unsigned int height;
    int num_components;
    int line_size;
    std::vector<unsigned char> data;
    uint8_t* data_ptr;

    ImageData() = default;
    ImageData(ImageData&& other) noexcept
        : width(other.width),
          height(other.height),
          num_components(other.num_components),
          line_size(other.line_size),
          data(std::move(other.data)) {
        data_ptr = &data[0];
        other.data_ptr = nullptr;
        other.width = 0;
        other.height = 0;
    }
    ImageData& operator=(ImageData&& other) noexcept {
        if (this != &other) {
            width = other.width;
            height = other.height;
            num_components = other.num_components;
            line_size = other.line_size;
            data = std::move(other.data);
            data_ptr = &data[0];

            other.data_ptr = nullptr;
            other.width = 0;
            other.height = 0;
        }
        return *this;
    }
};

class RawImageFileSource : public RawImageSource {
public:
    static std::unique_ptr<RawImageFileSource> Create(std::string filename);
    int Start(VerImageFormat pixel_format, int width, int height) override;
    absl::StatusOr<std::optional<RawImageToken>> UpdateImage(
            int64_t target_time_us,
            std::optional<RawImageToken> token,
            std::function<absl::Status(const RawImageBufferView*)> updater)
            override;
    int Stop() override;

private:
    explicit RawImageFileSource(std::string file, ImageData&& image);
    std::string file_;
    ImageData image_;
};

}  // namespace ver
}  // namespace android
