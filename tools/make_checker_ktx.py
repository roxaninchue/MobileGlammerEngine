#!/usr/bin/env python3
"""Writes the demo's checker texture: app/src/main/assets/textures/checker.ktx.

KTX 1.1, GL_RGBA8, 64x64 with a full box-filtered mip chain. Monochrome (white / grey) so
the per-sprite tint decides the shade. Rerun only if you want a different pattern.
"""
import os
import struct

SIZE, CELL = 64, 8
LIGHT, DARK = 255, 140
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "app", "src", "main", "assets", "textures", "checker.ktx")


def base_level():
    px = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            v = LIGHT if ((x // CELL) + (y // CELL)) % 2 == 0 else DARK
            px += bytes((v, v, v, 255))
    return SIZE, SIZE, bytes(px)


def downsample(w, h, px):
    nw, nh = max(1, w // 2), max(1, h // 2)
    out = bytearray()
    for y in range(nh):
        for x in range(nw):
            for c in range(4):
                s = 0
                for dy in (0, 1):
                    for dx in (0, 1):
                        sx, sy = min(w - 1, 2 * x + dx), min(h - 1, 2 * y + dy)
                        s += px[(sy * w + sx) * 4 + c]
                out.append((s + 2) // 4)
    return nw, nh, bytes(out)


def main():
    levels = [base_level()]
    while levels[-1][0] > 1 or levels[-1][1] > 1:
        levels.append(downsample(*levels[-1]))
    ident = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])
    header = struct.pack("<13I", 0x04030201, 0x1401, 1, 0x1908, 0x8058, 0x1908,
                         SIZE, SIZE, 0, 0, 1, len(levels), 0)
    body = bytearray()
    for _, _, px in levels:
        body += struct.pack("<I", len(px)) + px
        body += b"\0" * ((4 - len(px) % 4) % 4)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(ident + header + body)
    print(f"Wrote {OUT}: {SIZE}x{SIZE} RGBA8, {len(levels)} mips")


if __name__ == "__main__":
    main()
