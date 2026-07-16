#!/usr/bin/env python3
"""Build a deterministic, CDN-friendly MacMu Android 16 image bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Any, BinaryIO, Iterable


FORMAT_NAME = "macmu-system-image"
FORMAT_VERSION = 2
DEFAULT_PACKAGE_NAME = "aosp16-arm64"
DEFAULT_CHUNK_SIZE = 64 * 1024 * 1024
DEFAULT_CHUNK_THRESHOLD = 8 * 1024 * 1024
COPY_BUFFER_SIZE = 1024 * 1024
ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)
ZIP_ENTRY_NAME = "payload"

REQUIRED_FILES = (
    "advancedFeatures.ini",
    "android-info.txt",
    "VerifiedBootParams.textproto",
    "kernel-ranchu",
    "ramdisk.img",
    "system-qemu.img",
    "userdata.img",
    "vendor-qemu.img",
    "vendor_boot.img",
)
OPTIONAL_FILES = (
    "build.prop",
    "dtb.img",
    "fstab.ranchu",
    "kernel_cmdline.txt",
    "system-qemu-config.txt",
)
IMAGE_INFO = (
    b"format=1\n"
    b"product=macmu_sdk_phone64_arm64\n"
    b"device=emu64a\n"
    b"variant=user\n"
    b"api=36\n"
)


class PackagingError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while data := source.read(COPY_BUFFER_SIZE):
            digest.update(data)
    return digest.hexdigest()


def validate_package_name(name: str) -> None:
    if not name or name in {".", ".."} or "/" in name or "\\" in name:
        raise PackagingError("--package-name must be a single directory name")


def validate_metadata_image(path: Path) -> None:
    if path.stat().st_size != 64 * 1024 * 1024:
        raise PackagingError(f"metadata image must be exactly 64 MiB: {path}")
    with path.open("rb") as source:
        source.seek(512)
        if source.read(8) != b"EFI PART":
            raise PackagingError(f"metadata image does not contain a GPT header: {path}")
        source.seek(1080)
        if source.read(16) != "metadata".encode("utf-16le"):
            raise PackagingError(f"metadata image first GPT partition is not named metadata: {path}")
        source.seek(1049656)
        if source.read(2) != b"\x53\xef":
            raise PackagingError(f"metadata partition is not ext4: {path}")
        source.seek(1049720)
        if source.read(8) != b"metadata":
            raise PackagingError(f"metadata ext4 filesystem is not labelled metadata: {path}")


def regular_zip_info(name: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, ZIP_TIMESTAMP)
    info.create_system = 3
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (stat.S_IFREG | 0o644) << 16
    info._compresslevel = 9  # zipfile has no public per-entry setter.
    return info


def directory_zip_info(name: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name.rstrip("/") + "/", ZIP_TIMESTAMP)
    info.create_system = 3
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = (stat.S_IFDIR | 0o755) << 16
    return info


def write_file_entry(archive: zipfile.ZipFile, entry_name: str, source_path: Path) -> None:
    with source_path.open("rb") as source, archive.open(
        regular_zip_info(entry_name), "w", force_zip64=True
    ) as destination:
        shutil.copyfileobj(source, destination, COPY_BUFFER_SIZE)


def write_bytes_entry(archive: zipfile.ZipFile, entry_name: str, data: bytes) -> None:
    with archive.open(regular_zip_info(entry_name), "w", force_zip64=True) as destination:
        destination.write(data)


def finalize_object(temp_path: Path, objects_dir: Path) -> dict[str, object]:
    object_sha256 = sha256_file(temp_path)
    object_path = objects_dir / f"{object_sha256}.zip"
    if object_path.exists():
        if (
            object_path.stat().st_size == temp_path.stat().st_size
            and sha256_file(object_path) == object_sha256
        ):
            temp_path.unlink()
        else:
            os.replace(temp_path, object_path)
    else:
        os.replace(temp_path, object_path)
    object_path.chmod(0o644)
    return {
        "object": f"objects/{object_path.name}",
        "object_size": object_path.stat().st_size,
        "object_sha256": object_sha256,
    }


def build_base_object(
    output_dir: Path,
    objects_dir: Path,
    package_name: str,
    base_files: Iterable[tuple[str, Path]],
) -> dict[str, object]:
    fd, temp_name = tempfile.mkstemp(prefix=".base.", suffix=".zip", dir=output_dir)
    os.close(fd)
    temp_path = Path(temp_name)
    try:
        with zipfile.ZipFile(
            temp_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9, allowZip64=True
        ) as archive:
            archive.writestr(directory_zip_info(package_name), b"")
            for name, source_path in sorted(base_files):
                write_file_entry(archive, f"{package_name}/{name}", source_path)
            write_bytes_entry(archive, f"{package_name}/macmu-image-info.txt", IMAGE_INFO)
        return finalize_object(temp_path, objects_dir)
    finally:
        temp_path.unlink(missing_ok=True)


def build_chunk_object(
    source: BinaryIO,
    output_dir: Path,
    objects_dir: Path,
    size: int,
    full_digest: Any,
) -> dict[str, object]:
    fd, temp_name = tempfile.mkstemp(prefix=".chunk.", suffix=".zip", dir=output_dir)
    os.close(fd)
    temp_path = Path(temp_name)
    chunk_digest = hashlib.sha256()
    try:
        with zipfile.ZipFile(
            temp_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9, allowZip64=True
        ) as archive:
            with archive.open(
                regular_zip_info(ZIP_ENTRY_NAME), "w", force_zip64=True
            ) as destination:
                remaining = size
                while remaining:
                    data = source.read(min(COPY_BUFFER_SIZE, remaining))
                    if not data:
                        raise PackagingError(
                            "source image changed or ended while it was being packaged"
                        )
                    destination.write(data)
                    chunk_digest.update(data)
                    full_digest.update(data)
                    remaining -= len(data)
        result = finalize_object(temp_path, objects_dir)
        result.update(
            {
                "size": size,
                "sha256": chunk_digest.hexdigest(),
                "entry": ZIP_ENTRY_NAME,
            }
        )
        return result
    finally:
        temp_path.unlink(missing_ok=True)


def choose_metadata_image(source_dir: Path, explicit: Path | None) -> Path:
    if explicit:
        path = explicit
    elif (source_dir / "encryptionkey.img").is_file():
        path = source_dir / "encryptionkey.img"
    elif (source_dir / "metadata.img").is_file():
        path = source_dir / "metadata.img"
    else:
        raise PackagingError(
            "missing encryptionkey.img; create it with scripts/create_android16_metadata_image.sh"
        )
    if not path.is_file():
        raise PackagingError(f"metadata image does not exist: {path}")
    return path.resolve()


def collect_source_files(source_dir: Path, metadata_image: Path) -> list[tuple[str, Path]]:
    for name in REQUIRED_FILES:
        if not (source_dir / name).is_file():
            raise PackagingError(f"missing required image file: {source_dir / name}")
    files = [(name, (source_dir / name).resolve()) for name in REQUIRED_FILES]
    files.extend(
        (name, (source_dir / name).resolve())
        for name in OPTIONAL_FILES
        if (source_dir / name).is_file()
    )
    files.append(("encryptionkey.img", metadata_image))
    return sorted(files)


def package(args: argparse.Namespace) -> None:
    source_dir = args.source_dir.resolve()
    output_dir = args.output_dir.resolve()
    validate_package_name(args.package_name)
    if not source_dir.is_dir():
        raise PackagingError(f"source directory does not exist: {source_dir}")
    if args.chunk_size <= 0:
        raise PackagingError("--chunk-size-mib must be greater than zero")
    if args.chunk_threshold <= 0:
        raise PackagingError("--chunk-threshold-mib must be greater than zero")
    if args.base_url and not args.base_url.startswith("https://"):
        raise PackagingError("--base-url must use HTTPS")

    metadata_image = choose_metadata_image(source_dir, args.metadata_image)
    validate_metadata_image(metadata_image)
    source_files = collect_source_files(source_dir, metadata_image)

    output_dir.mkdir(parents=True, exist_ok=True)
    objects_dir = output_dir / "objects"
    objects_dir.mkdir(parents=True, exist_ok=True)
    for stale in output_dir.glob(".chunk.*.zip"):
        stale.unlink()
    for stale in output_dir.glob(".base.*.zip"):
        stale.unlink()

    base_files = [
        (name, path)
        for name, path in source_files
        if path.stat().st_size < args.chunk_threshold
    ]
    chunked_files = [
        (name, path)
        for name, path in source_files
        if path.stat().st_size >= args.chunk_threshold
    ]

    print(f"Creating base object with {len(base_files) + 1} files", flush=True)
    base = build_base_object(output_dir, objects_dir, args.package_name, base_files)

    manifest_files: list[dict[str, object]] = []
    for name, path in chunked_files:
        file_size = path.stat().st_size
        full_digest = hashlib.sha256()
        chunks: list[dict[str, object]] = []
        print(f"Chunking {name} ({file_size} bytes)", flush=True)
        with path.open("rb") as source:
            offset = 0
            while offset < file_size:
                chunk_size = min(args.chunk_size, file_size - offset)
                chunk = build_chunk_object(
                    source, output_dir, objects_dir, chunk_size, full_digest
                )
                chunk["offset"] = offset
                chunks.append(chunk)
                offset += chunk_size
        if path.stat().st_size != file_size:
            raise PackagingError(f"source image changed while packaging: {path}")
        manifest_files.append(
            {
                "path": name,
                "size": file_size,
                "sha256": full_digest.hexdigest(),
                "chunks": chunks,
            }
        )

    base_url = args.base_url.rstrip("/") + "/" if args.base_url else ""
    manifest = {
        "format": FORMAT_NAME,
        "version": FORMAT_VERSION,
        "product": "macmu_sdk_phone64_arm64",
        "device": "emu64a",
        "variant": "user",
        "api": 36,
        "root": args.package_name,
        "chunk_size": args.chunk_size,
        "base_url": base_url,
        "base": base,
        "files": manifest_files,
    }
    manifest_bytes = (
        json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    manifest_path = output_dir / "manifest.json"
    manifest_temp = output_dir / ".manifest.json.tmp"
    manifest_temp.write_bytes(manifest_bytes)
    manifest_temp.chmod(0o644)
    os.replace(manifest_temp, manifest_path)
    manifest_digest = hashlib.sha256(manifest_bytes).hexdigest()
    digest_path = output_dir / "manifest.json.sha256"
    digest_temp = output_dir / ".manifest.json.sha256.tmp"
    digest_temp.write_text(
        f"{manifest_digest}  manifest.json\n", encoding="utf-8"
    )
    digest_temp.chmod(0o644)
    os.replace(digest_temp, digest_path)

    referenced_objects = {base["object"]}
    for file_record in manifest_files:
        referenced_objects.update(chunk["object"] for chunk in file_record["chunks"])
    total_object_size = sum(
        (output_dir / object_path).stat().st_size for object_path in referenced_objects
    )
    print(f"Created {manifest_path}", flush=True)
    print(f"Created {len(referenced_objects)} objects ({total_object_size} bytes)", flush=True)
    print(f"Manifest SHA-256: {manifest_digest}", flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Create manifest.json plus independent deterministic ZIP objects for "
            "a MacMu Android 16 image."
        )
    )
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--metadata-image", type=Path)
    parser.add_argument("--package-name", default=DEFAULT_PACKAGE_NAME)
    parser.add_argument(
        "--base-url",
        default="",
        help="Optional HTTPS CDN base URL stored in the manifest.",
    )
    parser.add_argument(
        "--chunk-size-mib",
        dest="chunk_size",
        type=lambda value: int(value) * 1024 * 1024,
        default=DEFAULT_CHUNK_SIZE,
    )
    parser.add_argument(
        "--chunk-threshold-mib",
        dest="chunk_threshold",
        type=lambda value: int(value) * 1024 * 1024,
        default=DEFAULT_CHUNK_THRESHOLD,
    )
    return parser.parse_args()


def main() -> int:
    try:
        package(parse_args())
        return 0
    except (OSError, PackagingError, zipfile.BadZipFile) as error:
        print(f"package_aosp16_chunked.py: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
