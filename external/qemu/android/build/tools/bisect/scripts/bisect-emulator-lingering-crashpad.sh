#!/bin/sh

# This script uses `emu-bisect` to find a build where `crashpad_handler`
# process lingers after the emulator has been terminated.

# **Overview**
# `emu-bisect` uses binary search to identify a specific build where a shell command starts to fail.
# In this case, we're looking for a build where the emulator fails to clean up crashpad_handler
# processes upon exit.

# **Usage**
# The following command runs `emu-bisect` with the specified options:

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

emu-bisect \
	--num 1024 \
	--artifact sdk-repo-darwin_aarch64-emulator-{bid}.zip \
	--build_target emulator-mac_aarch64_gfxstream \
	--unzip \
	--token $TOKEN \
	"$SCRIPT_DIR/check-lingering-crashpad.sh \"\${ARTIFACT_UNZIP_DIR}/emulator/emulator -avd 36 -wipe-data\""

#
#--bad 13334188 \
# --good 11905877 \
# --remote_machine work \

# **Explanation**
# * `--num 1024`:  Limits the search to a maximum of 1024 builds.
# * `--artifact sdk-repo-darwin_aarch64-emulator-{bid}.zip`: Specifies the Darwin ARM64 artifact.
# * `--build_target emulator-mac_aarch64_gfxstream`: Specifies the Mac ARM64 build target.
# * `--unzip`: Automatically unzips the downloaded artifact.
# * `check-lingering-crashpad.sh`: This script wraps the emulator launch, waits 10s, terminates it,
#   and then verifies if `crashpad_handler` is still running.
