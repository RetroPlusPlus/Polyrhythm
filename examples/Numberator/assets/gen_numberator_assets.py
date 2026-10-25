#!/usr/bin/env python3
"""Author the Numberator calculator's assets — all as PNG files (the demo loads them, embeds them, and
never declares pixel bytes inline).

Numberator is a working calculator styled after the classic Mac OS "Platinum" look, built to show the
engine rendering a non-game UI from the same primitives a game uses:

  numberator_palette.png — 7x1, 16-bit RGBA. One shared palette: entry 0 is alpha 0 (transparent, so a
                           glyph's background drops out — MATERIAL transparency), 1 black, 2 shadow,
                           3 body, 4 face, 5 highlight, 6 white. Loaded with loadPaletteImage.
  numberator_chrome.png  — indexed 8-bit, 8 chrome TILES (8x8): body, the title bar (pinstripe + a
                           separator + a close box), and the sunken display well's 9-slice (face + a
                           corner / a horizontal edge / a vertical edge, the rest produced by FLIPS).
  numberator_map.png     — 16-bit grayscale MAP: one id per 8x8 cell of the 248x344 window. loadMapPng
                           reads it, a TileCatalog maps each id to a chrome tile + flip, and
                           assembleTilemap builds the chrome layer — the engine's image-driven tilemap path.
  numberator_buttons.png — indexed 8-bit, 2 button SPRITES (48x40): a raised beveled key. The demo flips
                           a pressed key X+Y so the bevel inverts to a sunken look (the 32-bit press).
                           Slot 0 = number key (light face), slot 1 = function key (darker face).
  numberator_font.png    — indexed 8-bit, 20 glyph SPRITES (24x32): the digits and operators, ink in
                           index 1 over transparent index 0. Authored at 12x16 and scaled x2.

Index legend (the shared palette's entries): 0 transparent · 1 black · 2 shadow · 3 body · 4 face ·
5 highlight · 6 white. The embedded PLTE on the indexed files is for eyeballing them; the engine
recolors through the palette image.

Engine-original art. One-time authoring tool, kept committed so the binary assets stay regenerable and
auditable — the posture of examples/assets/gen_tilemap_demo.py (the indexed + map encoders this mirrors)
and examples/palette_image_demo/assets/gen_palette_demo_assets.py (the 16-bit RGBA encoder).
Dependency-free (zlib + struct), no Pillow.

Run from the engine repo root:  python3 examples/Numberator/assets/gen_numberator_assets.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent  # engine/examples/Numberator/assets

# Shared palette indices.
TRANSP, BLACK, SHADOW, BODY, FACE, LITE, WHITE = range(7)

# Viewer-only PLTE for the indexed files (engine recolors through numberator_palette.png).
PLTE = [(187, 187, 187), (0, 0, 0), (128, 128, 128), (187, 187, 187),
        (214, 214, 214), (236, 236, 236), (255, 255, 255)]

# The real palette emitted as a 16-bit-RGBA palette image: entry 0 is alpha 0 (transparent).
PALETTE_RGBA = [
    (0, 0, 0, 0),          # 0 transparent
    (0, 0, 0, 255),        # 1 black
    (128, 128, 128, 255),  # 2 shadow
    (187, 187, 187, 255),  # 3 body
    (214, 214, 214, 255),  # 4 face
    (236, 236, 236, 255),  # 5 highlight
    (255, 255, 255, 255),  # 6 white
]


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _png(width, height, bitdepth, colortype, scanlines, palette=None):
    sig = b"\x89PNG\r\n\x1a\n"
    out = sig + _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, bitdepth, colortype, 0, 0, 0))
    if palette is not None:
        out += _chunk(b"PLTE", b"".join(struct.pack(">BBB", *c) for c in palette))
    out += _chunk(b"IDAT", zlib.compress(scanlines, 9)) + _chunk(b"IEND", b"")
    return out


def write_indexed8(path, rows):
    h, w = len(rows), len(rows[0])
    raw = bytearray()
    for row in rows:
        raw.append(0)
        raw.extend(row)
    path.write_bytes(_png(w, h, 8, 3, bytes(raw), PLTE))


def write_palette_rgba16(path, entries):
    raw = bytearray([0])
    for (r, g, b, a) in entries:
        raw.extend(struct.pack(">HHHH", r * 257, g * 257, b * 257, a * 257))
    path.write_bytes(_png(len(entries), 1, 16, 6, bytes(raw)))


def write_map16(path, grid):
    h, w = len(grid), len(grid[0])
    raw = bytearray()
    for row in grid:
        raw.append(0)
        for v in row:
            raw.extend(struct.pack(">H", v & 0xFFFF))
    path.write_bytes(_png(w, h, 16, 0, bytes(raw)))


# ── Drawing helpers on a 2-D index grid ────────────────────────────────────────────────────────────
def fill(g, x, y, w, h, idx):
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            if 0 <= yy < len(g) and 0 <= xx < len(g[0]):
                g[yy][xx] = idx


def hline(g, x, y, w, idx): fill(g, x, y, w, 1, idx)
def vline(g, x, y, h, idx): fill(g, x, y, 1, h, idx)


def outline(g, x, y, w, h, idx):
    hline(g, x, y, w, idx); hline(g, x, y + h - 1, w, idx)
    vline(g, x, y, h, idx); vline(g, x + w - 1, y, h, idx)


def raised(g, x, y, w, h, face):
    """Raised bevel: black outline, highlight top/left, shadow bottom/right."""
    fill(g, x, y, w, h, face)
    outline(g, x, y, w, h, BLACK)
    hline(g, x + 1, y + 1, w - 2, WHITE); vline(g, x + 1, y + 1, h - 2, WHITE)
    hline(g, x + 1, y + h - 2, w - 2, SHADOW); vline(g, x + w - 2, y + 1, h - 2, SHADOW)


# ── Close-box sprite — a Platinum-proportioned 13x13 raised box (~55% of the title bar's height),
# centered in a 16x16 cell (atlas cells are 8-aligned) on a transparent surround. The sheet loads with
# index-0 transparency, so the sprite's silhouette — the click target — is the box, not the cell.
CLOSE_CELL = 16
CLOSE_BOX  = 13

def build_closebox():
    g = [[TRANSP for _ in range(CLOSE_CELL)] for _ in range(CLOSE_CELL)]
    o = (CLOSE_CELL - CLOSE_BOX) // 2
    raised(g, o, o, CLOSE_BOX, CLOSE_BOX, BODY)
    return g


# ── Chrome tile sheet (8 tiles, 8x8) — slot index = position in the strip ──────────────────────────
TILE = 8
BODY_T, TITLE_FILL_T, TITLE_SEP_T, CLOSEBOX_T, DISP_FACE_T, DISP_CORNER_T, DISP_HEDGE_T, DISP_VEDGE_T = range(8)


def build_chrome_sheet():
    g = [[BODY for _ in range(TILE * 8)] for _ in range(TILE)]

    def tile(i): return (i * TILE, 0)

    # BODY — flat body fill.
    fill(g, *tile(BODY_T), TILE, TILE, BODY)
    # TITLE_FILL — body with two shadow pinstripes.
    x0, _ = tile(TITLE_FILL_T); fill(g, x0, 0, TILE, TILE, FACE)
    hline(g, x0, 2, TILE, SHADOW); hline(g, x0, 5, TILE, SHADOW)
    # TITLE_SEP — pinstripe with a black bottom separator.
    x0, _ = tile(TITLE_SEP_T); fill(g, x0, 0, TILE, TILE, FACE)
    hline(g, x0, 2, TILE, SHADOW); hline(g, x0, 5, TILE, SHADOW); hline(g, x0, 7, TILE, BLACK)
    # CLOSEBOX — a small raised square centered in a face tile.
    x0, _ = tile(CLOSEBOX_T); fill(g, x0, 0, TILE, TILE, FACE)
    raised(g, x0 + 1, 1, 6, 6, BODY)
    # DISP_FACE — white well interior.
    fill(g, *tile(DISP_FACE_T), TILE, TILE, WHITE)
    # DISP_CORNER — sunken top-left corner (black outline, shadow inner, white).
    x0, _ = tile(DISP_CORNER_T); fill(g, x0, 0, TILE, TILE, WHITE)
    hline(g, x0, 0, TILE, BLACK); vline(g, x0, 0, TILE, BLACK)
    hline(g, x0 + 1, 1, TILE - 1, SHADOW); vline(g, x0 + 1, 1, TILE - 1, SHADOW)
    # DISP_HEDGE — sunken top edge.
    x0, _ = tile(DISP_HEDGE_T); fill(g, x0, 0, TILE, TILE, WHITE)
    hline(g, x0, 0, TILE, BLACK); hline(g, x0, 1, TILE, SHADOW)
    # DISP_VEDGE — sunken left edge.
    x0, _ = tile(DISP_VEDGE_T); fill(g, x0, 0, TILE, TILE, WHITE)
    vline(g, x0, 0, TILE, BLACK); vline(g, x0 + 1, 0, TILE, SHADOW)
    return g


# ── Chrome MAP — chrome_id returns a ROLE index 0..12; the map stores map_id(role) = role*65535/12, so
# the 13 ids spread evenly across the full 16-bit range and the map reads as distinct gray levels to the
# eye (the tilemap_demo id-spreading trick — small ids would all be near-black). assets.cpp's catalog
# mirrors ROLES exactly: each role's 16-bit id maps to a chrome tile slot + flip.
#   ROLES (role index -> chrome tile, flip):
#     0 body          1 title fill     2 title separator  3 close box      4 well face
#     5 well corner   6 corner flipX   7 corner flipY     8 corner flipXY
#     9 well h-edge  10 h-edge flipY  11 well v-edge     12 v-edge flipX
ROLE_COUNT = 13
VIEW_W, VIEW_H = 248, 344
COLS_T, ROWS_T = VIEW_W // TILE, VIEW_H // TILE          # 31 x 43
TITLE_ROWS = 3                                            # rows 0..2 are the title bar
WELL_C0, WELL_C1, WELL_R0, WELL_R1 = 2, 28, 5, 10        # display-well cell rectangle (inclusive)


def chrome_id(cx, cy):
    if WELL_C0 <= cx <= WELL_C1 and WELL_R0 <= cy <= WELL_R1:
        left, right = cx == WELL_C0, cx == WELL_C1
        top, bottom = cy == WELL_R0, cy == WELL_R1
        if (left or right) and (top or bottom):
            return {(0, 0): 5, (1, 0): 6, (0, 1): 7, (1, 1): 8}[(int(right), int(bottom))]
        if top or bottom:
            return 10 if bottom else 9
        if left or right:
            return 12 if right else 11
        return 4
    if cy < TITLE_ROWS:
        if cy == TITLE_ROWS - 1:
            return 2                                      # separator row
        return 1                                          # title fill (the close box is a SPRITE, not a map cell)
    return 0                                              # body


def map_id(role):
    return role * 65535 // (ROLE_COUNT - 1)               # spread 13 roles across the full 16-bit range


def build_map():
    return [[map_id(chrome_id(cx, cy)) for cx in range(COLS_T)] for cy in range(ROWS_T)]


# ── Button sprites (2 keys, 48x40) ─────────────────────────────────────────────────────────────────
BTN_W, BTN_H = 48, 40


def build_buttons():
    g = [[BODY for _ in range(BTN_W * 2)] for _ in range(BTN_H)]
    raised(g, 0, 0, BTN_W, BTN_H, FACE)          # slot 0 — number key (light)
    raised(g, BTN_W, 0, BTN_W, BTN_H, BODY)      # slot 1 — function key (darker)
    return g


# ── Glyph font: authored 12x16, scaled x2 into 24x32 cells ('#' ink -> index 1) ───────────────────
GLYPH_W, GLYPH_H = 12, 16
CELL_W, CELL_H = 24, 32
GLYPHS = {
    "0": ["...####...", "..#....#..", ".#......#.", ".#.....##.", ".#....#.#.", ".#...#..#.", ".#..#...#.", ".#.#....#.", ".##.....#.", ".#......#.", "..#....#..", "...####..."],
    "1": ["....##....", "...###....", "..#.##....", ".....##...", ".....##...", ".....##...", ".....##...", ".....##...", ".....##...", ".....##...", "...######.", "...######."],
    "2": ["..#####...", ".#.....#..", "#.......#.", "........#.", ".......#..", "......#...", ".....#....", "....#.....", "...#......", "..#.......", ".#########", ".#########"],
    "3": [".######...", "......##..", ".......#..", "......##..", "...###....", "......##..", ".......##.", "........#.", "#.......#.", ".#.....#..", "..#####...", ".........."],
    "4": ["......##..", ".....###..", "....#.##..", "...#..##..", "..#...##..", ".#....##..", "#.....##..", "#########.", "#########.", "......##..", "......##..", "......##.."],
    "5": [".########.", ".#........", ".#........", ".#####....", ".#....##..", ".......#..", "........#.", "#.......#.", "#.......#.", ".#.....#..", "..#####...", ".........."],
    "6": ["...####...", "..#....#..", ".#........", ".#........", ".#####....", ".#....##..", ".#.....#..", ".#......#.", ".#.....#..", "..#....#..", "...####...", ".........."],
    "7": [".########.", ".#......#.", "#......#..", ".......#..", "......#...", "......#...", ".....#....", ".....#....", "....#.....", "....#.....", "...#......", "...#......"],
    "8": ["...####...", "..#....#..", ".#......#.", "..#....#..", "...####...", "..#....#..", ".#......#.", ".#......#.", ".#......#.", "..#....#..", "...####...", ".........."],
    "9": ["...####...", "..#....#..", ".#......#.", ".#......#.", ".#......#.", "..#....##.", "...####.#.", "........#.", "........#.", "..#....#..", "...####...", ".........."],
    ".": ["..........", "..........", "..........", "..........", "..........", "..........", "..........", "..........", "..........", "...##.....", "..####....", "...##....."],
    "-": ["..........", "..........", "..........", "..........", "..........", ".######...", ".######...", "..........", "..........", "..........", "..........", ".........."],
    "+": ["..........", "..........", "...##.....", "...##.....", "...##.....", "########..", "########..", "...##.....", "...##.....", "...##.....", "..........", ".........."],
    "*": ["..........", "..........", "#.....#...", ".#...#....", "..#.#.....", "...#......", "..#.#.....", ".#...#....", "#.....#...", "..........", "..........", ".........."],
    "/": ["..........", "...##.....", "...##.....", "..........", "########..", "########..", "..........", "...##.....", "...##.....", "..........", "..........", ".........."],
    "=": ["..........", "..........", "..........", "########..", "########..", "..........", "########..", "########..", "..........", "..........", "..........", ".........."],
    "~": ["...##.....", "...##.....", "...##.....", "########..", "...##.....", "...##.....", "..........", "########..", "########..", "..........", "..........", ".........."],
    "%": ["##....#...", "##...#....", "....#.....", "...#......", "..#.......", ".#....##..", "#.....##..", "..........", "..........", "..........", "..........", ".........."],
    "C": ["...####...", "..#....#..", ".#......#.", ".#........", ".#........", ".#........", ".#........", ".#........", ".#......#.", "..#....#..", "...####...", ".........."],
    "<": ["..........", "....#.....", "...#......", "..#.......", ".#########", "..#.......", "...#......", "....#.....", "..........", "..........", "..........", ".........."],
}
GLYPH_ORDER = list("0123456789.-+*/=~%C<")  # font slot = position; 10 per row


def scale2(pat):
    out = []
    for line in pat:
        row = []
        for ch in line:
            row.extend([ch, ch])
        out.append(row); out.append(list(row))
    return out


def build_font():
    cols = 10
    rows = (len(GLYPH_ORDER) + cols - 1) // cols
    g = [[TRANSP for _ in range(CELL_W * cols)] for _ in range(CELL_H * rows)]
    for i, ch in enumerate(GLYPH_ORDER):
        big = scale2(GLYPHS[ch])                       # 24 wide x 32 tall
        ox = (i % cols) * CELL_W + (CELL_W - GLYPH_W * 2) // 2
        oy = (i // cols) * CELL_H + (CELL_H - GLYPH_H * 2) // 2
        for yy, line in enumerate(big):
            for xx, c in enumerate(line):
                if c == "#":
                    g[oy + yy][ox + xx] = BLACK
    return g


def main():
    write_palette_rgba16(HERE / "numberator_palette.png", PALETTE_RGBA)
    write_indexed8(HERE / "numberator_chrome.png", build_chrome_sheet())
    write_map16(HERE / "numberator_map.png", build_map())
    write_indexed8(HERE / "numberator_buttons.png", build_buttons())
    write_indexed8(HERE / "numberator_font.png", build_font())
    write_indexed8(HERE / "numberator_closebox.png", build_closebox())
    print(f"wrote numberator_palette.png (7x1 16-bit RGBA), numberator_chrome.png (64x8, 8 tiles), "
          f"numberator_map.png ({COLS_T}x{ROWS_T} 16-bit), numberator_buttons.png ({BTN_W*2}x{BTN_H}, 2 keys), "
          f"numberator_font.png ({CELL_W*10}x{CELL_H*2}, {len(GLYPH_ORDER)} glyphs: {''.join(GLYPH_ORDER)}), "
          f"numberator_closebox.png ({CLOSE_CELL}x{CLOSE_CELL})")


if __name__ == "__main__":
    main()
