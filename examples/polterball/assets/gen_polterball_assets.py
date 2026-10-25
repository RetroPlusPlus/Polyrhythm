#!/usr/bin/env python3
"""Author Polterball's indexed-PNG art: a maze-tile sheet, a sprite sheet, and a HUD font sheet.

All three are colortype-3 (indexed) PNGs — the pixels are palette INDICES, the color comes from
palettes the demo uploads and SELECTS per cell / per sprite (so one ghost shape recolors to each
ghost's color AND to the frightened blue, one wall block recolors hard vs soft, and the power
pellet pulses by palette selection alone). Index 0 is transparent on the sprite sheet (the sprite
path discards it) and "the dark background" on the tile/font sheets (the tile path draws entry 0).
The PNGs carry a neutral viewable PLTE; the demo overrides it with its own palettes.

  • polterball_tiles.png — five 32×32 maze-cell blocks in a row (160×32), each block a 4×4 group of
    8px tiles the demo stamps together: hard wall, soft (breakable) wall, floor+pellet, plain floor,
    pen gate. Index 1 = body, 2 = highlight, 3 = shade/crack.
  • polterball_sprites.png — a uniform 80×24 SpriteSeries grid, art top-left-anchored, index-0
    padded. Cells (left→right): paddle (80×16), ball (12×12), ghost frame A (24×24), ghost frame B
    (24×24, skirt wave), eyes (16×16), power pellet (16×16). Each game sprite draws at its OWN
    AssetDimensions from its slot's top-left. Ghost indices: 1 = body, 2 = eye white, 3 = pupil.
  • polterball_font.png — 8×8 Tileset cells: digits 0–9, A–Z, space (37 cells) + a full-width
    horizontal rule cell for the HUD border. White-on-transparent 5×7 glyphs, index 1 = lit.

Engine-original art (no Namco / Atari / arcade-ROM content). One-time authoring tool, kept
committed so the PNGs stay regenerable and their index planes auditable — the same dependency-free
encoder as the other gen_*.py scripts (stdlib zlib/struct only).

Run from the engine repo root:  python3 examples/polterball/assets/gen_polterball_assets.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent  # engine/examples/polterball/assets

# Neutral default palette so the files are viewable; the demo overrides via its own uploaded palettes.
# 0 = transparent/background, 1 = main, 2 = light, 3 = dark.
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


def _blank_plane(width: int, height: int) -> list[list[int]]:
    return [[0 for _ in range(width)] for _ in range(height)]


def _stamp(plane: list[list[int]], ox: int, oy: int, art: list[list[int]]) -> None:
    """Stamp a small index grid into `plane` at (ox, oy) — index 0 cells leave the destination alone."""
    for y, row in enumerate(art):
        for x, idx in enumerate(row):
            if idx:
                plane[oy + y][ox + x] = idx


# ── Maze-tile sheet: five 32×32 blocks (each a 4×4 group of 8px tiles) ────────────────────────────────
TILE_BLOCK = 32
TILE_CELLS = 5  # hard, soft, floor+pellet, floor, gate


def _hard_block() -> list[list[int]]:
    """A bevelled wall slab: light top/left edge, dark bottom/right edge, a mortar seam mid-height so a
    run of blocks reads as coursed stonework rather than a flat fill."""
    art = [[1 for _ in range(TILE_BLOCK)] for _ in range(TILE_BLOCK)]
    for y in range(TILE_BLOCK):
        for x in range(TILE_BLOCK):
            if y < 2 or x < 2:
                art[y][x] = 2
            if y > 29 or x > 29:
                art[y][x] = 3
    for x in range(2, 30):  # the seam
        art[15][x] = 3
        art[16][x] = 2
    return art


def _soft_block() -> list[list[int]]:
    """The breakable wall: the hard slab minus the seam, with a jagged diagonal crack (index 3) and a
    few chipped-out pixels (index 0) so it reads 'weaker' even before its rust palette recolors it."""
    art = [[1 for _ in range(TILE_BLOCK)] for _ in range(TILE_BLOCK)]
    for y in range(TILE_BLOCK):
        for x in range(TILE_BLOCK):
            if y < 2 or x < 2:
                art[y][x] = 2
            if y > 29 or x > 29:
                art[y][x] = 3
    x = 5.0
    for y in range(3, 29):  # the crack wanders down-right, kinking every few rows
        xi = int(x)
        art[y][max(2, min(29, xi))] = 3
        art[y][max(2, min(29, xi + 1))] = 3
        if y % 5 == 0:
            art[y][max(2, min(29, xi + 2))] = 0  # a chip
        x += 0.85 if (y // 4) % 2 == 0 else 0.35
    return art


def _pellet_block() -> list[list[int]]:
    """A corridor cell WITH its pellet: empty background, a small round dot at the cell center (the
    thing the ball eats). Eaten cells restamp as the plain floor block instead."""
    art = _blank_plane(TILE_BLOCK, TILE_BLOCK)
    for y in range(TILE_BLOCK):
        for x in range(TILE_BLOCK):
            if (x - 15.5) ** 2 + (y - 15.5) ** 2 <= 3.4 ** 2:
                art[y][x] = 1
    art[14][14] = 2  # glint
    return art


def _floor_block() -> list[list[int]]:
    return _blank_plane(TILE_BLOCK, TILE_BLOCK)


def _gate_block() -> list[list[int]]:
    """The pen gate: a horizontal bar across the cell middle — the door the ghosts pass through and
    the ball bounces off. Light top edge, dark bottom edge."""
    art = _blank_plane(TILE_BLOCK, TILE_BLOCK)
    for x in range(TILE_BLOCK):
        art[14][x] = 2
        art[15][x] = 1
        art[16][x] = 1
        art[17][x] = 3
    return art


def build_tiles() -> bytes:
    width, height = TILE_BLOCK * TILE_CELLS, TILE_BLOCK
    plane = _blank_plane(width, height)
    for i, block in enumerate([_hard_block(), _soft_block(), _pellet_block(), _floor_block(),
                               _gate_block()]):
        _stamp(plane, i * TILE_BLOCK, 0, block)
    return _png_indexed8(width, height, plane)


# ── Sprite sheet ──────────────────────────────────────────────────────────────────────────────────────
CELL_W, CELL_H = 80, 24           # uniform SpriteSeries cell (fits the largest sprite, the 80×16 paddle)
SPRITE_CELLS = 6                  # paddle, ball, ghost A, ghost B, eyes, power pellet


def _paddle(w: int = 80, h: int = 16) -> list[list[int]]:
    """A rounded paddle bar: light highlight on top, dark shade on the bottom, clipped corners."""
    art = _blank_plane(w, h)
    for y in range(h):
        for x in range(w):
            if (x < 3 or x >= w - 3) and (y < 2 or y >= h - 2):
                continue  # clip the corners for a capsule look
            if y < 2:
                art[y][x] = 2
            elif y >= h - 3:
                art[y][x] = 3
            else:
                art[y][x] = 1
    return art


def _ball(d: int = 12) -> list[list[int]]:
    """A filled disc with an off-center highlight so it reads as a ball, not a square."""
    art = _blank_plane(d, d)
    c = (d - 1) / 2.0
    r = d / 2.0
    for y in range(d):
        for x in range(d):
            if (x - c) ** 2 + (y - c) ** 2 <= r * r:
                art[y][x] = 1
                if (x - c + 1.4) ** 2 + (y - c + 1.4) ** 2 <= (r * 0.45) ** 2:
                    art[y][x] = 2
                elif (x - c - 1.8) ** 2 + (y - c - 1.6) ** 2 <= (r * 0.34) ** 2:
                    art[y][x] = 3
    return art


def _ghost(frame: int, d: int = 24) -> list[list[int]]:
    """The classic dome-and-skirt ghost: a round top half, a solid body, and a scalloped hem whose
    points alternate between the two frames (the walk 'wave'). Indices: 1 body, 2 eye white, 3 pupil —
    so ONE shape serves every ghost color AND the frightened blue by palette selection."""
    art = _blank_plane(d, d)
    for y in range(d):
        for x in range(d):
            if y <= 11:
                if (x - 11.5) ** 2 + (y - 11.5) ** 2 <= 12.0 ** 2:
                    art[y][x] = 1
            elif y <= 19:
                art[y][x] = 1
    profile = [4, 3, 1, 0, 1, 3]          # hem depth per column phase — hanging points every 6px
    shift = 0 if frame == 0 else 3        # frame B shifts the points half a period: the wave
    for x in range(d):
        keep = profile[(x + shift) % 6]
        for k in range(4):
            if k < keep:
                art[20 + k][x] = 1
    for ex in (4, 14):                    # eye whites (5×6) with low-set pupils (2×2)
        for y in range(6, 12):
            for x in range(ex, ex + 5):
                art[y][x] = 2
        for y in range(9, 11):
            for x in range(ex + 1, ex + 3):
                art[y][x] = 3
    return art


def _eyes(d: int = 16) -> list[list[int]]:
    """The downed ghost's disembodied eyes flying home: two white ovals with low pupils."""
    art = _blank_plane(d, d)
    for ex in (2, 9):
        for y in range(3, 13):
            for x in range(ex, ex + 5):
                dx, dy = x - (ex + 2), y - 7.5
                if (dx * dx) / 4.0 + (dy * dy) / 16.0 <= 1.0:
                    art[y][x] = 1
        for y in range(8, 11):
            for x in range(ex + 1, ex + 3):
                art[y][x] = 3
    return art


def _power(d: int = 16) -> list[list[int]]:
    """The power pellet: a fat orb with a glint. Its pulse is palette animation, not art."""
    art = _blank_plane(d, d)
    for y in range(d):
        for x in range(d):
            if (x - 7.5) ** 2 + (y - 7.5) ** 2 <= 6.2 ** 2:
                art[y][x] = 1
    for y in range(4, 7):
        for x in range(4, 7):
            if (x - 5.5) ** 2 + (y - 5.5) ** 2 <= 1.9 ** 2:
                art[y][x] = 2
    return art


def build_sprites() -> bytes:
    width, height = CELL_W * SPRITE_CELLS, CELL_H
    plane = _blank_plane(width, height)
    _stamp(plane, 0 * CELL_W, 0, _paddle())
    _stamp(plane, 1 * CELL_W, 0, _ball())
    _stamp(plane, 2 * CELL_W, 0, _ghost(0))
    _stamp(plane, 3 * CELL_W, 0, _ghost(1))
    _stamp(plane, 4 * CELL_W, 0, _eyes())
    _stamp(plane, 5 * CELL_W, 0, _power())
    return _png_indexed8(width, height, plane)


# ── Font sheet (5×7 glyphs in 8×8 cells: 0–9, A–Z, space, + a border rule) ────────────────────────────
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
    (HERE / "polterball_tiles.png").write_bytes(build_tiles())
    print(f"polterball_tiles.png: {TILE_BLOCK * TILE_CELLS}x{TILE_BLOCK}, {TILE_CELLS} blocks "
          f"(hard, soft, pellet, floor, gate)")
    (HERE / "polterball_sprites.png").write_bytes(build_sprites())
    print(f"polterball_sprites.png: {CELL_W * SPRITE_CELLS}x{CELL_H}, {SPRITE_CELLS} cells "
          f"(paddle, ball, ghost A, ghost B, eyes, power)")
    (HERE / "polterball_font.png").write_bytes(build_font())
    print(f"polterball_font.png: {FONT_CELL * (len(FONT_ORDER) + 1)}x{FONT_CELL}, "
          f"{len(FONT_ORDER)} glyph cells (0-9, A-Z, space) + 1 border rule")


if __name__ == "__main__":
    main()
