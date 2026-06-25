# Copyright 2022 - The Android Open Source Project
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
import json
import logging
import os
import platform
import subprocess
from collections import UserDict
from pathlib import Path


class BaseEnvironment(UserDict):
    def __init__(self, aosp: Path):
        super().__init__(
            {
                "PATH": str(
                    aosp
                    / "prebuilts"
                    / "ninja"
                    / f"{platform.system().lower()}-x86"
                )
                + os.pathsep
                + os.environ.get("PATH", "")
            }
        )


class PosixEnvironment(BaseEnvironment):
    def __init__(self, aosp: Path):
        super().__init__(aosp)


class VisualStudioNotFoundException(Exception):
    pass


class VisualStudioMissingVarException(Exception):
    pass


class VisualStudioNativeWorkloadNotFoundException(Exception):
    pass


class WindowsEnvironment(BaseEnvironment):
    def __init__(self, aosp: Path, visual_studio_version: str):
        assert platform.system() == "Windows"
        super().__init__(aosp)
        self.visual_studio_version = visual_studio_version
        for key in os.environ:
            self[key] = os.environ[key]
            # Also add the upper case version of the key, as python dicts are
            # case sensitive, but windows is not.
            self[key.upper()] = os.environ[key]

        vs = self._visual_studio()
        env_lines = subprocess.check_output(
            [vs, "&&", "set"], encoding="utf-8"
        ).splitlines()
        for line in env_lines:
            if "=" in line:
                key, val = line.split("=", 1)
                # Variables in windows are case insensitive, but not in python dict!
                self[key] = val
                self[key.upper()] = val

        if not "VSINSTALLDIR" in self:
            raise VisualStudioMissingVarException("Missing VSINSTALLDIR in environment")

        if not "VCTOOLSINSTALLDIR" in self:
            raise VisualStudioMissingVarException(
                "Missing VCTOOLSINSTALLDIR in environment"
            )

    def _visual_studio(self):
        """Finds the visual studio installation

        Raises:
            VisualStudioNotFoundException: When visual studio was not found
            VisualStudioNativeWorkloadNotFoundException: When the native workload was not found

        Returns:
            _type_: _description_
        """
        prgrfiles = Path(os.getenv("ProgramFiles(x86)", r"C:\Program Files (x86)"))
        res = subprocess.check_output(
            [
                str(
                    prgrfiles / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
                ),
                "-requires",
                "Microsoft.VisualStudio.Workload.NativeDesktop",
                "-sort",
                "-format",
                "json",
                "-utf8",
                "-version",
                self.visual_studio_version,
            ]
        )
        vsresult = json.loads(res)
        logging.info("Discovered: %s", vsresult)
        if len(vsresult) == 0:
            raise VisualStudioNativeWorkloadNotFoundException(
                f"No visual studio with the native desktop load available for version '{self.visual_studio_version}'."
            )

        for install in vsresult:
            logging.debug("Considering %s", install["displayName"])
            candidates = list(Path(install["installationPath"]).glob("**/vcvars64.bat"))

            if len(candidates) > 0:
                return candidates[0].absolute()

        # Oh oh, no visual studio..
        raise VisualStudioNotFoundException(
            "Unable to detect a visual studio installation with the native desktop workload."
        )


ENVIRONMENT = None


def get_default_environment(aosp: Path, visual_studio_version: str):
    global ENVIRONMENT

    if ENVIRONMENT is None:
        if platform.system() == "Windows":
            ENVIRONMENT = WindowsEnvironment(aosp, visual_studio_version)
        else:
            ENVIRONMENT = PosixEnvironment(aosp)

    return ENVIRONMENT
