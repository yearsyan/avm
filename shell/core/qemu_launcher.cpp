// SPDX-License-Identifier: MIT
//
// qemu launcher + teardown. Moved verbatim from the original single-file
// MacMu.mm. Pure C++.

#include "qemu_launcher.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "macmu_input_protocol.h"

extern char** environ;

namespace {

// Stable child fd used only for the frame doorbell. Keep it away from stdio and
// the low-numbered fds qemu may inherit from its launcher.
constexpr int kChildFrameDoorbellFd = 198;
constexpr const char* kFrameDoorbellFdEnv = "MACMU_FRAME_DOORBELL_FD";
constexpr const char* kLegacyFrameDoorbellFdEnv = "AEMU_FRAME_DOORBELL_FD";bool has_key(const std::vector<std::pair<std::string, std::string>>& overrides,
             const std::string& key) {
    for (const auto& override : overrides) {
        if (override.first == key) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> make_environment(
    const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::vector<std::string> environment;
    for (char** current = environ; current && *current; ++current) {
        std::string entry(*current);
        const size_t equals = entry.find('=');
        const std::string key = equals == std::string::npos ? entry : entry.substr(0, equals);
        if (!has_key(overrides, key)) {
            environment.push_back(std::move(entry));
        }
    }
    for (const auto& override : overrides) {
        environment.push_back(override.first + "=" + override.second);
    }
    return environment;
}

std::vector<char*> make_cstring_vector(std::vector<std::string>& values) {
    std::vector<char*> pointers;
    pointers.reserve(values.size() + 1);
    for (std::string& value : values) {
        pointers.push_back(value.data());
    }
    pointers.push_back(nullptr);
    return pointers;
}

bool set_close_on_exec(int fd, bool enabled) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return false;
    }
    const int nextFlags = enabled ? (flags | FD_CLOEXEC) : (flags & ~FD_CLOEXEC);
    return fcntl(fd, F_SETFD, nextFlags) == 0;
}

}  // namespace

pid_t launch_qemu(const ShellOptions& options, int frameDoorbellFd, int inputFd) {
    std::vector<std::string> args = {
        options.qemuPath,
        "-avd",
        options.avdName,
        "-no-window",
        "-no-audio",
        "-no-snapshot",
        "-no-boot-anim",
        "-gpu",
        "host",
    };
    // When the caller supplies an explicit system-images path, forward it to
    // qemu as -sysdir. This lets qemu resolve the AVD's image search path
    // directly and removes any need for ANDROID_SDK_ROOT / ANDROID_HOME.
    if (!options.systemPath.empty()) {
        args.push_back("-sysdir");
        args.push_back(options.systemPath);
    }
    if (options.wipeData) {
        args.push_back("-wipe-data");
    }
    if (!options.guestRpcSocketPath.empty()) {
        args.push_back("-unix-pipe");
        args.push_back(options.guestRpcSocketPath);
    }
    std::vector<char*> argv = make_cstring_vector(args);

    std::vector<std::pair<std::string, std::string>> overrides = {
        {"MACMU_IOSURFACE_EXPORT", "1"},
        {"AEMU_IOSURFACE_EXPORT", "1"},
        {"ANDROID_EMULATOR_LAUNCHER_DIR", options.launcherDir},
        {"ANDROID_EMULATOR_WRAPPER_PID", std::to_string(getpid())},
        {"MACMU_GUEST_RPC_SOCKET", options.guestRpcSocketPath},
        {"MACMU_GUEST_AGENT_IMAGE", options.guestAgentImagePath},
        {"MACMU_GUEST_RAMDISK", options.guestRamdiskPath},
        {"MACMU_GUEST_RAMDISK_OVERLAY", options.guestRamdiskOverlayPath},
        {"DYLD_LIBRARY_PATH", options.dyldLibraryPath},
        {kFrameDoorbellFdEnv,
         frameDoorbellFd >= 0 ? std::to_string(kChildFrameDoorbellFd) : ""},
        {kLegacyFrameDoorbellFdEnv,
         frameDoorbellFd >= 0 ? std::to_string(kChildFrameDoorbellFd) : ""},
        {macmu::kInputFdEnv, inputFd >= 0 ? std::to_string(macmu::kInputChildFd) : ""},
        {macmu::kInputSocketEnv, options.inputSocketPath},
        {"LC_ALL", "C"},
        {"MESA_RGB_VISUAL", "TrueColor 24"},
        {"SWIFT_BACKTRACE", "enable=no"},
    };
    std::vector<std::string> environment = make_environment(overrides);
    std::vector<char*> envp = make_cstring_vector(environment);

    posix_spawn_file_actions_t fileActions;
    posix_spawn_file_actions_t* fileActionsPtr = nullptr;
    bool restoreFrameDoorbellCloexec = false;
    bool restoreInputCloexec = false;

    auto ensure_file_actions = [&]() -> bool {
        if (fileActionsPtr) {
            return true;
        }
        if (posix_spawn_file_actions_init(&fileActions) != 0) {
            return false;
        }
        fileActionsPtr = &fileActions;
        return true;
    };
    auto add_dup2 = [&](int parentFd, int childFd) -> bool {
        int actionResult =
            posix_spawn_file_actions_adddup2(&fileActions, parentFd, childFd);
        if (actionResult == 0 && parentFd != childFd) {
            actionResult = posix_spawn_file_actions_addclose(&fileActions, parentFd);
        }
        if (actionResult != 0) {
            std::fprintf(stderr, "Failed to prepare fd %d inheritance: %s\n", parentFd,
                         std::strerror(actionResult));
            return false;
        }
        return true;
    };

    if (frameDoorbellFd >= 0) {
        if (frameDoorbellFd == kChildFrameDoorbellFd) {
            if (!set_close_on_exec(frameDoorbellFd, false)) {
                std::fprintf(stderr, "Failed to prepare frame doorbell fd %d: %s\n",
                             frameDoorbellFd, std::strerror(errno));
                return -1;
            }
            restoreFrameDoorbellCloexec = true;
        } else {
            if (!ensure_file_actions()) {
                std::fprintf(stderr, "Failed to initialize qemu spawn file actions\n");
                return -1;
            }
            if (!add_dup2(frameDoorbellFd, kChildFrameDoorbellFd)) {
                posix_spawn_file_actions_destroy(&fileActions);
                return -1;
            }
        }
    }

    if (inputFd >= 0) {
        if (inputFd == macmu::kInputChildFd) {
            if (!set_close_on_exec(inputFd, false)) {
                std::fprintf(stderr, "Failed to prepare input fd %d: %s\n", inputFd,
                             std::strerror(errno));
                if (restoreFrameDoorbellCloexec) {
                    set_close_on_exec(frameDoorbellFd, true);
                }
                if (fileActionsPtr) {
                    posix_spawn_file_actions_destroy(&fileActions);
                }
                return -1;
            }
            restoreInputCloexec = true;
        } else {
            if (!ensure_file_actions()) {
                std::fprintf(stderr, "Failed to initialize qemu spawn file actions\n");
                if (restoreFrameDoorbellCloexec) {
                    set_close_on_exec(frameDoorbellFd, true);
                }
                return -1;
            }
            if (!add_dup2(inputFd, macmu::kInputChildFd)) {
                posix_spawn_file_actions_destroy(&fileActions);
                return -1;
            }
        }
    }

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);

    pid_t pid = -1;
    const int result = posix_spawn(&pid, options.qemuPath.c_str(), fileActionsPtr, &attributes,
                                   argv.data(), envp.data());
    posix_spawnattr_destroy(&attributes);
    if (fileActionsPtr) {
        posix_spawn_file_actions_destroy(&fileActions);
    }
    if (restoreFrameDoorbellCloexec) {
        set_close_on_exec(frameDoorbellFd, true);
    }
    if (restoreInputCloexec) {
        set_close_on_exec(inputFd, true);
    }
    if (result != 0) {
        std::fprintf(stderr, "Failed to launch qemu-system-aarch64-headless: %s\n",
                     std::strerror(result));
        return -1;
    }
    return pid;
}

void terminate_qemu(pid_t pid) {
    if (pid <= 0) {
        return;
    }

    if (waitpid(pid, nullptr, WNOHANG) == pid) {
        return;
    }

    if (kill(-pid, SIGTERM) != 0) {
        kill(pid, SIGTERM);
    }
    for (int i = 0; i < 50; ++i) {
        if (waitpid(pid, nullptr, WNOHANG) == pid) {
            return;
        }
        usleep(100000);
    }

    if (kill(-pid, SIGKILL) != 0) {
        kill(pid, SIGKILL);
    }
    waitpid(pid, nullptr, 0);
}
