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
  -DOPTION_AEMU_NO_QT_UI=ON \
  -DOPTION_MINBUILD=ON \
  -DOPTION_CRASHUPLOAD=OFF \
  -DGFXSTREAM=ON \
  -DANDROID_TARGET_TAG=darwin-aarch64

cmake --build "${BUILD_DIR}" --target qemu-system-aarch64-headless emulator
