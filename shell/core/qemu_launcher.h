// SPDX-License-Identifier: MIT
//
// qemu-system-aarch64-headless process lifecycle: posix_spawn with the right
// environment, and ordered SIGTERM/SIGKILL teardown. Pure C++.

#ifndef MACMU_SHELL_QEMU_LAUNCHER_H
#define MACMU_SHELL_QEMU_LAUNCHER_H

#include <sys/types.h>

#include "shell_options.h"

// Spawns qemu-system-aarch64-headless in a new process group with the shell's
// IOSurface-export environment. If |frameDoorbellFd| is non-negative, it is
// inherited into the child and advertised through MACMU_FRAME_DOORBELL_FD. If
// |inputFd| is non-negative, it is inherited into the child (at the fixed fd
// macmu::kInputChildFd) and advertised through MACMU_INPUT_FD, so host pointer
// input reaches the guest without a filesystem socket. If |controlFd| is
// non-negative, it is inherited at macmu::kControlChildFd and advertised
// through MACMU_CONTROL_FD (the MMCP control plane; the caller must close its
// copy of the remote end after the spawn). Returns the child pid, or -1 on
// failure (a message is printed to stderr).
pid_t launch_qemu(const ShellOptions& options, int frameDoorbellFd = -1, int inputFd = -1,
                  int controlFd = -1);

// Signal the process group rooted at |pid|. These functions never call
// waitpid(): the supervisor thread is the single owner of child reaping.
void request_qemu_termination(pid_t pid);
void force_kill_qemu(pid_t pid);

#endif  // MACMU_SHELL_QEMU_LAUNCHER_H
