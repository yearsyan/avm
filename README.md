# MacMu

macOS arm64 headless Android Emulator / QEMU build.

- Guest backend: `qemu-system-aarch64-headless`
- UI shell: `macmu`
- Display path: gfxstream final frames exported through IOSurface
- No Qt emulator launcher in the core-only distribution

Graphics architecture: [docs/GRAPHICS_ARCHITECTURE.md](docs/GRAPHICS_ARCHITECTURE.md)

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
aemu_aosp35_arm64
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
