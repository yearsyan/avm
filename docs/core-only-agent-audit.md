# Core-Only Agent Compilation Audit

Date: 2026-06-28
Status: **Audit only — no CMake changes applied.** This document records findings
for a future pruning PR.

## Background

MacMu is permanently configured as **core-only**:
- `external/qemu/CMakeLists.txt:139` sets `OPTION_AEMU_CORE_ONLY TRUE` (plain
  `set()`, not a cacheable `option()`, so it does not appear in `CMakeCache.txt`).
- `external/qemu/CMakeLists.txt:132` defines `AEMU_CORE_ONLY=1` globally, so
  every `#ifdef AEMU_CORE_ONLY` source guard takes effect.

AGENTS.md states the following surfaces were "removed or avoided where possible":
recording, virtual scene, modem/netsim, telephony, Qt UI, WebRTC. This audit
checks how completely that removal is reflected at the source/CMake level for
the per-feature "agent" implementation files.

## Mechanism

All 7 agent files live in `external/qemu/android-qemu2-glue/` and are listed
**unconditionally** in the `android-qemu2-glue_src` source list
(`android-qemu2-glue/CMakeLists.txt:41-69`), which feeds `libqemu2-glue`, which
is linked directly into `qemu-system-aarch64-headless` via
`android_add_qemu_headless_executable` (`android.cmake:1510-1517`).

Unlike `qemu-img` (correctly guarded by `if(NOT OPTION_AEMU_CORE_ONLY)` at
`CMakeLists.txt:981`), the agent source list has **no such guard**. Whether a
file contributes real code therefore depends entirely on in-source
`#ifdef AEMU_CORE_ONLY` guards, not on CMake exclusion.

## Per-file classification

| File | Classification | Notes |
|------|----------------|-------|
| `qemu-telephony-agent-impl.c` | **STUBBED** | Whole body `#ifdef`'d out; real modem code excluded. |
| `qemu-cellular-agent-impl.c` | **STUBBED** | Whole body `#ifdef`'d out; real amodem/netshaper excluded. |
| `qemu-virtual-scene-agent-impl.cpp` | **STUBBED** | No-op vtable selected; `VirtualSceneManager.h` excluded. |
| `qemu-automation-agent-impl.cpp` | **STUBBED** | Per-function guards; `AutomationController.h` excluded. |
| `qemu-record-screen-agent-impl.c` | **PARTIALLY COMPILED** | Only shmem sub-module stubbed; vtable still wires real `emulator_window_*_recording` / `android_screenShot`; `screen-recorder.h` always included. |
| `qemu-sensors-agent-impl.cpp` | **STILL COMPILED** | Sensor/physical-model functions unguarded; binds real `android_sensors_*` / `android_physical_model_*`. |
| `emulation/WifiService.cpp` | **STILL COMPILED (heaviest)** | No top-level guard; builds `HostapdController` + `VirtioWifiForwarder` + slirp. Only the `NetsimWifiForwarder` branch is suppressed. |

## Still-compiled detail

1. **`emulation/WifiService.cpp`** (209 lines) — heaviest. Instantiates
   `HostapdController` (hostapd event loop), `VirtioWifiForwarder`, and slirp
   networking. Transitively drags in `VirtioWifiForwarder.cpp` (~25 KB, also
   unconditionally in the same source list at line 40) and the `hostapd`
   library (linked at `android-qemu2-glue/CMakeLists.txt:96`). This contradicts
   the AGENTS.md claim that "netsim" surfaces were removed — only the optional
   netsim redirector is disabled; the full virtio-wifi/hostapd path is live.

2. **`qemu-sensors-agent-impl.cpp`** (85 lines) — sensor/physical-model agent
   functions are unguarded, so the physical-model + sensor subsystems remain
   live in the binary. Only the `automation_advance_time()` body is stubbed.

3. **`qemu-record-screen-agent-impl.c`** (50 lines) — small, but the recording
   + screenshot entry points (`emulator_window_start_recording`,
   `android_screenShot`) remain wired to real implementations, keeping the
   screen-recorder code path reachable. AGENTS.md lists "recording" as removed.

## Recommendation for a future pruning PR

These changes would shrink the headless binary and its dependency closure
without affecting the MacMu headless-launch use case. **They are not applied
here** because removing them risks changing qemu's device/feature negotiation
behavior and needs separate validation.

1. Wrap the still-compiled agent entries in
   `if(NOT OPTION_AEMU_CORE_ONLY)` inside `android-qemu2-glue/CMakeLists.txt`
   (the source list at lines 41-69, plus the `VirtioWifiForwarder.cpp` entry
   and the `hostapd` link at line 96).
2. Add a top-level `#ifdef AEMU_CORE_ONLY` stub to `emulation/WifiService.cpp`
   mirroring the telephony/cellular pattern, OR exclude it from the source list.
3. Guard the unconditional record-screen vtable wiring and `screen-recorder.h`
   include in `qemu-record-screen-agent-impl.c`.
4. Guard the sensor/physical-model function bodies in
   `qemu-sensors-agent-impl.cpp` if the guest does not exercise them (validate
   against `aemu_aosp35_arm64` boot + launcher first).
5. After each step, rebuild and re-run the AGENTS.md boot validation to confirm
   `Boot completed`, the launcher reaches the foreground, and SurfaceFlinger
   layers are still active.

## Verification baseline (current, pre-prune)

With the audit's "no change" posture, the current tree builds and runs:
- `Boot completed in 12257 ms`
- `topResumedActivity=...com.android.launcher3/.uioverrides.QuickstepLauncher`
- SurfaceFlinger: 83 active layers
- Screenshot (launcher + Settings) shows real Android UI, not black
- macmu shell CPU 0.0% on static screen (event-driven), rises during animation
