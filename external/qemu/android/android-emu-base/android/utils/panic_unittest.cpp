// Copyright 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may not use a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "android/utils/panic.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdarg.h>
#include <stdio.h>
#include "aemu/base/logging/Log.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

void redirectStdoutToStdErr() {
#ifndef _WIN32
    dup2(fileno(stderr), fileno(stdout));
#else
    _dup2(_fileno(stderr), _fileno(stdout));
#endif
}

// We expect to exit, upon a panic.
TEST(Panic, DefaultHandlerTerminates) {
#ifdef _WIN32
    GTEST_SKIP() << "Expect exit is not well supported on windows.";
#endif
    base_configure_logs(LoggingFlags::kLogDefaultOptions);
    EXPECT_EXIT(android_panic("Test panic message: %d", 42),
                ::testing::ExitedWithCode(1), "");
}

// We expect an exit that will be displayed to the user.
// Note: that stderr is buffered so if you add more tests that write
// to stderr, you will likely start to see failures in this test.
TEST(Panic, DefaultHandlerTerminesWithAMessage) {
#ifdef _WIN32
    GTEST_SKIP() << "Expect exit is not well supported on windows.";
#endif
            base_configure_logs(LoggingFlags::kLogDefaultOptions);
    fflush(stdout);
    fflush(stderr);
    EXPECT_EXIT(redirectStdoutToStdErr();
                android_panic("Test panic message: %d", 42),
                ::testing::ExitedWithCode(1),
                ".*FATAL        | Test panic message: 42.*");
}
