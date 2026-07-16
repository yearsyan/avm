#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  package_aosp16_image.sh --source-dir DIR --output FILE [options]

Options:
  --metadata-image FILE   Use FILE as encryptionkey.img when DIR does not
                          already contain one.
  --package-name NAME     Top-level directory inside the zip [aosp16-arm64].
  -h, --help              Show this help.

The archive contains only files used by the MacMu runtime. Raw partition
images and standalone super/product/system_ext images remain build/debug
artifacts and are intentionally excluded.
EOF
}

die() {
  printf 'package_aosp16_image.sh: error: %s\n' "$*" >&2
  exit 1
}

source_dir=""
output=""
metadata_image=""
package_name="aosp16-arm64"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --source-dir)
      source_dir="$2"
      shift 2
      ;;
    --output)
      output="$2"
      shift 2
      ;;
    --metadata-image)
      metadata_image="$2"
      shift 2
      ;;
    --package-name)
      package_name="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown argument: $1"
      ;;
  esac
done

[[ -n "$source_dir" ]] || die "--source-dir is required"
[[ -n "$output" ]] || die "--output is required"
[[ -d "$source_dir" ]] || die "source directory does not exist: $source_dir"
[[ "$package_name" != */* && -n "$package_name" ]] ||
  die "--package-name must be a single directory name"
command -v zip >/dev/null 2>&1 || die "missing required command: zip"
command -v dd >/dev/null 2>&1 || die "missing required command: dd"
command -v od >/dev/null 2>&1 || die "missing required command: od"

source_dir="$(cd "$source_dir" && pwd)"
mkdir -p "$(dirname "$output")"
output_dir="$(cd "$(dirname "$output")" && pwd)"
output="$output_dir/$(basename "$output")"

required_files=(
  advancedFeatures.ini
  android-info.txt
  VerifiedBootParams.textproto
  kernel-ranchu
  ramdisk.img
  system-qemu.img
  userdata.img
  vendor-qemu.img
  vendor_boot.img
)
optional_files=(
  build.prop
  dtb.img
  fstab.ranchu
  kernel_cmdline.txt
  system-qemu-config.txt
)

for file in "${required_files[@]}"; do
  [[ -f "$source_dir/$file" ]] || die "missing required image file: $source_dir/$file"
done

if [[ -z "$metadata_image" ]]; then
  if [[ -f "$source_dir/encryptionkey.img" ]]; then
    metadata_image="$source_dir/encryptionkey.img"
  elif [[ -f "$source_dir/metadata.img" ]]; then
    metadata_image="$source_dir/metadata.img"
  else
    die "missing encryptionkey.img; create it with scripts/create_android16_metadata_image.sh"
  fi
fi
[[ -f "$metadata_image" ]] || die "metadata image does not exist: $metadata_image"
metadata_image="$(cd "$(dirname "$metadata_image")" && pwd)/$(basename "$metadata_image")"

metadata_size="$(wc -c < "$metadata_image" | tr -d '[:space:]')"
[[ "$metadata_size" == "67108864" ]] ||
  die "metadata image must be exactly 64 MiB: $metadata_image"

hex_at() {
  local offset="$1"
  local count="$2"
  dd if="$metadata_image" bs=1 skip="$offset" count="$count" 2>/dev/null |
    od -An -tx1 |
    tr -d '[:space:]'
}

[[ "$(hex_at 512 8)" == "4546492050415254" ]] ||
  die "metadata image does not contain a GPT header"
[[ "$(hex_at 1080 16)" == "6d006500740061006400610074006100" ]] ||
  die "metadata image first GPT partition is not named metadata"
[[ "$(hex_at 1049656 2)" == "53ef" ]] ||
  die "metadata partition is not ext4"
[[ "$(hex_at 1049720 8)" == "6d65746164617461" ]] ||
  die "metadata ext4 filesystem is not labelled metadata"

stage="$(mktemp -d "${TMPDIR:-/tmp}/macmu-aosp16-package.XXXXXX")"
trap 'rm -rf "$stage"' EXIT
package_root="$stage/$package_name"
mkdir -p "$package_root"

stage_file() {
  local source="$1"
  local destination="$2"
  ln "$source" "$destination" 2>/dev/null || cp "$source" "$destination"
}

for file in "${required_files[@]}"; do
  stage_file "$source_dir/$file" "$package_root/$file"
done
for file in "${optional_files[@]}"; do
  if [[ -f "$source_dir/$file" ]]; then
    stage_file "$source_dir/$file" "$package_root/$file"
  fi
done
stage_file "$metadata_image" "$package_root/encryptionkey.img"

printf '%s\n' \
  'format=1' \
  'product=macmu_sdk_phone64_arm64' \
  'device=emu64a' \
  'variant=user' \
  'api=36' \
  > "$package_root/macmu-image-info.txt"

rm -f "$output" "$output.sha256"
(
  cd "$stage"
  zip -9 -X -r "$output" "$package_name"
)

(
  cd "$output_dir"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$(basename "$output")"
  else
    shasum -a 256 "$(basename "$output")"
  fi
) > "$output.sha256"

printf 'Created %s\n' "$output"
printf 'Created %s\n' "$output.sha256"
