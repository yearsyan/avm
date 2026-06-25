---
name: emulator-build
description: Instructions for building the Android Emulator. Use when a full rebuild or incremental build is required, or when the user asks about the build process.
---

# Emulator Build Skill

This skill provides instructions on how to build the Android Emulator.

## Core Procedures

### Full Rebuild
A full rebuild should be performed when you want to ensure a clean state or when making significant changes to the build configuration.

- **Linux/macOS**:
  Run from the `external/qemu` directory:
  ```bash
  ./android/rebuild.sh --qtwebengine
  ```
- **Windows**:
  Run from the `external/qemu` directory:
  ```cmd
  android\rebuild.cmd --qtwebengine
  ```

### Incremental Build
Incremental builds are much faster and should be used during regular development to compile changes.

- **All Platforms**:
  Run from the `external/qemu` directory:
  ```bash
  python3 android/build/python/cmake.py --task Compile
  ```

## Contextual Notes
- All build commands MUST be executed from the `external/qemu` directory.
- The `--qtwebengine` flag ensures that the Qt WebEngine component is included in the build, which is necessary for certain UI features.
