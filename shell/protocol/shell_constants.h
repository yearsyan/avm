// SPDX-License-Identifier: MIT
//
// Compile-time default paths/names shared between option parsing and the qemu
// launcher. Plain C++ header.

#ifndef MACMU_SHELL_CONSTANTS_H
#define MACMU_SHELL_CONSTANTS_H

namespace macmu {

inline constexpr const char* kQemuHeadlessRelativePath =
    "qemu/darwin-aarch64/qemu-system-aarch64-headless";
inline constexpr const char* kDefaultAvdName = "aemu_aosp35_arm64";

}  // namespace macmu

#endif  // MACMU_SHELL_CONSTANTS_H
