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

#include "android/metrics/ConsoleEventTracker.h"

#include <gtest/gtest.h>
#include <memory>
#include <map>
#include <set>

#include "android/metrics/MetricsReporter.h"
#include "android/metrics/tests/MockMetricsReporter.h"
#include "android/metrics/tests/MockMetricsWriter.h"

using namespace android::metrics;

namespace android {
namespace metrics {
extern void set_unittest_Reporter(MetricsReporter::Ptr newPtr);
}
}  // namespace android

namespace {

static constexpr const char* kVersion = "version";
static constexpr const char* kFullVersion = "fullVersion";
static constexpr const char* kQemuVersion = "qemuVersion";
static constexpr const char* kSessionId = "session";

class ConsoleEventTrackerTest : public ::testing::Test {
public:
    std::shared_ptr<MockMetricsWriter> mWriter{
            std::make_shared<MockMetricsWriter>(kSessionId)};
    MockMetricsReporter* mReporter;

    void SetUp() override {
        MultipleConsoleEventsTracker::get().clearForTest();
        android::metrics::set_unittest_Reporter(
                std::make_unique<MockMetricsReporter>(true, mWriter, kVersion,
                                                      kFullVersion, kQemuVersion));
        mReporter = (MockMetricsReporter*)&MetricsReporter::get();
    }

    void TearDown() override {
        android::metrics::set_unittest_Reporter(nullptr);
        MultipleConsoleEventsTracker::get().clearForTest();
    }
};

TEST_F(ConsoleEventTrackerTest, increments_commands) {
    auto tracker = std::make_shared<ConsoleEventTracker>(
            android_studio::EmulatorConsoleEvent::TELNET);
    tracker->increment("geo");
    tracker->increment("geo");
    tracker->increment("rotate");

    auto events = tracker->currentEvents();
    ASSERT_EQ(events.size(), 2);

    bool foundGeo = false;
    bool foundRotate = false;
    for (const auto& event : events) {
        if (event.command() == "geo") {
            EXPECT_EQ(event.value(), 2);
            EXPECT_EQ(event.source(), android_studio::EmulatorConsoleEvent::TELNET);
            foundGeo = true;
        } else if (event.command() == "rotate") {
            EXPECT_EQ(event.value(), 1);
            EXPECT_EQ(event.source(), android_studio::EmulatorConsoleEvent::TELNET);
            foundRotate = true;
        }
    }
    EXPECT_TRUE(foundGeo);
    EXPECT_TRUE(foundRotate);
}

TEST_F(ConsoleEventTrackerTest, tracks_detailed_subcommands) {
    auto tracker = std::make_shared<ConsoleEventTracker>(
            android_studio::EmulatorConsoleEvent::TELNET);

    // Simulating the resolution logic for subcommands
    tracker->increment("avd status");
    tracker->increment("avd status");
    tracker->increment("avd snapshot load");
    tracker->increment("event mouse");
    tracker->increment("geo");

    auto events = tracker->currentEvents();
    std::map<std::string, int64_t> counts;
    for (const auto& event : events) {
        counts[event.command()] = event.value();
    }

    EXPECT_EQ(counts["avd status"], 2);
    EXPECT_EQ(counts["avd snapshot load"], 1);
    EXPECT_EQ(counts["event mouse"], 1);
    EXPECT_EQ(counts["geo"], 1);
}

TEST_F(ConsoleEventTrackerTest, tracks_command_path_without_arguments) {
    auto tracker = std::make_shared<ConsoleEventTracker>(
            android_studio::EmulatorConsoleEvent::TELNET);

    // In prod, for "avd snapshot load my_snap", we should only report "avd snapshot load".
    // This test ensures the tracker correctly records exactly what it is given.
    std::string command_path = "avd snapshot load";
    tracker->increment(command_path);

    auto events = tracker->currentEvents();
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].command(), "avd snapshot load");
}

TEST_F(ConsoleEventTrackerTest, batches_on_exit) {
    auto tracker = std::make_shared<ConsoleEventTracker>(
            android_studio::EmulatorConsoleEvent::TELNET);

    tracker->increment("geo");
    tracker->increment("rotate");

    bool delivered = false;
    int found = 0;
    mReporter->mOnReportConditional =
            [&delivered, &found](MetricsReporter::ConditionalCallback cb) {
                android_studio::AndroidStudioEvent event;
                if (cb(&event)) {
                    if (event.kind() == android_studio::AndroidStudioEvent::
                                                EMULATOR_CONSOLE_EVENTS) {
                        delivered = true;
                        for (int i = 0; i < event.emulator_console_events_size();
                             ++i) {
                            const auto& ce = event.emulator_console_events(i);
                            if (ce.command() == "geo" || ce.command() == "rotate") {
                                EXPECT_EQ(ce.source(),
                                          android_studio::EmulatorConsoleEvent::
                                                  TELNET);
                                found++;
                            }
                        }
                    }
                }
            };

    MetricsReporter::get().stop(MetricsStopReason::METRICS_STOP_GRACEFUL);
    EXPECT_TRUE(delivered);
    EXPECT_EQ(found, 2);
}

TEST_F(ConsoleEventTrackerTest, handles_multiple_trackers_same_source) {
    auto tracker1 = std::make_shared<ConsoleEventTracker>(
            android_studio::EmulatorConsoleEvent::TELNET);
    auto tracker2 = std::make_shared<ConsoleEventTracker>(
            android_studio::EmulatorConsoleEvent::TELNET);

    tracker1->increment("geo");
    tracker2->increment("rotate");

    bool delivered = false;
    std::set<std::string> commands;
    mReporter->mOnReportConditional =
            [&delivered, &commands](MetricsReporter::ConditionalCallback cb) {
                android_studio::AndroidStudioEvent event;
                if (cb(&event)) {
                    if (event.kind() == android_studio::AndroidStudioEvent::
                                                EMULATOR_CONSOLE_EVENTS) {
                        delivered = true;
                        for (int i = 0; i < event.emulator_console_events_size();
                             ++i) {
                            commands.insert(event.emulator_console_events(i).command());
                        }
                    }
                }
            };

    MetricsReporter::get().stop(MetricsStopReason::METRICS_STOP_GRACEFUL);
    EXPECT_TRUE(delivered);
    EXPECT_TRUE(commands.count("geo"));
    EXPECT_TRUE(commands.count("rotate"));
}

}  // namespace
