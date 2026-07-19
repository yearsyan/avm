# Android Image Distribution

MacMu supports two transport formats for the same Android 16 `user` image:

1. A complete ZIP for offline installation and simple release attachments.
2. A v2 chunk manifest plus immutable objects for CDN delivery and incremental
   updates.

Both formats reconstruct the same validated `aosp16-arm64` system image
directory. Machine creation and qemu startup do not depend on which transport
was used.

## Complete ZIP

Build the complete archive with:

```sh
scripts/package_aosp16_image.sh \
  --source-dir "$SYS" \
  --output .codex-local/macmu-aosp16-arm64-system-image.zip
```

The script also creates
`macmu-aosp16-arm64-system-image.zip.sha256`. This is the preferred format for
offline copies and users who want one file.

## Chunked CDN Bundle

Build the chunked transport with:

```sh
scripts/package_aosp16_chunked.py \
  --source-dir "$SYS" \
  --output-dir .codex-local/macmu-aosp16-arm64-chunked
```

The default policy is:

- Chunk files of 8 MiB or larger.
- Use 64 MiB uncompressed chunks.
- Compress every chunk as an independent deterministic ZIP object.
- Store the chunk as the ZIP entry `payload`.
- Name every object from the SHA-256 of its complete ZIP bytes.
- Put small image files and `macmu-image-info.txt` in one base object.
- Deduplicate identical objects across files and image releases.

The output layout is:

```text
macmu-aosp16-arm64-chunked/
  manifest.json
  manifest.json.sha256
  objects/
    <sha256>.zip
    <sha256>.zip
    ...
```

Use a clean output directory for a minimal single-release upload. The packager
does not delete older unreferenced objects from an existing output directory,
which also permits one directory to retain objects shared by multiple
manifests.

To make a manifest point directly at a CDN object prefix:

```sh
scripts/package_aosp16_chunked.py \
  --source-dir "$SYS" \
  --output-dir .codex-local/macmu-aosp16-arm64-chunked \
  --base-url https://cdn.example.com/macmu/images/aosp16-arm64/
```

`--base-url` must use HTTPS. When it is omitted, object paths are resolved
relative to the local or remote manifest.

## Manifest Contract

The stable identity fields are:

```json
{
  "format": "macmu-system-image",
  "version": 2,
  "product": "macmu_sdk_phone64_arm64",
  "device": "emu64a",
  "variant": "user",
  "api": 36,
  "root": "aosp16-arm64",
  "chunk_size": 67108864
}
```

`base` describes the archive containing the small files. Every entry in
`files` records the reconstructed file size and SHA-256 plus an ordered,
contiguous list of chunks. Each chunk records:

- Raw offset, size, and SHA-256.
- Object path, compressed size, and compressed SHA-256.
- The ZIP entry name, currently `payload`.

The manifest and object archives are deterministic for the same input files
and packaging options.

## CDN Policy

Upload `manifest.json` and every referenced `objects/<sha256>.zip`. Recommended
cache behavior:

- Object archives: immutable, long-lived cache (`max-age=31536000, immutable`).
- Versioned manifest URLs: long-lived cache is safe.
- A mutable `latest/manifest.json`: short cache or explicit revalidation.
- Enable byte ranges for object archives so interrupted downloads can resume.
- Preserve the exact object bytes; their filename and manifest SHA-256 cover
  the compressed archive, not only the uncompressed payload.

Publishing a new image normally uploads only new object hashes plus its new
manifest. Unchanged 64 MiB regions remain CDN and client-cache hits.

## MacMu Import

When the managed system image is absent and no explicit source was supplied,
MacMu does not start a download. The setup screen offers **Official Image** and
**Other Source…**. The official option uses:

```text
https://storage.macmu.org/images/aosp16-arm64/manifest.json
```

The other-source picker accepts:

- A local complete `.zip`.
- A local chunk `manifest.json`.
- A local directory containing `manifest.json` and its relative `objects/`
  files.

For a hosted manifest, launch MacMu once with:

```sh
build/cmake/distribution/emulator/macmu \
  --import-image https://cdn.example.com/macmu/images/aosp16-arm64/manifest.json
```

`--import-image` also accepts a chunk directory directly:

```sh
build/cmake/distribution/emulator/macmu \
  --import-image /path/to/macmu-aosp16-arm64-chunked
```

The equivalent environment variable is:

```text
MACMU_IMPORT_IMAGE
```

An explicit source starts unattended and bypasses the chooser. Automation that
wants the official image can opt in with:

```sh
build/cmake/distribution/emulator/macmu --auto-image-import
```

or:

```text
MACMU_AUTO_IMPORT_IMAGE=1
```

`--no-auto-image-import` or `MACMU_AUTO_IMPORT_IMAGE=0` forces the interactive
chooser when a launcher or wrapper otherwise enables automatic import.

MacMu imports a source only when its managed system image is absent.
HTTPS object downloads use eight workers, retries, byte-range resume, compressed
object size/SHA-256 validation, and full reconstructed-file SHA-256 validation.
Verified remote objects are cached at:

```text
~/Library/MacMu/cache/image-objects/<object-sha256>.zip
```

The final image directory is installed atomically only after reconstruction
and the normal MacMu image validation succeed. Remote complete ZIP URLs are
not accepted; publish the chunk manifest for network installation.

## Current R2 Publication

The production image is stored in the Cloudflare R2 bucket `macmu` through the
custom domain `storage.macmu.org`:

```text
images/aosp16-arm64/manifest.json
images/aosp16-arm64/manifest.json.sha256
images/aosp16-arm64/objects/<sha256>.zip
```

Object archives use `public, max-age=31536000, immutable`. The mutable
manifest uses `public, max-age=300, must-revalidate` and is uploaded only after
all referenced objects have been published and checked.
