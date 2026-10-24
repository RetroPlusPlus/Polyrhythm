#!/usr/bin/env python3
"""Author the atlas-load demo's numbered, license-clean indexed PNGs.

The atlas-load demo (examples/atlas_load_demo.cpp) needs source images whose cells are
visually distinct and ORDINAL — each 8×8 cell shows its own natural-reading-order index as a
tiny digit — so that when the slicer carves a grid in any of the 8 read orders and the demo
lays the slots in slot order, the on-screen digit sequence reads the carve order directly.

These are engine-original images (no third-party art). One-time authoring tool, kept
committed so the binary assets stay regenerable and their exact index planes are auditable —
the same posture as tests/fixtures/gen_fixtures.py (whose minimal PNG encoder this mirrors so
that frozen fixtures tool stays untouched).

Dependency-free: a minimal 8-bit-palette PNG encoder over the standard library (zlib + struct),
so it runs anywhere Python does — no Pillow. Emits non-interlaced, filter-0, 8-bit PLTE PNGs.

Run from the engine repo root:  python3 examples/assets/gen_atlas_demo_assets.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent  # engine/examples/assets

CELL = 8  # the engine's 8px atlas cell (kAtlasCellPx)

# 3×5 digit glyphs (rows top→bottom, '1' = foreground). Only 0..5 are needed — the demo's
# largest grid is 6 cells (3×2 / 2×3), ordinals 0..5.
DIGITS: dict[int, list[str]] = {
    0: ["111", "101", "101", "101", "111"],
    1: ["010", "110", "010", "010", "111"],
    2: ["111", "001", "111", "100", "111"],
    3: ["111", "001", "111", "001", "111"],
    4: ["101", "101", "111", "001", "001"],
    5: ["111", "100", "111", "001", "111"],
}

# Palette indices: 0 = dark corner-notch marker, 1 = white digit, 2..7 = one distinct hue per
# ordinal 0..5 (so cells differ in color as well as digit). The demo hand-builds its own color
# for these indices; this PLTE only matters if the raw PNG is viewed directly.
PALETTE: list[tuple[int, int, int, int]] = [
    (30, 30, 36, 255),     # 0 marker
    (240, 240, 245, 255),  # 1 white digit
    (200, 70, 60, 255),    # 2 ordinal 0 — red
    (220, 140, 60, 255),   # 3 ordinal 1 — orange
    (210, 200, 70, 255),   # 4 ordinal 2 — yellow
    (90, 180, 90, 255),    # 5 ordinal 3 — green
    (70, 130, 210, 255),   # 6 ordinal 4 — blue
    (160, 100, 200, 255),  # 7 ordinal 5 — violet
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
    """One 8×8 cell: a flat background hue (index 2+ordinal), the ordinal's digit in white
    (index 1) at offset (3,2), and a dark top-left corner notch (index 0) so each cell's
    boundary + orientation is unmistakable."""
    bg = 2 + ordinal
    cell = [[bg for _ in range(CELL)] for _ in range(CELL)]
    glyph = DIGITS[ordinal]
    for gy, grow in enumerate(glyph):
        for gx, on in enumerate(grow):
            if on == "1":
                cell[2 + gy][3 + gx] = 1
    cell[0][0] = 0  # corner notch (a small top-left triangle)
    cell[0][1] = 0
    cell[1][0] = 0
    return cell


def grid(cols: int, rows: int) -> list[list[int]]:
    """A cols×rows grid of numbered 8×8 cells in natural reading order (ordinal = row*cols+col).
    The natural ordinal equals each cell's atlas tile index under LeftRightThenDown, so the
    baked digit == the tile index the slicer emits."""
    width, height = cols * CELL, rows * CELL
    plane = [[0 for _ in range(width)] for _ in range(height)]
    for r in range(rows):
        for c in range(cols):
            cell = cell_plane(r * cols + c)
            for y in range(CELL):
                for x in range(CELL):
                    plane[r * CELL + y][c * CELL + x] = cell[y][x]
    return plane


# (filename, cols, rows) — the demo's arrangements. The Single arrangement reuses grid_3x2.
ASSETS = [
    ("atlas_strip_h.png", 2, 1),  # 16×8  — 1-D horizontal strip + the 16×8→2 user example
    ("atlas_strip_v.png", 1, 2),  # 8×16  — 1-D vertical strip
    ("atlas_grid_2x2.png", 2, 2),  # 16×16 — square grid + the 16×16→4 user example
    ("atlas_grid_3x2.png", 3, 2),  # 24×16 — non-square + the all-8-orders comparison + Single
    ("atlas_grid_2x3.png", 2, 3),  # 16×24 — non-square (transposed)
]


def main() -> None:
    for name, cols, rows in ASSETS:
        plane = grid(cols, rows)
        (HERE / name).write_bytes(_png_indexed8(cols * CELL, rows * CELL, plane))
        print(f"{name}: {cols * CELL}x{rows * CELL}, {cols}x{rows} cells, "
              f"natural tile order 0..{cols * rows - 1}")


if __name__ == "__main__":
    main()
