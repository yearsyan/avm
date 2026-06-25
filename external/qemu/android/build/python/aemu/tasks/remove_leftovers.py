# Copyright 2022 - The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the 'License');
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an 'AS IS' BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
import csv
import io
import logging
import os
import subprocess
from pathlib import Path

from aemu.tasks.build_task import BuildTask


class RemoveLeftoverTask(BuildTask):
    """Best effort to remove any leftover windows processes."""

    def __init__(self, target: str, destination: Path):
        super().__init__()
        self.target = target
        self.destination = Path(destination).resolve()

    def do_run(self):
        """Runs the task to remove leftover processes if the target is Windows."""
        if self.target != "windows":
            return
        has_contents = self.destination.exists() and (len(os.listdir(self.destination)) > 0)
        if not has_contents:
            logging.info(
                "No contents at the destination directory %s, nothing to do.",
                self.destination,
            )
            return
        try:
            self._kill_proc()
        except Exception as e:
            logging.warning(
                "Failed to kill processes in %s: %s",
                self.destination,
                e,
                exc_info=True,
            )

    def _kill_proc(self):
        """Finds and terminates all processes running from the destination directory.

        Note: This method only terminates processes whose executable is located
        within the destination directory. It does not find processes that have an
        open handle to a file or directory within the destination directory but are
        running from elsewhere, as that would require non-standard libraries
        like psutil. This is sufficient for terminating dangling tests.
        """
        logging.info("Searching for processes running from %s", self.destination)

        try:
            result = subprocess.run(
                ["tasklist", "/V", "/FO", "CSV"],
                capture_output=True,
                text=True,
                check=True,
                encoding="utf-8",
                errors="ignore",
            )
        except (subprocess.CalledProcessError, FileNotFoundError) as e:
            logging.error("Failed to run tasklist: %s", e)
            return

        # The output of tasklist can have some warnings we need to filter out
        # So we find the first line that starts with "Image Name"
        output = result.stdout
        header_pos = output.find('"Image Name"')
        if header_pos == -1:
            logging.warning("Could not find CSV header in tasklist output.")
            return

        csv_data = output[header_pos:]
        reader = csv.DictReader(io.StringIO(csv_data))

        killed_pids = set()
        for row in reader:
            try:
                pid = row.get("PID")
                path_str = row.get("Image Path Name")

                if not pid or not path_str or path_str == "N/A":
                    continue

                proc_path = Path(path_str).resolve()

                if self.destination in proc_path.parents:
                    pid_int = int(pid)
                    if pid_int not in killed_pids:
                        logging.info(
                            "Found process %s (PID: %s) running from %s. Terminating...",
                            row.get("Image Name"),
                            pid_int,
                            proc_path,
                        )
                        try:
                            subprocess.run(
                                ["taskkill", "/F", "/PID", str(pid_int)],
                                check=True,
                                capture_output=True,
                            )
                            killed_pids.add(pid_int)
                            logging.info("Terminated process with PID %s.", pid_int)
                        except subprocess.CalledProcessError as e:
                            # It might have already been terminated, or we lack permissions.
                            if "not found" not in e.stderr.lower():
                                logging.warning(
                                    "Failed to terminate process with PID %s: %s",
                                    pid_int,
                                    e.stderr.strip(),
                                )

            except (ValueError, TypeError) as e:
                logging.warning(
                    "Failed to parse process information: %s, row: %s", e, row
                )

        if not killed_pids:
            logging.info("No processes found running from %s", self.destination)
