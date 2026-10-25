#!/usr/bin/env python3
"""Author Bongusoid's indexed-PNG art (BONGUSOID-S1): a sprite sheet + a HUD font sheet.

Both are colortype-3 (indexed) PNGs — the pixels are palette INDICES, the color comes from palettes the
demo uploads and SELECTS per sprite (so one brick shape recolors to each row's color, and one silver /
gold shape recolors to metallic palettes). Index 0 is transparent: the sprite path discards color-index
0, and the tile path leaves the font's index-0 pixels as the layer background. The PNGs carry a neutral
viewable PLTE; the demo overrides it with its own palettes.

  • bongusoid_sprites.png — a uniform 80×24 SpriteSeries grid, one logical sprite per cell, art top-left-
    anchored, index-0 padded. Cells (read order = left→right): Vaus (80×16), ball (12×12), brick (40×18),
    silver brick (40×18), gold brick (40×18). Each game sprite draws at its OWN AssetDimensions from its
    slot's top-left, so a 12×12 ball reads only the ball cell's top-left 12×12 (the breakout solid-atlas
    pattern, but over real art). Index 1 = body, 2 = light bevel/highlight, 3 = dark bevel/shade.
  • bongusoid_font.png — 8×8 Tileset cells: digits 0–9, then A–Z, then space (37 cells). White-on-
    transparent 5×7 glyphs at a 1px margin; index 1 = lit. Drives the HUD + title text on a tile layer.

Engine-original art (no Taito / arcade-ROM content). One-time authoring tool, kept committed so the PNGs
stay regenerable and their index planes auditable — same dependency-free encoder as the other gen_*.py.

Run from the engine repo root:  python3 examples/bongusoid/assets/gen_bongusoid_assets.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent  # engine/examples/bongusoid/assets

# Neutral default palette so the files are viewable; the demo overrides via its own uploaded palettes.
# 0 = transparent, 1 = main, 2 = light, 3 = dark.
PALETTE = [(0, 0, 0), (224, 224, 232), (255, 255, 255), (120, 120, 132)]


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _png_indexed8(width: int, height: int, plane: list[list[int]]) -> bytes:
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)  # 8-bit, colortype 3 (palette)
    plte = b"".join(struct.pack(">BBB", r, g, b) for (r, g, b) in PALETTE)
    raw = bytearray()
    for row in plane:
        raw.append(0)  # filter type 0 (none)
        raw.extend(row)
    return (sig + _chunk(b"IHDR", ihdr) + _chunk(b"PLTE", plte)
            + _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _chunk(b"IEND", b""))


# ── Sprite sheet ──────────────────────────────────────────────────────────────────────────────────────
CELL_W, CELL_H = 80, 24           # uniform SpriteSeries cell (fits the largest sprite, the 80×16 Vaus)
SPRITE_CELLS = 5                  # Vaus, ball, brick, silver, gold


def _blank_plane(width: int, height: int) -> list[list[int]]:
    return [[0 for _ in range(width)] for _ in range(height)]


def _stamp(plane: list[list[int]], ox: int, oy: int, art: list[list[int]]) -> None:
    """Stamp a small index grid into `plane` at (ox, oy) — index 0 cells leave the destination alone."""
    for y, row in enumerate(art):
        for x, idx in enumerate(row):
            if idx:
                plane[oy + y][ox + x] = idx


def _vaus(w: int = 80, h: int = 16) -> list[list[int]]:
    """A rounded paddle bar: light highlight on top, dark shade on the bottom, clipped corners."""
    art = _blank_plane(w, h)
    for y in range(h):
        for x in range(w):
            # Clip the four corners for a rounded capsule look.
            if (x < 3 or x >= w - 3) and (y < 2 or y >= h - 2):
                continue
            if y < 2:
                art[y][x] = 2          # top highlight
            elif y >= h - 3:
                art[y][x] = 3          # bottom shade
            else:
                art[y][x] = 1          # body
    return art


def _ball(d: int = 12) -> list[list[int]]:
    """A filled disc with an off-center light highlight (upper-left) AND a dark pip (lower-right), so the
    ball has a clear orientation and its rotation (the S2 spin) reads instead of looking static."""
    art = _blank_plane(d, d)
    c = (d - 1) / 2.0
    r = d / 2.0
    for y in range(d):
        for x in range(d):
            if (x - c) ** 2 + (y - c) ** 2 <= r * r:
                art[y][x] = 1
                if (x - c + 1.4) ** 2 + (y - c + 1.4) ** 2 <= (r * 0.45) ** 2:
                    art[y][x] = 2      # light highlight, upper-left
                elif (x - c - 1.8) ** 2 + (y - c - 1.6) ** 2 <= (r * 0.34) ** 2:
                    art[y][x] = 3      # dark pip, lower-right — makes the spin legible
    return art


def _brick(w: int = 40, h: int = 18, style: str = "plain") -> list[list[int]]:
    """A beveled brick: light top/left edge, dark bottom/right edge, with clipped corners. `style` adds a
    metallic sheen (silver) or a sparkle pair (gold) in index 2 so the three brick cells read distinct
    even before the demo recolors them by palette selection."""
    art = _blank_plane(w, h)
    for y in range(h):
        for x in range(w):
            if (x == 0 or y == 0) and (x + y) == 0:   # only the single TL corner pixel
                pass
            if (x == 0 and y == 0) or (x == w - 1 and y == 0) \
               or (x == 0 and y == h - 1) or (x == w - 1 and y == h - 1):
                continue                               # clip corners (rounded)
            if y == 0 or x == 0:
                art[y][x] = 2                          # light bevel (top / left)
            elif y == h - 1 or x == w - 1:
                art[y][x] = 3                          # dark bevel (bottom / right)
            else:
                art[y][x] = 1                          # fill
    if style == "silver":
        for x in range(4, w - 4):                      # a horizontal sheen line
            art[4][x] = 2
    elif style == "gold":
        for (sx, sy) in [(8, 5), (9, 5), (8, 6), (24, 9), (25, 9), (24, 10)]:  # two sparkles
            art[sy][sx] = 2
    return art


def build_sprites() -> bytes:
    width, height = CELL_W * SPRITE_CELLS, CELL_H
    plane = _blank_plane(width, height)
    _stamp(plane, 0 * CELL_W, 0, _vaus())
    _stamp(plane, 1 * CELL_W, 0, _ball())
    _stamp(plane, 2 * CELL_W, 0, _brick(style="plain"))
    _stamp(plane, 3 * CELL_W, 0, _brick(style="silver"))
    _stamp(plane, 4 * CELL_W, 0, _brick(style="gold"))
    return _png_indexed8(width, height, plane)


# ── Font sheet (5×7 glyphs in 8×8 cells: 0–9, A–Z, space) ───────────────────────────────────────────────
GLYPH_W, GLYPH_H = 5, 7
FONT_CELL = 8

_GLYPHS: dict[str, list[str]] = {
    "0": [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    "1": ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "2": [".###.", "#...#", "....#", "..##.", ".#...", "#....", "#####"],
    "3": ["#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."],
    "4": ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    "5": ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
    "6": ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."],
    "7": ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
    "8": [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    "9": [".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "D": ["###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."],
    "E": ["#####", "#....", "#....", "###..", "#....", "#....", "#####"],
    "F": ["#####", "#....", "#....", "###..", "#....", "#....", "#...."],
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "I": [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "J": ["..###", "...#.", "...#.", "...#.", "#..#.", "#..#.", ".##.."],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "M": ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    "N": ["#...#", "##..#", "#.#.#", "#.#.#", "#..##", "#...#", "#...#"],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "Q": [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "S": [".###.", "#...#", "#....", ".###.", "....#", "#...#", ".###."],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "V": ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "W": ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"],
    "X": ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
    " ": [".....", ".....", ".....", ".....", ".....", ".....", "....."],
}

# Cell order: digits 0–9 (slots 0–9), letters A–Z (slots 10–35), space (slot 36). The demo maps a
# character to its slot the same way: '0'..'9' → 0..9, 'A'..'Z' → 10..35, ' ' → 36.
FONT_ORDER = [str(d) for d in range(10)] + [chr(ord("A") + i) for i in range(26)] + [" "]

# One extra cell after the glyphs (slot 37): a full-width horizontal RULE for the HUD's bottom border.
# Unlike the 5×7 glyphs (which carry a 1px margin), the rule spans all 8 columns so a row of these cells
# joins into one continuous line. The demo reads it as the LAST font cell (BongAssets::borderTile).
BORDER_SLOT = len(FONT_ORDER)  # 37


def build_font() -> bytes:
    n = len(FONT_ORDER) + 1  # glyph cells + the border-rule cell
    width, height = FONT_CELL * n, FONT_CELL
    plane = _blank_plane(width, height)
    for k, ch in enumerate(FONT_ORDER):
        grid = _GLYPHS[ch]
        for gy in range(GLYPH_H):
            for gx in range(GLYPH_W):
                if grid[gy][gx] == "#":
                    plane[1 + gy][k * FONT_CELL + 1 + gx] = 1   # 1px top/left margin in the 8×8 cell
    for x in range(FONT_CELL):                                  # the rule: full-width 2px line, cell bottom
        plane[6][BORDER_SLOT * FONT_CELL + x] = 1
        plane[7][BORDER_SLOT * FONT_CELL + x] = 1
    return _png_indexed8(width, height, plane)


def main() -> None:
    (HERE / "bongusoid_sprites.png").write_bytes(build_sprites())
    print(f"bongusoid_sprites.png: {CELL_W * SPRITE_CELLS}x{CELL_H}, {SPRITE_CELLS} cells "
          f"(vaus, ball, brick, silver, gold)")
    (HERE / "bongusoid_font.png").write_bytes(build_font())
    print(f"bongusoid_font.png: {FONT_CELL * (len(FONT_ORDER) + 1)}x{FONT_CELL}, "
          f"{len(FONT_ORDER)} glyph cells (0-9, A-Z, space) + 1 border rule")


if __name__ == "__main__":
    main()
