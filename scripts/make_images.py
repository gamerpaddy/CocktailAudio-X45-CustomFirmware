#!/usr/bin/env python3
"""Draw the four small pictures the unit's updater shows while it installs a package.

    make_images.py OUTDIR

The updater expects these names and sizes: TOP.png (1024x70 banner), PKG_TOP.png (46x46 icon),
PRO_BK_8.png and PRO_SEL_8.png (1024x8 progress bar, background and filled part). The pictures
are plain flat colours and a gradient, made here, so the package needs nothing from anywhere else.
"""
import os
import struct
import sys
import zlib


def png(path, w, h, pixel, alpha):
    """Write a PNG. pixel(x, y) returns (r, g, b) or (r, g, b, a)."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type: none
        for x in range(w):
            raw.extend(pixel(x, y))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6 if alpha else 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def top(x, y):
    # dark blue-grey banner, slightly lighter towards the right, thin bright line at the bottom
    base = 28 + 22 * x // 1024
    if y >= 66:
        return (70, 130, 200)
    return (base, base + 6, base + 16)


def icon(x, y):
    # rounded square, blue, with a lighter inner square
    cx, cy = x - 22.5, y - 22.5
    r = 8.0
    ax, ay = max(abs(cx) - (22.5 - r), 0), max(abs(cy) - (22.5 - r), 0)
    if ax * ax + ay * ay > r * r:
        return (0, 0, 0, 0)
    if abs(cx) < 11 and abs(cy) < 11:
        return (200, 225, 250, 255)
    return (50, 110, 190, 255)


def main(out):
    os.makedirs(out, exist_ok=True)
    png(os.path.join(out, "TOP.png"), 1024, 70, top, False)
    png(os.path.join(out, "PKG_TOP.png"), 46, 46, icon, True)
    png(os.path.join(out, "PRO_BK_8.png"), 1024, 8, lambda x, y: (60, 64, 72, 255), True)
    png(os.path.join(out, "PRO_SEL_8.png"), 1024, 8, lambda x, y: (70, 150, 235, 255), True)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
