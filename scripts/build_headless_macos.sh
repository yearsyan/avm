#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build/cmake}"
CONAN_DIR="${CONAN_DIR:-${BUILD_DIR}/conan}"
MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-11.0}"
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-${MACOS_DEPLOYMENT_TARGET}}"

conan install "${ROOT}" \
  -of "${CONAN_DIR}" \
  -s os=Macos \
  -s os.version="${MACOS_DEPLOYMENT_TARGET}" \
  -s arch=armv8 \
  -s build_type=Release \
  -r conancenter \
  --build=missing

cmake -S "${ROOT}/external/qemu" -B "${BUILD_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${ROOT}/external/qemu/android/build/cmake/toolchain-darwin-aarch64.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}" \
  -DAEMU_CONAN_DEPS_DIR="${CONAN_DIR}" \
  -DOPTION_CRASHUPLOAD=NONE \
  -DGFXSTREAM=ON \
  -DANDROID_TARGET_TAG=darwin-aarch64

# gfxstream_backend is dlopen'd at runtime by libgfxstream_backend.dylib; it is
# not pulled in by the headless/emulator link line, so build it explicitly.
# Conan shared runtime dylibs are copied into lib64 by CMake.
cmake --build "${BUILD_DIR}" --target aemu-conan-shared-runtime qemu-system-aarch64-headless emulator gfxstream_backend

# Populate ${BUILD_DIR}/distribution/emulator for packaging.
cmake --build "${BUILD_DIR}" --target install
