// SPDX-License-Identifier: MIT
//
// Option/environment/path resolution for the MacMu shell. Moved verbatim from
// the original single-file MacMu.mm; the anonymous-namespace helpers below are
// file-local and only ShellOptions/parse_options are exported.

#include "shell_options.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <mach-o/dyld.h>
#include <string>
#include <unistd.h>

#include "macmu_input_protocol.h"

namespace {

std::string env_or_default(const char* name, const std::string& fallback) {
    if (const char* value = std::getenv(name)) {
        if (value[0] != '\0') {
            return value;
        }
    }
    return fallback;
}

std::string env_or_default(const char* name, const char* legacy_name,
                           const std::string& fallback) {
    return env_or_default(name, env_or_default(legacy_name, fallback));
}

std::string path_join(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs.back() == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

std::string directory_name(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return ".";
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

std::string base_name(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

bool file_exists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

std::string current_working_directory() {
    char path[PATH_MAX];
    if (!getcwd(path, sizeof(path))) {
        return {};
    }
    return path;
}

std::string executable_directory() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return {};
    }
    path.resize(std::strlen(path.c_str()));
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved)) {
        path = resolved;
    }
    return directory_name(path);
}

std::string default_launcher_dir() {
    const std::string executable_dir = executable_directory();
    if (!executable_dir.empty() &&
        file_exists(path_join(executable_dir, macmu::kQemuHeadlessRelativePath))) {
        return executable_dir;
    }

    if (base_name(executable_dir) == "MacOS") {
        const std::string contents_dir = directory_name(executable_dir);
        if (base_name(contents_dir) == "Contents") {
            const std::string bundled_dist = path_join(contents_dir, "Resources/emulator");
            if (file_exists(path_join(bundled_dist, macmu::kQemuHeadlessRelativePath))) {
                return bundled_dist;
            }
        }
    }

    if (!executable_dir.empty()) {
        const std::string sibling_dist =
            path_join(directory_name(executable_dir), "distribution/emulator");
        if (file_exists(path_join(sibling_dist, macmu::kQemuHeadlessRelativePath))) {
            return sibling_dist;
        }
    }

    const std::string cwd = current_working_directory();
    if (!cwd.empty()) {
        const std::string repo_dist = path_join(cwd, "build/cmake/distribution/emulator");
        if (file_exists(path_join(repo_dist, macmu::kQemuHeadlessRelativePath))) {
            return repo_dist;
        }
    }

    return executable_dir.empty() ? "." : executable_dir;
}

std::string default_qemu_path(const std::string& launcher_dir) {
    return path_join(launcher_dir, macmu::kQemuHeadlessRelativePath);
}

std::string default_dyld_library_path(const std::string& launcher_dir) {
    const std::string lib64 = path_join(launcher_dir, "lib64");
    return lib64 + ":" + path_join(lib64, "gles_angle") + ":" + path_join(lib64, "vulkan");
}

std::string default_guest_ramdisk_overlay_path(const std::string& launcher_dir) {
    (void)launcher_dir;
    return {};
}

std::string default_guest_ramdisk_path(const std::string& launcher_dir) {
    const std::string path = path_join(launcher_dir, "lib/macmu-ramdisk.img");
    return file_exists(path) ? path : std::string();
}

std::string default_guest_agent_image_path(const std::string& launcher_dir) {
    const std::string path = path_join(launcher_dir, "lib/macmu-agent.img");
    return file_exists(path) ? path : std::string();
}

std::string default_guest_rpc_socket_path() {
    return "/tmp/macmu.rpc." + std::to_string(static_cast<unsigned>(getpid())) + ".sock";
}

}  // namespace

ShellOptions parse_options(int argc, char** argv) {
    ShellOptions options;
    options.launcherDir =
        env_or_default("MACMU_LAUNCHER_DIR", "AEMU_SHELL_LAUNCHER_DIR", default_launcher_dir());
    options.qemuPath =
        env_or_default("MACMU_QEMU_PATH", "AEMU_SHELL_QEMU_PATH",
                       default_qemu_path(options.launcherDir));
    options.dyldLibraryPath =
        env_or_default("MACMU_DYLD_LIBRARY_PATH", "AEMU_SHELL_DYLD_LIBRARY_PATH",
                       default_dyld_library_path(options.launcherDir));
    options.inputSocketPath =
        env_or_default(macmu::kInputSocketEnv,
                       "/tmp/macmu.input." + std::to_string(static_cast<unsigned>(getpid())) +
                           ".sock");
    // The system-images path replaces the ANDROID_SDK_ROOT/ANDROID_HOME
    // dependency: it is forwarded to qemu as -sysdir, which lets qemu resolve
    // the AVD's image search path without an SDK root. No default is synthesized
    // (empty == not provided); the caller is expected to pass --system-path or
    // set MACMU_SYSTEM_PATH / AEMU_SHELL_SYSTEM_PATH.
    options.systemPath =
        env_or_default("MACMU_SYSTEM_PATH", "AEMU_SHELL_SYSTEM_PATH", options.systemPath);
    options.guestRpcSocketPath =
        env_or_default("MACMU_GUEST_RPC_SOCKET", "AEMU_SHELL_GUEST_RPC_SOCKET",
                       default_guest_rpc_socket_path());
    options.guestAgentImagePath =
        env_or_default("MACMU_GUEST_AGENT_IMAGE", "AEMU_SHELL_GUEST_AGENT_IMAGE",
                       default_guest_agent_image_path(options.launcherDir));
    options.guestRamdiskOverlayPath =
        env_or_default("MACMU_GUEST_RAMDISK_OVERLAY", "AEMU_SHELL_GUEST_RAMDISK_OVERLAY",
                       default_guest_ramdisk_overlay_path(options.launcherDir));
    options.guestRamdiskPath =
        env_or_default("MACMU_GUEST_RAMDISK", "AEMU_SHELL_GUEST_RAMDISK",
                       default_guest_ramdisk_path(options.launcherDir));
    options.avdName = env_or_default("MACMU_AVD_NAME", "AEMU_SHELL_AVD_NAME", options.avdName);
    bool qemu_path_overridden =
        std::getenv("MACMU_QEMU_PATH") != nullptr ||
        std::getenv("AEMU_SHELL_QEMU_PATH") != nullptr;
    bool dyld_library_path_overridden =
        std::getenv("MACMU_DYLD_LIBRARY_PATH") != nullptr ||
        std::getenv("AEMU_SHELL_DYLD_LIBRARY_PATH") != nullptr;
    bool guest_ramdisk_overlay_overridden =
        std::getenv("MACMU_GUEST_RAMDISK_OVERLAY") != nullptr ||
        std::getenv("AEMU_SHELL_GUEST_RAMDISK_OVERLAY") != nullptr;
    bool guest_ramdisk_overridden =
        std::getenv("MACMU_GUEST_RAMDISK") != nullptr ||
        std::getenv("AEMU_SHELL_GUEST_RAMDISK") != nullptr;
    bool guest_agent_image_overridden =
        std::getenv("MACMU_GUEST_AGENT_IMAGE") != nullptr ||
        std::getenv("AEMU_SHELL_GUEST_AGENT_IMAGE") != nullptr;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const char* option) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %s\n", option);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--launcher-dir") {
            if (const char* value = require_value("--launcher-dir")) {
                options.launcherDir = value;
                if (!qemu_path_overridden) {
                    options.qemuPath = default_qemu_path(options.launcherDir);
                }
                if (!dyld_library_path_overridden) {
                    options.dyldLibraryPath = default_dyld_library_path(options.launcherDir);
                }
                if (!guest_ramdisk_overlay_overridden) {
                    options.guestRamdiskOverlayPath =
                        default_guest_ramdisk_overlay_path(options.launcherDir);
                }
                if (!guest_ramdisk_overridden) {
                    options.guestRamdiskPath = default_guest_ramdisk_path(options.launcherDir);
                }
                if (!guest_agent_image_overridden) {
                    options.guestAgentImagePath =
                        default_guest_agent_image_path(options.launcherDir);
                }
            }
        } else if (arg == "--qemu") {
            if (const char* value = require_value("--qemu")) {
                options.qemuPath = value;
                qemu_path_overridden = true;
            }
        } else if (arg == "--dyld-library-path") {
            if (const char* value = require_value("--dyld-library-path")) {
                options.dyldLibraryPath = value;
                dyld_library_path_overridden = true;
            }
        } else if (arg == "--input-socket") {
            if (const char* value = require_value("--input-socket")) {
                options.inputSocketPath = value;
            }
        } else if (arg == "--system-path") {
            if (const char* value = require_value("--system-path")) {
                options.systemPath = value;
            }
        } else if (arg == "--guest-rpc-socket") {
            if (const char* value = require_value("--guest-rpc-socket")) {
                options.guestRpcSocketPath = value;
            }
        } else if (arg == "--guest-agent-image") {
            if (const char* value = require_value("--guest-agent-image")) {
                options.guestAgentImagePath = value;
                guest_agent_image_overridden = true;
            }
        } else if (arg == "--guest-ramdisk-overlay") {
            if (const char* value = require_value("--guest-ramdisk-overlay")) {
                options.guestRamdiskOverlayPath = value;
                guest_ramdisk_overlay_overridden = true;
            }
        } else if (arg == "--guest-ramdisk") {
            if (const char* value = require_value("--guest-ramdisk")) {
                options.guestRamdiskPath = value;
                guest_ramdisk_overridden = true;
            }
        } else if (arg == "--avd") {
            if (const char* value = require_value("--avd")) {
                options.avdName = value;
            }
        } else if (arg == "--wipe-data") {
            options.wipeData = true;
        } else if (arg == "--open-display") {
            options.openDisplay = true;
        }
    }
    return options;
}
