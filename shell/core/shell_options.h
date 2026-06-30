// SPDX-License-Identifier: MIT
//
// Parsed shell options + argv/environment resolver. Pure C++.

#ifndef MACMU_SHELL_OPTIONS_H
#define MACMU_SHELL_OPTIONS_H

#include <string>

#include "shell_constants.h"

struct ShellOptions {
    std::string launcherDir;
    std::string qemuPath;
    std::string dyldLibraryPath;
    std::string inputSocketPath;
    // Explicit path to the system-images directory, forwarded to qemu as
    // -sysdir. When set, the shell no longer needs ANDROID_SDK_ROOT /
    // ANDROID_HOME: qemu resolves the AVD's image search path from this
    // directory directly. Empty means "not provided".
    std::string systemPath;
    // Host Unix domain socket used by the guest MacMu RPC agent through
    // virtio-vsock's pipe:unix transport.
    std::string guestRpcSocketPath;
    // Build-time ext4/GPT image carrying the guest MacMu RPC agent and init rc.
    // Empty means no guest agent injection.
    std::string guestAgentImagePath;
    // Optional debug-only initramfs overlay carrying the guest MacMu RPC agent
    // and init rc. Empty means no initramfs overlay injection.
    std::string guestRamdiskOverlayPath;
    // Build-time ramdisk image generated from the target system image's base
    // ramdisk plus the MacMu fstab overlay.
    std::string guestRamdiskPath;
    std::string avdName = macmu::kDefaultAvdName;
    bool wipeData = false;
    bool openDisplay = false;
};

// Resolves command-line flags layered on top of MACMU_*/AEMU_SHELL_*/legacy
// environment variables and executable-relative defaults.
ShellOptions parse_options(int argc, char** argv);

#endif  // MACMU_SHELL_OPTIONS_H
