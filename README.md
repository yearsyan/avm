# MacMu

macOS arm64 headless Android Emulator / QEMU build.

- Guest backend: `qemu-system-aarch64-headless`
- UI shell: `macmu`
- Display path: gfxstream final frames exported through IOSurface
- No Qt emulator launcher in the core-only distribution

Graphics architecture: [docs/GRAPHICS_ARCHITECTURE.md](docs/GRAPHICS_ARCHITECTURE.md)

AOSP image build and launch notes:
[docs/AOSP_OFFICIAL_IMAGES.md](docs/AOSP_OFFICIAL_IMAGES.md)

The Android 16 `user` system image is distributed separately from the small
MacMu app DMG. MacMu supports both a complete offline ZIP and a CDN-friendly
chunk manifest.

Build the complete archive with:

```sh
./scripts/package_aosp16_image.sh \
  --source-dir <aosp16-sysdir> \
  --output macmu-aosp16-arm64-system-image.zip
```

Build the chunked form with:

```sh
./scripts/package_aosp16_chunked.py \
  --source-dir <aosp16-sysdir> \
  --output-dir macmu-aosp16-arm64-chunked
```

On first launch with no managed image, MacMu automatically imports:

```text
https://storage.macmu.org/images/aosp16-arm64/manifest.json
```

The **Import Image…** picker remains available for a complete ZIP or local
`manifest.json`. Override the hosted source with `--import-image <source>`, or
disable the automatic download with `--no-auto-image-import`. MacMu validates
and installs the image, creates its managed AVD, and starts Android
automatically.

Packaging, manifest, CDN, and cache details:
[docs/IMAGE_DISTRIBUTION.md](docs/IMAGE_DISTRIBUTION.md)

## Build

```sh
./scripts/build_headless_macos.sh
```

Output:

```text
build/cmake/distribution/emulator/
```

GitHub Actions artifact:

```text
macmu-macos-arm64
```

It contains:

```text
macmu-macos-arm64.zip
macmu-macos-arm64-app.zip
macmu-macos-arm64.dmg
```

## Run

```sh
./build/cmake/distribution/emulator/macmu
```

Default AVD:

```text
macmu_aosp16_arm64
```

Use another AVD:

```sh
./build/cmake/distribution/emulator/macmu --avd <name>
```

## License

`shell/` is MIT licensed.

QEMU, gfxstream, Android Emulator, and bundled runtime components keep their
upstream licenses and notices. Keep the generated license and notice files when
redistributing builds.
