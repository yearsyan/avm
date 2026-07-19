# MacMu

MacMu is a native macOS Android application runtime for Apple silicon, built
from a focused Android Emulator / QEMU core.

MacMu is **not a headless end-user application**. It provides a visible AppKit
launcher and opens Android applications in native macOS windows. Only the QEMU
backend runs without its own window: the upstream Qt emulator UI is removed,
while MacMu renders gfxstream output through IOSurface and Metal.

![MacMu Applications window showing Android apps on macOS](docs/images/macmu-applications.jpeg)

## Features

- Native AppKit Applications window with search and refresh.
- Android applications open in independent, resizable macOS windows.
- Hypervisor.Framework acceleration on macOS arm64.
- Host GPU rendering through gfxstream, Vulkan/MoltenVK, IOSurface, and Metal.
- Keyboard input through an Android UHID device, plus pointer and touch input.
- Install one or more APKs with the `+` button or by dragging them into the
  Applications window after Android is ready.
- Right-click uninstall for removable applications; protected system apps are
  identified and cannot be uninstalled from the UI.
- Up to five Android application windows can run at the same time.
- Android 16 AOSP `user` image initialization from the MacMu-hosted image or a
  local ZIP, chunk manifest, or manifest directory.

## Architecture

| Component | Role |
| --- | --- |
| `macmu` | Visible native macOS shell: setup, application launcher, input, and per-app windows. |
| `qemu-system-aarch64-headless` | Hidden Android guest backend; no Qt window or upstream emulator desktop UI. |
| Hypervisor.Framework | Arm64 CPU virtualization. |
| gfxstream + Vulkan/OpenGL | Guest GPU acceleration on the host Apple GPU. |
| IOSurface + Metal | GPU frame sharing and presentation in native macOS windows. |
| MacMu guest agent | Application discovery, launch/close, APK install, uninstall, and input control. |

The shell launches QEMU directly and uses Android virtual displays to give each
opened application its own host window. Pixels stay on the GPU path; the control
channel carries display metadata and events rather than copied frame payloads.

See [Graphics Architecture](docs/GRAPHICS_ARCHITECTURE.md) for the rendering
and frame-channel design.

## Requirements

- Apple silicon Mac (`arm64`).
- macOS 11 or later.
- A supported Android 16 AOSP arm64 image. The image is distributed separately
  from the smaller MacMu application package.

The currently validated guest target is:

```text
product=macmu_sdk_phone64_arm64
device=emu64a
variant=user
target=android-36
```

## First Launch

When no managed image is installed, MacMu waits for a source choice and does
not download anything automatically:

- **Official Image** imports the MacMu-hosted Android 16 image manifest.
- **Other Source…** accepts a complete image ZIP, a local `manifest.json`, or a
  directory containing `manifest.json` and its relative objects.

The official manifest is:

```text
https://storage.macmu.org/images/aosp16-arm64/manifest.json
```

After validation and reconstruction, MacMu installs the image, prepares its
managed AVD, starts Android, and loads the Applications window.

For unattended initialization, provide an explicit source:

```sh
build/cmake/distribution/emulator/macmu \
  --import-image /path/to/image.zip

build/cmake/distribution/emulator/macmu \
  --import-image /path/to/chunk-directory

build/cmake/distribution/emulator/macmu \
  --import-image https://cdn.example.com/android/manifest.json
```

Automation can select the MacMu-hosted source with `--auto-image-import` or
`MACMU_AUTO_IMPORT_IMAGE=1`.

See [Android Image Distribution](docs/IMAGE_DISTRIBUTION.md) and
[AOSP Image Build and Validation](docs/AOSP_OFFICIAL_IMAGES.md) for the full
image contract.

## Build

Configure and build the no-Qt backend plus the native MacMu shell with:

```sh
./scripts/build_headless_macos.sh
```

The script retains its historical name: “headless” refers to the QEMU target,
not to the MacMu application.

The runnable distribution is written to:

```text
build/cmake/distribution/emulator/
```

The main entry point is:

```sh
./build/cmake/distribution/emulator/macmu
```

Release packaging produces:

```text
macmu-macos-arm64.zip
macmu-macos-arm64-app.zip
macmu-macos-arm64.dmg
```

The system image is packaged separately. Build its complete and chunked forms
with:

```sh
./scripts/package_aosp16_image.sh \
  --source-dir <aosp16-sysdir> \
  --output macmu-aosp16-arm64-system-image.zip

./scripts/package_aosp16_chunked.py \
  --source-dir <aosp16-sysdir> \
  --output-dir macmu-aosp16-arm64-chunked
```

## Run

Launch the native shell:

```sh
./build/cmake/distribution/emulator/macmu
```

MacMu manages its default image and AVD under:

```text
~/Library/MacMu/images/aosp16-arm64
~/Library/MacMu/avd/macmu_aosp16_arm64.avd
```

The default AVD name is `macmu_aosp16_arm64`. Development runs can override it
with `--avd <name>` and can override the image directory with
`--system-path <dir>`.

## Documentation

- [Graphics Architecture](docs/GRAPHICS_ARCHITECTURE.md)
- [Android Image Distribution](docs/IMAGE_DISTRIBUTION.md)
- [AOSP Image Build and Validation](docs/AOSP_OFFICIAL_IMAGES.md)
- [Frame Channel v2 Control Plane](docs/FRAME_CHANNEL_V2_CONTROL_PLANE.md)
- [Prebuilt Dependencies](docs/PREBUILTS.md)

## License

The native shell under `shell/` is MIT licensed.

QEMU, gfxstream, Android Emulator, and bundled runtime components retain their
upstream licenses and notices. Keep the generated license and notice files when
redistributing builds.
