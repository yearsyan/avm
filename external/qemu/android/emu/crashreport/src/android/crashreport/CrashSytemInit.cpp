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
#include "android/crashreport/CrashConsent.h"
#include "android/crashreport/CrashReporter.h"

#include "aemu/base/logging/Log.h"

namespace android {
namespace crashreport {

bool inject_consent_provider(CrashConsent* myProvider) {
    delete myProvider;
    return CrashReporter::get()->initialize();
}

}  // namespace crashreport
}  // namespace android

extern "C" {

bool crashhandler_init(int argc, char** argv) {
    (void)argc;
    (void)argv;

    const auto reporter = android::crashreport::CrashReporter::get();
    if (!reporter->initialize()) {
        return false;
    }

    reporter->hangDetector().pause(true);
    dinfo("Crashpad reporting disabled in AEMU core-only build.");
    return true;
}

void upload_crashes(void) {}

}
