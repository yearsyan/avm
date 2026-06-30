#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

import binascii
import hashlib
import math
import struct
import sys
import uuid


SECTOR_SIZE = 512
PARTITION_ENTRY_COUNT = 128
PARTITION_ENTRY_SIZE = 128
FIRST_PARTITION_LBA = 2048
LINUX_FILESYSTEM_GUID = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")


def crc32(data):
    return binascii.crc32(data) & 0xFFFFFFFF


def ceil_div(value, divisor):
    return (value + divisor - 1) // divisor


def padded(data, size):
    if len(data) > size:
        raise ValueError(f"data is larger than {size} bytes")
    return data + b"\0" * (size - len(data))


def partition_name(name):
    encoded = name.encode("utf-16le")
    if len(encoded) > 72:
        raise ValueError("GPT partition name is too long")
    return padded(encoded, 72)


def gpt_header(current_lba, backup_lba, first_usable_lba, last_usable_lba,
               disk_guid, entries_lba, entries_crc):
    header_size = 92
    header = struct.pack(
        "<8sIIIIQQQQ16sQIII",
        b"EFI PART",
        0x00010000,
        header_size,
        0,
        0,
        current_lba,
        backup_lba,
        first_usable_lba,
        last_usable_lba,
        disk_guid.bytes_le,
        entries_lba,
        PARTITION_ENTRY_COUNT,
        PARTITION_ENTRY_SIZE,
        entries_crc,
    )
    header = bytearray(padded(header, SECTOR_SIZE))
    struct.pack_into("<I", header, 16, crc32(bytes(header[:header_size])))
    return bytes(header)


def protective_mbr(total_lbas):
    mbr = bytearray(SECTOR_SIZE)
    sectors = min(total_lbas - 1, 0xFFFFFFFF)
    entry = struct.pack(
        "<B3sB3sII",
        0,
        b"\x00\x02\x00",
        0xEE,
        b"\xff\xff\xff",
        1,
        sectors,
    )
    mbr[446:446 + len(entry)] = entry
    mbr[510:512] = b"\x55\xaa"
    return bytes(mbr)


def main(argv):
    if len(argv) not in (3, 4):
        print("usage: wrap_ext4_gpt.py <input.ext4> <output.img> [partition-name]",
              file=sys.stderr)
        return 2

    src_path = argv[1]
    dst_path = argv[2]
    name = argv[3] if len(argv) == 4 else "macmu"

    with open(src_path, "rb") as src:
        ext4 = src.read()

    ext4_sha = hashlib.sha256(ext4).hexdigest()
    disk_guid = uuid.uuid5(uuid.NAMESPACE_URL, f"macmu-disk:{name}:{ext4_sha}")
    part_guid = uuid.uuid5(uuid.NAMESPACE_URL, f"macmu-partition:{name}:{ext4_sha}")

    partition_sectors = ceil_div(len(ext4), SECTOR_SIZE)
    partition_first_lba = FIRST_PARTITION_LBA
    partition_last_lba = partition_first_lba + partition_sectors - 1
    entries_sectors = ceil_div(PARTITION_ENTRY_COUNT * PARTITION_ENTRY_SIZE,
                               SECTOR_SIZE)
    backup_header_lba = partition_last_lba + entries_sectors + 1
    backup_entries_lba = backup_header_lba - entries_sectors
    total_lbas = backup_header_lba + 1

    first_usable_lba = 34
    last_usable_lba = backup_entries_lba - 1

    first_entry = struct.pack(
        "<16s16sQQQ72s",
        LINUX_FILESYSTEM_GUID.bytes_le,
        part_guid.bytes_le,
        partition_first_lba,
        partition_last_lba,
        0,
        partition_name(name),
    )
    entries = padded(first_entry, PARTITION_ENTRY_COUNT * PARTITION_ENTRY_SIZE)
    entries_crc = crc32(entries)

    primary_header = gpt_header(
        1,
        backup_header_lba,
        first_usable_lba,
        last_usable_lba,
        disk_guid,
        2,
        entries_crc,
    )
    backup_header = gpt_header(
        backup_header_lba,
        1,
        first_usable_lba,
        last_usable_lba,
        disk_guid,
        backup_entries_lba,
        entries_crc,
    )

    image = bytearray(total_lbas * SECTOR_SIZE)
    image[0:SECTOR_SIZE] = protective_mbr(total_lbas)
    image[SECTOR_SIZE:2 * SECTOR_SIZE] = primary_header
    primary_entries_offset = 2 * SECTOR_SIZE
    image[primary_entries_offset:primary_entries_offset + len(entries)] = entries
    partition_offset = partition_first_lba * SECTOR_SIZE
    image[partition_offset:partition_offset + len(ext4)] = ext4
    backup_entries_offset = backup_entries_lba * SECTOR_SIZE
    image[backup_entries_offset:backup_entries_offset + len(entries)] = entries
    backup_header_offset = backup_header_lba * SECTOR_SIZE
    image[backup_header_offset:backup_header_offset + SECTOR_SIZE] = backup_header

    with open(dst_path, "wb") as dst:
        dst.write(image)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
