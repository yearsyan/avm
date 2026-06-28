// SPDX-License-Identifier: MIT
//
// Parsed shell options + argv/environment resolver. Pure C++.

#ifndef MACMU_SHELL_OPTIONS_H
#define MACMU_SHELL_OPTIONS_H

#include <string>

#include "shell_constants.h"

struct ShellOptions {
    bool launchQemu = true;
    std::string launcherDir;
    std::string qemuPath;
    std::string dyldLibraryPath;
    std::string inputSocketPath;
    // Explicit path to the system-images directory, forwarded to qemu as
    // -sysdir. When set, the shell no longer needs ANDROID_SDK_ROOT /
    // ANDROID_HOME: qemu resolves the AVD's image search path from this
    // directory directly. Empty means "not provided".
    std::string systemPath;
    std::string avdName = macmu::kDefaultAvdName;
};

// Resolves command-line flags layered on top of MACMU_*/AEMU_SHELL_*/legacy
// environment variables and executable-relative defaults.
ShellOptions parse_options(int argc, char** argv);

#endif  // MACMU_SHELL_OPTIONS_H
