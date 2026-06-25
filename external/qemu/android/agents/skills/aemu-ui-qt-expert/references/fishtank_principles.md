# Fishtank Architectural Principles

These principles represent the **Strategic Intent** for the Fishtank UI. They must be followed to ensure long-term maintainability and framework independence.

## 1. Absolute Decoupling
Fishtank is a decoupled UI that must remain completely decoupled from the emulator backend.
- **No Backend Headers:** Never include headers from `android/android-emu` or other backend-specific directories that are not part of the common/public interface.
- **Communication via gRPC Only:** All interaction with the backend must occur through the gRPC interface. Agents (e.g., `sFishtankQAndroidSensorsAgent`) act as the local proxy for these remote calls to an **Embedded Emulator**.

## 2. Stateless UI Layer
The UI components in Fishtank should not maintain their own "Source of Truth" for physical or system state.
- **Push-Based Updates:** UI state should be driven by events received from the backend (e.g., via gRPC streams).
- **Reactive Design:** When a UI element (like a slider) is moved, it should send a command to the backend. The UI should only update its visual state when it receives the confirmation or state event back from the backend.

## 3. Framework Independence (Future-Proofing)
The codebase must be designed with the intention of eventually swapping the Qt framework for a different one (e.g., Compose Multiplatform).
- **Separate Logic from View:** Keep business logic, event handling, and gRPC client code in pure C++ classes that do not inherit from Qt classes (`QWidget`, `QObject`) where possible.
- **Abstraction Layers:** Use interfaces or "Controllers" to wrap framework-specific code. The core logic should call these interfaces.
- **Minimal Qt Footprint:** Avoid using Qt-specific containers (`QString`, `QList`) in the core logic layer; prefer standard C++ types (`std::string`, `std::vector`).

## 4. Testability via Mocks
Every component must be unit-testable without a running emulator.
- **Controller Pattern:** Every complex UI page must have a Controller interface.
## 5. Event Model: Push vs. Pull
Fishtank transitions the UI from being a passive data source to an active event producer.

- **Surrogate Backend:** Fishtank uses a `messagePump` thread to simulate the QEMU main loop. This is a compatibility shim for legacy code (like physical keyboard handling) that still relies on the `mSkinEventQueue`.
- **Direct Push Mandate:** High-priority UI controls (Toolbar, Extended Controls) must skip the legacy queue. They should call Agent methods directly. This ensures that a button press results in an immediate gRPC call rather than waiting for the `messagePump` to "discover" the event in the next poll cycle.
- **Decoupled Notification:** The legacy `onNewUserEvent` notification is strictly for single-process architectures where the backend needs to be woken up to poll a local memory structure. It must remain a no-op in Fishtank to avoid process-local hangs.
