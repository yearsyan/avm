#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

LOCK_FILE="${ROOT}/prebuilts.lock"
DEST_ROOT="${ROOT}"
CACHE_DIR="${ROOT}/.cache/prebuilts-src"
MODE=""
LOCAL_ROOT=""
ARTIFACT=""
ARTIFACT_SHA256="${MACMU_PREBUILTS_ARTIFACT_SHA256:-${AEMU_PREBUILTS_ARTIFACT_SHA256:-}}"
MAKE_ARTIFACT=""
FORCE=0
SKIP_DIGEST=0

usage() {
  cat <<'EOF'
Usage:
  scripts/bootstrap-prebuilts.sh --from-local <android-emu-root> [--force]
  scripts/bootstrap-prebuilts.sh --from-git [--cache-dir <dir>] [--force]
  scripts/bootstrap-prebuilts.sh --from-artifact <file-or-url> --artifact-sha256 <sha256> [--force]
  scripts/bootstrap-prebuilts.sh --verify
  scripts/bootstrap-prebuilts.sh --make-artifact <path.tar.gz>

Options:
  --dest-root <dir>       Install or verify prebuilts under this repository root.
  --lock-file <file>      Use a different lock file.
  --cache-dir <dir>       Git sparse checkout cache for --from-git.
  --skip-digest           Skip content digest verification.
  --force                 Replace locked prebuilt paths if they already exist.
  -h, --help              Show this help.

Environment:
  MACMU_PREBUILTS_ARTIFACT_SHA256
EOF
}

log() {
  printf '[prebuilts] %s\n' "$*"
}

die() {
  printf '[prebuilts] error: %s\n' "$*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

while (($#)); do
  case "$1" in
    --from-local)
      MODE="local"
      LOCAL_ROOT="${2:?missing path for --from-local}"
      shift 2
      ;;
    --from-git)
      MODE="git"
      shift
      ;;
    --from-artifact)
      MODE="artifact"
      ARTIFACT="${2:?missing file or URL for --from-artifact}"
      shift 2
      ;;
    --verify)
      MODE="verify"
      shift
      ;;
    --make-artifact)
      MODE="make-artifact"
      MAKE_ARTIFACT="${2:?missing path for --make-artifact}"
      shift 2
      ;;
    --artifact-sha256)
      ARTIFACT_SHA256="${2:?missing sha256 for --artifact-sha256}"
      shift 2
      ;;
    --dest-root)
      DEST_ROOT="${2:?missing path for --dest-root}"
      shift 2
      ;;
    --lock-file)
      LOCK_FILE="${2:?missing path for --lock-file}"
      shift 2
      ;;
    --cache-dir)
      CACHE_DIR="${2:?missing path for --cache-dir}"
      shift 2
      ;;
    --skip-digest)
      SKIP_DIGEST=1
      shift
      ;;
    --force)
      FORCE=1
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

[[ -n "${MODE}" ]] || die "choose one mode; run with --help for usage"
[[ -f "${LOCK_FILE}" ]] || die "lock file not found: ${LOCK_FILE}"

DEST_ROOT="$(cd "${DEST_ROOT}" && pwd)"
LOCK_FILE="$(cd "$(dirname "${LOCK_FILE}")" && pwd)/$(basename "${LOCK_FILE}")"
CACHE_DIR="$(mkdir -p "${CACHE_DIR}" && cd "${CACHE_DIR}" && pwd)"

read_lock() {
  local callback="$1"
  local line kind name dest url rev digest paths

  while IFS= read -r line || [[ -n "${line}" ]]; do
    [[ -z "${line}" || "${line}" == \#* ]] && continue
    read -r kind name dest url rev digest paths <<<"${line}"
    [[ "${kind}" == "repo" ]] || die "unsupported lock entry: ${kind}"
    [[ -n "${name}" && -n "${dest}" && -n "${url}" && -n "${rev}" && -n "${digest}" && -n "${paths}" ]] ||
      die "malformed lock entry: ${line}"
    "${callback}" "${name}" "${dest}" "${url}" "${rev}" "${digest}" ${paths}
  done <"${LOCK_FILE}"
}

content_digest() {
  local base="$1"
  shift
  local list
  list="$(mktemp)"

  (
    cd "${base}"
    local path
    for path in "$@"; do
      [[ -e "${path}" ]] || die "missing locked prebuilt path: ${base}/${path}"
      if [[ -d "${path}" ]]; then
        find "${path}" -type f -print
      else
        printf '%s\n' "${path}"
      fi
    done
  ) | LC_ALL=C sort >"${list}"

  while IFS= read -r file; do
    (cd "${base}" && shasum -a 256 "${file}")
  done <"${list}" | shasum -a 256 | cut -d' ' -f1
  rm -f "${list}"
}

verify_entry() {
  local name="$1" dest="$2" url="$3" rev="$4" expected_digest="$5"
  shift 5
  local base="${DEST_ROOT}/${dest}"

  [[ -d "${base}" ]] || die "missing prebuilt destination for ${name}: ${base}"
  if [[ "${SKIP_DIGEST}" == "1" ]]; then
    log "skip digest: ${name}"
    return
  fi

  local actual_digest
  actual_digest="$(content_digest "${base}" "$@")"
  [[ "${actual_digest}" == "${expected_digest}" ]] ||
    die "digest mismatch for ${name}: expected ${expected_digest}, got ${actual_digest}"
  log "verified ${name} (${actual_digest})"
}

verify_all() {
  read_lock verify_entry
}

ensure_replaceable_entry() {
  local name="$1" dest="$2" url="$3" rev="$4" digest="$5"
  shift 5
  local path target

  for path in "$@"; do
    target="${DEST_ROOT}/${dest}/${path}"
    if [[ -e "${target}" ]]; then
      [[ "${FORCE}" == "1" ]] ||
        die "${target} already exists; pass --force to replace locked prebuilt paths"
      rm -rf "${target}"
    fi
  done
}

copy_path() {
  local src="$1" dst="$2"

  [[ -e "${src}" ]] || die "source path not found: ${src}"
  mkdir -p "$(dirname "${dst}")"
  if [[ -d "${src}" ]]; then
    mkdir -p "${dst}"
    rsync -a --delete --exclude='.git' "${src}/" "${dst}/"
  else
    rsync -a "${src}" "${dst}"
  fi
}

copy_entry_from_base() {
  local src_base="$1" name="$2" dest="$3" url="$4" rev="$5" digest="$6"
  shift 6
  local path

  [[ -d "${src_base}" ]] || die "source root missing for ${name}: ${src_base}"
  for path in "$@"; do
    copy_path "${src_base}/${path}" "${DEST_ROOT}/${dest}/${path}"
  done
}

copy_entry_from_tree() {
  local tree_root="$1"
  shift
  local dest="$2"
  copy_entry_from_base "${tree_root}/${dest}" "$@"
}

check_local_revision_entry() {
  local name="$1" dest="$2" url="$3" rev="$4" digest="$5"
  local src_base="${LOCAL_ROOT}/${dest}"

  [[ -d "${src_base}" ]] || die "local source missing for ${name}: ${src_base}"
  need_cmd git
  local actual
  actual="$(git -C "${src_base}" rev-parse HEAD)"
  [[ "${actual}" == "${rev}" ]] ||
    die "local source revision mismatch for ${name}: expected ${rev}, got ${actual}"
}

copy_local_entry() {
  check_local_revision_entry "$@"
  copy_entry_from_tree "${LOCAL_ROOT}" "$@"
}

git_checkout_entry() {
  local name="$1" dest="$2" url="$3" rev="$4" digest="$5"
  shift 5
  local repo_dir="${CACHE_DIR}/${name}"

  need_cmd git
  if [[ ! -d "${repo_dir}/.git" ]]; then
    rm -rf "${repo_dir}"
    log "clone ${name}"
    git clone --filter=blob:none --no-checkout "${url}" "${repo_dir}"
  fi

  log "checkout ${name}@${rev}"
  git -C "${repo_dir}" fetch --filter=blob:none origin \
    '+refs/heads/*:refs/remotes/origin/*' \
    '+refs/tags/*:refs/tags/*'
  git -C "${repo_dir}" cat-file -e "${rev}^{commit}" ||
    die "locked revision is not available in ${url}: ${rev}"
  git -C "${repo_dir}" checkout --detach "${rev}"
  git -C "${repo_dir}" sparse-checkout init --no-cone
  for path in "$@"; do
    printf '/%s\n/%s/**\n' "${path}" "${path}"
  done | git -C "${repo_dir}" sparse-checkout set --stdin
  git -C "${repo_dir}" checkout --detach "${rev}"

  copy_entry_from_base "${repo_dir}" "${name}" "${dest}" "${url}" "${rev}" "${digest}" "$@"
}

download_artifact() {
  local source="$1" out="$2"

  case "${source}" in
    http://*|https://*)
      need_cmd curl
      curl -fL "${source}" -o "${out}"
      ;;
    file://*)
      cp "${source#file://}" "${out}"
      ;;
    *)
      cp "${source}" "${out}"
      ;;
  esac
}

verify_artifact_sha() {
  local file="$1"

  [[ -n "${ARTIFACT_SHA256}" ]] || die "--artifact-sha256 is required for --from-artifact"
  local actual
  actual="$(shasum -a 256 "${file}" | cut -d' ' -f1)"
  [[ "${actual}" == "${ARTIFACT_SHA256}" ]] ||
    die "artifact sha256 mismatch: expected ${ARTIFACT_SHA256}, got ${actual}"
}

artifact_member_entry() {
  local name="$1" dest="$2" url="$3" rev="$4" digest="$5"
  shift 5
  local path

  for path in "$@"; do
    printf '%s/%s\n' "${dest}" "${path}"
  done
}

make_artifact_entry() {
  local name="$1" dest="$2" url="$3" rev="$4" digest="$5"
  shift 5
  local path

  for path in "$@"; do
    [[ -e "${DEST_ROOT}/${dest}/${path}" ]] || die "cannot archive missing path: ${DEST_ROOT}/${dest}/${path}"
    printf '%s/%s\n' "${dest}" "${path}"
  done
}

run_install_from_local() {
  LOCAL_ROOT="$(cd "${LOCAL_ROOT}" && pwd)"
  read_lock ensure_replaceable_entry
  read_lock copy_local_entry
  verify_all
}

run_install_from_git() {
  read_lock ensure_replaceable_entry
  read_lock git_checkout_entry
  verify_all
}

run_install_from_artifact() {
  local tmp archive list
  tmp="$(mktemp -d)"
  archive="${tmp}/prebuilts-archive"
  list="${tmp}/prebuilts-list"

  download_artifact "${ARTIFACT}" "${archive}"
  verify_artifact_sha "${archive}"

  read_lock ensure_replaceable_entry
  read_lock artifact_member_entry | LC_ALL=C sort >"${list}"
  tar -xf "${archive}" -C "${DEST_ROOT}" -T "${list}"
}

run_make_artifact() {
  verify_all
  mkdir -p "$(dirname "${MAKE_ARTIFACT}")"
  local list
  list="$(mktemp)"
  read_lock make_artifact_entry | LC_ALL=C sort >"${list}"
  (cd "${DEST_ROOT}" && tar -czf "${MAKE_ARTIFACT}" -T "${list}")
  rm -f "${list}"
  shasum -a 256 "${MAKE_ARTIFACT}" | sed 's/^/[prebuilts] artifact sha256: /'
}

need_cmd shasum
need_cmd rsync
need_cmd tar

case "${MODE}" in
  local)
    run_install_from_local
    ;;
  git)
    run_install_from_git
    ;;
  artifact)
    [[ -n "${ARTIFACT}" ]] || die "missing artifact"
    run_install_from_artifact
    ;;
  verify)
    verify_all
    ;;
  make-artifact)
    [[ -n "${MAKE_ARTIFACT}" ]] || die "missing artifact output path"
    run_make_artifact
    ;;
  *)
    die "unknown mode: ${MODE}"
    ;;
esac

log "done"
