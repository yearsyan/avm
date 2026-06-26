#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build/cmake}"
CONAN_DIR="${CONAN_DIR:-${BUILD_DIR}/conan}"
MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-11.0}"
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-${MACOS_DEPLOYMENT_TARGET}}"
CMAKE_GENERATOR="${CMAKE_GENERATOR:-Ninja}"

if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  cached_generator="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt" | head -n 1)"
  if [[ -n "${cached_generator}" ]]; then
    CMAKE_GENERATOR="${cached_generator}"
  fi
fi

if [[ -z "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]]; then
  CMAKE_BUILD_PARALLEL_LEVEL="$(
    sysctl -n hw.logicalcpu 2>/dev/null ||
      sysctl -n hw.ncpu 2>/dev/null ||
      printf ''
  )"
fi

cmake_build() {
  local args=(--build "${BUILD_DIR}")
  if [[ -n "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]]; then
    args+=(--parallel "${CMAKE_BUILD_PARALLEL_LEVEL}")
  else
    args+=(--parallel)
  fi
  args+=("$@")
  cmake "${args[@]}"
}

conan install "${ROOT}" \
  -of "${CONAN_DIR}" \
  -s os=Macos \
  -s os.version="${MACOS_DEPLOYMENT_TARGET}" \
  -s arch=armv8 \
  -s build_type=Release \
  -r conancenter \
  --build=missing

cmake_args=(
  -S "${ROOT}/external/qemu"
  -B "${BUILD_DIR}"
  -G "${CMAKE_GENERATOR}"
  -DCMAKE_TOOLCHAIN_FILE="${ROOT}/external/qemu/android/build/cmake/toolchain-darwin-aarch64.cmake"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}"
  -DCMAKE_SKIP_INSTALL_ALL_DEPENDENCY=ON
  -DAEMU_CONAN_DEPS_DIR="${CONAN_DIR}"
  -DOPTION_CRASHUPLOAD=NONE
  -DGFXSTREAM=ON
  -DANDROID_TARGET_TAG=darwin-aarch64
)

if [[ -n "${OPTION_CCACHE:-}" ]]; then
  cmake_args+=("-DOPTION_CCACHE=${OPTION_CCACHE}")
elif command -v ccache >/dev/null 2>&1; then
  cmake_args+=("-DOPTION_CCACHE=$(command -v ccache)")
fi

cmake "${cmake_args[@]}"

# gfxstream_backend is dlopen'd at runtime by libgfxstream_backend.dylib; it is
# not pulled in by the headless/emulator link line, so build it explicitly.
# Conan shared runtime dylibs are copied into lib64 by CMake.
cmake_build --target \
  aemu-conan-shared-runtime \
  glib2_darwin-aarch64 \
  mksdcard \
  android-emu-agents \
  android-emu-base-logging \
  android-emu-metrics \
  android-emu-protos \
  android-emu-shared \
  emugl_common \
  ANGLE_DEPENDENCIES \
  E2FSPROGS_DEPENDENCIES \
  VULKAN_DEPENDENCIES \
  gfxstream_backend \
  emulator \
  qemu-system-aarch64-headless

# Populate ${BUILD_DIR}/distribution/emulator for packaging.
cmake --install "${BUILD_DIR}" --config Release
