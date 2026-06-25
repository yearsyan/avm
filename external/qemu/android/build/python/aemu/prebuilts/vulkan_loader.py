#!/usr/bin/env python
#
# Copyright 2024 - The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import logging
import os
import shutil
import subprocess
import sys
from pathlib import Path
import platform

import aemu.prebuilts.deps.common as deps_common

AOSP_ROOT = Path(__file__).resolve().parents[7]
HOST_OS = platform.system().lower()
HOST_ARCH = deps_common.getHostArchitecture()

VULKAN_LOADER_REPO_URL = "https://github.com/KhronosGroup/Vulkan-Loader.git"
VULKAN_LOADER_GIT_SHA = "bac4131"
CMAKE_PATH = os.path.join(AOSP_ROOT, "prebuilts", "cmake", HOST_OS + "-x86", "bin")
VULKAN_LOADER_PREBUILTS_ARCH = "aarch64" if HOST_ARCH == "aarch64" else "x86_64"

VULKAN_LOADER_PREBUILTS_PATH = (
    AOSP_ROOT
    / "prebuilts"
    / "android-emulator-build"
    / "common"
    / "vulkan"
    / f"{HOST_OS}-{VULKAN_LOADER_PREBUILTS_ARCH}"
)
VULKAN_LOADER_SHA1_FILE = "vulkan_loader.sha1"


def installVulkanLoader(builddir, installdir):
    """Installs the output files from `builddir` to `installdir`."""
    logging.info("Installing Vulkan-Loader from %s to %s", builddir, installdir)
    # Before installing, remove any existing files that are part of this prebuilt.
    # This is to avoid having stale files around, but don't remove the dir because
    # other prebuilts might be available.
    if installdir.exists():
        for item in os.listdir(builddir):
            path = os.path.join(installdir, item)
            if os.path.lexists(path):
                if os.path.islink(path) or os.path.isfile(path):
                    os.remove(path)
                elif os.path.isdir(path):
                    shutil.rmtree(path)

    shutil.copytree(builddir, installdir, symlinks=True, dirs_exist_ok=True)

    # Create the SHA1 file in the target directory.
    with open(installdir / VULKAN_LOADER_SHA1_FILE, "w") as f:
        f.write(VULKAN_LOADER_GIT_SHA)

    logging.info("Installed Vulkan-Loader files to %s", installdir)


def _build_linux(args, prebuilts_out_dir):
    """Builds Vulkan-Loader from source using a self-contained Dockerfile for Linux."""
    docker_image_name = "vulkan-builder-glibc2.27:latest"
    dockerfile_name = "Dockerfile.vulkan_loader"
    if HOST_ARCH == "aarch64":
        docker_image_name = "vulkan-builder-glibc2.27-aarch64:latest"
        dockerfile_name = "Dockerfile.vulkan_loader.aarch64"

    logging.info(f"Extracting Vulkan Loader from Docker container using {dockerfile_name}...")
    container_name = "vulkan_loader_extractor"
    script_dir = Path(__file__).parent
    dockerfile_path = script_dir / dockerfile_name

    # 1. Build Docker image, passing the SHA as an argument
    logging.info(f"Building podman image: {docker_image_name}")
    subprocess.run(
        [
            "podman",
            "build",
            "--cgroup-manager=cgroupfs",
            "-t",
            docker_image_name,
            "-f",
            str(dockerfile_path),
            "--build-arg",
            f"VULKAN_LOADER_GIT_SHA={VULKAN_LOADER_GIT_SHA}",
            str(script_dir),
        ],
        check=True,
    )

    # 2. Create a container from the pre-built image.
    try:
        subprocess.run(
            ["podman", "create", "--name", container_name, docker_image_name],
            check=True,
            capture_output=True,
            text=True,
        )
    except subprocess.CalledProcessError as e:
        logging.error("Failed to create podman container.")
        logging.error(f"podman image '{docker_image_name}' might not exist.")
        logging.error(
            "Build it first by running: podman build -t {} -f {} .".format(
                docker_image_name,
                Path(__file__).parent / "Dockerfile.vulkan_loader",
            )
        )
        logging.error(f"Stderr: {e.stderr}")
        sys.exit(1)

    build_dir = Path(prebuilts_out_dir) / "vulkan-loader-build"
    artifacts_dir = build_dir / "artifacts"
    os.makedirs(artifacts_dir, exist_ok=True)

    try:
        # 3. Copy the built library and its symlinks from the container.
        container_lib_dir = "/opt/vulkan_loader/lib"
        logging.info(f"Copying {container_lib_dir} from container to {artifacts_dir}")
        subprocess.run(
            ["podman", "cp", f"{container_name}:{container_lib_dir}/.", str(artifacts_dir)],
            check=True,
        )
    finally:
        # 4. Clean up the temporary container.
        logging.info(f"Removing temporary container: {container_name}")
        subprocess.run(["podman", "rm", container_name], check=True, capture_output=True)

    # 5. Return the artifacts directory.
    logging.info("Successfully extracted Vulkan-Loader from Docker.")
    return artifacts_dir


def _build_and_stage_unsafe_windows_variant(cmake_executable, env, clone_dir, build_config, safe_artifacts_dir):
    """Builds the 'unsafe' variant of the Vulkan loader for Windows."""
    logging.info("Building Vulkan-Loader (unsafe variant)...")
    cmake_build_dir_unsafe = clone_dir / "build_unsafe"
    install_dir_unsafe = clone_dir / "install_unsafe"
    os.makedirs(cmake_build_dir_unsafe, exist_ok=True)

    cmake_cmd_unsafe = [
        str(cmake_executable),
        "-S",
        ".",
        "-B",
        "build_unsafe",
        "-D",
        "UPDATE_DEPS=On",
        f"-DCMAKE_BUILD_TYPE={build_config}",
        f"-DCMAKE_INSTALL_PREFIX={install_dir_unsafe}",
        "-DLOADER_USE_UNSAFE_FILE_SEARCH=ON",
    ]
    subprocess.run(cmake_cmd_unsafe, cwd=clone_dir, check=True, env=env)
    subprocess.run([str(cmake_executable), "--build", "build_unsafe", "--config", build_config], cwd=clone_dir, check=True, env=env)
    subprocess.run([str(cmake_executable), "--install", "build_unsafe", "--config", build_config], cwd=clone_dir, check=True, env=env)

    # Rename and copy the unsafe DLL to the main artifacts directory
    unsafe_dll_path = install_dir_unsafe / "bin" / "vulkan-1.dll"
    if unsafe_dll_path.exists():
        renamed_dll_path = safe_artifacts_dir / "vulkan-1-unsafe.dll"
        shutil.copy(str(unsafe_dll_path), str(renamed_dll_path))
        logging.info(f"Created unsafe variant at {renamed_dll_path}")
    else:
        logging.warning("Could not find vulkan-1.dll for unsafe variant.")


def _build_native(args, prebuilts_out_dir):
    """Builds Vulkan-Loader from source natively for non-Linux hosts."""
    # Determine which cmake executable to use.
    cmake_executable = None
    prebuilt_cmake_bin_path = Path(CMAKE_PATH)
    cmake_exe_name = "cmake.exe" if HOST_OS == "windows" else "cmake"
    prebuilt_cmake_exe = prebuilt_cmake_bin_path / cmake_exe_name

    if prebuilt_cmake_exe.exists():
        logging.info(f"Using prebuilt cmake: {prebuilt_cmake_exe}")
        cmake_executable = str(prebuilt_cmake_exe)
    else:
        logging.error(f"Prebuilt cmake not found at {prebuilt_cmake_bin_path}. Cannot proceed without prebuilt cmake.")
        return False # Return False on failure

    # Create a modified environment for subprocesses to find the prebuilt cmake.
    env = os.environ.copy()
    env["PATH"] = str(prebuilt_cmake_bin_path) + os.pathsep + env.get("PATH", "")

    logging.info("Building Vulkan-Loader (native)...")

    build_dir = Path(prebuilts_out_dir) / "vulkan-loader-build"
    clone_dir = build_dir / "Vulkan-Loader"

    # Always ensure a clean clone of the repository.
    if clone_dir.exists():
        logging.info(f"Removing existing clone directory: {clone_dir}")
        if HOST_OS == "windows":
            subprocess.run(["cmd", "/c", "rd", "/s", "/q", str(clone_dir)], check=True)
        else:
            subprocess.run(["rm", "-rf", str(clone_dir)], check=True)
    os.makedirs(clone_dir, exist_ok=True)

    logging.info(f"Cloning Vulkan-Loader repository from {VULKAN_LOADER_REPO_URL} to {clone_dir}...")
    subprocess.run(
        ["git", "clone", VULKAN_LOADER_REPO_URL, str(clone_dir)], check=True
    )
    subprocess.run(
        ["git", "checkout", VULKAN_LOADER_GIT_SHA], cwd=clone_dir, check=True
    )

    logging.info("Configuring Vulkan-Loader with CMake...")
    cmake_build_dir = clone_dir / "build"
    install_dir = clone_dir / "install"
    os.makedirs(cmake_build_dir, exist_ok=True)

    build_config = args.config.capitalize()

    cmake_cmd = [
        str(cmake_executable),
        "-S",
        ".",
        "-B",
        "build",
        "-D",
        "UPDATE_DEPS=On",
        f"-DCMAKE_BUILD_TYPE={build_config}",
        f"-DCMAKE_INSTALL_PREFIX={install_dir}",
        "-DCMAKE_NO_SYSTEM_FROM_IMPORTED=ON",
    ]

    subprocess.run(cmake_cmd, cwd=clone_dir, check=True, env=env)

    logging.info("Building Vulkan-Loader...")
    subprocess.run([str(cmake_executable), "--build", "build", "--config", build_config], cwd=clone_dir, check=True, env=env)

    logging.info("Installing Vulkan-Loader...")
    subprocess.run([str(cmake_executable), "--install", "build", "--config", build_config], cwd=clone_dir, check=True, env=env)

    artifacts_dir = install_dir / "lib"
    if HOST_OS == "windows":
        artifacts_dir = install_dir / "bin"
        _build_and_stage_unsafe_windows_variant(cmake_executable, env, clone_dir, build_config, artifacts_dir)

    logging.info("Successfully built Vulkan-Loader!")
    return artifacts_dir


def buildPrebuilt(args, prebuilts_out_dir, check_sha1=False):
    """Builds Vulkan-Loader from source."""

    # 1. Check if we need to build at all.
    sha1_file = VULKAN_LOADER_PREBUILTS_PATH / VULKAN_LOADER_SHA1_FILE
    if check_sha1 and sha1_file.exists():
        with open(sha1_file, "r") as f:
            prebuilt_sha = f.read().strip()
        if prebuilt_sha == VULKAN_LOADER_GIT_SHA:
            logging.info(
                "Prebuilt Vulkan-Loader SHA (%s) matches target SHA. Skipping build.",
                prebuilt_sha,
            )
            return

    # 2. Delegate to the appropriate build function based on the host OS.
    artifacts_dir = None
    if HOST_OS == "linux":
        artifacts_dir = _build_linux(args, prebuilts_out_dir)
    else:
        artifacts_dir = _build_native(args, prebuilts_out_dir)

    if artifacts_dir:
        installVulkanLoader(artifacts_dir, VULKAN_LOADER_PREBUILTS_PATH)

        if args.dist:
            vulkan_loader_install_dir = Path(prebuilts_out_dir) / "vulkan_loader"
            installVulkanLoader(artifacts_dir, vulkan_loader_install_dir)
