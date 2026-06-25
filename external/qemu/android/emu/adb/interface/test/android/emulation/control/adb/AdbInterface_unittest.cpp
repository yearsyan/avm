// Copyright (C) 2016 The Android Open Source Project
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

#include "android/emulation/control/adb/AdbInterface.h"
#include "aemu/base/Optional.h"
#include "android/emulation/control/TestAdbInterface.h"

#include "aemu/base/async/Looper.h"
#include "aemu/base/files/PathUtils.h"
#include "android/base/testing/TestSystem.h"
#include "aemu/base/threads/Async.h"
#include "android/emulation/ConfigDirs.h"
#include "android/console.h"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <thread>

using android::base::AsyncThreadWithLooper;
using android::base::Looper;
using android::base::Optional;
using android::base::PathUtils;
using android::base::System;
using android::base::TestSystem;
using android::base::TestTempDir;
using android::emulation::AdbInterface;
using android::emulation::FakeAdbDaemon;
using android::emulation::StaticAdbLocator;

class AdbInterfaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!mAsyncThread)
            mAsyncThread.reset(new AsyncThreadWithLooper);
        mLooper = mAsyncThread->getLooper();
    }

    void TearDown() override {
        mAsyncThread.reset();
        mLooper = nullptr;
    }

    std::unique_ptr<AsyncThreadWithLooper> mAsyncThread;
    Looper* mLooper;
};

class AdbTestSystem : public TestSystem {
public:
    AdbTestSystem(bool success = true)
        : TestSystem("/progdir",
                     System::kProgramBitness,
                     "/homedir",
                     "/appdir"),
          mSuccess(success) {}

    Optional<std::string> runCommandWithResult(
            const std::vector<std::string>& commandLine,
            System::Duration timeoutMs = kInfinite,
            System::ProcessExitCode* outExitCode = nullptr) override {
        mLastCommand = commandLine;
        if (!mSuccess)
            return {};

        return Optional(std::string("Android Debug Bridge version 2.3.41"));
    }

    bool runCommand(const std::vector<std::string>& commandLine,
                    RunOptions options,
                    System::Duration timeoutMs,
                    System::ProcessExitCode* outExitCode,
                    System::Pid* outChildPid,
                    const std::string& outputFilePath) override {
        mLastCommand = commandLine;
        if (outExitCode) *outExitCode = 0;
        return true;
    }

    std::vector<std::string> mLastCommand;

private:
    bool mSuccess;
};

TEST_F(AdbInterfaceTest, create) {
    // Default creation still works..
    EXPECT_NE(nullptr, AdbInterface::create(nullptr).get());
}

TEST_F(AdbInterfaceTest, findAdbFromAndroidSdkRootIgnoreBadExec) {
    // We should not find an ADB executable that does not provide a proper
    // version string. or fails to execute.
    AdbTestSystem system(false);
    TestTempDir* dir = system.getTempRoot();
    ASSERT_TRUE(dir->makeSubDir("Sdk"));
    ASSERT_TRUE(dir->makeSubDir("Sdk/platforms"));
    ASSERT_TRUE(dir->makeSubDir("Sdk/platform-tools"));
    ASSERT_TRUE(
            dir->makeSubFile(PathUtils::join("Sdk", "platform-tools",
                                             PathUtils::toExecutableName("adb"))
                                     .c_str()));
    std::string sdkRoot = PathUtils::join(dir->path(), "Sdk");
    system.envSet("ANDROID_SDK_ROOT", sdkRoot);

    auto adb = AdbInterface::Builder()
                       .setLooper(mLooper)
                       .setAdbDaemon(new FakeAdbDaemon())
                       .build();
    std::string expectedAdbPath = PathUtils::join(
            sdkRoot, "platform-tools", PathUtils::toExecutableName("adb"));
    EXPECT_NE(expectedAdbPath, adb->detectedAdbPath());
}

TEST_F(AdbInterfaceTest, findAdbFromAndroidSdkRoot) {
    AdbTestSystem system;
    TestTempDir* dir = system.getTempRoot();
    ASSERT_TRUE(dir->makeSubDir("Sdk"));
    ASSERT_TRUE(dir->makeSubDir("Sdk/platforms"));
    ASSERT_TRUE(dir->makeSubDir("Sdk/platform-tools"));
    ASSERT_TRUE(
            dir->makeSubFile(PathUtils::join("Sdk", "platform-tools",
                                             PathUtils::toExecutableName("adb"))
                                     .c_str()));
    std::string sdkRoot = PathUtils::join(dir->path(), "Sdk");
    system.envSet("ANDROID_SDK_ROOT", sdkRoot);

    auto adb = AdbInterface::Builder()
                       .setLooper(mLooper)
                       .setAdbDaemon(new FakeAdbDaemon())
                       .build();
    std::string expectedAdbPath = PathUtils::join(
            sdkRoot, "platform-tools", PathUtils::toExecutableName("adb"));
    EXPECT_EQ(expectedAdbPath, adb->detectedAdbPath());
}

TEST_F(AdbInterfaceTest, deriveAdbFromExecutable) {
    AdbTestSystem system;
    TestTempDir* dir = system.getTempRoot();

    ASSERT_TRUE(dir->makeSubDir("Sdk"));
    ASSERT_TRUE(dir->makeSubDir("Sdk/tools"));
    ASSERT_TRUE(dir->makeSubDir("Sdk/platforms"));
    ASSERT_TRUE(dir->makeSubDir("Sdk/platform-tools"));
    ASSERT_TRUE(
            dir->makeSubFile(PathUtils::join("Sdk", "platform-tools",
                                             PathUtils::toExecutableName("adb"))
                                     .c_str()));
    system.envSet("ANDROID_EMULATOR_LAUNCHER_DIR", dir->path());
    std::string sdkRoot = PathUtils::join(dir->path(), "Sdk");
    system.setLauncherDirectory(PathUtils::join(sdkRoot, "tools"));
    EXPECT_EQ(PathUtils::join(sdkRoot, "tools"), system.getLauncherDirectory());
    auto adb = AdbInterface::Builder()
                       .setLooper(mLooper)
                       .setAdbDaemon(new FakeAdbDaemon())
                       .build();
    std::string expectedAdbPath = PathUtils::join(
            sdkRoot, "platform-tools", PathUtils::toExecutableName("adb"));
    EXPECT_EQ(expectedAdbPath, adb->detectedAdbPath());
}

TEST_F(AdbInterfaceTest, findAdbOnPath) {
    AdbTestSystem system;
    TestTempDir* dir = system.getTempRoot();

    ASSERT_TRUE(dir->makeSubDir("some_dir"));
    ASSERT_TRUE(dir->makeSubFile(
            PathUtils::join("some_dir", PathUtils::toExecutableName("adb"))
                    .c_str()));
    std::string execPath = PathUtils::join(dir->path(), "some_dir");
    std::string expectedAdbPath =
            PathUtils::join(execPath, PathUtils::toExecutableName("adb"));
    system.setWhich(expectedAdbPath);
    auto adb = AdbInterface::Builder()
                       .setLooper(mLooper)
                       .setAdbDaemon(new FakeAdbDaemon())
                       .build();
    EXPECT_EQ(expectedAdbPath, adb->detectedAdbPath());
}

TEST_F(AdbInterfaceTest, staleAdbVersion) {
    auto daemon = new FakeAdbDaemon();
    auto locator = new StaticAdbLocator({{"v1", 1}, {"v2", 2}, {"v3", 3}});
    auto adb = AdbInterface::Builder()
                       .setLooper(mLooper)
                       .setAdbLocator(locator)
                       .setAdbDaemon(daemon)
                       .build();

    // We should have selected an ancient adb version as we have no new ones.
    EXPECT_FALSE(adb->isAdbVersionCurrent());
}

TEST_F(AdbInterfaceTest, noStaleAdbVersion) {
    auto daemon = new FakeAdbDaemon();
    auto locator = new StaticAdbLocator({{"v1", 1}, {"v2", 2}, {"v3", 40}});
    auto adb = AdbInterface::Builder()
                       .setLooper(mLooper)
                       .setAdbLocator(locator)
                       .setAdbDaemon(daemon)
                       .build();

    // We should not have selected an ancient adb version
    EXPECT_TRUE(adb->isAdbVersionCurrent());
}

TEST_F(AdbInterfaceTest, recentAdbVersion) {
    auto daemon = new FakeAdbDaemon();
    auto locator = new StaticAdbLocator({{"v1", 1}, {"v2", 2}, {"v3", 40}});
    auto adb = AdbInterface::Builder()
                       .setLooper(mLooper)
                       .setAdbLocator(locator)
                       .setAdbDaemon(daemon)
                       .build();

    // We have selected a modern one
    daemon->setProtocolVersion(android::base::makeOptional(40));
    EXPECT_EQ("v3", adb->adbPath());
    EXPECT_TRUE(adb->isAdbVersionCurrent());
}

TEST_F(AdbInterfaceTest, selectMatchingAdb) {
    auto daemon = new FakeAdbDaemon();
    auto locator = new StaticAdbLocator({{"v1", 1}, {"v2", 2}, {"v3", 3}});
    auto adb = AdbInterface::Builder()
                       .setLooper(mLooper)
                       .setAdbLocator(locator)
                       .setAdbDaemon(daemon)
                       .build();

    // Unknown protocol, we should pick the first.
    daemon->setProtocolVersion({});
    EXPECT_EQ("v1", adb->adbPath());

    daemon->setProtocolVersion(android::base::makeOptional(3));
    EXPECT_EQ("v3", adb->adbPath());

    daemon->setProtocolVersion(android::base::makeOptional(2));
    EXPECT_EQ("v2", adb->adbPath());
}

TEST_F(AdbInterfaceTest, useSerialNumberFromAgent) {
    AdbTestSystem system;
    auto daemon = new FakeAdbDaemon();
    auto locator = new StaticAdbLocator({{"adb", 40}});
    auto adb = AdbInterface::Builder()
                       .setLooper(mLooper)
                       .setAdbLocator(locator)
                       .setAdbDaemon(daemon)
                       .build();

    // Set the serial number port in the agent.
    getConsoleAgents()->settings->set_android_serial_number_port(5556);

    // Run a command.
    auto cmd = adb->runAdbCommand({"shell", "ls"}, [](const android::emulation::OptionalAdbCommandResult&) {}, 5000);

    // Wait for the command to "finish" (it's async).
    int retries = 100;
    while (cmd->inFlight() && retries-- > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Verify that the command used -s emulator-5556
    bool foundSerial = false;
    for (size_t i = 0; i < system.mLastCommand.size(); ++i) {
        if (system.mLastCommand[i] == "-s" &&
            i + 1 < system.mLastCommand.size() &&
            system.mLastCommand[i + 1] == "emulator-5556") {
            foundSerial = true;
            break;
        }
    }
    EXPECT_TRUE(foundSerial);
}
