#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

import sys


def find_trailer_start(data: bytes, *, final: bool) -> int:
    marker = b"TRAILER!!!"
    pos = len(data) if final else 0
    while True:
        pos = data.rfind(marker, 0, pos) if final else data.find(marker, pos)
        if pos < 110:
            raise RuntimeError("newc TRAILER!!! entry not found")
        header = pos - 110
        if data[header:header + 6] in (b"070701", b"070702"):
            namesize = int(data[header + 94:header + 102], 16)
            filesize = int(data[header + 54:header + 62], 16)
            if namesize == len(marker) + 1 and filesize == 0:
                return header
        pos = pos - 1 if final else pos + 1


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: merge_newc.py <base.cpio> <overlay.cpio> <out.cpio>", file=sys.stderr)
        return 2

    base_path, overlay_path, out_path = sys.argv[1:]
    with open(base_path, "rb") as f:
        base = f.read()
    with open(overlay_path, "rb") as f:
        overlay = f.read()

    base_trailer = find_trailer_start(base, final=False)
    overlay_trailer = find_trailer_start(overlay, final=True)
    with open(out_path, "wb") as f:
        f.write(base[:base_trailer])
        f.write(overlay[:overlay_trailer])
        f.write(base[base_trailer:])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
