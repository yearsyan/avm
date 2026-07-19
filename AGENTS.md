# AGENTS.md

## Instruction Files

`AGENTS.md` is the source of truth for repository instructions. `CLAUDE.md` is
only a thin pointer for Claude-style tooling.

When opening or relying on `CLAUDE.md`, immediately read this `AGENTS.md` file
too. If a requested change appears to belong in `CLAUDE.md`, make the
substantive update here in `AGENTS.md` instead, then check this file for
adjacent stale guidance that should be updated at the same time. Keep
`CLAUDE.md` as a short redirect unless the redirect policy itself changes.

## Project Purpose

MacMu is a macOS arm64 focused cut of Android Emulator / QEMU.

The goal is to keep the emulator core needed for Android guest execution,
CPU acceleration, and GPU acceleration, while replacing the Qt desktop UI
launcher with a native AppKit product UI and removing nonessential
remote/control features. The intended architecture is:

- Host platform: macOS arm64.
- CPU acceleration: Hypervisor.Framework.
- Product display mode: visible native AppKit application launcher and per-app
  macOS windows.
- Emulator backend: headless / no Qt window; users do not interact with this
  process directly.
- Rendering: keep host GPU acceleration through gfxstream.
- UI shell: `shell/macmu`, which directly launches the headless QEMU backend,
  manages Android applications, and renders exported IOSurfaces in native
  windows.
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

Use a normal non-ATD Android 16 AOSP arm64 emulator image for graphical
validation. The source-built image flow is documented in
`docs/AOSP_OFFICIAL_IMAGES.md`; do not treat SDK ATD images as graphics proof.

The current validated target is:

```text
product=macmu_sdk_phone64_arm64
device=emu64a
variant=user
target=android-36
```

The MacMu-managed default AVD is:

```text
macmu_aosp16_arm64
```

The MacMu-managed default system image directory is:

```text
~/Library/MacMu/images/aosp16-arm64
```

The end-user system image directory must contain the optimized runtime image
set used by MacMu:

```text
advancedFeatures.ini
android-info.txt
VerifiedBootParams.textproto
encryptionkey.img
kernel-ranchu
ramdisk.img
vendor_boot.img
system-qemu.img
vendor-qemu.img
userdata.img
```

`advancedFeatures.ini` is required for the Android 16 emulator boot contract,
including `AndroidbootProps2`, dynamic partitions, virtio devices, and the
metadata/encryption-key disk slot. Treating it as optional can leave a fresh
AVD stuck with an offline adb transport.

`system-qemu.img` already contains the GPT `vbmeta` and `super` partitions.
Standalone `super.img`, raw `system.img` / `vendor.img`, and separate
`product-qemu.img` / `system_ext-qemu.img` are build/debug artifacts and should
not be shipped in the end-user image zip.

MacMu supports both release transports:

- A complete optimized ZIP produced by `scripts/package_aosp16_image.sh`.
- A v2 `manifest.json` plus content-addressed object ZIPs produced by
  `scripts/package_aosp16_chunked.py`.

The chunked transport uses 64 MiB raw chunks by default, independently
compresses each chunk, deduplicates objects by their compressed SHA-256, and
reconstructs the same validated image directory as the complete ZIP. Keep the
full ZIP for offline/backward-compatible distribution and use the manifest
form for CDN delivery. The complete contract is in
`docs/IMAGE_DISTRIBUTION.md`.

Do not use ATD images for graphics/scrcpy validation. One known-bad example is:

```text
system-images;android-35;aosp_atd;arm64-v8a
```

The ATD image reports as `sdk_slim_arm64` and uses:

```text
com.android.fakesystemapp/.launcher.EmptyHomeActivity
```

That image booted successfully but produced black/gray screenshots and scrcpy
output even under the official SDK emulator. Treat that as an image limitation,
not as proof that the QEMU-to-native-shell rendering path is broken.

## Runtime Notes

Direct backend launch shape for diagnostics (normal product runs launch
`build/cmake/distribution/emulator/macmu` instead):

```sh
ANDROID_EMULATOR_LAUNCHER_DIR=build/cmake/distribution/emulator \
DYLD_LIBRARY_PATH=build/cmake/distribution/emulator/lib64:build/cmake/distribution/emulator/lib64/gles_angle:build/cmake/distribution/emulator/lib64/vulkan \
build/cmake/distribution/emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless \
  -avd macmu_aosp16_arm64 \
  -sysdir "$HOME/Library/MacMu/images/aosp16-arm64" \
  -no-window -no-audio -no-snapshot -no-boot-anim \
  -gpu host
```

No `ANDROID_SDK_ROOT` / `ANDROID_HOME` is required: qemu resolves the AVD's
image search path from `-sysdir` directly, and the AVD itself is located via
`ANDROID_AVD_HOME` or MacMu's managed AVD home under `~/Library/MacMu/avd`
(independent of the SDK root). The shell passes the same `-sysdir` through when
launched with `--system-path <dir>` (or the `MACMU_SYSTEM_PATH` /
`AEMU_SHELL_SYSTEM_PATH` env var). Product-style shell runs default to
`~/Library/MacMu/images/aosp16-arm64` and can import either a complete AOSP16
image ZIP, a chunk `manifest.json`, or a directory containing `manifest.json`
into that directory. The first-run source can also be supplied with
`--import-image` / `MACMU_IMPORT_IMAGE`; HTTPS sources must be manifests.
Verified remote objects are resumed and cached under
`~/Library/MacMu/cache/image-objects`.

When the managed image is absent and no explicit source is set, MacMu waits at
the setup screen instead of downloading automatically. The user chooses either
**Official Image** or **Other Source…**. The official option imports:

```text
https://storage.macmu.org/images/aosp16-arm64/manifest.json
```

**Other Source…** accepts a local ZIP, manifest, or manifest directory. An
explicit `--import-image` / `MACMU_IMPORT_IMAGE` source still starts unattended.
Automation can opt into unattended official-image initialization with
`--auto-image-import` or `MACMU_AUTO_IMPORT_IMAGE=1`; `--no-auto-image-import`
forces the interactive chooser when a wrapper environment enables it.

If a previous emulator session was killed while it was writing a snapshot, the
next cold boot can segfault inside `drive_init` / `blk_bs` (the qcow2-on-qcow2
snapshot restore dereferences a NULL `BlockBackend`). That is an AVD state
problem, not a build/pruning regression — the official SDK emulator hits it too
on the same AVD. Recover by wiping the AVD's userdata/snapshot state:

```sh
# add -wipe-data to the launch flags once to reset the AVD, then boot normally
... qemu-system-aarch64-headless -avd macmu_aosp16_arm64 -no-window \
    -no-audio -no-snapshot -no-boot-anim -wipe-data -gpu host
```

Alternatively remove the AVD lock files:
`rm -f ~/Library/MacMu/avd/macmu_aosp16_arm64.avd/*.lock`

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

This tree is permanently configured as **MacMu core-only**: macOS arm64, a
headless/no-Qt QEMU backend consumed by the visible native AppKit shell, HVF CPU
acceleration + gfxstream GPU acceleration + Vulkan runtime, no upstream Qt UI,
no WebRTC/netsim/modem-simulator/recording/virtualscene/telephony-gRPC, and a
single aarch64 guest architecture. The `OPTION_AEMU_CORE_ONLY` CMake variable
is a constant `TRUE` (kept so legacy `if(NOT OPTION_AEMU_CORE_ONLY)` guards
resolve to the disabled branch); `AEMU_CORE_ONLY=1` is always defined as a
compile definition because some backend source files use it to select headless
stubs.

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

For Android 16 validation, use the checklist in `docs/AOSP_OFFICIAL_IMAGES.md`.
Expected evidence includes `sys.boot_completed=1`, `ro.build.type=user`,
`ro.debuggable=0`, `ro.secure=1`, virtio block/GPU modules loaded, and
SurfaceFlinger reporting host Apple GPU OpenGL/Vulkan paths. Older android-35
default-image boot evidence remains useful only as a historical graphics
baseline for this pruned tree.
