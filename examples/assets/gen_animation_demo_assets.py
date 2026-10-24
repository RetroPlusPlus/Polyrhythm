#!/usr/bin/env python3
"""Author the animation demo's numbered, single-color indexed tileset.

The animation demo (examples/animation_demo.cpp) must let you SEE the difference between frame
animation and palette-cycling animation. The atlas-load demo's sheets bake a DIFFERENT hue into
each cell (cell 0 red, cell 1 orange, …), so an animation that merely advances the frame ALSO
appears to change color — which makes frame animation indistinguishable from palette cycling.

This sheet fixes that: every cell uses the SAME two palette indices — index 0 background, index 1
the digit — and differs ONLY in the digit's SHAPE. So under one fixed palette the six numbered
cells are the same color and differ only by their number (frame animation = the number changes,
color constant), while holding one cell and swapping the palette recolors that single number
(palette cycling = color changes, number constant). The numbers stay so the frame change is
visible either way.

These are engine-original images (no third-party art). One-time authoring tool, kept
committed so the asset stays regenerable and its index plane is auditable — the same posture as
gen_atlas_demo_assets.py (whose minimal, Pillow-free PNG encoder this mirrors). It writes only its
own new asset; the atlas-load demo's assets and generator are left untouched (examples accumulate).

Dependency-free: a minimal 8-bit-palette PNG encoder over the standard library (zlib + struct).

Run from the engine repo root:  python3 examples/assets/gen_animation_demo_assets.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent  # engine/examples/assets

CELL = 8  # the engine's 8px atlas cell (kAtlasCellPx)

# 3×5 digit glyphs (rows top→bottom, '1' = the digit/foreground). Cells 0..5 — the demo's
# animations index into the first six numbered frames.
DIGITS: dict[int, list[str]] = {
    0: ["111", "101", "101", "101", "111"],
    1: ["010", "110", "010", "010", "111"],
    2: ["111", "001", "111", "100", "111"],
    3: ["111", "001", "111", "001", "111"],
    4: ["101", "101", "111", "001", "001"],
    5: ["111", "100", "111", "001", "111"],
}

# Exactly TWO indices, IDENTICAL in every cell: 0 = background, 1 = digit. Because the index plane
# of every cell uses the same two indices, a single fixed palette renders all six numbers the same
# color (they differ only by digit shape) — and swapping the palette recolors the digit. The PLTE
# below is only for viewing the raw PNG; the demo uploads its own palettes. Kept legible/grayscale.
PALETTE: list[tuple[int, int, int, int]] = [
    (28, 28, 34, 255),     # 0 background — dark
    (238, 238, 242, 255),  # 1 digit — near-white
]


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _png_indexed8(width: int, height: int, indices: list[list[int]]) -> bytes:
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)  # 8-bit, colortype 3 (palette)
    plte = b"".join(struct.pack(">BBB", r, g, b) for (r, g, b, _a) in PALETTE)
    raw = bytearray()
    for row in indices:
        raw.append(0)  # filter type 0 (none)
        raw.extend(row)
    return (sig + _chunk(b"IHDR", ihdr) + _chunk(b"PLTE", plte)
            + _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _chunk(b"IEND", b""))


def cell_plane(ordinal: int) -> list[list[int]]:
    """One 8×8 cell: background index 0 everywhere, the ordinal's digit in index 1 centered. Every
    cell uses the SAME two indices — only the digit's shape changes between cells."""
    cell = [[0 for _ in range(CELL)] for _ in range(CELL)]  # all background (index 0)
    glyph = DIGITS[ordinal]
    xoff, yoff = 2, 1  # center the 3×5 glyph in the 8×8 cell
    for gy, grow in enumerate(glyph):
        for gx, on in enumerate(grow):
            if on == "1":
                cell[yoff + gy][xoff + gx] = 1  # digit (index 1)
    return cell


def strip(count: int) -> list[list[int]]:
    """A horizontal strip of `count` numbered 8×8 cells in reading order (ordinal 0..count-1).
    cellsAcross = count, so each cell's natural tile index == its digit under LeftRightThenDown."""
    width, height = count * CELL, CELL
    plane = [[0 for _ in range(width)] for _ in range(height)]
    for c in range(count):
        cell = cell_plane(c)
        for y in range(CELL):
            for x in range(CELL):
                plane[y][c * CELL + x] = cell[y][x]
    return plane


def main() -> None:
    count = 6  # digits 0..5
    name = "anim_numbers.png"
    plane = strip(count)
    (HERE / name).write_bytes(_png_indexed8(count * CELL, CELL, plane))
    print(f"{name}: {count * CELL}x{CELL}, {count} cells (digits 0..{count - 1}), "
          f"two indices (0 bg, 1 digit) — same colour per fixed palette, shape-only differences")


if __name__ == "__main__":
    main()
