#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  sign_macos_distribution.sh --dist-dir DIR --identity IDENTITY --entitlements FILE [--keychain KEYCHAIN]

Signs every Mach-O file in a MacMu distribution with a Developer ID
identity. Libraries and the external shell are signed first without process
entitlements; qemu executables are signed last with the supplied entitlements.

Environment fallbacks:
  MACOS_CODESIGN_IDENTITY
  MACOS_CODESIGN_ENTITLEMENTS
  MACOS_CODESIGN_KEYCHAIN
EOF
}

dist_dir=""
identity="${MACOS_CODESIGN_IDENTITY:-}"
entitlements="${MACOS_CODESIGN_ENTITLEMENTS:-}"
keychain="${MACOS_CODESIGN_KEYCHAIN:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dist-dir)
      dist_dir="$2"
      shift 2
      ;;
    --identity)
      identity="$2"
      shift 2
      ;;
    --entitlements)
      entitlements="$2"
      shift 2
      ;;
    --keychain)
      keychain="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$dist_dir" || -z "$identity" || -z "$entitlements" ]]; then
  usage >&2
  exit 2
fi

if [[ ! -d "$dist_dir" ]]; then
  echo "Distribution directory does not exist: $dist_dir" >&2
  exit 1
fi

if [[ ! -f "$entitlements" ]]; then
  echo "Entitlements file does not exist: $entitlements" >&2
  exit 1
fi

sign_args=(--force --timestamp --options runtime --sign "$identity")
if [[ -n "$keychain" ]]; then
  sign_args+=(--keychain "$keychain")
fi

is_macho() {
  local path="$1"
  file -b "$path" | grep -Eq 'Mach-O|universal binary'
}

needs_process_entitlements() {
  local rel="$1"
  [[ "$rel" == qemu/* ]]
}

macho_files=()
while IFS= read -r -d '' path; do
  if is_macho "$path"; then
    macho_files+=("$path")
  fi
done < <(find "$dist_dir" -type f -print0)

if [[ "${#macho_files[@]}" -eq 0 ]]; then
  echo "No Mach-O files found under $dist_dir" >&2
  exit 1
fi

echo "Signing ${#macho_files[@]} Mach-O files under $dist_dir"

for path in "${macho_files[@]}"; do
  rel="${path#"$dist_dir"/}"
  if needs_process_entitlements "$rel"; then
    continue
  fi
  echo "codesign: $rel"
  codesign "${sign_args[@]}" "$path"
done

for path in "${macho_files[@]}"; do
  rel="${path#"$dist_dir"/}"
  if ! needs_process_entitlements "$rel"; then
    continue
  fi
  echo "codesign with entitlements: $rel"
  codesign "${sign_args[@]}" --entitlements "$entitlements" "$path"
done

for path in "${macho_files[@]}"; do
  codesign --verify --strict --verbose=2 "$path"
done

echo "Signed distribution successfully"
