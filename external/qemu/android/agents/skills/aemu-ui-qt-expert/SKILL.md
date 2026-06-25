---
name: aemu-ui-qt-expert
description: Expert in the Android Emulator Standalone and Embedded UI (Qt-based), and the Decoupled Fishtank UI. Trigger when the user mentions "Qt", "emulator UI", or "fishtank". Use for modifying, debugging, or extending the host-side user interface, including the main window, extended controls, toolbars, and multi-display support.
---

# AEMU Qt UI Expert

Expert guidance for working with the Android Emulator standalone UI, written in C++ using the Qt framework.

## Core Terminology & Architecture

- **Standalone Emulator**: The traditional emulator where the QEMU backend and the Qt UI run together in a single process.
- **Embedded Emulator**: A mode where the Standalone Emulator runs with its main window hidden and a gRPC server active. This is primarily used by **Android Studio** to embed the emulator view. Android Studio interacts with the emulator and controls the Qt UI (e.g., opening Extended Controls) via gRPC.
- **Fishtank UI**: A decoupled version of the Qt UI that runs without a built-in QEMU backend. It acts as a gRPC client, interacting with a remote emulator backend (typically an Embedded Emulator instance) via gRPC, similar to how Android Studio operates.

## Core Procedures

### Architectural Navigation
- **Base Directory**: `$REPO_ROOT/external/qemu/android/android-ui/modules/aemu-ui-qt`
- **Fishtank Directory**: `$REPO_ROOT/external/qemu/android/android-ui/apps/fishtank`
- **Main Window**: `EmulatorQtWindow` in `$REPO_ROOT/external/qemu/android/android-ui/modules/aemu-ui-qt/src/android/skin/qt/emulator-qt-window.cpp`.
- **Extended Controls**: `ExtendedWindow` in `$REPO_ROOT/external/qemu/android/android-ui/modules/aemu-ui-qt/src/android/skin/qt/extended-window.cpp`.
- **Toolbar**: `ToolWindow` in `$REPO_ROOT/external/qemu/android/android-ui/modules/aemu-ui-qt/src/android/skin/qt/tool-window.cpp`.
- **Entry Point**: `$REPO_ROOT/external/qemu/android/android-ui/modules/aemu-ui-qt/src/android/skin/qt/winsys-qt.cpp` implements the `winsys.h` interface.

### Implementing Thread-Safe UI Calls
When calling UI code from the emulator thread, you MUST use Qt signals and slots to ensure the code executes on the Qt Main Thread.

1.  **Define Signal**: Add a signal to the class header.
2.  **Add Slot**: Add a corresponding private slot that performs the actual work.
3.  **Cross-Thread Execution**:
    - For async calls: `emit mySignal(args);`
    - For sync (blocking) calls: Pass a `QSemaphore*`, emit the signal, and call `semaphore->acquire()`.

### UI Development
- **Layouts**: Prefer editing `.ui` files with Qt Designer.
- **Resources**: Add icons and assets to `resources.qrc` or `static_resources.qrc`.
- **Styling**: Stylesheets are used for branding and dark mode support; see `$REPO_ROOT/external/qemu/android/android-ui/modules/aemu-ui-qt/src/android/skin/qt/stylesheet.cpp`.

### Backend Interaction
Interact with the emulator exclusively via agents.
- **Agent Abstraction Mandate**: To support the ongoing transition to a decoupled architecture (Fishtank), UI code must NEVER access `qemulator->ui` or global backend state directly.
- **The Bridge Pattern**: Agents (via `getConsoleAgents()`) are the mandatory abstraction layer. The UI should call an Agent method; whether that Agent then performs a direct C call (legacy mode) or a gRPC call (Fishtank mode) is an implementation detail hidden from the UI.
- **Agent Stub Audit**: When functionality is missing or broken in decoupled modes, always audit the agent implementations (e.g., `$REPO_ROOT/external/qemu/android/android-ui/apps/fishtank/fishtank_window_agent.cpp`) for incomplete `TODO` stubs or commented-out code.

### Fishtank UI (Decoupled Mode)
Fishtank is a specialized, decoupled version of the emulator UI (`$REPO_ROOT/external/qemu/android/android-ui/apps/fishtank`) that operates as a gRPC client.
- **Mandate**: All development must adhere to the [Fishtank Architectural Principles](references/fishtank_principles.md).
- **Goal**: Move all functionality to a decoupled gRPC interface with Agents serving as the abstraction layer.
- **Event Loop**: Uses `receivePhysicalStateEvents` (gRPC stream) for real-time physical model updates (Position, Rotation, Hinge), replacing legacy polling.
- **Workflow**: Strictly follows TDD and atomic commits.
    - **TDD**: Write unit tests for all logic changes (see `*_unittest.cpp`).
    - **Documentation**: Update `DESIGN.md`, `SENSORS_AGENT.md`, and `WORKFLOW.md` after every task.
- **Agents**: Implements its own specialized agents (e.g., `sFishtankQAndroidSensorsAgent`) that proxy calls to the gRPC backend via gRPC, similar to Android Studio.

### Event Loop & UI Event Handling
Understand the difference between the legacy "Pull" model and the modern "Push" model required for decoupled UIs.

- **Standalone Mode (Pull Model)**: The QEMU `MainLoopThread` (backend) is the active participant. It periodically polls the UI for events via `skin_event_poll`.
- **Decoupled Mode (Surrogate Poll)**: Since the real backend is in a separate process, Fishtank uses a `messagePump` (in `$REPO_ROOT/external/qemu/android/android-ui/apps/fishtank/main.cpp`) as a surrogate backend thread. This thread runs a loop that calls `emulator_window_refresh` to drain the legacy skin event queue and forward events via Agents.
- **Push Model Mandate**: For modern UI components (like ToolWindow buttons), **always bypass the queue**. Call the Agent methods (e.g., `sendKey`) directly to ensure immediate gRPC delivery and avoid reliance on the surrogate polling loop.
- **No-Op onNewUserEvent**: Do NOT implement `onNewUserEvent` in gRPC-backed agents. In decoupled mode, the UI process cannot safely "notify" a remote backend to poll a local queue; implementing this often leads to re-entrancy hangs or deadlocks.

### Testing and Verification
- **Unit Tests**: Found in `$REPO_ROOT/external/qemu/android/android-ui/modules/aemu-ui-qt/test/` or alongside source as `*_unittest.cpp`.
- **Note on Unit Testing**: Currently, the Qt UI lacks comprehensive unit tests. A large refactor is required to decouple components enough to allow for effective testing.
- **Page/Controller Pattern**: For new pages in Extended Controls, use the Page/Controller pattern to allow mocking the backend in tests.
- **Visual Verification**: Always verify changes visually, as many UI issues (layout, clipping, focus) are not caught by unit tests.

## Common Tasks

### Adding a new page to Extended Controls
1. Create a `.ui` file for the layout.
2. Create a page class inheriting from `QWidget`.
3. Create a controller interface and mock for testing.
4. Register the new page in `ExtendedWindow::adjustTabs`.

### Debugging UI Events
- Trace events in `EmulatorQtWindow::event` or specific handlers like `keyPressEvent`.
- Use `QtLogger` for UI-side logging.

## References
- See [architecture.md](references/architecture.md) for detailed multi-threading and component details.
