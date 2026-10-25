#!/usr/bin/env python3
"""Author the cracked-wall demo's assets: one indexed tile atlas + three 16-bit RGBA palette images.

The demo (examples/wall_cracks_demo/main.cpp) draws a brick wall over a drifting background. The tile
art carries only INDICES; the colors — and crucially their ALPHA — live in palette IMAGES, 16-bit RGBA
PNGs the demo loads with Renderer::loadPaletteImage. Transparency is the palette entry's own alpha (a
real PNG alpha channel), never a color key: an entry at alpha 0 is a hole, 1-254 alpha-blends.

  wall_atlas.png — indexed 8-bit, a horizontal strip of 8x8 tiles (the indices only):
    0 BRICK_A    a brick: mortar along the top + left edges, BRICK_LIGHT body
    1 BRICK_B    a brick with a BRICK_DARK body (the running-bond two-tone)
    2 HOLE       every pixel is index 0; through the wall palette (entry 0 alpha 0) the cell discards,
                 so a missing brick reveals the background
    3 WEATHERED  a brick whose body is the WEATHERED index; that palette entry is partial-alpha, so the
                 background faintly bleeds through (the 1-254 blend)
    4 BG         a checker of BG_A / BG_B; the background layer tiles this and scrolls, so its motion is
                 visible through the holes
    5 CRACK_1    a soft-edged fracture: CRACK_LINE core + CRACK_EDGE beside it on an index-0 field, so
                 only the crack composites onto the wall
    6 CRACK_2    the fracture's continuation, for stacking a longer crack

  wall_palette.png / bg_palette.png / crack_palette.png — 9x1 16-bit RGBA, one pixel per palette entry
  (index k = pixel k). Each fills the entries its tiles use and carries the alpha that makes the holes:
    wall : 0 GAP a=0 · 1 MORTAR · 2 BRICK_DARK · 3 BRICK_LIGHT · 4 WEATHERED a=110 (translucent)
    bg   : 5 BG_A · 6 BG_B (opaque — the thing seen through the holes)
    crack: 0 GAP a=0 (the sprite surround) · 7 CRACK_LINE (opaque) · 8 CRACK_EDGE a=150 (translucent)
  Entries an image's tiles never sample are opaque-black filler, kept so index k means the same thing in
  every palette.

Engine-original art (no third-party content). One-time authoring tool, kept committed so the
binary assets stay regenerable and auditable — the posture of tests/fixtures/gen_fixtures.py,
examples/assets/gen_tilemap_demo.py (the indexed encoder), and
examples/palette_image_demo/assets/gen_palette_demo_assets.py (the 16-bit RGBA encoder), which this
mirrors.

Dependency-free: minimal PNG encoders over the standard library (zlib + struct), so it runs anywhere
Python does — no Pillow.

Run from the engine repo root:  python3 examples/wall_cracks_demo/assets/gen_wall_cracks_assets.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent  # engine/examples/wall_cracks_demo/assets


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_indexed8(path: Path, width: int, height: int, rows: list[list[int]],
                   palette: list[tuple[int, int, int]]) -> None:
    """Indexed 8-bit PNG (colortype 3): one palette-index byte per pixel + an embedded PLTE."""
    raw = bytearray()
    for row in rows:
        raw.append(0)  # filter type 0 (none)
        raw.extend(row)
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)
    plte = b"".join(struct.pack(">BBB", *c) for c in palette)
    out = sig + _chunk(b"IHDR", ihdr) + _chunk(b"PLTE", plte) \
        + _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _chunk(b"IEND", b"")
    path.write_bytes(out)


def write_palette_rgba16(path: Path, entries: list[tuple[int, int, int, int]]) -> None:
    """A palette image: 9x1 16-bit truecolor-alpha PNG (colortype 6, bitdepth 16), one pixel per entry.
    Authored as 8-bit RGBA and widened x257 (255 -> 65535) so the alpha channel is exact and real."""
    raw = bytearray([0])  # one scanline, filter type 0
    for (r, g, b, a) in entries:
        raw.extend(struct.pack(">HHHH", r * 257, g * 257, b * 257, a * 257))
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", len(entries), 1, 16, 6, 0, 0, 0)
    out = sig + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _chunk(b"IEND", b"")
    path.write_bytes(out)


# ── The 8x8 tiles, one digit per pixel = its palette index ────────────────────────────────────────
BRICK_A = ["11111111", "13333333", "13333333", "13333333", "13333333", "13333333", "13333333", "13333333"]
BRICK_B = ["11111111", "12222222", "12222222", "12222222", "12222222", "12222222", "12222222", "12222222"]
HOLE    = ["00000000"] * 8
WEATHER = ["11111111", "14444444", "14444444", "14444444", "14444444", "14444444", "14444444", "14444444"]
BG      = ["55556666", "55556666", "55556666", "55556666", "66665555", "66665555", "66665555", "66665555"]
# CRACK_1 core meanders col 3->2, CRACK_EDGE (8) rides beside it; the rest is index 0.
CRACK_1 = ["00070000", "00078000", "00780000", "00780000", "00070000", "00078000", "00780000", "00780000"]
# CRACK_2 continues the core col 2->1.
CRACK_2 = ["00780000", "00780000", "00780000", "07800000", "07800000", "07800000", "07800000", "07800000"]

TILES = [BRICK_A, BRICK_B, HOLE, WEATHER, BG, CRACK_1, CRACK_2]

# Atlas PLTE — for VIEWING the indices only (the engine recolors via the palette images; an index's
# real color + alpha come from those). Roughly the wall/bg/crack tones; no color key — index 0 is a
# plain dark tone, and the holes come from the palette images' alpha, not from any atlas color.
ATLAS_PLTE = [
    (20, 18, 22),    # 0
    (64, 60, 58),    # 1 MORTAR
    (120, 62, 50),   # 2 BRICK_DARK
    (170, 92, 70),   # 3 BRICK_LIGHT
    (175, 100, 80),  # 4 WEATHERED
    (48, 128, 138),  # 5 BG_A
    (28, 84, 100),   # 6 BG_B
    (18, 12, 12),    # 7 CRACK_LINE
    (40, 26, 22),    # 8 CRACK_EDGE
]

OPAQUE_FILLER = (0, 0, 0, 255)  # an entry no tile drawn through this palette ever samples

# index:        0                1                2                 3                  4
WALL_PALETTE = [(0, 0, 0, 0),    (64, 60, 58, 255), (120, 62, 50, 255), (170, 92, 70, 255), (175, 100, 80, 110)] \
    + [OPAQUE_FILLER] * 4  # 5..8 unused by wall tiles
BG_PALETTE = [OPAQUE_FILLER] * 5 + [(48, 128, 138, 255), (28, 84, 100, 255)] + [OPAQUE_FILLER] * 2  # 5,6
CRACK_PALETTE = [(0, 0, 0, 0)] + [OPAQUE_FILLER] * 6 + [(18, 12, 12, 255), (40, 26, 22, 150)]        # 0,7,8


def main() -> None:
    cell = 8
    width, height = cell * len(TILES), cell
    rows = [[int(ch) for tile in TILES for ch in tile[y]] for y in range(height)]
    write_indexed8(HERE / "wall_atlas.png", width, height, rows, ATLAS_PLTE)
    write_palette_rgba16(HERE / "wall_palette.png", WALL_PALETTE)
    write_palette_rgba16(HERE / "bg_palette.png", BG_PALETTE)
    write_palette_rgba16(HERE / "crack_palette.png", CRACK_PALETTE)
    print(f"wrote wall_atlas.png ({width}x{height}, {len(TILES)} 8x8 tiles, indexed 8-bit)")
    print("wrote wall_palette.png / bg_palette.png / crack_palette.png (9x1 16-bit RGBA, real alpha)")
    print("  wall : 0 GAP a=0  4 WEATHERED a=110   crack : 0 GAP a=0  8 CRACK_EDGE a=150")


if __name__ == "__main__":
    main()
