#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  create_android16_metadata_image.sh OUTPUT

Creates the 64 MiB GPT/ext4 metadata disk used by the MacMu Android 16 image.
Run this on the Linux AOSP build host. It does not require root or loop devices.
EOF
}

die() {
  printf 'create_android16_metadata_image.sh: error: %s\n' "$*" >&2
  exit 1
}

if [[ $# -ne 1 || "$1" == "-h" || "$1" == "--help" ]]; then
  usage
  [[ $# -eq 1 ]] && exit 0
  exit 2
fi

output="$1"

command -v truncate >/dev/null 2>&1 || die "missing required command: truncate"
command -v sgdisk >/dev/null 2>&1 || die "missing required command: sgdisk"
command -v mke2fs >/dev/null 2>&1 || die "missing required command: mke2fs"

mkdir -p "$(dirname "$output")"
output_dir="$(cd "$(dirname "$output")" && pwd)"
output="$output_dir/$(basename "$output")"
temporary="$output.tmp.$$"
trap 'rm -f "$temporary"' EXIT

# The emulator exposes this historical encryption-key disk slot as a virtio
# block device. Android 16's first-stage fstab discovers a GPT partition named
# "metadata" on that device and mounts its ext4 filesystem at /metadata.
truncate -s 64M "$temporary"
sgdisk --clear \
  --new=1:2048:+62M \
  --change-name=1:metadata \
  --typecode=1:8300 \
  "$temporary" >/dev/null

# Format the partition in place. The fixed 1 MiB offset and 62 MiB size avoid
# losetup/sudo while leaving room for both primary and backup GPT structures.
mke2fs -q -t ext4 -F -b 4096 -L metadata \
  -E offset=1048576 \
  "$temporary" 15872

mv "$temporary" "$output"
trap - EXIT
printf 'Created %s\n' "$output"
