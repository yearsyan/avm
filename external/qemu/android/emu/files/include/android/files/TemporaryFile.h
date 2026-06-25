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
#pragma once

#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"

#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include "aemu/base/system/Win32UnicodeString.h"
#else
#include <stdlib.h>
#include <unistd.h>
#endif

namespace android {
namespace files {

/**
 * @brief A RAII class for creating and automatically deleting a temporary file.
 *
 * This class creates a temporary file upon construction and deletes it upon
 * destruction. It is platform-independent and handles Unicode paths on
 * Windows.
 */
class TemporaryFile {
public:
    /**
     * @brief Constructs a TemporaryFile object.
     *
     * Creates a temporary file in the system's temporary directory. The
     * filename will start with the given prefix.
     *
     * @param prefix The prefix for the temporary filename. Defaults to
     * "tempfile".
     */
    TemporaryFile(const std::string& prefix = "tempfile") {
#ifdef _WIN32
        WCHAR tempPath[MAX_PATH];
        DWORD ret = GetTempPathW(MAX_PATH, tempPath);
        if (ret > MAX_PATH || ret == 0) {
            // Failed to get temp path
            return;
        }

        WCHAR tempFileName[MAX_PATH];
        android::base::Win32UnicodeString wprefix(prefix);
        UINT unique =
                GetTempFileNameW(tempPath, wprefix.c_str(), 0, tempFileName);
        if (unique == 0) {
            // Failed to create temp file
            return;
        }
        mPath = android::base::Win32UnicodeString::convertToUtf8(tempFileName);
#else
        std::string temp_dir = android::base::System::get()->getTempDir();
        mPath = android::base::PathUtils::join(temp_dir, prefix + "-XXXXXX");

        // mkstemp needs a writable C string.
        std::vector<char> path_vec(mPath.begin(), mPath.end());
        path_vec.push_back('\0');

        int fd = mkstemp(path_vec.data());
        if (fd != -1) {
            close(fd);
            mPath.assign(path_vec.data());
        } else {
            mPath.clear();
        }
#endif
    }

    /**
     * @brief Destroys the TemporaryFile object.
     *
     * The temporary file is deleted from the filesystem.
     */
    ~TemporaryFile() {
        if (!mPath.empty()) {
#ifdef _WIN32
            android::base::Win32UnicodeString wpath(mPath);
            DeleteFileW(wpath.c_str());
#else
            ::unlink(mPath.c_str());
#endif
        }
    }

    /**
     * @brief Gets the path of the temporary file.
     *
     * @return A const reference to the path of the temporary file. The path
     * will be an empty string if the file could not be created.
     */
    const std::string& path() const { return mPath; }

    /**
     * @brief Checks if the temporary file was created successfully.
     *
     * @return true if the temporary file is valid, false otherwise.
     */
    bool valid() const { return !mPath.empty(); }

private:
    DISALLOW_COPY_AND_ASSIGN(TemporaryFile);
    std::string mPath;
};

}  // namespace files
}  // namespace android