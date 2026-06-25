// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "aemu/base/files/Stream.h"
#include "render-utils/stream.h"

namespace android {
namespace snapshot {

class GfxstreamStreamAdapter : public gfxstream::Stream {
  public:
    GfxstreamStreamAdapter(android::base::Stream* stream)
        : mStream(stream) {}

    ssize_t read(void* buffer, size_t size) override {
        return mStream->read(buffer, size);
    }

    ssize_t write(const void* buffer, size_t size) override {
        return mStream->write(buffer, size);
    }

  private:
    android::base::Stream* const mStream = nullptr;
};

}  // namespace snapshot
}  // namespace android