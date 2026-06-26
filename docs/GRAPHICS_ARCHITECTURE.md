# MacMu Graphics Architecture

MacMu is split into two processes:

- `MacMu.app/Contents/MacOS/macmu`: the macOS shell and app entry point.
- `qemu-system-aarch64-headless`: the Android guest runtime and gfxstream host.

The shell does not render Android UI by itself. Android renders inside the guest,
gfxstream composes the guest frame on the host GPU, and MacMu displays the final
exported frame.

## Process Layout

```text
MacMu.app
  Contents/MacOS/macmu
  Contents/Resources/emulator/
    qemu/darwin-aarch64/qemu-system-aarch64-headless
    lib64/
    lib64/gles_angle/
    lib64/vulkan/
```

When launched normally, `macmu` starts qemu with:

```text
-avd <name> -no-window -no-audio -no-snapshot -no-boot-anim -gpu host
```

The shell sets:

```text
MACMU_IOSURFACE_EXPORT=1
MACMU_IOSURFACE_EXPORT_PATH=/tmp/macmu-iosurface.json
ANDROID_EMULATOR_LAUNCHER_DIR=<app>/Contents/Resources/emulator
DYLD_LIBRARY_PATH=<emulator>/lib64:<emulator>/lib64/gles_angle:<emulator>/lib64/vulkan
```

For transition compatibility with older local gfxstream builds, the shell also
sets the old `AEMU_IOSURFACE_EXPORT` and `AEMU_IOSURFACE_EXPORT_PATH` variables.

## Frame Export Path

The guest Android stack renders through the existing emulator/gfxstream path:

```text
Android guest
  -> gralloc / hwcomposer / SurfaceFlinger
  -> virtio-gpu / gfxstream
  -> host ColorBuffer / DisplayVk or DisplayGl
  -> IOSurface export
  -> macmu Metal view
```

MacMu adds an IOSurface sink at the final display post stage:

- Vulkan path: `DisplayVk::postToIosurface()`.
- OpenGL path: `DisplayGl::post()` with `IosurfaceGlDisplaySink`.

The sink currently exports the first posted display layer. Rotation and color
transforms are intentionally ignored in this first implementation.

## qemu to Shell Communication

There is no socket, gRPC, shared command channel, or frame byte stream between
qemu and the shell.

The communication contract is:

1. qemu/gfxstream creates or reuses a global IOSurface.
2. qemu/gfxstream GPU-blits the final ColorBuffer into that IOSurface.
3. qemu/gfxstream writes a small JSON metadata file atomically.
4. `macmu` polls the metadata file.
5. `macmu` calls `IOSurfaceLookup(iosurface_id)`.
6. `macmu` imports the IOSurface as a Metal texture and draws it.

The metadata file looks like:

```json
{
  "iosurface_id": 69,
  "width": 1080,
  "height": 2400,
  "pixel_format": "BGRA8Unorm",
  "frame": 70,
  "timestamp_ns": 929738659083416
}
```

The JSON file is only a handle exchange and frame counter. Pixel data is not
serialized through the file.

## Copy and Synchronization Model

The current implementation is deliberately simple:

- qemu/gfxstream owns the source ColorBuffer.
- qemu/gfxstream owns the exported IOSurface target.
- Each frame is copied with a GPU blit/copy into the IOSurface target.
- Metadata is published only after the frame copy completes.
- The shell samples the IOSurface through Metal.

So the shell-side display does not copy pixels on the CPU, but the current
gfxstream export path does perform a GPU copy from the final ColorBuffer into
the IOSurface. A future optimization could make the final ColorBuffer itself
IOSurface-backed and remove this extra GPU blit.

## Shell Rendering

`macmu` uses AppKit + MetalKit:

- `MTKView` owns the drawable.
- `IOSurfaceLookup()` opens the qemu-published surface.
- `newTextureWithDescriptor:iosurface:plane:` imports it into Metal.
- A full-screen triangle samples the IOSurface texture.
- The Metal viewport is aspect-fit, so resizing the window preserves guest
  aspect ratio and uses letterboxing instead of stretching.

The shell is also responsible for child process lifetime. It launches qemu in a
new process group and terminates that group when the app exits.

## Packaging

CI produces three artifacts:

- `macmu-macos-arm64.zip`: raw distribution layout.
- `macmu-macos-arm64-app.zip`: `MacMu.app`.
- `macmu-macos-arm64.dmg`: installable DMG with `MacMu.app` and an
  `/Applications` symlink.

The app bundle keeps qemu and its runtime libraries under
`Contents/Resources/emulator`. The app entry point remains `Contents/MacOS/macmu`.

## Current Limitations

- Single display export path.
- Only the first posted layer is exported.
- Rotation and color transforms are ignored.
- Frame delivery is metadata polling, not event-driven.
- There is an extra GPU blit from ColorBuffer to IOSurface.
- The `AEMU_*` exporter variables are still emitted for compatibility with
  older local builds; they can be removed after all builds consume `MACMU_*`.
