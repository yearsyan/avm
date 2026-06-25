// Copyright 2025 The Android Open Source Project
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

#include "android/files/TemporaryFile.h"

#include "android/base/system/System.h"

#include <gtest/gtest.h>

#include <string>

namespace android {
namespace files {

TEST(TemporaryFile, Basic) {
    std::string path;
    {
        TemporaryFile tf;
        EXPECT_TRUE(tf.valid());
        path = tf.path();
        EXPECT_TRUE(android::base::System::get()->pathExists(path));
    }
    EXPECT_FALSE(android::base::System::get()->pathExists(path));
}

TEST(TemporaryFile, Prefix) {
    TemporaryFile tf("my-prefix");
    EXPECT_TRUE(tf.valid());
}

}  // namespace files
}  // namespace android
