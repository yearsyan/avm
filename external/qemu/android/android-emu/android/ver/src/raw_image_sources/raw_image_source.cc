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
#include "raw_image_source.h"
#include <cstdint>
#include <optional>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace android {
namespace ver {

SolidColorImageSource::SolidColorImageSource()
    : SolidColorImageSource({0, 0, 0}) {}

SolidColorImageSource::SolidColorImageSource(Color c)
    : buffer_({image_, sizeof(image_), VerImageFormat::RGBA8, 1, 1}) {
    image_[0] = c.r;
    image_[1] = c.g;
    image_[2] = c.b;
    image_[3] = 0xff;
}

int SolidColorImageSource::Start(VerImageFormat pixel_format, int width, int height) {
    return 0;
}

absl::StatusOr<std::optional<RawImageToken>> SolidColorImageSource::UpdateImage(
        int64_t target_time_us,
        std::optional<RawImageToken> token,
        std::function<absl::Status(const RawImageBufferView*)> updater) {
    if (token.has_value() && token.value().token == 1) {
        return std::nullopt;
    }
    absl::Status ret = updater(&buffer_);
    if (ret.ok()) {
        return RawImageToken{1};
    } else {
        return ret;
    }
}

int SolidColorImageSource::Stop() {
    return 0;
}

}  // namespace ver
}  // namespace android