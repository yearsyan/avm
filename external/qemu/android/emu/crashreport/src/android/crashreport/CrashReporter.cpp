// Copyright 2015 The Android Open Source Project
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
#include "android/crashreport/CrashReporter.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "aemu/base/files/PathUtils.h"
#include "aemu/base/memory/LazyInstance.h"
#include "android/base/system/System.h"
#include "android/utils/debug.h"

using android::base::LazyInstance;
using android::base::PathUtils;
using android::base::System;

namespace android {
namespace crashreport {

const constexpr char kCrashDataDir[] = "emu-crash-core-only";
const constexpr char kCrashOnExitFileName[] = "crash-on-exit";

LazyInstance<CrashReporter> sCrashReporter = LAZY_INSTANCE_INIT;

CrashReporter::CrashReporter() = default;

std::string CrashReporter::databaseDirectory() {
    return android::base::pj(System::get()->getTempDir(), kCrashDataDir);
}

bool CrashReporter::initialize() {
    mDumpDir = databaseDirectory();
    mInitialized = true;
    return true;
}

const std::string& CrashReporter::getDumpDir() const {
    return mDumpDir;
}

CrashReporter* CrashReporter::get() {
    return sCrashReporter.ptr();
}

void CrashReporter::destroy() {}

void CrashReporter::AppendDump(const char* message) {
    if (message) {
        mLastDumpMessage.append(message);
    }
}

void CrashReporter::GenerateDump(const char* message) {
    AppendDump(message);
}

ANDROID_NORETURN void CrashReporter::GenerateDumpAndDie(const char* message) {
    if (message) {
        derror("Fatal emulator error: %s", message);
        AppendDump(message);
    }
    abort();
}

void CrashReporter::SetExitMode(const char* message) {
    if (mHangDetector) {
        mHangDetector->stop();
    }
    attachData(kCrashOnExitFileName, message ? message : "", true);
}

void CrashReporter::passDumpMessage(const char* message) {
    AppendDump(message);
}

void CrashReporter::attachData(std::string name,
                               std::string data,
                               bool replace) {
    if (name.empty()) {
        name = "attachment";
    }

    if (replace || !mAttachments.count(name)) {
        mAttachments[std::move(name)] = std::move(data);
        return;
    }

    mAttachments[name].append(data);
}

void CrashReporter::uploadEntries() {}

HangDetector& CrashReporter::hangDetector() {
    if (!mHangDetector) {
        mHangDetector = std::make_unique<HangDetector>([](auto message) {
            const std::string text(message);
            CrashReporter::get()->GenerateDumpAndDie(text.c_str());
        });
    }

    return *mHangDetector;
}

}  // namespace crashreport
}  // namespace android

using android::crashreport::CrashReporter;

extern "C" {

void crashhandler_append_message(const char* message) {
    const auto reporter = CrashReporter::get();
    if (reporter && reporter->active()) {
        reporter->AppendDump(message);
    }
}

void crashhandler_append_message_format_v(const char* format, va_list args) {
    char message[2048] = {};
    vsnprintf(message, sizeof(message) - 1, format, args);
    crashhandler_append_message(message);
}

void crashhandler_append_message_format(const char* format, ...) {
    va_list args;
    va_start(args, format);
    crashhandler_append_message_format_v(format, args);
    va_end(args);
}

ANDROID_NORETURN void crashhandler_die(const char* message) {
    const auto reporter = CrashReporter::get();
    if (reporter && reporter->active()) {
        reporter->GenerateDumpAndDie(message);
    }

    derror("Emulator: exiting because of the internal error '%s'",
           message ? message : "");
    _exit(1);
}

ANDROID_NORETURN void crashhandler_die_format_v(const char* format,
                                                va_list args) {
    char message[2048] = {};
    vsnprintf(message, sizeof(message) - 1, format, args);
    crashhandler_die(message);
}

ANDROID_NORETURN void crashhandler_die_format(const char* format, ...) {
    char message[2048] = {};
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message) - 1, format, args);
    va_end(args);
    crashhandler_die(message);
}

void crashhandler_add_string(const char* name, const char* string) {
    const auto reporter = CrashReporter::get();
    if (reporter && reporter->active()) {
        reporter->attachData(name ? name : "", string ? string : "");
    }
}

void crashhandler_add_string_format_v(const char* name,
                                      const char* format,
                                      va_list args) {
    char message[2048] = {};
    vsnprintf(message, sizeof(message) - 1, format, args);
    crashhandler_add_string(name, message);
}

void crashhandler_add_string_format(const char* name, const char* format, ...) {
    va_list args;
    va_start(args, format);
    crashhandler_add_string_format_v(name, format, args);
    va_end(args);
}

void crashhandler_exitmode(const char* message) {
    const auto reporter = CrashReporter::get();
    if (reporter && reporter->active()) {
        reporter->SetExitMode(message);
    }
}

bool crashhandler_copy_attachment(const char* destination, const char* source) {
    const auto reporter = CrashReporter::get();
    if (!(reporter && reporter->active()) || !source) {
        return false;
    }

    std::ifstream sourceFile(PathUtils::asUnicodePath(source).c_str(),
                             std::ios::binary);
    std::stringstream buffer;
    buffer << sourceFile.rdbuf();

    if (sourceFile.bad()) {
        return false;
    }

    reporter->attachData(destination ? destination : "", buffer.str(), true);
    return true;
}

}  // extern "C"
