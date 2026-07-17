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
ANDROID_EMULATOR_WRAPPER_PID=<macmu pid>
MACMU_FRAME_DOORBELL_FD=<inherited child fd>
ANDROID_EMULATOR_LAUNCHER_DIR=<app>/Contents/Resources/emulator
DYLD_LIBRARY_PATH=<emulator>/lib64:<emulator>/lib64/gles_angle:<emulator>/lib64/vulkan
```

For transition compatibility with older local gfxstream builds, the shell also
sets the old `AEMU_IOSURFACE_EXPORT` and `AEMU_FRAME_DOORBELL_FD` variables.
It no longer sets `MACMU_IOSURFACE_EXPORT_PATH` or
`AEMU_IOSURFACE_EXPORT_PATH`.

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

MacMu adds IOSurface sinks at two stages:

- Display 0 (final present): `DisplayVk::postToIosurface()` on the Vulkan
  path, `DisplayGl::post()` with `IosurfaceGlDisplaySink` on the OpenGL path.
- Secondary displays (GL composition only): `PostWorkerGl::composeImpl`
  exports each display's composition target ColorBuffer right after its
  guest compose completes (`exportComposedDisplay`, sharing the blit/publish
  machinery in `host/gl/iosurface_gl_export.{h,cpp}`).

Each display publishes into its own frame-channel slot (slot index ==
Android display id). Rotation and color transforms are intentionally ignored
in this first implementation.

## qemu to Shell Communication

Pixel data is not sent through a socket, file, gRPC stream, or shared command
channel. The IOSurface itself carries the GPU pixels across the process
boundary. The only frame control plane between qemu and the shell is a small
POSIX shared-memory metadata payload plus an inherited Unix datagram socket
doorbell.

The communication contract is:

1. `macmu` creates a POSIX shm object named `/macmu.frame.<wrapper-pid>`, where
   `<wrapper-pid>` is the shell pid exported to qemu as
   `ANDROID_EMULATOR_WRAPPER_PID`.
2. `macmu` creates an `AF_UNIX` `SOCK_DGRAM` socketpair and passes the producer
   endpoint to qemu by fd inheritance. The inherited child fd is advertised
   through `MACMU_FRAME_DOORBELL_FD` and, for transition compatibility,
   `AEMU_FRAME_DOORBELL_FD`.
3. qemu/gfxstream opens the existing shm object, validates its magic/version,
   and uses the inherited doorbell fd.
4. qemu/gfxstream creates or reuses a global IOSurface.
5. qemu/gfxstream publishes a posted ColorBuffer's IOSurface when available;
   otherwise it GPU-blits the final ColorBuffer into an export IOSurface.
6. qemu/gfxstream writes the latest metadata into shm, publishes the frame
   number last with release ordering, then rings the doorbell best-effort.
7. `macmu` waits on the doorbell, drains coalesced notifications, reads the
   shm payload with a seqlock-style frame-counter check, and calls
   `IOSurfaceLookup(iosurface_id)`.
8. `macmu` imports the IOSurface as a Metal texture and draws it.

The shm wire layout (protocol v2) is a header plus a fixed table of 16
per-display slots; slot index == Android display id:

```c
struct ShmHeader {
    uint64_t magic;          // 'MACMUFRM'
    uint32_t version;        // 2
    uint32_t payloadOffset;  // offset of slot[0]
    uint32_t slotCount;      // 16
    uint32_t slotStride;     // 64
    uint64_t reserved[2];
};

struct ShmDisplaySlot {      // one cache line per display
    uint64_t seq;            // odd/even seqlock; odd while producer writes
    uint64_t frame;          // monotonic per slot; 0 = never published
    uint32_t iosurfaceId;
    uint32_t width;
    uint32_t height;
    uint32_t dpi;            // 0 = unknown (control plane is authoritative)
    uint32_t pixelFormat;    // fourcc, 'BGRA'
    uint32_t flags;          // bit0: primary display
    uint64_t timestampNs;
    uint8_t  pad[16];
};
```

One doorbell serves all displays; its datagram payload
(`{displayId, reserved, frame}`) is advisory and the consumer re-scans the
slot table on any wake. Display add/remove/list rides the separate MMCP
control channel on inherited fd 197 (see
docs/FRAME_CHANNEL_V2_CONTROL_PLANE.md).

Before a removed display id becomes reusable, qemu disables its export,
advances a producer-local lifecycle generation, and clears its slot back to
`frame == 0`. GPU exports capture that generation before doing work, so a late
frame from the previous application cannot repopulate the slot after the id is
assigned to another application. Until the new display publishes its first
frame, the Metal view presents only its clear color.

The struct is duplicated in `hardware/google/gfxstream/host/common/iosurface_export.cpp`
and `shell/core/frame_consumer.cpp` so the MIT shell remains self-contained.
Any incompatible layout change must update both copies and bump the shm version.

## Copy and Synchronization Model

The Vulkan implementation first attempts a zero-copy path:

- displayable BGRA ColorBuffers are created as Metal IOSurface-exportable images;
- `DisplayVk` exports the posted ColorBuffer's IOSurface directly;
- qemu/gfxstream performs a Vulkan barrier/queue handoff before publishing
  metadata.

The default validated path is currently OpenGL composition. `ColorBufferGl`
first attempts to make its main `GL_TEXTURE_2D` IOSurface-backed, but the macOS
CGL/translator stack currently rejects that binding. It then creates a
ColorBuffer-owned IOSurface mirror using `GL_TEXTURE_RECTANGLE`; `DisplayGl`
publishes that ColorBuffer IOSurface directly instead of owning a separate
display export surface. The mirror is synchronized from the ColorBuffer's main
GL texture with a GPU blit.

If the source ColorBuffer is not exportable or mirrorable, is not an RGBA/BGRA
display format, has a different display-frame size, or the driver refuses the
export, the implementation falls back to the original copy path:

- qemu/gfxstream owns the source ColorBuffer.
- qemu/gfxstream owns the exported IOSurface target.
- Each frame is copied with a GPU blit/copy into the IOSurface target.
- Metadata is published only after the frame copy completes.
- The shell samples the IOSurface through Metal.

So the shell-side display does not copy pixels on the CPU. On the Vulkan/BGRA
zero-copy path, the final ColorBuffer is the exported IOSurface. On the current
OpenGL path, the ColorBuffer owns the IOSurface mirror that is exported to the
shell. Fallback paths still perform a GPU copy from the final ColorBuffer into
an IOSurface-backed target image.

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

## Skin and Cutout Handling

MacMu treats the guest display as a framebuffer, not as an Android Studio device
frame. The core-only runtime does not project Android Emulator skin semantics
into the guest:

- no `qemu.skin` / `androidboot.qemu.skin` boot property is emitted;
- no Pixel/device skin overlay packages are enabled through adb;
- no skin layout cutout or rounded-corner overlay is enabled from the emulator
  `layout` file;
- no Pixel/Fold skin files are copied into userdata for display configuration.

AVD hardware settings such as LCD width, height, density, and multi-touch remain
active. The shell may add its own optional host-side decoration later, but the
guest framebuffer remains free of emulator device-frame policy by default.

## Packaging

CI produces three artifacts:

- `macmu-macos-arm64.zip`: raw distribution layout.
- `macmu-macos-arm64-app.zip`: `MacMu.app`.
- `macmu-macos-arm64.dmg`: installable DMG with `MacMu.app` and an
  `/Applications` symlink.

The app bundle keeps qemu and its runtime libraries under
`Contents/Resources/emulator`. The app entry point remains `Contents/MacOS/macmu`.

## Current Limitations

- Secondary displays are exported only on the GL composition path; Vulkan
  composition still exports display 0 only.
- Only the first posted layer is exported.
- Rotation and color transforms are ignored.
- Non-RGBA/BGRA, non-exportable, or non-mirrorable ColorBuffers still use an
  extra GPU blit from ColorBuffer to IOSurface.
- The `AEMU_*` exporter and frame-doorbell variables are still emitted for
  compatibility with older local builds; they can be removed after all builds
  consume `MACMU_*`.
