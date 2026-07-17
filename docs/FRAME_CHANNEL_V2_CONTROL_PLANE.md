# Frame Channel v2 and Shell Control Plane — Design

Status: implemented (frame channel v2, control plane v1, per-display GL
compose export, shell multi-window, and the guest agent control RPC channel).
Implementation deltas from the original proposal are marked "IMPL:" inline.
Scope: the qemu↔shell frame metadata protocol (data plane) and a new
runtime command channel (control plane). The guest agent control RPC channel
(`MacMuAgent`: `apps` / `launch <component> <displayId>` over a second
`pipe:unix:` connection, host side `shell/core/guest_control_client.*`) was
implemented alongside; display ids assigned through the control plane are the
same ids the agent uses for display-targeted launches. The agent also sends
the `com.android.emulator.multidisplay.START` broadcast on connect, replacing
the adb-based service start that core-only MacMu does not have.

## Goals

- Make the frame export protocol display-indexed so one qemu instance can
  export N displays to N shell views, keeping the existing zero-copy
  IOSurface + shm + doorbell model.
- Add a bidirectional shell↔qemu command channel for runtime control:
  display add/remove/list first; screenshots, rotation, clipboard, and status
  events later.
- Keep the existing security posture: no filesystem sockets, no network
  listeners, fd inheritance and per-pid shm only.
- Keep the MIT shell self-contained: protocol definitions stay duplicated on
  both sides of the license boundary, guarded by magic + version.

## Non-goals

- No pixel data on either channel. Pixels cross the boundary only inside
  IOSurfaces.
- No cross-version compatibility between shell and qemu. They ship in one app
  bundle and upgrade in lockstep; version fields exist to fail closed, not to
  negotiate feature subsets.
- No guest-facing changes in this design. MultiDisplayPipe and the goldfish
  pipe transports already handle guest notification.

---

# Part 1: Frame Channel v2

## Current state (v1)

One POSIX shm object `/macmu.frame.<wrapper-pid>` holds a single
`ShmHeader` + `ShmPayload`. The shell (consumer) creates and unlinks the shm;
gfxstream (producer) opens it and validates magic/version. A `SOCK_DGRAM`
socketpair end inherited as child fd 198 is the doorbell. The producer writes
the payload, issues a release fence, writes `frame` last; the consumer does a
bounded seqlock-style retry read keyed on `frame`.

Two v1 limitations drive v2:

1. There is exactly one payload and it carries no display id.
2. The torn-read detector is keyed on `frame` being written last. A consumer
   that reads while the producer has written the new `iosurfaceId` but not yet
   the new `frame` observes a consistent-looking snapshot pairing the new
   surface with the old frame number. Harmless for a single display, but v2
   makes the payload wider and multi-writer-adjacent, so v2 upgrades to a real
   odd/even seqlock.

## Considered: per-display shm objects

`macmu.frame.<pid>.<displayId>`, one shm + payload per display. Rejected:

- The consumer-creates model means the shell must create/unlink shm objects on
  every display hotplug, ordered against control-plane commands and against
  qemu restarts. That is lifecycle machinery with several failure interleavings.
- The producer would need to re-open shm objects at runtime instead of once at
  startup.
- The savings (no fixed slot cap) buys nothing: Android multi-display ids are
  small integers with an ecosystem cap around 11 displays.

## Chosen: one shm object, fixed slot table

One shm object per shell instance, sized once, mapped once by each side,
containing a fixed array of per-display slots. Slot index == Android display
id. Displays beyond the slot cap are rejected at the control plane, never at
the frame channel.

### Wire layout

Header magic stays `MACMUFRM`; `version` becomes 2, so a v1 producer opening a
v2 shm fails closed (it requires version == 1), and vice versa.

```c
constexpr uint32_t kFrameSlotCount = 16;

struct ShmHeaderV2 {
    uint64_t magic;         // 'MACMUFRM' (unchanged from v1)
    uint32_t version;       // 2
    uint32_t payloadOffset; // byte offset of slot[0] from mapping start
    uint32_t slotCount;     // kFrameSlotCount
    uint32_t slotStride;    // sizeof(ShmDisplaySlot) == 64
    uint64_t reserved[2];   // zero; room for future header growth
};

// One per display, exactly one cache line so adjacent slots do not
// false-share between the producer post thread and the consumer.
struct ShmDisplaySlot {
    uint64_t seq;          // seqlock: odd while the producer is writing
    uint64_t frame;        // monotonic per slot; 0 = never published
    uint32_t iosurfaceId;
    uint32_t width;
    uint32_t height;
    uint32_t dpi;
    uint32_t pixelFormat;  // fourcc, currently 'BGRA'
    uint32_t flags;        // bit0: primary display
    uint64_t timestampNs;  // producer steady clock
    uint8_t  pad[16];      // pad to 64 bytes (IMPL: 16, not 8; the field set
                           // above totals 48 bytes)
};
static_assert(sizeof(ShmDisplaySlot) == 64);
```

Total mapping: `sizeof(ShmHeaderV2) + 16 * 64` ≈ 1.1 KiB.

As in v1, the struct definitions are duplicated in
`hardware/google/gfxstream/host/common/iosurface_export.cpp` (producer) and
`shell/core/frame_consumer.cpp` (consumer) so the MIT shell stays
self-contained. Both copies carry a comment pointing at the other and at this
document. Any layout change bumps `version` in both copies in the same change.

### Seqlock discipline

Producer, per publish:

1. `seq += 1` (now odd), release fence.
2. Write all payload fields including `frame` (monotonic increment).
3. Release fence, `seq += 1` (now even).

Consumer, per read (bounded retries as today):

1. Read `seq`; if odd, retry.
2. Acquire fence, read payload fields, acquire fence.
3. Re-read `seq`; accept iff unchanged and even.

`frame` remains the "is there anything new for this display" cursor the
consumer compares against its per-display last-seen value; it no longer doubles
as the torn-read detector.

### Doorbell v2

Same transport: one `SOCK_DGRAM` socketpair, consumer keeps one end, producer
inherits the other as child fd 198 via `MACMU_FRAME_DOORBELL_FD`. One doorbell
serves all displays. The datagram payload becomes:

```c
struct FrameDoorbellMsg {
    uint32_t displayId;
    uint32_t reserved;   // zero
    uint64_t frame;
};
```

The payload is advisory (useful for tracing). The consumer must not trust it:
on wake it drains the queue and re-scans the slot table, comparing each slot's
`frame` against its per-display cursor. Scanning 16 cache lines is cheaper than
maintaining correctness under datagram coalescing and drops (`MSG_DONTWAIT`
sends may be discarded under load, as in v1).

### Producer (gfxstream) behavior

Display 0 keeps the existing sink hooks unchanged: `DisplayGl::post()` with
`IosurfaceGlDisplaySink`, and `DisplayVk::postToIosurface()`. They publish to
slot 0 instead of the single payload.

Secondary displays hook the per-display composition path instead of the final
present. Grounding in the current tree:

- Guest hwcomposer compose requests arrive as `ComposeDevice_v2`, which
  carries `displayId` (`host/hwc2.h`).
- `FrameBuffer::composeWithCallback` flattens these to `FlatComposeRequest`
  (also display-indexed) and composes into that display's target ColorBuffer;
  `FrameBuffer::getDisplayColorBuffer(displayId, ...)` resolves the binding.

IMPL: guest secondary displays on phone images are VirtualDisplays composed
inside the guest into a single fixed BufferQueue buffer; the host receives a
`MultiDisplayPipe` BIND only when the buffer handle changes (per AOSP
`MultiDisplayProvider` JNI: `if (mCb != cb->hostHandle)`), i.e. effectively
once — there is NO per-frame host signal. Export is therefore driven by three
triggers, all funneling into `PostCmd::MacMuExportDisplay` on the post worker
(`PostWorkerGl::exportDisplayImpl`, sharing the blit/publish machinery with
the display-0 sink via `host/gl/iosurface_gl_export.{h,cpp}`); nothing depends
on any other display's post cadence:

1. BIND events: `MultiDisplay::setDisplayColorBuffer` →
   `android_notifyDisplayColorBufferChanged` → renderer (covers buffer
   rebinds and the first frame).
2. Window-surface flushes: `rcFlushWindowColorBuffer` →
   `FrameBuffer::notifyColorBufferFlushed` → CB→display reverse lookup
   (covers guests that swap through goldfish EGL window surfaces).
3. Shell-paced streaming (the universal fallback): while it has a window
   sampling a display, the shell sends
   `DISPLAY_STREAM {displayId, enabled, maximumFramesPerSecond}` using the
   maximum refresh rate of that window's current `NSScreen`. The qemu control
   receiver independently paces each enabled display to that ceiling and calls
   `android_exportDisplayFrame(displayId)` at each deadline. Moving a window
   between screens updates its rate. Streaming stops on window close,
   DISPLAY_REMOVE, or channel teardown, so an unwatched display costs nothing.
   A missing/zero rate from an older 8-byte request falls back to 60 Hz.

`exportComposedDisplay()` additionally covers hwc-composed secondary displays
(e.g. automotive images), where a compose for `displayId != 0` lands
host-side.

Note on id spaces: the control plane, frame slots, and input protocol all use
the emulator MultiDisplay index (1..5). Android assigns a different logical
display id to the VirtualDisplay; the guest agent translates at its boundary
(uniqueId "virtual:com.android.emulator.multidisplay:<1234561+index>") for
`start-activity --display` and input injection.

The original compose-hook design text follows; it resolves the display's target ColorBuffer, ensure
its IOSurface mirror exists (reusing the existing `ColorBufferGl`
GL_TEXTURE_RECTANGLE mirror machinery and the Vulkan IOSurface-exportable image
path — same code, different caller), synchronize the mirror, then publish to
slot `displayId`. The same fallback ladder as v1 applies per display
(direct export → CB-owned mirror → GPU blit into a sink-owned IOSurface).

Publish rules:

- A slot is published only after a real frame exists. `frame == 0` means "this
  display has never produced pixels"; the shell renders its placeholder clear
  color until then.
- Display removal disables export, advances a producer-local lifecycle
  generation, and clears that slot back to `frame == 0` before the ordered
  `DISPLAY_REMOVE_OK` / `DISPLAY_REMOVED` messages make the id reusable.
  Every GPU export captures the generation before it starts and supplies it
  when publishing; an export from the previous display instance that finishes
  late is discarded instead of repopulating the reused slot.
- On display resize the producer allocates the new IOSurface and publishes it
  with the next frame; the consumer treats any `iosurfaceId` change as
  "release old texture, `IOSurfaceLookup` the new id" (v1 already behaves this
  way).

### Consumer (shell) behavior

- `FrameConsumer` maps the table once and keeps a `lastFrame[16]` cursor array.
- One doorbell thread (today's per-window thread becomes a single shared
  thread): wake → drain → scan slots → for each slot with
  `frame > lastFrame[i]`, update the cursor and mark the display's view dirty
  on the main queue.
- The shell keeps a display registry mapping `displayId → NSWindow/MTKView`.
  Slot activity for an id with no registered window is ignored (frames keep
  flowing; nothing samples them).
- On qemu exit/restart the shell zeroes the whole slot table and its cursor
  array before respawning (generation handling already exists via
  `_qemuGeneration`); the producer in the new qemu maps a clean table.

### Failure modes

| Case | Behavior |
| --- | --- |
| v1 producer × v2 shm (or reverse) | Producer rejects on `version` check, logs, no frames. Control-plane `HELLO` (Part 2) surfaces the mismatch explicitly. |
| qemu killed mid-write (slot left odd) | Shell zeroes the table on restart; within a session an odd `seq` only causes bounded retries and the next frame republishes. |
| Doorbell datagrams dropped | Next successful send wakes the consumer; scan picks up all pending slots. The consumer also does a final non-blocking read at timeout, as in v1. |
| Display removed while a view samples | IOSurface stays alive under the consumer's reference until it releases; control event tells the shell to close the window and drop the texture. |
| Old export finishes after a display id is reused | Its captured lifecycle generation no longer matches, so `FrameChannel::publish` drops it. The cleared slot remains empty until the new display publishes. |
| Slot id ≥ 16 requested | Rejected at `DISPLAY_ADD` with an error; the frame channel never sees it. |

---

# Part 2: Control Plane

## Requirements

- Bidirectional, runtime, low-rate (a few messages per second at most).
- Request/response with correlation ids, plus unsolicited events (qemu → shell).
- Must exist before multi-display work starts, because display add/remove is
  its first client; designed so screenshots, rotation, clipboard, boot status,
  and agent health ride the same channel later.
- Same trust domain and lifetime as the frame channel: created by the shell per
  qemu launch, torn down with the child process.

## Rejected alternatives

- **QMP / qemu monitor**: this fork's QMP is ancient, JSON-typed through vl.c
  plumbing, and adding Android-side commands there couples shell features to
  qemu core code. The emulator upstream itself bypassed QMP (console/gRPC).
- **gRPC**: deliberately pruned from the core-only build; reintroducing it for
  a handful of fixed-shape messages is disproportionate weight, especially in
  the MIT shell.
- **Filesystem Unix socket**: bind/unlink TOCTOU, residue, cross-user exposure
  — the same reasons the input path moved to inherited fds.

## Chosen: inherited SOCK_STREAM socketpair, length-prefixed binary frames

The shell creates an `AF_UNIX` `SOCK_STREAM` socketpair, keeps one end, and
`dup2()`s the other into **child fd 197**, advertised via `MACMU_CONTROL_FD`.
This is the third instance of an established pattern (frame doorbell = 198,
input = 199), and `qemu_launcher.cpp` already has the dup2/CLOEXEC machinery.

Stream (not datagram) because responses can exceed datagram comfort
(`DISPLAY_LIST`, future screenshot metadata) and ordering matters for
request/response pairing.

### Framing

```c
constexpr uint32_t kCtrlMagic = 0x4d4d4350;      // 'MMCP'
constexpr uint16_t kCtrlVersion = 1;
constexpr uint32_t kCtrlMaxPayload = 64 * 1024;  // hard cap, both sides

struct CtrlFrameHeader {       // packed, 16 bytes
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t requestId;        // sender-unique for requests; echoed in the
                               // response; 0 for events
    uint32_t length;           // payload bytes following the header
};
```

Rules:

- Bad magic/version or `length > kCtrlMaxPayload` is a protocol failure: the
  reader closes the channel and logs. There is no resync on a corrupt stream.
- Unknown **request** type → `ERROR` response with `code = UNSUPPORTED`.
  Unknown **event** type → ignored. This lets either side add messages without
  breaking the other within a version.
- `type` ranges: `0x0001–0x00FF` requests (shell→qemu), `0x0101–0x01FF`
  responses (`request | 0x0100`), `0x0201–0x02FF` events (qemu→shell).
  `0x01FF` is the generic `ERROR` response.

### Message catalog v1

Payloads are packed little-endian structs, duplicated across the boundary like
every other MacMu protocol header.

| Type | Name | Payload | Notes |
| --- | --- | --- | --- |
| 0x0001 | `HELLO` | `{u32 protoVersion}` | First message on the channel, sent by the shell. |
| 0x0101 | `HELLO_ACK` | `{u32 protoVersion, u32 capabilities, u32 maxDisplays, u32 frameShmVersion}` | `frameShmVersion` gives explicit diagnosability for frame-channel skew. |
| 0x0002 | `PING` | empty | Liveness; shell may use it to distinguish "wedged" from "slow boot". |
| 0x0102 | `PONG` | `{u64 timestampNs}` | |
| 0x0010 | `DISPLAY_ADD` | `{u32 displayId, u32 width, u32 height, u32 dpi, u32 flags}` | `displayId = UINT32_MAX` asks qemu to allocate one. |
| 0x0110 | `DISPLAY_ADD_OK` | `{u32 displayId}` | Means "host accepted", not "guest live" — see sequencing below. |
| 0x0011 | `DISPLAY_REMOVE` | `{u32 displayId}` | |
| 0x0111 | `DISPLAY_REMOVE_OK` | empty | |
| 0x0012 | `DISPLAY_LIST` | empty | |
| 0x0112 | `DISPLAY_LIST_OK` | `{u32 count, {u32 id, u32 w, u32 h, u32 dpi, u32 flags}[count]}` | |
| 0x0201 | `EVENT_DISPLAY` | `{u32 displayId, u32 state, u32 w, u32 h, u32 dpi, u32 flags}` | `state ∈ {ADDED, CHANGED, REMOVED}`; fires on guest-confirmed changes, including ones not initiated by the shell. |
| 0x0202 | `EVENT_BOOT` | `{u32 stage}` | Reserved in the protocol header; not emitted yet (IMPL: deferred). |
| 0x01FF | `ERROR` | `{u32 code, u32 msgLen, char msg[msgLen]}` | Correlated via `requestId`. |

### qemu-side integration

A `macmu-control-receiver.{h,cpp}` in `android-qemu2-glue`, modeled directly on
`macmu-input-receiver.cpp`:

- Started from `qemu-setup.cpp` next to `macmu_input_receiver_start()`, after
  the console agents exist.
- One reader thread parses frames; command execution hops to the main looper
  via `android::base::ThreadLooper::runOnMainLooper()` (same pattern as input
  delivery), because display mutation must run on the qemu main loop.
- `DISPLAY_ADD/REMOVE` call
  `android::MultiDisplay::getInstance()->setMultiDisplay(id, -1, -1, w, h,
  dpi, flags, add)` — the same entry point the emulator console used. Guest
  notification then flows through the existing `MultiDisplayPipe`, and
  gfxstream binding through the existing `FrameBuffer::setDisplayPose` /
  `getDisplayColorBuffer` plumbing. No new guest code.
- `EVENT_DISPLAY` hooks the existing MultiDisplay state-change notification
  path so guest-confirmed changes (and non-shell-initiated ones) are reported.
- Writes are serialized by a mutex; the writer never blocks the main looper
  (non-blocking fd + small bounded egress queue; if the queue overflows the
  channel is declared broken and closed — the shell treats that as a qemu
  restart trigger, consistent with its supervisor loop).

### Shell-side integration

A `ControlChannel` class in `shell/core` (pure C++, like the other senders):

- Owns the socketpair; `qemu_launcher` gains the fd-197 dup2 wiring.
- `request(type, payload, timeoutMs) → future<Result>`; requestId is a
  monotonically increasing counter; a reader thread demultiplexes responses to
  pending futures and dispatches events to a registered callback (bounced to
  the main queue by the AppKit layer).
- Default request timeout 5 s; timeout marks the request failed but does not
  kill the channel (the response is discarded if it arrives late).
- Lifetime matches a qemu generation: created before spawn, destroyed after
  `waitpid`, exactly like the input socketpair. `HELLO` is sent once the fd is
  readable; until `HELLO_ACK`, display commands are queued or rejected.

### End-to-end sequence: adding a display

```text
shell                          qemu (main looper)                guest
  |  DISPLAY_ADD(id=?,1080x600) |                                  |
  |---------------------------->| setMultiDisplay(alloc id=2,...)  |
  |        DISPLAY_ADD_OK(id=2) |--- MultiDisplayPipe add -------->|
  |<----------------------------|                                  |
  | create window, bind slot 2  |                DisplayManager    |
  |                             |<-- guest enables display --------|
  |     EVENT_DISPLAY(2, ADDED) |                                  |
  |<----------------------------| SurfaceFlinger composes display 2|
  |                             | compose(displayId=2) → slot 2    |
  |<== doorbell + slot 2 frame ==                                  |
  | first frame renders         |                                  |
```

The shell may create the window at `DISPLAY_ADD_OK` (placeholder until the
first frame) or wait for `EVENT_DISPLAY`; the frame channel needs no
per-display setup either way — that is the payoff of the fixed slot table.

`DISPLAY_ADD_OK`'s id is also what the guest agent needs for
`am start --display 2` style operations; the shell is the single owner of the
window↔displayId mapping.

---

# Rollout plan

Each phase lands and validates independently; shell and qemu halves of a phase
land in one change because they upgrade in lockstep.

1. **Frame channel v2, single display.** New header/slot structs, odd/even
   seqlock, doorbell struct, slot 0 only. Behavior identical to today;
   validates the protocol swap in isolation. Update
   `GRAPHICS_ARCHITECTURE.md` wire-layout section in the same change.
2. **Control channel core.** fd-197 wiring, framing codec both sides,
   `HELLO`/`PING`/`ERROR`, receiver + `ControlChannel` classes, `EVENT_BOOT`.
   No display commands yet. Validated by shell logging `HELLO_ACK` and boot
   events on every launch.
3. **Display commands + per-display sink.** `DISPLAY_ADD/REMOVE/LIST`,
   `EVENT_DISPLAY`, gfxstream post-compose hook publishing slots 1+, shell
   display registry and multi-window. First real multi-display milestone.
4. **Input hardening.** Per-gesture path latching in `macmu_input_view.mm`
   (the datagram-vs-agent race), and displayId routing from the registry
   instead of the constant 0. The input protocols already carry displayId
   end to end; no protocol change.

# Testing

- **Codec unit tests** (shell side, plain C++): framing round-trips, truncated
  header/payload, oversize length, unknown types, seqlock torn-read retry
  (producer thread hammering a slot while consumer reads).
- **Skew test**: v1 producer against v2 shm must fail closed with the version
  log line and zero frames; `HELLO_ACK.frameShmVersion` mismatch must surface
  in the shell status window.
- **Restart test**: kill -9 qemu mid-render; shell must zero slots, respawn,
  and recover frames on all open display windows.
- **Hotplug soak**: scripted add/remove of display 1–3 in a loop while
  display 0 renders; no stuck touches, no leaked IOSurface references
  (`footprint`/Instruments check), doorbell thread count stays 1.

# Future extensions (explicitly designed-for, not built now)

- `SCREENSHOT` request: qemu blits a display's ColorBuffer into a one-shot
  IOSurface and returns its id — pixels still never cross the socket.
- Rotation/color-transform metadata in `ShmDisplaySlot` (`pad` bytes are
  reserved for this).
- `EVENT_AGENT` reporting guest agent connect/disconnect, so the shell can
  drive the input-path latch from an authoritative signal instead of
  `ready()` polling.
- Clipboard and graceful-shutdown requests over the same message space.
