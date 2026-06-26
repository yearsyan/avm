#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  package_macos_app.sh --dist-dir DIR --out-dir DIR [options]

Options:
  --app-name NAME              App bundle display name [MacMu]
  --binary-name NAME           App entry executable name [macmu]
  --bundle-id ID               CFBundleIdentifier [dev.macmu.MacMu]
  --package-basename NAME      Output archive base name [macmu-macos-arm64]
  --version VERSION            CFBundleShortVersionString [0.1.0]
  --build-version VERSION      CFBundleVersion [1]
  --no-dmg                     Do not create a DMG.
  --no-zip                     Do not create an app zip.
  -h, --help                   Show this help.
EOF
}

die() {
  printf 'package_macos_app.sh: error: %s\n' "$*" >&2
  exit 1
}

dist_dir=""
out_dir=""
app_name="MacMu"
binary_name="macmu"
bundle_id="dev.macmu.MacMu"
package_basename="macmu-macos-arm64"
version="0.1.0"
build_version="${GITHUB_RUN_NUMBER:-1}"
create_dmg=1
create_zip=1
qemu_headless_rel="qemu/darwin-aarch64/qemu-system-aarch64-headless"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dist-dir)
      dist_dir="$2"
      shift 2
      ;;
    --out-dir)
      out_dir="$2"
      shift 2
      ;;
    --app-name)
      app_name="$2"
      shift 2
      ;;
    --binary-name)
      binary_name="$2"
      shift 2
      ;;
    --bundle-id)
      bundle_id="$2"
      shift 2
      ;;
    --package-basename)
      package_basename="$2"
      shift 2
      ;;
    --version)
      version="$2"
      shift 2
      ;;
    --build-version)
      build_version="$2"
      shift 2
      ;;
    --no-dmg)
      create_dmg=0
      shift
      ;;
    --no-zip)
      create_zip=0
      shift
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

[[ -n "$dist_dir" ]] || die "--dist-dir is required"
[[ -n "$out_dir" ]] || die "--out-dir is required"
[[ -d "$dist_dir" ]] || die "distribution directory does not exist: $dist_dir"
[[ -x "$dist_dir/$binary_name" ]] || die "missing app entry executable: $dist_dir/$binary_name"
[[ -x "$dist_dir/$qemu_headless_rel" ]] || die "missing qemu backend: $dist_dir/$qemu_headless_rel"
[[ -f "$dist_dir/LICENSE.shell-MIT" ]] || die "missing shell license: $dist_dir/LICENSE.shell-MIT"

command -v ditto >/dev/null 2>&1 || die "missing required command: ditto"
if [[ "$create_dmg" == "1" ]]; then
  command -v hdiutil >/dev/null 2>&1 || die "missing required command: hdiutil"
fi

mkdir -p "$out_dir"
out_dir="$(cd "$out_dir" && pwd)"
dist_dir="$(cd "$dist_dir" && pwd)"

app_dir="$out_dir/$app_name.app"
contents_dir="$app_dir/Contents"
macos_dir="$contents_dir/MacOS"
resources_dir="$contents_dir/Resources"
bundled_dist_dir="$resources_dir/emulator"
app_zip="$out_dir/$package_basename-app.zip"
dmg_root="$out_dir/$package_basename-dmg-root"
dmg_path="$out_dir/$package_basename.dmg"

rm -rf "$app_dir" "$app_zip" "$dmg_root" "$dmg_path"
mkdir -p "$macos_dir" "$resources_dir"

ditto "$dist_dir" "$bundled_dist_dir"
rm -f "$bundled_dist_dir/$binary_name"
rm -f "$bundled_dist_dir/aemu-iosurface-shell"
rm -f "$bundled_dist_dir/emulator"

ditto "$dist_dir/$binary_name" "$macos_dir/$binary_name"
chmod 755 "$macos_dir/$binary_name"

cat > "$contents_dir/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>en</string>
  <key>CFBundleDisplayName</key>
  <string>$app_name</string>
  <key>CFBundleExecutable</key>
  <string>$binary_name</string>
  <key>CFBundleIdentifier</key>
  <string>$bundle_id</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>$app_name</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>$version</string>
  <key>CFBundleVersion</key>
  <string>$build_version</string>
  <key>LSApplicationCategoryType</key>
  <string>public.app-category.developer-tools</string>
  <key>LSMinimumSystemVersion</key>
  <string>11.0</string>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>
EOF

printf 'APPL????' > "$contents_dir/PkgInfo"

test -x "$macos_dir/$binary_name"
test -x "$bundled_dist_dir/$qemu_headless_rel"
test -f "$bundled_dist_dir/LICENSE.shell-MIT"
test ! -e "$bundled_dist_dir/$binary_name"
test ! -e "$bundled_dist_dir/aemu-iosurface-shell"
test ! -e "$bundled_dist_dir/emulator"

if [[ "$create_zip" == "1" ]]; then
  ditto -c -k --sequesterRsrc --keepParent "$app_dir" "$app_zip"
fi

if [[ "$create_dmg" == "1" ]]; then
  mkdir -p "$dmg_root"
  ditto "$app_dir" "$dmg_root/$app_name.app"
  ln -s /Applications "$dmg_root/Applications"
  hdiutil create -volname "$app_name" -srcfolder "$dmg_root" -ov -format UDZO "$dmg_path"
  test -f "$dmg_path"
fi

printf 'Created %s\n' "$app_dir"
if [[ "$create_zip" == "1" ]]; then
  printf 'Created %s\n' "$app_zip"
fi
if [[ "$create_dmg" == "1" ]]; then
  printf 'Created %s\n' "$dmg_path"
fi
