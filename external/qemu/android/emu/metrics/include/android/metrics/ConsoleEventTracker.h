// Copyright 2024 The Android Open Source Project
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

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "aemu/base/Compiler.h"
#include "android/metrics/export.h"
#include "android/metrics/studio_stats_wrapper.pb.h"

namespace android {
namespace metrics {

// A ConsoleEventTracker can be used to track console command usage.
// Events will be batched and submitted in a single EMULATOR_CONSOLE_EVENTS event
// when the metrics system shuts down.
class AEMU_METRICS_API ConsoleEventTracker
    : public std::enable_shared_from_this<ConsoleEventTracker> {
public:
    ConsoleEventTracker(::android_studio::EmulatorConsoleEvent_Source source);

    void increment(const std::string& command, int amount = 1);
    std::vector<::android_studio::EmulatorConsoleEvent> currentEvents();

private:
    ::android_studio::EmulatorConsoleEvent_Source mSource;
    std::unordered_map<std::string, int64_t> mCommandCounter;
    std::mutex mLock;
    bool mRegistered;
};

// This class is used to batch individual ConsoleEventTrackers.
class AEMU_METRICS_API MultipleConsoleEventsTracker {
public:
    DISALLOW_COPY_ASSIGN_AND_MOVE(MultipleConsoleEventsTracker);
    MultipleConsoleEventsTracker() = default;
    ~MultipleConsoleEventsTracker() = default;

    static MultipleConsoleEventsTracker& get();
    void registerTracker(std::shared_ptr<ConsoleEventTracker> tracker);
    void clearForTest();

private:
    std::mutex mCallbackLock;
    std::vector<std::shared_ptr<ConsoleEventTracker>> mTrackers;
};

}  // namespace metrics
}  // namespace android
