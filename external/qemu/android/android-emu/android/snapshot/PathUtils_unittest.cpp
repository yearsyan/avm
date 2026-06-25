// Copyright (C) 2026 The Android Open Source Project
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

#include "android/snapshot/PathUtils.h"

#include "aemu/base/files/PathUtils.h"
#include "android/avd/info.h"
#include "android/console.h"
#include <gtest/gtest.h>
#include <string>

namespace android {
namespace snapshot {

class PathUtilsTest : public ::testing::Test {
protected:
    void SetUp() override {
        mAvdInfo = avdInfo_new_for_testing(AVD_PHONE);
        getConsoleAgents()->settings->inject_AvdInfo(mAvdInfo);
    }

    void TearDown() override {
        getConsoleAgents()->settings->inject_AvdInfo(nullptr);
        if (mAvdInfo) {
            avdInfo_free(mAvdInfo);
        }
    }

    AvdInfo* mAvdInfo = nullptr;
};

TEST_F(PathUtilsTest, getSnapshotDir_SafeNames) {
    std::string baseDir = getSnapshotBaseDir();

    // Normal safe names should just be joined
    EXPECT_EQ(base::PathUtils::join(baseDir, "normal_name"), getSnapshotDir("normal_name"));
    EXPECT_EQ(base::PathUtils::join(baseDir, "normal-name.1"), getSnapshotDir("normal-name.1"));
    EXPECT_EQ(base::PathUtils::join(baseDir, "boot_1"), getSnapshotDir("boot_1"));
}

TEST_F(PathUtilsTest, getSnapshotDir_Unicode) {
    std::string baseDir = getSnapshotBaseDir();

    // Unicode characters should be preserved (not sanitized to underscores)
    EXPECT_EQ(base::PathUtils::join(baseDir, "快照"), getSnapshotDir("快照"));
    EXPECT_EQ(base::PathUtils::join(baseDir, "März"), getSnapshotDir("März"));
    EXPECT_EQ(base::PathUtils::join(baseDir, "snap_快照"), getSnapshotDir("snap_快照"));
}

TEST_F(PathUtilsTest, getSnapshotDir_PathTraversal) {
    std::string baseDir = getSnapshotBaseDir();

    // Path traversal characters should be replaced with '_'
    EXPECT_EQ(base::PathUtils::join(baseDir, ".._.._.._etc_passwd"), getSnapshotDir("../../../etc/passwd"));
    EXPECT_EQ(base::PathUtils::join(baseDir, "_absolute_path"), getSnapshotDir("/absolute/path"));
    EXPECT_EQ(base::PathUtils::join(baseDir, ".._.._win.ini"), getSnapshotDir("..\\..\\win.ini"));
}

TEST_F(PathUtilsTest, getSnapshotDir_ShellCharacters) {
    std::string baseDir = getSnapshotBaseDir();

    // Shell special characters should be replaced with '_'
    EXPECT_EQ(base::PathUtils::join(baseDir, "snap_calc"), getSnapshotDir("snap&calc"));
    EXPECT_EQ(base::PathUtils::join(baseDir, "snap_dir"), getSnapshotDir("snap|dir"));
    EXPECT_EQ(base::PathUtils::join(baseDir, "snap_echo"), getSnapshotDir("snap;echo"));
    EXPECT_EQ(base::PathUtils::join(baseDir, "snap_test"), getSnapshotDir("snap*test"));
    EXPECT_EQ(base::PathUtils::join(baseDir, "snap_space_test"), getSnapshotDir("snap space test"));
}

TEST_F(PathUtilsTest, getSnapshotDir_SpecialCases) {
    std::string baseDir = getSnapshotBaseDir();

    // Empty, "." and ".." should be replaced with "_"
    EXPECT_EQ(base::PathUtils::join(baseDir, "_"), getSnapshotDir(""));
    EXPECT_EQ(base::PathUtils::join(baseDir, "_"), getSnapshotDir(nullptr));
    EXPECT_EQ(base::PathUtils::join(baseDir, "_"), getSnapshotDir("."));
    EXPECT_EQ(base::PathUtils::join(baseDir, "_"), getSnapshotDir(".."));
}

}  // namespace snapshot
}  // namespace android
