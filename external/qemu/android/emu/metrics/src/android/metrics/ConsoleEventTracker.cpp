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

#include "aemu/base/memory/LazyInstance.h"
#include "android/metrics/MetricsReporter.h"

namespace android {
namespace metrics {

ConsoleEventTracker::ConsoleEventTracker(
        ::android_studio::EmulatorConsoleEvent_Source source)
    : mSource(source), mRegistered(false) {}

void ConsoleEventTracker::increment(const std::string& command, int amount) {
    const std::lock_guard<std::mutex> lock(mLock);
    if (!mRegistered) {
        MultipleConsoleEventsTracker::get().registerTracker(shared_from_this());
        mRegistered = true;
    }
    mCommandCounter[command] += amount;
}

std::vector<::android_studio::EmulatorConsoleEvent>
ConsoleEventTracker::currentEvents() {
    const std::lock_guard<std::mutex> lock(mLock);
    std::vector<::android_studio::EmulatorConsoleEvent> events;
    for (const auto& [command, count] : mCommandCounter) {
        if (count > 0) {
            ::android_studio::EmulatorConsoleEvent event;
            event.set_command(command);
            event.set_source(mSource);
            event.set_value(count);
            events.push_back(event);
        }
    }
    return events;
}

static base::LazyInstance<MultipleConsoleEventsTracker> sTrackerInstance = {};

MultipleConsoleEventsTracker& MultipleConsoleEventsTracker::get() {
    return sTrackerInstance.get();
}

void MultipleConsoleEventsTracker::registerTracker(
        std::shared_ptr<ConsoleEventTracker> tracker) {
    const std::lock_guard<std::mutex> lock(mCallbackLock);

    if (mTrackers.empty()) {
        MetricsReporter::get().reportOnExit(
                [=](android_studio::AndroidStudioEvent* event) {
                    const std::lock_guard<std::mutex> lock(mCallbackLock);
                    for (auto const& t : mTrackers) {
                        for (auto const& currentEvt : t->currentEvents()) {
                            auto newEvent =
                                    event->add_emulator_console_events();
                            *newEvent = currentEvt;
                        }
                    }

                    event->set_kind(android_studio::AndroidStudioEvent::
                                            EMULATOR_CONSOLE_EVENTS);
                });
    }
    mTrackers.push_back(tracker);
}

void MultipleConsoleEventsTracker::clearForTest() {
    const std::lock_guard<std::mutex> lock(mCallbackLock);
    mTrackers.clear();
}

}  // namespace metrics
}  // namespace android
