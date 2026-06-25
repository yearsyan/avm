# AEMU Qt UI Expert Architecture

## Multi-threading Model

The Android Emulator UI operates in a multi-threaded environment where the **Qt Main Thread** (GUI thread) and the **Emulator Thread** (QEMU/Guest thread) must interact safely.

### Cross-thread Communication

To call from the Emulator thread to the Qt thread:
1. **Signals and Slots**: Use Qt's signal/slot mechanism. Emit a signal from the emulator thread; the corresponding slot will execute on the Qt thread.
2. **Blocking Calls**: If the emulator thread needs to wait for the UI operation to complete, pass a `QSemaphore*` to the signal. The slot should release the semaphore upon completion.

```cpp
// In EmulatorQtWindow.h
signals:
    void getDevicePixelRatio(double* out_dpr, QSemaphore* semaphore = NULL);

// In winsys-qt.cpp
if (onMainQtThread()) {
    window->getDevicePixelRatio(dpr, nullptr);
} else {
    QSemaphore semaphore;
    window->getDevicePixelRatio(dpr, &semaphore);
    semaphore.acquire();
}
```

## Core Components

### 1. `EmulatorQtWindow`
The primary window class. It handles:
- Rendering the guest screen via `SharedMemoryRenderer`.
- Input event translation (keyboard, mouse, touch, pen).
- Window management (resize, zoom, rotation).
- Drag and drop (APK installation, file pushing).

### 2. `ExtendedWindow`
Manages the "Extended Controls" dialog. It uses a sidebar-based navigation to switch between different control pages.

### 3. `ToolWindow`
The vertical toolbar next to the emulator screen, providing quick access to common actions (power, volume, rotation, etc.).

### 4. `MultiDisplayWidget`
Handles additional displays for the emulator, allowing for multi-monitor guest configurations.

## UI Design and Resources

### Qt Designer (`.ui` files)
Layouts are primarily defined in `.ui` files. These are compiled into C++ code at build time.
- `extended.ui`: Layout for the extended controls container.
- `tools.ui`: Layout for the vertical toolbar.

### Qt Resources (`.qrc` files)
Images, fonts, and stylesheets are bundled into the executable via `.qrc` files (e.g., `resources.qrc`).

## Interaction with Backend

The UI interacts with the emulator backend through **Agents**. Key agents include:
- `UiEmuAgent`: General emulator control.
- `AdbInterface`: Interaction with the guest via ADB.
- `MultiDisplayAgent`: Management of guest displays.

## Testing Pattern

Extended control pages typically follow a "Controller" pattern to facilitate testing:
- **Page**: The UI class (e.g., `BatteryPage`).
- **Controller**: An interface defining the backend operations (e.g., `BatteryController`).
- **Implementations**: A "Legacy" controller using agents or a "gRPC" controller (used in Embedded or Decoupled modes).
- **Mocks**: Used in unit tests to verify UI interactions without a full emulator backend.

## Fishtank Decoupled UI

Fishtank is a specialized, decoupled Qt application that provides the emulator UI experience but runs without a built-in QEMU backend.

### 1. Decoupled Communication (gRPC)
Unlike the **Standalone Emulator UI** which uses local agents, Fishtank interacts with the backend (typically an **Embedded Emulator**) exclusively via gRPC.
- **Clients**: Uses `EmulatorControllerClient` and `SensorServiceClient` to talk to the backend.
- **Service Discovery**: Uses a discovery file (passed via `-fishtank <file>`) to locate the backend's gRPC port.

### 2. Event-Driven Physical State (Push Model)
Fishtank uses the `SensorService` to subscribe to physical state events (Position, Rotation, Hinge).
- **Stream**: `receivePhysicalStateEvents` stream replaces polling.
- **UI Sync**: Events from the stream are translated into `QAndroidPhysicalStateAgent` callbacks, which trigger the UI's internal polling/animation timers.

### 3. Specialized Proxy Agents
Fishtank implements its own versions of the standard AEMU agents (e.g., `sFishtankQAndroidSensorsAgent`). These agents act as proxies that convert standard agent calls into gRPC RPCs.

### 4. Isolated Deployment
The Fishtank build process is isolated (using `EXCLUDE_FROM_ALL` in CMake) to produce a distribution directory (`$REPO_ROOT/external/qemu/objs/distribution-fishtank`) containing its own Qt libraries and dependencies.

## Technical Debt: The `messagePump` Loop

Fishtank currently uses a `messagePump` (in `$REPO_ROOT/external/qemu/android/android-ui/apps/fishtank/main.cpp`) as a surrogate for the QEMU `MainLoopThread`. This is a significant piece of architectural technical debt required to keep the legacy C "Skin" code alive in a decoupled environment.

### Why it exists
Legacy modules like `keyboard.c` and `ui.c` were designed for a single-process model where a backend thread periodically "polls" the UI. `messagePump` provides this heartbeat (ticking every 10ms) to:
- Flush the `SkinKeyboard` keycode buffer.
- Manage character repeat rates and multi-stage key mappings.
- Process events queued in `mSkinEventQueue`.

### Migration Challenges
Eliminating `messagePump` is difficult due to several "implicit logic" gaps in the current gRPC implementation:
1. **`SkinKeyboard` State Machine:** Legacy code handles "stuck key" prevention and modifier state (Shift/Ctrl/Alt) synchronization. Bypassing the queue requires re-implementing this logic in C++ to avoid guest-side state desync.
2. **Key Aliasing/Translation:** The legacy stack performs complex mapping (e.g., `.key` files) and handles Evdev sequencing (`EV_KEY` -> `EV_SYN`).
3. **De Facto Mutex:** Because legacy C code is often non-thread-safe, `messagePump` acts as a single-threaded execution context that prevents race conditions.

### Long-term Strategy
The goal is to move to a **Pure Push Model**:
- **Starve the Pump:** Shift all input handlers (`keyPressEvent`, `mouseMoveEvent`) to call Agent methods directly (bypassing the queue).
- **Translation Library:** Move the keycode translation and layout mapping logic into a standalone C++ library used by the UI Agent layer.
- **Kill Switch:** Once `mSkinEventQueue` remains perpetually empty and no legacy module requires a "tick," the `messagePump` thread and its associated 10ms latency can be removed.

Example Test:
```cpp
TEST_F(BatteryPageTest, SliderUpdatesChargeLevel) {
    auto mockController = std::make_unique<MockBatteryController>();
    EXPECT_CALL(*mockController, setChargeLevel(80));
    m_page->setControllerForTest(std::move(mockController));
    // Simulate UI slider move to 80...
}
```
