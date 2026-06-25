# Copyright 2025 - The Android Open Source Project
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

import platform
import logging
import re
import shutil
from pathlib import Path


from aemu.process.command import Command
from aemu.process.log_handler import LogHandler
from aemu.process.environment import get_default_environment
from aemu.tasks.build_task import BuildTask


class PrebuiltsTask(BuildTask):
    """Compiles the prebuilts."""

    def __init__(self, args, is_emulator_build):
        super().__init__()
        self.args = args
        self.is_emulator_build = is_emulator_build

    def do_run(self):
        from aemu.prebuilts import buildPrebuilts
        buildPrebuilts(self.args, self.is_emulator_build)