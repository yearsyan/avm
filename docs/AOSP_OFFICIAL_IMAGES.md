# AOSP Official Images

This document describes how to build a normal AOSP arm64 emulator image and how
to run it with MacMu. The flow is for a non-ATD, graphical AOSP image built from
source, not for the Android SDK downloaded system image.

The validated target is Android 16:

```text
product: sdk_phone64_arm64
device:  emu64a
variant: user
```

The `user` variant is intentional. Do not use `userdebug` when validating the
release-style image path.

## Build on the AOSP Host

Use a Linux build host with the AOSP tree checked out. The examples below use
the current remote build machine layout:

```sh
ssh aosp
cd /home/user/aosp/android16
```

Initialize the build environment and select the Android 16 emulator product:

```sh
. build/envsetup.sh
lunch sdk_phone64_arm64 trunk_staging user
```

Some older trees use the single-argument lunch form instead:

```sh
lunch sdk_phone64_arm64-trunk_staging-user
```

Build the image set used by MacMu:

```sh
m ramdisk vendorbootimage systemimage vendorimage productimage \
  systemextimage superimage userdataimage
```

The expected output directory is:

```text
out/target/product/emu64a
```

The important artifacts are:

```text
android-info.txt
VerifiedBootParams.textproto
dtb.img
ramdisk.img
vendor_boot.img
system-qemu.img
vendor-qemu.img
product-qemu.img
system_ext-qemu.img
userdata.img
vbmeta.img
super.img
```

Keep the raw partition images too when archiving the build, because they are
useful for inspection and debugging:

```text
system.img
vendor.img
product.img
system_ext.img
system_dlkm.img
```

The product uses the ranchu 6.12 kernel from AOSP prebuilts:

```text
prebuilts/qemu-kernel/arm64/6.12/kernel-6.12-gz
```

Stage it as `kernel-ranchu` in the MacMu system image directory.

## Known Build Fix

If Soong reports a duplicate `vendor/etc/ueventd.rc` install between the
platform device tree and `device/kernel_launcher/vz_arm64`, give the VZ device
tree its own Soong namespace and add that namespace to the product:

```bp
// device/kernel_launcher/vz_arm64/Android.bp
soong_namespace {}
```

```make
# device/kernel_launcher/vz_arm64/aosp_vz_arm64.mk
PRODUCT_SOONG_NAMESPACES += device/kernel_launcher/vz_arm64
```

Re-run the same `m ...` command after applying the fix.

## Stage the Image Directory on macOS

Create a clean MacMu system image directory:

```sh
SYS=/Users/u/workspace/aemu/.codex-local/aosp16-emu64a-user-sysdir
mkdir -p "$SYS"
```

Copy the original AOSP build outputs:

```sh
rsync -av --progress \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/android-info.txt \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/VerifiedBootParams.textproto \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/dtb.img \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/ramdisk.img \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/vendor_boot.img \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/system-qemu.img \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/vendor-qemu.img \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/product-qemu.img \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/system_ext-qemu.img \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/userdata.img \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/vbmeta.img \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/super.img \
  "$SYS"/
```

Copy optional raw partition images for debugging:

```sh
rsync -av --progress \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/system.img \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/vendor.img \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/product.img \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/system_ext.img \
  aosp:/home/user/aosp/android16/out/target/product/emu64a/system_dlkm.img \
  "$SYS"/
```

Copy the kernel and add the kernel command line file:

```sh
rsync -av --progress \
  aosp:/home/user/aosp/android16/prebuilts/qemu-kernel/arm64/6.12/kernel-6.12-gz \
  "$SYS/kernel-ranchu"

printf '8250.nr_uarts=1\n' > "$SYS/kernel_cmdline.txt"
```

Do not manually merge `vendor_boot.img` into `ramdisk.img`. MacMu's qemu startup
path reads `vendor_boot.img`, extracts the vendor ramdisk blob, and appends it
to the initrd at runtime. This is required for Android 16 because the first-stage
virtio modules live in the vendor ramdisk, including `virtio_mmio.ko` and
`virtio_blk.ko`.

## Create the Metadata Disk

The Android 16 emulator fstab first-stage mounts `/metadata` by partition name.
Use a small GPT disk with a partition named `metadata`. The examples below run
on the Linux AOSP host because it has the usual disk tools:

```sh
ssh aosp
mkdir -p /tmp/aosp16-metadata-disk
cd /tmp/aosp16-metadata-disk

dd if=/dev/zero of=metadata.img bs=1M count=64
sgdisk --clear \
  --new=1:2048:0 \
  --change-name=1:metadata \
  --typecode=1:8300 \
  metadata.img

loop=$(sudo losetup --show -Pf metadata.img)
sudo mkfs.ext4 -F -L metadata "${loop}p1"
sudo losetup -d "$loop"
```

Copy it to macOS:

```sh
rsync -av --progress aosp:/tmp/aosp16-metadata-disk/metadata.img "$SYS/metadata.img"
```

MacMu currently reuses the emulator encryption-key disk slot for this metadata
disk in local validation:

```sh
cp "$SYS/metadata.img" "$SYS/encryptionkey.img"
```

## Create an AVD

Use a separate AVD home for this image set:

```sh
AVD_HOME=/Users/u/workspace/aemu/.codex-local/avd-home-aosp16
AVD_NAME=aosp16_emu64a_user
AVD_DIR="$AVD_HOME/$AVD_NAME.avd"

mkdir -p "$AVD_DIR"
```

Create the root AVD ini:

```sh
cat > "$AVD_HOME/$AVD_NAME.ini" <<EOF
avd.ini.encoding=UTF-8
path=$AVD_DIR
path.rel=avd/$AVD_NAME.avd
target=android-36
EOF
```

Create `config.ini`. These are the important values; it is fine to copy a
known-good arm64 AVD config and update the same fields:

```ini
avd.id=aosp16_emu64a_user
avd.name=aosp16_emu64a_user
abi.type=arm64-v8a
hw.cpu.arch=arm64
hw.cpu.ncore=4
hw.ramSize=2G
hw.lcd.width=1080
hw.lcd.height=2400
hw.lcd.density=420
hw.gpu.enabled=yes
hw.gpu.mode=host
disk.dataPartition.size=6G
disk.systemPartition.size=0
disk.vendorPartition.size=0
image.sysdir.1=/Users/u/workspace/aemu/.codex-local/aosp16-emu64a-user-sysdir/
tag.id=default
tag.ids=default
target=android-36
userdata.useQcow2=no
```

Link the writable AVD content images to the original qemu images:

```sh
ln -s "$SYS/system-qemu.img" "$AVD_DIR/system-qemu.img"
ln -s "$SYS/vendor-qemu.img" "$AVD_DIR/vendor-qemu.img"
cp "$SYS/encryptionkey.img" "$AVD_DIR/encryptionkey.img"
```

## Backend Smoke Test

After building MacMu, install the qemu backend into the distribution directory:

```sh
cmake --build build/cmake --target qemu-system-aarch64-headless
cmake --install build/cmake --config Release
```

Run the backend directly when debugging image boot problems:

```sh
ANDROID_AVD_HOME="$AVD_HOME" \
ANDROID_EMULATOR_LAUNCHER_DIR=/Users/u/workspace/aemu/build/cmake/distribution/emulator \
DYLD_LIBRARY_PATH=/Users/u/workspace/aemu/build/cmake/distribution/emulator/lib64:/Users/u/workspace/aemu/build/cmake/distribution/emulator/lib64/gles_angle:/Users/u/workspace/aemu/build/cmake/distribution/emulator/lib64/vulkan \
/Users/u/workspace/aemu/build/cmake/distribution/emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless \
  -avd "$AVD_NAME" \
  -sysdir "$SYS" \
  -no-window -no-audio -no-snapshot -no-boot-anim \
  -wipe-data \
  -gpu host \
  -show-kernel -verbose
```

Expected log evidence:

```text
vendor_boot ramdisk appended: .../vendor_boot.img
init: Loading module /lib/modules/virtio_mmio.ko
init: Loading module /lib/modules/virtio_blk.ko
Boot completed in ...
```

Expected guest properties:

```sh
adb -s emulator-5554 shell getprop ro.build.type
adb -s emulator-5554 shell getprop ro.build.version.release
adb -s emulator-5554 shell getprop ro.build.version.sdk
adb -s emulator-5554 shell getprop sys.boot_completed
```

Expected values:

```text
user
16
36
1
```

## MacMu Shell Launch

The product entry point is the shell:

```sh
/Users/u/workspace/aemu/build/cmake/distribution/emulator/macmu \
  --launcher-dir /Users/u/workspace/aemu/build/cmake/distribution/emulator \
  --avd-home "$AVD_HOME" \
  --system-path "$SYS" \
  --avd "$AVD_NAME"
```

For strict original-image validation, use the original AOSP `ramdisk.img`:

```sh
/Users/u/workspace/aemu/build/cmake/distribution/emulator/macmu \
  --launcher-dir /Users/u/workspace/aemu/build/cmake/distribution/emulator \
  --avd-home "$AVD_HOME" \
  --system-path "$SYS" \
  --guest-ramdisk "$SYS/ramdisk.img" \
  --avd "$AVD_NAME"
```

Current note: the bundled `lib/macmu-ramdisk.img` and `lib/macmu-agent.img` are
for the MacMu guest-agent path. If Android 16 user images stay at ADB `offline`
with the default shell launch, validate the original image path with a launcher
directory that does not contain those two files, while still pointing qemu and
libraries at the real distribution:

```sh
SHADOW=/Users/u/workspace/aemu/.codex-local/macmu-launcher-no-agent
REAL=/Users/u/workspace/aemu/build/cmake/distribution/emulator

mkdir -p "$SHADOW/lib"
ln -s "$REAL/qemu" "$SHADOW/qemu"
ln -s "$REAL/lib64" "$SHADOW/lib64"
ln -s "$REAL/bin64" "$SHADOW/bin64"
ln -s "$REAL/mksdcard" "$SHADOW/mksdcard"
ln -s "$REAL/source.properties" "$SHADOW/source.properties"

for file in "$REAL"/lib/*; do
  base=${file##*/}
  case "$base" in
    macmu-agent.img|macmu-ramdisk.img) ;;
    *) ln -s "$file" "$SHADOW/lib/$base" ;;
  esac
done

/Users/u/workspace/aemu/build/cmake/distribution/emulator/macmu \
  --launcher-dir "$SHADOW" \
  --avd-home "$AVD_HOME" \
  --system-path "$SYS" \
  --guest-ramdisk "$SYS/ramdisk.img" \
  --avd "$AVD_NAME"
```

This still starts through `macmu`; it only prevents the shell from selecting the
bundled guest-agent ramdisk while validating the original AOSP ramdisk.

## Validation Checklist

Use these checks after boot:

```sh
adb devices -l
adb -s emulator-5554 shell getprop sys.boot_completed
adb -s emulator-5554 shell getprop ro.build.type
adb -s emulator-5554 shell getprop ro.debuggable
adb -s emulator-5554 shell getprop ro.secure
adb -s emulator-5554 shell lsmod | grep virtio
adb -s emulator-5554 shell dumpsys SurfaceFlinger | sed -n '/GLES:/,/RenderEngine tracked buffers/p'
adb -s emulator-5554 exec-out screencap -p > aosp16-screencap.png
```

Expected:

```text
sys.boot_completed=1
ro.build.type=user
ro.debuggable=0
ro.secure=1
virtio_mmio, virtio_blk, virtio_gpu loaded
GLES: Google (Apple), Android Emulator OpenGL ES Translator (Apple M4)
ANDROID_EMU_vulkan present in SurfaceFlinger extensions
```
