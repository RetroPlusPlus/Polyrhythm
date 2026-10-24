#!/usr/bin/env python3
"""Author the Space Invaders demo's sprite sheet: ONE indexed PNG holding every sprite, laid out as a
single row of 8x8 cells so Renderer::loadAtlas can slice it into addressable slots.

This is the ASSET-LOAD route, contrasting Centipede's embedded-bytes route: examples/centipede_demo.cpp
builds its indexed atlas from in-code byte grids and uploadAtlas; examples/space_invaders_demo.cpp loads
THIS committed indexed PNG via loadAtlas(...) and uses the returned manifest's slots. Both are indexed
(the pixels are palette INDICES, color comes from palettes the demo uploads) — only the atlas SOURCE
differs.

Index 0 = transparent background (the sprite path discards it), 1 = the sprite's main color slot, 2 = an
accent slot (the cannon tip, the UFO windows). The PNG's own PLTE is a neutral default so the file is a
valid viewable indexed image; the demo selects its own palettes per sprite (per-row alien colors, etc.).

Engine-original art (no Taito / arcade-ROM content). One-time authoring tool, kept committed so the PNG
stays regenerable and its index plane is auditable — same posture + dependency-free PNG encoder as the
other gen_*.py tools.

Run from the engine repo root:  python3 examples/assets/gen_space_invaders.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent  # engine/examples/assets

# Each sprite is an 8x8 grid: '.' = 0 (transparent), '1' = main, '2' = accent. The ORDER here is the
# slot order loadAtlas(SpriteSeries, LeftRightThenDown) returns — the demo's sprite enum matches it.
SPRITES: list[tuple[str, list[str]]] = [
    ("squidA", ["...11...", "..1111..", ".111111.", "11.11.11", "11111111", ".1.11.1.", "1......1", ".1....1."]),
    ("squidB", ["...11...", "..1111..", ".111111.", "11.11.11", "11111111", ".1.11.1.", ".1....1.", "1......1"]),
    ("crabA",  ["1..11..1", "11111111", "11.11.11", "11111111", ".111111.", "..1..1..", ".1.11.1.", "1.1..1.1"]),
    ("crabB",  ["1..11..1", "11111111", "11.11.11", "11111111", ".111111.", ".1.11.1.", "1.1..1.1", "..1..1.."]),
    ("octoA",  ["..1111..", ".111111.", "11111111", "11.11.11", "11111111", "..1..1..", ".11..11.", "11....11"]),
    ("octoB",  ["..1111..", ".111111.", "11111111", "11.11.11", "11111111", ".1.11.1.", "1.1..1.1", "1.1..1.1"]),
    ("cannon", ["...22...", "...22...", "..1111..", ".111111.", "11111111", "11111111", "11111111", "........"]),
    ("bullet", ["........", "...11...", "...11...", "...11...", "........", "........", "........", "........"]),
    ("bomb",   ["...1....", "..1.....", "...1....", "....1...", "...1....", "..1.....", "...1....", "........"]),
    ("ufo",    ["........", "..1111..", ".122221.", "11111111", ".1.11.1.", "........", "........", "........"]),
    ("explo",  ["1..1..1.", ".1.1.1..", "..111...", "11.1.11.", "..111...", ".1.1.1..", "1..1..1.", "........"]),
    ("bunker", ["11111111", "11111111", "11111111", "11111111", "11111111", "11111111", "11111111", "11111111"]),
    ("bunkerX",["1.1.11.1", "111.1111", ".1111.1.", "11.1111.", "1111.11.", ".11.111.", "1.1111.1", "11.1.11."]),
]

# Neutral default palette (the file is viewable; the demo overrides via its own uploaded palettes).
PALETTE = [(0, 0, 0), (235, 235, 245), (170, 170, 185)]

CELL = 8


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _png_indexed8(width: int, height: int, indices: list[list[int]]) -> bytes:
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)  # 8-bit, colortype 3 (palette)
    plte = b"".join(struct.pack(">BBB", r, g, b) for (r, g, b) in PALETTE)
    raw = bytearray()
    for row in indices:
        raw.append(0)  # filter type 0 (none)
        raw.extend(row)
    return (sig + _chunk(b"IHDR", ihdr) + _chunk(b"PLTE", plte)
            + _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _chunk(b"IEND", b""))


def _idx(ch: str) -> int:
    return 0 if ch == "." else int(ch)


def main() -> None:
    n = len(SPRITES)
    width, height = CELL * n, CELL  # one row of n cells
    plane = [[0 for _ in range(width)] for _ in range(height)]
    for k, (_name, grid) in enumerate(SPRITES):
        for gy in range(CELL):
            row = grid[gy] if gy < len(grid) else ""
            for gx in range(CELL):
                ch = row[gx] if gx < len(row) else "."
                plane[gy][k * CELL + gx] = _idx(ch)
    (HERE / "space_invaders.png").write_bytes(_png_indexed8(width, height, plane))
    print(f"space_invaders.png: {width}x{height}, {n} sprite cells "
          f"({', '.join(name for name, _ in SPRITES)})")


if __name__ == "__main__":
    main()
