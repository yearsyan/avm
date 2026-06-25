// Copyright (C) 2024 The Android Open Source Project
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


// Wraps the studio_stats.pb.h header to prevent compilation errors on Windows.
// It undefines macros (e.g., WAIT_FAILED, WAIT_TIMEOUT) that conflict with
// the Windows SDK. This is necessary because the emulator and Android Studio
// share the same source .proto file, but the conflicting symbols are not
// needed in the emulator.

#pragma once

#ifdef _WIN32
#undef ERROR_FILE_NOT_FOUND
#undef WAIT_FAILED
#undef WAIT_TIMEOUT
#undef WINDOWS
#endif  // _WIN32

#include "studio_stats.pb.h"
