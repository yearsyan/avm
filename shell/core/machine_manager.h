// SPDX-License-Identifier: MIT

#ifndef MACMU_SHELL_MACHINE_MANAGER_H
#define MACMU_SHELL_MACHINE_MANAGER_H

#include <string>

#include "shell_options.h"

std::string macmu_machine_path(const ShellOptions& options);
std::string macmu_machine_ini_path(const ShellOptions& options);
bool macmu_ensure_runtime_directories(const ShellOptions& options, std::string* error);
bool macmu_system_image_exists(const ShellOptions& options);
bool macmu_machine_exists(const ShellOptions& options);
bool macmu_create_default_machine(const ShellOptions& options, std::string* error);

#endif  // MACMU_SHELL_MACHINE_MANAGER_H
