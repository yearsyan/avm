// SPDX-License-Identifier: MIT
//
// Compile-time default paths/names shared between option parsing and the qemu
// launcher. Plain C++ header.

#ifndef MACMU_SHELL_CONSTANTS_H
#define MACMU_SHELL_CONSTANTS_H

namespace macmu {

inline constexpr const char* kQemuHeadlessRelativePath =
    "qemu/darwin-aarch64/qemu-system-aarch64-headless";
inline constexpr const char* kDefaultAvdName = "macmu_aosp16_arm64";
inline constexpr const char* kDefaultImageManifestUrl =
    "https://storage.macmu.org/images/aosp16-arm64/manifest.json";

}  // namespace macmu

#endif  // MACMU_SHELL_CONSTANTS_H
