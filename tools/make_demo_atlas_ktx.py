#!/usr/bin/env python3
"""Writes the M1 demo atlas: app/src/main/assets/textures/demo_atlas.ktx.

KTX 1.1, GL_RGBA8, 128x128: a 4x4 grid of 32x32 tiles, index row-major from the top-left,
matching mge::demo::AtlasTile:
  0 solid stone   1 stone with grass top   2 platform planks   3 crate (greyscale, tinted)
  4 ball (greyscale, tinted, alpha)   5 goal stripes   6 backdrop bricks (greyscale)   7 white
Three mip levels only (down to 8x8 tiles) so lower mips never blend whole tiles together.
Deterministic (fixed-seed noise); rerun only to change the art.
"""
import math
import os
import random
import struct

TILE, GRID = 32, 4
SIZE = TILE * GRID
MIPS = 3
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "app", "src", "main", "assets", "textures", "demo_atlas.ktx")
rnd = random.Random(1234)


def clamp(v):
    return max(0, min(255, int(round(v))))


def stone(x, y, n):
    edge = x in (0, TILE - 1) or y in (0, TILE - 1)
    v = 92 + n * 22 - (26 if edge else 0)
    if (x // 8 + y // 16) % 2 == 0 and x % 8 == 0:
        v -= 18
    return (v, v + 2, v + 8, 255)


def tile_pixel(t, x, y):
    n = rnd.random()
    if t == 0:
        return stone(x, y, n)
    if t == 1:
        if y < 6 + (1 if (x * 7) % 5 == 0 else 0):
            g = 120 + n * 40
            return (g * 0.35, g, g * 0.3, 255)
        return stone(x, y, n)
    if t == 2:
        plank = (y // 8) % 2
        base = 150 if plank else 132
        v = base + n * 18 - (30 if y % 8 == 0 else 0) - (20 if x in (0, TILE - 1) else 0)
        return (v, v * 0.72, v * 0.45, 255)
    if t == 3:
        border = x < 3 or y < 3 or x > TILE - 4 or y > TILE - 4
        diag = abs(x - y) < 2 or abs(x + y - (TILE - 1)) < 2
        v = 165 if border or diag else 235 - n * 12
        return (v, v, v, 255)
    if t == 4:
        cx = cy = (TILE - 1) / 2
        r = TILE / 2 - 0.5
        d = math.hypot(x - cx, y - cy)
        a = max(0.0, min(1.0, r - d + 0.5))
        shade = 255 - 70 * max(0.0, (x - cx + y - cy) / (2 * r))   # light from the top-left
        hi = 1.0 if math.hypot(x - cx + 5, y - cy + 5) < 4 else 0.0
        v = shade * (1 - hi) + 255 * hi
        return (v, v, v, a * 255)
    if t == 5:
        stripe = ((x + y) // 6) % 2
        return ((60, 52, 20, 255) if stripe else (110, 92, 28, 255))
    if t == 6:
        row = y // 8
        off = 8 if row % 2 else 0
        mortar = y % 8 == 0 or (x + off) % 16 == 0
        v = 120 if mortar else 200 + n * 30
        return (v, v, v, 255)
    return (255, 255, 255, 255)


def base_level():
    px = bytearray(SIZE * SIZE * 4)
    for t in range(GRID * GRID):
        tx, ty = (t % GRID) * TILE, (t // GRID) * TILE
        for y in range(TILE):
            for x in range(TILE):
                r, g, b, a = tile_pixel(t, x, y)
                i = ((ty + y) * SIZE + tx + x) * 4
                px[i:i + 4] = bytes((clamp(r), clamp(g), clamp(b), clamp(a)))
    return SIZE, SIZE, bytes(px)


def downsample(w, h, px):
    nw, nh = w // 2, h // 2
    out = bytearray()
    for y in range(nh):
        for x in range(nw):
            for c in range(4):
                s = sum(px[((2 * y + dy) * w + 2 * x + dx) * 4 + c] for dy in (0, 1) for dx in (0, 1))
                out.append((s + 2) // 4)
    return nw, nh, bytes(out)


def main():
    levels = [base_level()]
    while len(levels) < MIPS:
        levels.append(downsample(*levels[-1]))
    ident = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])
    header = struct.pack("<13I", 0x04030201, 0x1401, 1, 0x1908, 0x8058, 0x1908, SIZE, SIZE, 0, 0, 1, len(levels), 0)
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
