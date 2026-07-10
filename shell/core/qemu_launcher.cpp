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
#include <unistd.h>
#include <utility>
#include <vector>

#include "macmu_control_protocol.h"
#include "macmu_input_protocol.h"

extern char** environ;

namespace {

// Stable child fd used only for the frame doorbell. Keep it away from stdio and
// the low-numbered fds qemu may inherit from its launcher.
constexpr int kChildFrameDoorbellFd = 198;
constexpr int kMinTemporarySpawnFd = 256;
constexpr const char* kFrameDoorbellFdEnv = "MACMU_FRAME_DOORBELL_FD";
constexpr const char* kLegacyFrameDoorbellFdEnv = "AEMU_FRAME_DOORBELL_FD";

bool has_key(const std::vector<std::pair<std::string, std::string>>& overrides,
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

}  // namespace

pid_t launch_qemu(const ShellOptions& options, int frameDoorbellFd, int inputFd,
                  int controlFd) {
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
    // When the caller supplies an explicit Android image path, forward it to
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
    if (!options.guestCtrlSocketPath.empty()) {
        args.push_back("-unix-pipe");
        args.push_back(options.guestCtrlSocketPath);
    }
    std::vector<char*> argv = make_cstring_vector(args);

    std::vector<std::pair<std::string, std::string>> overrides = {
        {"MACMU_IOSURFACE_EXPORT", "1"},
        {"AEMU_IOSURFACE_EXPORT", "1"},
        {"ANDROID_EMULATOR_LAUNCHER_DIR", options.launcherDir},
        {"ANDROID_EMULATOR_WRAPPER_PID", std::to_string(getpid())},
        {"ANDROID_AVD_HOME", options.avdHome},
        {"MACMU_APP_DATA_DIR", options.appDataDir},
        {"MACMU_AVD_HOME", options.avdHome},
        {"MACMU_GUEST_RPC_SOCKET", options.guestRpcSocketPath},
        {"MACMU_GUEST_CTRL_SOCKET", options.guestCtrlSocketPath},
        {"MACMU_GUEST_AGENT_IMAGE", options.guestAgentImagePath},
        {"MACMU_GUEST_RAMDISK", options.guestRamdiskPath},
        {"MACMU_GUEST_RAMDISK_OVERLAY", options.guestRamdiskOverlayPath},
        {"DYLD_LIBRARY_PATH", options.dyldLibraryPath},
        {kFrameDoorbellFdEnv,
         frameDoorbellFd >= 0 ? std::to_string(kChildFrameDoorbellFd) : ""},
        {kLegacyFrameDoorbellFdEnv,
         frameDoorbellFd >= 0 ? std::to_string(kChildFrameDoorbellFd) : ""},
        {macmu::kInputFdEnv, inputFd >= 0 ? std::to_string(macmu::kInputChildFd) : ""},
        {macmu::kControlFdEnv, controlFd >= 0 ? std::to_string(macmu::kControlChildFd) : ""},
        {macmu::kInputSocketEnv, options.inputSocketPath},
        {"LC_ALL", "C"},
        {"MESA_RGB_VISUAL", "TrueColor 24"},
        {"SWIFT_BACKTRACE", "enable=no"},
    };
    std::vector<std::string> environment = make_environment(overrides);
    std::vector<char*> envp = make_cstring_vector(environment);

    posix_spawn_file_actions_t fileActions;
    posix_spawn_file_actions_t* fileActionsPtr = nullptr;
    std::vector<int> temporarySpawnFds;

    auto close_temporary_spawn_fds = [&]() {
        for (int fd : temporarySpawnFds) {
            close(fd);
        }
        temporarySpawnFds.clear();
    };

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

    auto duplicate_for_spawn = [&](int parentFd, const char* label) -> int {
        const int dupFd = fcntl(parentFd, F_DUPFD_CLOEXEC, kMinTemporarySpawnFd);
        if (dupFd < 0) {
            std::fprintf(stderr, "Failed to duplicate %s fd %d for qemu spawn: %s\n", label,
                         parentFd, std::strerror(errno));
            return -1;
        }
        temporarySpawnFds.push_back(dupFd);
        return dupFd;
    };

    auto add_inherited_fd = [&](int parentFd, int childFd, const char* label) -> bool {
        if (parentFd < 0) {
            return true;
        }
        if (!ensure_file_actions()) {
            std::fprintf(stderr, "Failed to initialize qemu spawn file actions\n");
            return false;
        }
        // Always duplicate first. This prevents a source fd from colliding with
        // another channel's fixed child fd while file actions are applied.
        const int spawnFd = duplicate_for_spawn(parentFd, label);
        if (spawnFd < 0) {
            return false;
        }
        int actionResult = posix_spawn_file_actions_adddup2(&fileActions, spawnFd, childFd);
        if (actionResult == 0) {
            actionResult = posix_spawn_file_actions_addclose(&fileActions, spawnFd);
        }
        if (actionResult != 0) {
            std::fprintf(stderr, "Failed to prepare %s fd %d -> %d inheritance: %s\n", label,
                         parentFd, childFd, std::strerror(actionResult));
            return false;
        }
        return true;
    };

#if defined(__APPLE__) && defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    // CLOEXEC_DEFAULT is the fd allowlist. Preserve stdio explicitly for qemu
    // diagnostics, then add only the three protocol descriptors below.
    for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
        if (fcntl(fd, F_GETFD) < 0) {
            continue;
        }
        if (!ensure_file_actions()) {
            std::fprintf(stderr, "Failed to initialize qemu spawn file actions\n");
            close_temporary_spawn_fds();
            return -1;
        }
        const int actionResult = posix_spawn_file_actions_addinherit_np(&fileActions, fd);
        if (actionResult != 0) {
            std::fprintf(stderr, "Failed to preserve qemu stdio fd %d: %s\n", fd,
                         std::strerror(actionResult));
            posix_spawn_file_actions_destroy(&fileActions);
            close_temporary_spawn_fds();
            return -1;
        }
    }
#endif

    if (!add_inherited_fd(frameDoorbellFd, kChildFrameDoorbellFd, "frame doorbell")) {
        if (fileActionsPtr) {
            posix_spawn_file_actions_destroy(&fileActions);
        }
        close_temporary_spawn_fds();
        return -1;
    }
    if (!add_inherited_fd(inputFd, macmu::kInputChildFd, "input")) {
        if (fileActionsPtr) {
            posix_spawn_file_actions_destroy(&fileActions);
        }
        close_temporary_spawn_fds();
        return -1;
    }
    if (!add_inherited_fd(controlFd, macmu::kControlChildFd, "control")) {
        if (fileActionsPtr) {
            posix_spawn_file_actions_destroy(&fileActions);
        }
        close_temporary_spawn_fds();
        return -1;
    }

    posix_spawnattr_t attributes;
    int attributeResult = posix_spawnattr_init(&attributes);
    const bool attributesInitialized = attributeResult == 0;
    short spawnFlags = POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    spawnFlags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    if (attributeResult == 0) {
        attributeResult = posix_spawnattr_setflags(&attributes, spawnFlags);
    }
    if (attributeResult == 0) {
        attributeResult = posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (attributeResult != 0) {
        std::fprintf(stderr, "Failed to initialize qemu spawn attributes: %s\n",
                     std::strerror(attributeResult));
        if (fileActionsPtr) {
            posix_spawn_file_actions_destroy(&fileActions);
        }
        if (attributesInitialized) {
            posix_spawnattr_destroy(&attributes);
        }
        close_temporary_spawn_fds();
        return -1;
    }

    pid_t pid = -1;
    const int result = posix_spawn(&pid, options.qemuPath.c_str(), fileActionsPtr, &attributes,
                                   argv.data(), envp.data());
    posix_spawnattr_destroy(&attributes);
    if (fileActionsPtr) {
        posix_spawn_file_actions_destroy(&fileActions);
    }
    close_temporary_spawn_fds();
    if (result != 0) {
        std::fprintf(stderr, "Failed to launch qemu-system-aarch64-headless: %s\n",
                     std::strerror(result));
        return -1;
    }
    return pid;
}

namespace {

void signal_qemu_process_group(pid_t pid, int signal) {
    if (pid <= 0) {
        return;
    }
    if (kill(-pid, signal) != 0) {
        kill(pid, signal);
    }
}

}  // namespace

void request_qemu_termination(pid_t pid) {
    signal_qemu_process_group(pid, SIGTERM);
}

void force_kill_qemu(pid_t pid) {
    signal_qemu_process_group(pid, SIGKILL);
}
