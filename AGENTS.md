# AGENTS.md

## Project Purpose

MacMu is a macOS arm64 focused cut of Android Emulator / QEMU.

The goal is to keep the emulator core needed for Android guest execution,
CPU acceleration, and GPU acceleration, while removing the Qt desktop UI
launcher and nonessential remote/control features. The intended architecture is:

- Host platform: macOS arm64.
- CPU acceleration: Hypervisor.Framework.
- Display mode: headless / no Qt window.
- Rendering: keep host GPU acceleration through gfxstream.
- UI shell: `shell/macmu`, which directly launches qemu headless
  and renders exported IOSurfaces.
- Licensing boundary: the shell source is MIT-licensed; qemu, gfxstream,
  Android Emulator, and bundled runtime components keep their upstream licenses
  and notices.
- Removed or avoided where possible: Qt UI, WebRTC, emulator desktop shell,
  recording, virtual scene, modem/netsim/control surfaces that are not needed
  for core execution.

The main runnable distribution is:

```sh
build/cmake/distribution/emulator
```

The shell entry binary is:

```sh
build/cmake/distribution/emulator/macmu
```

The qemu backend binary is:

```sh
build/cmake/distribution/emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless
```

## Image Type

Use a normal non-ATD AOSP arm64 system image for graphical validation:

```text
system-images;android-35;default;arm64-v8a
```

The local validation AVD created for this image is:

```text
aemu_aosp35_arm64
```

This image reports as:

```text
product=sdk_phone64_arm64
model=Android_SDK_built_for_arm64
```

It has a real Launcher:

```text
com.android.launcher3/.uioverrides.QuickstepLauncher
```

Do not use the ATD image for graphics/scrcpy validation:

```text
system-images;android-35;aosp_atd;arm64-v8a
```

The ATD image reports as `sdk_slim_arm64` and uses:

```text
com.android.fakesystemapp/.launcher.EmptyHomeActivity
```

That image booted successfully but produced black/gray screenshots and scrcpy
output even under the official SDK emulator. Treat that as an image limitation,
not as proof that the headless core rendering path is broken.

## Runtime Notes

Recommended launch shape:

```sh
ANDROID_SDK_ROOT=<android-sdk> \
ANDROID_HOME=<android-sdk> \
ANDROID_EMULATOR_LAUNCHER_DIR=build/cmake/distribution/emulator \
DYLD_LIBRARY_PATH=build/cmake/distribution/emulator/lib64:build/cmake/distribution/emulator/lib64/gles_angle:build/cmake/distribution/emulator/lib64/vulkan \
build/cmake/distribution/emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless \
  -avd aemu_aosp35_arm64 \
  -no-window -no-audio -no-snapshot -no-boot-anim \
  -gpu host
```

If a previous emulator session was killed while it was writing a snapshot, the
next cold boot can segfault inside `drive_init` / `blk_bs` (the qcow2-on-qcow2
snapshot restore dereferences a NULL `BlockBackend`). That is an AVD state
problem, not a build/pruning regression — the official SDK emulator hits it too
on the same AVD. Recover by wiping the AVD's userdata/snapshot state:

```sh
# add -wipe-data to the launch flags once to reset the AVD, then boot normally
... qemu-system-aarch64-headless -avd aemu_aosp35_arm64 -no-window \
    -no-audio -no-snapshot -no-boot-anim -wipe-data -gpu host
```

Alternatively remove the AVD lock files:
`rm -f ~/.android/avd/aemu_aosp35_arm64.avd/*.lock`

GPU acceleration is expected to show host Apple GPU paths, for example:

```text
GLES: Google (Apple), Android Emulator OpenGL ES Translator (Apple M4)
ANDROID_EMU_vulkan
composition=DEVICE
```

The Vulkan runtime is intentionally included in the distribution at:

```text
lib64/vulkan
```

Successful Vulkan runtime initialization should include log lines similar to:

```text
Added library: .../libvulkan.dylib
Found 1 Vulkan physical device(s)
Considering Vulkan physical device 0 : Apple M4
ColorBuffer ... VK_FORMAT_...
```

WebGL tests such as `https://webglsamples.org/aquarium/aquarium.html` running
at 60 Hz are useful practical evidence that the guest WebView/WebGL path is
using GPU acceleration, but emulator startup logs and SurfaceFlinger output are
the stronger source of truth for host GPU/Vulkan state.

## Build configuration (core-only)

This tree is permanently configured as **MacMu core-only**: macOS arm64,
headless, HVF CPU acceleration + gfxstream GPU acceleration + Vulkan runtime,
no Qt UI shell, no WebRTC/netsim/modem-simulator/recording/virtualscene/telephony-gRPC,
single aarch64 guest architecture. The `OPTION_AEMU_CORE_ONLY` CMake variable is
a constant `TRUE` (kept so legacy `if(NOT OPTION_AEMU_CORE_ONLY)` guards resolve
to the disabled branch); `AEMU_CORE_ONLY=1` is always defined as a compile
definition because some source files use it to select headless stubs.

Configure + build (Ninja):

```sh
cmake -S external/qemu -B build/cmake -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=external/qemu/android/build/cmake/toolchain-darwin-aarch64.cmake \
  -DANDROID_TARGET_TAG=darwin-aarch64 -DGFXSTREAM=ON
ninja -C build/cmake qemu-system-aarch64-headless
cmake --install build/cmake --config Release
cmake -S shell -B build/cmake/shell -DMACMU_BINARY_NAME=macmu
cmake --build build/cmake/shell --target macmu_shell
cmake --install build/cmake/shell --prefix build/cmake/distribution/emulator
scripts/package_macos_app.sh \
  --dist-dir build/cmake/distribution/emulator \
  --out-dir build/cmake/package \
  --package-basename macmu-macos-arm64
```

The toolchain expects a populated `build/cmake/toolchain/` (compilers)
and `prebuilts/` (binary deps), restored via `android/scripts/unix/build-qemu-android.sh`.

Verified boot evidence on this pruned tree (aemu_aosp35_arm64, `-wipe-data`
recovery boot): `Boot completed in 10571 ms`, `GLES: Google (Apple), Android
Emulator OpenGL ES Translator (Apple M4), OpenGL ES 3.0 (4.1 Metal - 90.5)`,
`Selecting Vulkan device: Apple M4`, guest `[ro.hardware.vulkan]: [ranchu]`,
SurfaceFlinger rendering 110 active layers (launcher/status bar/settings).
