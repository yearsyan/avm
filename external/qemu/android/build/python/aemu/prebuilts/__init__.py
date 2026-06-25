# Copyright 2023 - The Android Open Source Project
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

import logging
from pathlib import Path
import platform
import os
import zipfile
import aemu.prebuilts.angle as angle
import aemu.prebuilts.lavapipe as lavapipe
import aemu.prebuilts.moltenvk as moltenvk
import aemu.prebuilts.qt as qt
import aemu.prebuilts.vulkan_loader as vulkan_loader
import aemu.prebuilts.deps.common as deps_common

HOST_OS = platform.system().lower()
HOST_ARCH = deps_common.getHostArchitecture()

_prebuilts_dir_name = "prebuilts"
_prebuilts_zip_name = "PREBUILT-{prebuilt_name}-{build_number}.zip"
_prebuilt_funcs = {
    'qt': qt.buildPrebuilt,
    'angle': angle.buildPrebuilt,
    'vulkan_loader': vulkan_loader.buildPrebuilt,
    # Add more prebuilts here
}

if HOST_OS == "linux":
    _prebuilt_funcs.update({
        'lavapipe': lavapipe.buildPrebuilt,
    })

if HOST_OS == "darwin":
    _prebuilt_funcs.update(
        {
            "moltenvk": moltenvk.buildPrebuilt,
            "lavapipe": lavapipe.buildPrebuilt,
        }
    )

def buildPrebuilts(args, is_emulator_build):
    build_if_sha1_changed = False
    global _prebuilt_funcs
    # In an emulator build, we check if the SHA1 of the latest commit in each repository and build
    # it if different from the SHA1 of the current prebuilt.
    if is_emulator_build:
        logging.info("Prebuilts compilation in emulator build starting up for "
                        + f"{HOST_OS}-{HOST_ARCH}.")
        build_if_sha1_changed = True
        # Continue to add support for more platforms/prebuilts in emulator build.
        if HOST_OS == "darwin":
            _prebuilt_funcs = {
                "moltenvk": moltenvk.buildPrebuilt,
                "vulkan_loader": vulkan_loader.buildPrebuilt,
            }
        elif HOST_OS == "linux":
            _prebuilt_funcs = {
                'vulkan_loader': vulkan_loader.buildPrebuilt,
            }
        elif HOST_OS == "windows":
            _prebuilt_funcs = {
                'vulkan_loader': vulkan_loader.buildPrebuilt,
            }
        else:
            logging.info("Prebuilts compilation in emulator build is not supported yet for "
                         + f"{HOST_OS}-{HOST_ARCH}. Skipping.")
            return

    # out/prebuilts
    prebuilts_dir = os.path.join(args.out, _prebuilts_dir_name)
    build_list = args.prebuilts if args.prebuilts else _prebuilt_funcs.keys()
    orig_environ = os.environ.copy()
    logging.info("Prebuilts list: " + str(build_list))
    for prebuilt in build_list:
        logging.info(">> Building prebuilt [%s]", prebuilt)
        _prebuilt_funcs.get(prebuilt)(args, prebuilts_dir, build_if_sha1_changed)
        # Ours + third_party build scripts may modify our envirionment. We should start
        # from the same environment for each prebuilt.
        os.environ = orig_environ
    logging.info("Done building prebuilts list. Prebuilts located at [%s]", prebuilts_dir)

    if not args.dist:
        logging.info("Argument 'dist' is not provided, skipping zip file creation")
    else:
        # zip each prebuilt in out/prebuilts and binplace under dist/prebuilts/.
        dist_prebuilts_dir = Path(args.dist) / "prebuilts"
        logging.info("Argument 'dist' is provided, creating zip file for prebuilts at: %s", dist_prebuilts_dir)
        os.makedirs(dist_prebuilts_dir)
        for dir in Path(prebuilts_dir).glob("*"):
            bname = os.path.basename(dir)
            prebuilts_zip = os.path.join(
                    args.dist, _prebuilts_dir_name, _prebuilts_zip_name.format(
                        prebuilt_name=bname, build_number=args.sdk_build_number))
            logging.info(f"Creating {prebuilts_zip}")
            with zipfile.ZipFile(prebuilts_zip, "w", zipfile.ZIP_DEFLATED, allowZip64=True) as zipf:
                search_dir = dir
                for fname in search_dir.glob("**/*"):
                    arcname = fname.relative_to(search_dir)
                    logging.info("[%s] Adding %s as %s", prebuilts_zip, fname, arcname)
                    zipf.write(fname, arcname)

    logging.info("Successfully finished buildPrebuilts.")
