// SPDX-License-Identifier: MIT
//
// qemu launcher + teardown. Moved verbatim from the original single-file
// MacMu.mm. Pure C++.

#include "qemu_launcher.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

pid_t launch_qemu(const ShellOptions& options) {
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
    std::vector<char*> argv = make_cstring_vector(args);

    std::vector<std::string> environment = make_environment({
        {"MACMU_IOSURFACE_EXPORT", "1"},
        {"AEMU_IOSURFACE_EXPORT", "1"},
        {"ANDROID_EMULATOR_LAUNCHER_DIR", options.launcherDir},
        {"ANDROID_EMULATOR_WRAPPER_PID", std::to_string(getpid())},
        {macmu::kInputSocketEnv, options.inputSocketPath},
        {"DYLD_LIBRARY_PATH", options.dyldLibraryPath},
        {"LC_ALL", "C"},
        {"MESA_RGB_VISUAL", "TrueColor 24"},
        {"SWIFT_BACKTRACE", "enable=no"},
    });
    std::vector<char*> envp = make_cstring_vector(environment);

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);

    pid_t pid = -1;
    const int result = posix_spawn(&pid, options.qemuPath.c_str(), nullptr, &attributes,
                                   argv.data(), envp.data());
    posix_spawnattr_destroy(&attributes);
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
