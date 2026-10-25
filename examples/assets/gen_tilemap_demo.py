#!/usr/bin/env python3
"""Author the tilemap-import demo assets: a FONT sheet, a MENU-border sheet, and a 16-bit
GRAYSCALE map PNG that mixes BOTH sheets in one map (the multi-atlas headline).

All three are PNG IMAGES, never byte arrays:
  - font.png   — indexed 8-bit: 8 cells (space, H, E, L, O, W, R, D), one 8x8 glyph each.
  - menu.png   — indexed 8-bit: 4 cells (outer corner, horizontal edge, vertical edge, fill). These
                 are the FLIP-IRREDUCIBLE minimum for a rectangular border — flips mirror but do not
                 rotate 90 degrees:  corner → all 4 box corners,  h-edge → top+bottom,  v-edge →
                 left+right,  fill → interior. No tile a flip can produce is stored twice.
  - tilemap.png — 16-bit GRAYSCALE: each pixel's uint16 sample IS a catalog id. The ids are SPARSE,
                 spread evenly across 0..65535 (stride 4369), which does two things at once:
                   * actually TESTS 16-bit — ids like 4369/8738/… exceed 255, so an 8-bit decode would
                     misread them (4369 & 0xFF = 17). Values 0..15 would pass 8-bit unchanged — no test.
                   * stays EYEBALL-VERIFIABLE — 16 evenly-spaced samples render as 16 distinct gray
                     levels (0%, 6.7%, …, 100%), so the border + HELLO/WORLD layout is visible.
                 loadMapPng reads the uint16 sample as the id; buildTilemap looks up the catalog entry
                 whose `id` matches and emits the layer's cells.

Catalog index k → id = STRIDE*k. The id scheme is mirrored verbatim in examples/tilemap_demo.cpp.

Dependency-free PNG encoder (zlib + struct), same approach as tests/fixtures/gen_fixtures.py.
Run from the engine repo root:  python3 examples/assets/gen_tilemap_demo.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent  # engine/examples/assets

STRIDE = 65535 // 15  # = 4369; catalog index k has id = STRIDE*k, spanning the full 16-bit range


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _png(width: int, height: int, bitdepth: int, colortype: int,
         scanlines: bytes, palette: list[tuple[int, int, int]] | None) -> bytes:
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, bitdepth, colortype, 0, 0, 0)
    out = sig + _chunk(b"IHDR", ihdr)
    if palette is not None:
        out += _chunk(b"PLTE", b"".join(struct.pack(">BBB", *c) for c in palette))
    out += _chunk(b"IDAT", zlib.compress(scanlines, 9))
    out += _chunk(b"IEND", b"")
    return out


# Index 0 is the box/background, index 1 the glyph ink. A VISIBLE index 0 (dark blue, not black) makes
# the sheets eyeball-verifiable — the fill tile + each glyph's background read as a tile, not black void.
# The demo recolors via uploadPalette anyway, so this only affects how the file looks.
GLYPH_PLTE = [(24, 30, 60), (255, 255, 255)]


def write_indexed8(path: Path, glyphs: list[list[str]]) -> None:
    """A horizontal strip of 8x8 glyphs, '#' -> index 1, '.' -> index 0. 8-bit palette PNG."""
    h, w = 8, 8 * len(glyphs)
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter 0
        for g in glyphs:
            for x in range(8):
                raw.append(1 if g[y][x] == "#" else 0)
    path.write_bytes(_png(w, h, 8, 3, bytes(raw), GLYPH_PLTE))


def write_map16(path: Path, grid: list[list[int]]) -> None:
    """16-bit grayscale PNG: each sample is one big-endian uint16 — the catalog id at that cell."""
    h, w = len(grid), len(grid[0])
    raw = bytearray()
    for row in grid:
        raw.append(0)  # filter 0
        for v in row:
            raw.extend(struct.pack(">H", v & 0xFFFF))
    path.write_bytes(_png(w, h, 16, 0, bytes(raw), None))


# ── Font glyphs (8x8) — slot 0 space, then H E L O W R D (slots 1..7) ─────────────────────────────
SP = ["........"] * 8
H = ["........", ".#....#.", ".#....#.", ".######.", ".#....#.", ".#....#.", ".#....#.", "........"]
E = ["........", ".#####..", ".#......", ".####...", ".#......", ".#......", ".#####..", "........"]
L = ["........", ".#......", ".#......", ".#......", ".#......", ".#......", ".#####..", "........"]
O = ["........", "..###...", ".#...#..", ".#...#..", ".#...#..", ".#...#..", "..###...", "........"]
W = ["........", ".#...#..", ".#...#..", ".#.#.#..", ".#.#.#..", ".##.##..", ".#...#..", "........"]
R = ["........", ".####...", ".#...#..", ".####...", ".#.#....", ".#..#...", ".#...#..", "........"]
D = ["........", ".###....", ".#..#...", ".#...#..", ".#...#..", ".#..#...", ".###....", "........"]
FONT = [SP, H, E, L, O, W, R, D]

# ── Menu glyphs (8x8) — '#' = border line (index 1), '.' = box interior (index 0) ─────────────────
CORNER = ["########", "#.......", "#.......", "#.......", "#.......", "#.......", "#.......", "#......."]  # top + left
HEDGE  = ["########", "........", "........", "........", "........", "........", "........", "........"]  # top
VEDGE  = ["#.......", "#.......", "#.......", "#.......", "#.......", "#.......", "#.......", "#......."]  # left
FILL   = ["........"] * 8                                                                                  # interior
MENU = [CORNER, HEDGE, VEDGE, FILL]

# ── Catalog: index k → tile; the map stores id = STRIDE*k. (Mirrored in tilemap_demo.cpp.) ─────────
#  0 fill (menu 3) · 1 TL (menu 0) · 2 TR (menu 0 ^X) · 3 BL (menu 0 ^Y) · 4 BR (menu 0 ^XY)
#  5 top (menu 1) · 6 bottom (menu 1 ^Y) · 7 left (menu 2) · 8 right (menu 2 ^X)
#  9..15 = H E L O W R D (font 1..7)
FILL_, TL, TR, BL, BR, TOP, BOT, LFT, RGT = range(9)
GLYPH = {"H": 9, "E": 10, "L": 11, "O": 12, "W": 13, "R": 14, "D": 15}

MAP_W, MAP_H = 20, 18  # a Game Boy 160x144 viewport in 8px tiles


def build_index_grid() -> list[list[int]]:
    """The map as catalog INDICES (0..15) — readable; converted to ids (STRIDE*index) at write time."""
    g = [[FILL_ for _ in range(MAP_W)] for _ in range(MAP_H)]
    g[0][0], g[0][MAP_W - 1] = TL, TR
    g[MAP_H - 1][0], g[MAP_H - 1][MAP_W - 1] = BL, BR
    for x in range(1, MAP_W - 1):
        g[0][x], g[MAP_H - 1][x] = TOP, BOT
    for y in range(1, MAP_H - 1):
        g[y][0], g[y][MAP_W - 1] = LFT, RGT
    for i, ch in enumerate("HELLO"):
        g[3][3 + i] = GLYPH[ch]
    for i, ch in enumerate("WORLD"):
        g[5][3 + i] = GLYPH[ch]
    return g


def main() -> None:
    write_indexed8(HERE / "tilemap_demo_font.png", FONT)
    write_indexed8(HERE / "tilemap_demo_menu.png", MENU)
    grid = build_index_grid()
    id_grid = [[STRIDE * v for v in row] for row in grid]  # catalog index → sparse 16-bit id
    write_map16(HERE / "tilemap_demo_map.png", id_grid)
    print(f"wrote tilemap_demo_font.png (8 cells), tilemap_demo_menu.png (4 cells), "
          f"tilemap_demo_map.png ({MAP_W}x{MAP_H} 16-bit grayscale, ids = index*{STRIDE})")
    print("catalog index -> id:  " + ", ".join(f"{k}:{STRIDE*k}" for k in range(16)))
    print("map (catalog indices):")
    for row in grid:
        print("  " + " ".join(f"{v:2d}" for v in row))


if __name__ == "__main__":
    main()
