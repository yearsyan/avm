// Copyright 2022 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace android {
namespace crashreport {

template <std::size_t MaxSize>
class SimpleStringAnnotation {
public:
    SimpleStringAnnotation(const SimpleStringAnnotation&) = delete;
    SimpleStringAnnotation& operator=(const SimpleStringAnnotation&) = delete;

    SimpleStringAnnotation(std::string name, std::string msg)
        : mName(std::move(name)) {
        const auto dataSize = std::min<std::size_t>(msg.size(), MaxSize);
        mBuffer.assign(msg.begin(), msg.begin() + dataSize);
    }

private:
    std::string mName;
    std::vector<char> mBuffer;
};
}  // namespace crashreport
}  // namespace android
