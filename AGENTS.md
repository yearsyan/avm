# AGENTS.md

## Project Purpose

This project is a macOS arm64 focused cut of Android Emulator / QEMU.

The goal is to keep the emulator core needed for Android guest execution,
CPU acceleration, and GPU acceleration, while removing the external desktop UI
shell and nonessential remote/control features. The intended architecture is:

- Host platform: macOS arm64.
- CPU acceleration: Hypervisor.Framework.
- Display mode: headless / no Qt window.
- Rendering: keep host GPU acceleration through gfxstream.
- UI shell: provided externally by the integrator, not by Qt.
- Removed or avoided where possible: Qt UI, WebRTC, emulator desktop shell,
  recording, virtual scene, modem/netsim/control surfaces that are not needed
  for core execution.

The main runnable distribution is:

```sh
/Users/u/workspace/aemu/external/qemu/objs/distribution/emulator
```

The primary binary is:

```sh
/Users/u/workspace/aemu/external/qemu/objs/distribution/emulator/emulator
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
ANDROID_SDK_ROOT=/Users/u/android-sdk \
ANDROID_HOME=/Users/u/android-sdk \
DYLD_LIBRARY_PATH=/Users/u/workspace/aemu/external/qemu/objs/distribution/emulator/lib64 \
/Users/u/workspace/aemu/external/qemu/objs/distribution/emulator/emulator \
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
... emulator -avd aemu_aosp35_arm64 -no-window -no-audio -no-snapshot \
    -no-boot-anim -wipe-data -gpu host
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

This tree is permanently configured as **AEMU core-only**: macOS arm64,
headless, HVF CPU acceleration + gfxstream GPU acceleration + Vulkan runtime,
no Qt UI shell, no WebRTC/netsim/modem-simulator/recording/virtualscene/telephony-gRPC,
single aarch64 guest architecture. The `OPTION_AEMU_CORE_ONLY` CMake variable is
a constant `TRUE` (kept so legacy `if(NOT OPTION_AEMU_CORE_ONLY)` guards resolve
to the disabled branch); `AEMU_CORE_ONLY=1` is always defined as a compile
definition because some source files use it to select headless stubs.

Configure + build (Ninja):

```sh
cmake -S external/qemu -B external/qemu/objs -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=external/qemu/android/build/cmake/toolchain-darwin-aarch64.cmake \
  -DANDROID_TARGET_TAG=darwin-aarch64 -DGFXSTREAM=ON
ninja -C external/qemu/objs qemu-system-aarch64-headless emulator
```

The toolchain expects a populated `external/qemu/objs/toolchain/` (compilers)
and `prebuilts/` (binary deps), restored via `android/scripts/unix/build-qemu-android.sh`.

Verified boot evidence on this pruned tree (aemu_aosp35_arm64, `-wipe-data`
recovery boot): `Boot completed in 10571 ms`, `GLES: Google (Apple), Android
Emulator OpenGL ES Translator (Apple M4), OpenGL ES 3.0 (4.1 Metal - 90.5)`,
`Selecting Vulkan device: Apple M4`, guest `[ro.hardware.vulkan]: [ranchu]`,
SurfaceFlinger rendering 110 active layers (launcher/status bar/settings).
