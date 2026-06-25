/*
* Copyright (C) 2017 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#pragma once

#include "aemu/base/containers/SmallVector.h"
#include "aemu/base/export.h"
#include "aemu/base/files/StdioStream.h"
#include "aemu/base/synchronization/Lock.h"
#include "android/base/system/System.h"
#include "aemu/base/threads/Thread.h"
#include "host-common/snapshot_common.h"
#include "render-utils/snapshot_operations.h"

#include <functional>
#include <memory>
#include <unordered_map>

namespace android {
namespace snapshot {

class TextureLoader final : public gfxstream::ITextureLoader {
public:
    AEMU_EXPORT TextureLoader(android::base::StdioStream&& stream);

    AEMU_EXPORT bool start() override;
    AEMU_EXPORT void loadTexture(uint32_t texId, const loader_t& loader) override;

    AEMU_EXPORT bool hasError() const { return mHasError; }
    AEMU_EXPORT uint64_t diskSize() const { return mDiskSize; }
    AEMU_EXPORT bool compressed() const { return mVersion > 1; }

    AEMU_EXPORT void join() override {
        gfxstream::ITextureLoader::join();

        mStream.close();
        mEndTime = base::System::get()->getHighResTimeUs();
    }

    AEMU_EXPORT void interrupt() override {
        gfxstream::ITextureLoader::interrupt();

        mStream.close();
        mEndTime = base::System::get()->getHighResTimeUs();
    }

    // getDuration():
    // Returns true if there was save with measurable time
    // (and writes it to |duration| if |duration| is not null),
    // otherwise returns false.
    AEMU_EXPORT bool getDuration(uint64_t* duration) {
        if (mEndTime < mStartTime) {
            return false;
        }

        if (duration) {
            *duration = mEndTime - mStartTime;
        }
        return true;
    }

private:
    bool readIndex();

    android::base::StdioStream mStream;
    std::unordered_map<uint32_t, int64_t> mIndex;
    android::base::Lock mLock;
    bool mStarted = false;
    bool mHasError = false;
    int mVersion = 0;
    uint64_t mDiskSize = 0;
    base::System::Duration mStartTime = 0;
    base::System::Duration mEndTime = 0;
};

}  // namespace snapshot
}  // namespace android
