#!/usr/bin/env python
#
# Copyright 2024 - The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the',  help='License');
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an',  help='AS IS' BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import atexit
import logging
import os
import shutil
import subprocess
import sys
import tempfile
from aemu.prebuilts.deps.common import EXE_SUFFIX
import aemu.prebuilts.deps.common as deps_common
from pathlib import Path
import platform
import json

AOSP_ROOT = Path(__file__).resolve().parents[7]
HOST_OS = platform.system().lower()
HOST_ARCH = deps_common.getHostArchitecture()
PREBUILTS_ARCH = "x86_64" if HOST_ARCH == "x86_64" else "aarch64"
PLATFORM_NAME = f"{HOST_OS}-{PREBUILTS_ARCH}"

AOSP_MESA_SRC_PATH = os.path.join(AOSP_ROOT, "external", "mesa3d")
PREBUILTS_PATH = os.path.join(AOSP_ROOT, "prebuilts")

AOSP_LAVAPIPE_PREBUILTS_PATH = os.path.join(PREBUILTS_PATH, "android-emulator-build", "common", "vulkan", PLATFORM_NAME, "icds")
LAVAPIPE_SHA1_FILE = "lavapipe.sha1"

NINJA_PATH = os.path.join(PREBUILTS_PATH, "ninja", HOST_OS + "-x86")
GLSLANG_PATH = os.path.join(PREBUILTS_PATH, "android-emulator-build", "common", "vulkan", PLATFORM_NAME)

def checkDependencies():
    logging.info("Checking for required build dependencies..")
    # - Python >= 3.6.x
    logging.info(">> Checking for Python >= 3.6.x (%s)", sys.version)
    deps_common.checkPythonVersion(min_vers=(3, 6))

    logging.info(">> Checking CMake >= 3.19")
    deps_common.checkCmakeVersion(min_vers=(3, 19))

    logging.info(">> Checking Ninja >= 1.8.2")
    deps_common.checkNinjaVersion(min_vers=(1, 8, 2))

    logging.info(">> Checking for bison")
    deps_common.checkBisonVersion()
    logging.info(">> Checking for flex")
    deps_common.checkFlexVersion()

    logging.info("Library dependencies will be verified by meson");
    return True

def configureLavapipeBuild(srcdir, builddir):
    # TODO(b/446627550): Use ninja and meson prebuilts from prebuilts by making them
    # compatible for building lavapipe and add into search path instead
    if not shutil.which("ninja"):
        raise Exception(f"'ninja' is not available!")
    if not shutil.which("meson"):
        raise Exception(f"'meson' is not available!")

    if Path(builddir).exists():
        shutil.rmtree(builddir)
    old_umask = os.umask(0o027)
    os.makedirs(builddir)
    os.umask(old_umask)

    config_script = "meson"
    conf_args = [config_script, "setup",
                 builddir,
                 "-Dvulkan-drivers=swrast",
                 "-Dgallium-drivers=llvmpipe",
                 "-Dopengl=false",
                 "-Degl=disabled",
                 "-Dgles1=disabled",
                 "-Dgles2=disabled",
                 "-Dvideo-codecs=[]",
                 "-Dzstd=disabled",
                 "-Dshared-llvm=disabled"]

    debugBuild = False
    if debugBuild:
        conf_args.append("--buildtype=debug")

    if HOST_OS == "darwin":
        conf_args.append(f"-Dplatforms=macos")
    elif HOST_OS == "windows":
        conf_args.append(f"-Dplatforms=windows")
    elif HOST_OS == "linux":
        conf_args.append(f"-Dplatforms=x11,wayland")

    logging.info("[%s] Running %s in %s", builddir, config_script, srcdir)
    logging.info(conf_args)
    subprocess.run(args=conf_args, stderr=subprocess.STDOUT, check=True, cwd=srcdir, env=os.environ.copy())
    logging.info("%s succeeded", config_script)

def buildLavapipe(srcdir, builddir):
    ninja_build_cmd = ["ninja" + EXE_SUFFIX, "-C", builddir]
    logging.info(ninja_build_cmd)
    subprocess.run(args=ninja_build_cmd, stderr=subprocess.STDOUT, check=True, cwd=srcdir, env=os.environ.copy())

    # Create the SHA1 file
    git_sha1 = deps_common.getSHA1FromGitProject(AOSP_MESA_SRC_PATH)
    sha1_path = os.path.join(builddir, LAVAPIPE_SHA1_FILE)
    with open(sha1_path, 'w') as f:
        f.write(git_sha1)

    logging.info("Build succeeded")

def installLavapipe(builddir, installdir):
    libExtensions = {
        "linux": "so",
        "windows": "dll",
        "darwin": "dylib"
    }
    libExtension = libExtensions.get(HOST_OS, "so")

    def retargetICDFile(icdFile):
        try:
            with open(icdFile, 'r') as f:
                data = json.load(f)

            # Replace library path to use a local one, keep the extension
            data["ICD"]["library_path"] = f"./libvulkan_lvp.{libExtension}"

            with open(icdFile, 'w') as f:
                json.dump(data, f, indent=4)
            logging.info(f"Successfully modified {icdFile}")
        except FileNotFoundError:
            logging.error(f"Error: File not found at {icdFile}")
        except json.JSONDecodeError:
            logging.error(f"Error: Invalid JSON format in {icdFile}")
        except Exception as e:
            logging.error(f"An unexpected error occurred: {e}")

    os.makedirs(installdir,exist_ok=True)
    LAVAPIPE_PREFIX= os.path.join(builddir, "src/gallium/targets/lavapipe")
    LAVAPIPE_LIB_SRC = os.path.join(LAVAPIPE_PREFIX, f"libvulkan_lvp.{libExtension}")
    LAVAPIPE_LIB_DST = os.path.join(installdir, f"libvulkan_lvp.{libExtension}")
    LAVAPIPE_ICD_SRC = os.path.join(LAVAPIPE_PREFIX, f"lvp_icd.{PREBUILTS_ARCH}.json")
    LAVAPIPE_ICD_DST = os.path.join(installdir, "lvp_icd.json")
    LAVAPIPE_SHA1_SRC = os.path.join(builddir, LAVAPIPE_SHA1_FILE)
    LAVAPIPE_SHA1_DST = os.path.join(installdir, LAVAPIPE_SHA1_FILE)

    logging.info("Installing Lavapipe to %s", installdir)

    # The ICD file built in MESA expects the library to be installed in system path.
    # We will be shipping icd and library in the same dir so rewrite to point locally.
    retargetICDFile(LAVAPIPE_ICD_SRC)

    logging.info("Copying %s to %s", LAVAPIPE_ICD_SRC, LAVAPIPE_ICD_DST)
    shutil.copyfile(LAVAPIPE_ICD_SRC, LAVAPIPE_ICD_DST)
    logging.info("Copying %s to %s", LAVAPIPE_LIB_SRC, LAVAPIPE_LIB_DST)
    shutil.copyfile(LAVAPIPE_LIB_SRC, LAVAPIPE_LIB_DST)
    logging.info("Copying %s to %s", LAVAPIPE_SHA1_SRC, LAVAPIPE_SHA1_DST)
    shutil.copyfile(LAVAPIPE_SHA1_SRC, LAVAPIPE_SHA1_DST)
    logging.info("Installation succeeded")


def buildPrebuilt(args, prebuilts_out_dir, check_sha1=False):
    if check_sha1:
        logging.info("Checking Lavapipe SHA1...")
        try:
            if deps_common.isSHA1Same(git_src_dir=AOSP_MESA_SRC_PATH,
                                      sha1_file=AOSP_LAVAPIPE_PREBUILTS_PATH / LAVAPIPE_SHA1_FILE):
                logging.info("Same Lavapipe SHA1. Skipping Lavapipe build.")
                return
            else:
                logging.info("Different Lavapipe sha1 detected. Rebuilding Lavapipe.")
        except Exception as e:
            logging.fatal(e)

    # Use our prebuilts
    deps_common.addToSearchPath(NINJA_PATH)
    deps_common.addToSearchPath(GLSLANG_PATH)

    logging.info(os.environ)

    if not os.path.isdir(AOSP_MESA_SRC_PATH):
        logging.fatal("%s does not exist", AOSP_MESA_SRC_PATH)
    logging.info("MESA source: %s", AOSP_MESA_SRC_PATH)

    mesa_src_path = AOSP_MESA_SRC_PATH
    with tempfile.TemporaryDirectory() as mesa_build_path:
        logging.info("Building Lavapipe")
        configureLavapipeBuild(mesa_src_path, mesa_build_path)
        buildLavapipe(mesa_src_path, mesa_build_path)
        installLavapipe(mesa_build_path, AOSP_LAVAPIPE_PREBUILTS_PATH)

        if (args.dist):
            # Build will create a distribution zip file, move the files
            # also into prebuilts_out_dir
            moltenvk_install_dir = Path(prebuilts_out_dir) / "lavapipe"
            installLavapipe(mesa_build_path, moltenvk_install_dir)

        logging.info("Successfully built Lavapipe!")
