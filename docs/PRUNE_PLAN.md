# AEMU macOS arm64 Core Prune

This tree is an initial standalone cut of Android Emulator's QEMU core for
macOS arm64. It keeps the upstream directory shape so existing CMake files still
resolve paths, but removes the external Qt UI surface and large non-macOS build
payloads first.

## Scope

- Target platform: macOS arm64 only.
- Retained runtime shape: `emulator` launcher plus `qemu-system-aarch64-headless`.
- Retained acceleration: hvf, gfxstream, ANGLE/SwiftShader/Vulkan/VirGL prebuilts.
- Removed UI goal: no Qt launcher UI, no extended pages, no Qt widgets, no Qt
  distribution.
- External control: should be done through the emulator control surface
  (`grpc`/`protobuf`) or a thin host process that launches the headless core.

## Copied From Android Emulator

- `external/qemu`, excluding generated objects, Qt shell modules, tests, docs,
  linux-user/bsd-user, qga, roms, and misc contribution payloads.
- `hardware/google/aemu`
- `hardware/google/gfxstream`
- macOS arm64 build tools under `prebuilts/{cmake,ninja,python,clang}`.
- macOS arm64 runtime prebuilts from `prebuilts/android-emulator-build/common`.

Qt prebuilts are intentionally not copied.

## Dependency Strategy

Internal emulator prebuilts that are not practical Conan packages yet are copied
directly. General external libraries should move to Conan and be introduced via
`conanfile.py` plus `cmake/aemu-conan-deps.cmake`.

First Conan targets to wire:

- abseil
- protobuf
- grpc
- openssl / c-ares / zlib
- libpng
- libusb
- libxml2
- flatbuffers
- re2
- lz4
- fmt
- flex / bison / nasm as build tools

## Current Build State

This is not expected to be fully buildable immediately after the copy. The first
patch only adds `OPTION_AEMU_NO_QT_UI` and prevents headful Qt QEMU targets from
being generated. The next required step is replacing AOSP `external/*`
subdirectories with package targets from Conan in the CMake config layer.

The intended configure line after dependency wiring is:

```sh
conan install . -of build/conan -s os=Macos -s arch=armv8 -s build_type=Release -r conancenter --build=missing
cmake -S external/qemu -B build/cmake -DCMAKE_TOOLCHAIN_FILE=build/conan/conan_toolchain.cmake -DANDROID_TARGET_TAG=darwin-aarch64
cmake --build build/cmake --target emulator qemu-system-aarch64-headless
```

`conan install --build=never -r conancenter` currently resolves the dependency
graph but reports missing macOS arm64 prebuilts for bison, flatbuffers, grpc,
libbacktrace, libpng, libxml2, openssl, and protobuf. A real build needs
`--build=missing` or a private binary cache for these packages.

## Next Prune Pass

1. Convert `android/build/cmake/config/*.cmake` away from AOSP `external/*`
   source imports.
2. Split recording out of `aemu-ui-window` so `ffmpeg`, `x264`, and `libvpx`
   can become optional.
3. Keep only `aarch64-softmmu` device files and remove x86/armel device stubs.
4. Replace launcher assumptions so the external UI can own process lifecycle,
   windowing, texture import, and input injection.
5. Add a direct texture-export boundary if the UI is in-process, or keep gRPC
   control if the UI remains a separate process.
