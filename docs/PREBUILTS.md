# Prebuilt Bootstrap

This repository does not track `prebuilts/`. A fresh checkout needs a small
macOS arm64 subset of Android Emulator prebuilts before CMake can configure:

- `prebuilts/clang/host/darwin-x86/clang-r596125`
- `prebuilts/android-emulator-build/common/ANGLE/darwin-aarch64`
- `prebuilts/android-emulator-build/common/e2fsprogs/darwin-aarch64`
- `prebuilts/android-emulator-build/common/vulkan/darwin-aarch64`
- `prebuilts/android-emulator-build/qemu-android-deps/darwin-aarch64`
  pixman headers and library

The exact upstream repositories, revisions, and content digests are pinned in
`prebuilts.lock`.

## Preferred Independent Setup

Generate a cropped artifact once from a known-good machine, upload it to your
artifact store, then use that artifact in CI or clean developer machines:

```sh
./scripts/bootstrap-prebuilts.sh --make-artifact /tmp/aemu-prebuilts-darwin-aarch64.tar.gz

./scripts/bootstrap-prebuilts.sh \
  --from-artifact https://example.invalid/aemu-prebuilts-darwin-aarch64.tar.gz \
  --artifact-sha256 <sha256> \
  --force
```

This verifies the compressed artifact itself and extracts only the locked paths,
avoiding the full Android prebuilt history and a second copied tree on every
machine.

## Local Android Emulator Checkout

If a full Android Emulator checkout is already available, initialize from it:

```sh
./scripts/bootstrap-prebuilts.sh --from-local <android-emu-checkout> --force
```

The script checks that each source prebuilt repository is at the locked
revision before copying.

## Official Git Source

For environments without an internal artifact, the script can sparse-checkout
the locked paths directly from `android.googlesource.com`:

```sh
./scripts/bootstrap-prebuilts.sh --from-git --force
```

This is reproducible, but slower and heavier than using the cropped artifact.

## Verification

Check an existing `prebuilts/` tree against the lock:

```sh
./scripts/bootstrap-prebuilts.sh --verify
```
