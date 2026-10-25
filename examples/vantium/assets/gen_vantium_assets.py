#!/usr/bin/env python3
"""Author Vantium's indexed-PNG art: a 16x16-cell deck-tile strip, a sprite sheet, and a HUD font.

All three are colortype-3 (indexed) PNGs -- pixels are palette INDICES; color comes from palettes
the demo uploads and selects per cell / per sprite. EVERY palette in the game holds AT MOST 8
entries, so every index emitted here is 0..7 (asserted at write time). Index 0 is transparent on
the sprite sheet and on the deck strip (the deck layer's holes reveal the starfield beneath).

The art direction is rich, dense hull greebling achieved procedurally -- the technique set:
  - 7-step luminance ramps (indices 1..7, dark to light, per palette family),
  - 50% checker dithering between adjacent ramp steps on large faces,
  - bevel passes (light top/left, dark bottom/right) + 1px inner shadows,
  - etched panel seams and corner rivets,
  - radial shading for the big rotor disc, louvred slats for the vent,
  - deterministic star scatter (a fixed LCG -- no randomness between runs).

  - vantium_tiles.png   -- a single row of 16x16 art cells (each stamps as a 2x2 group of 8px
    engine tiles): deck plates x3, hazard trim, corner trim, tall-structure block, pipe, fuel pod
    (2 cells) + scorched variant, strip chevron, star cells, an empty cell, the 64x64 ROTOR sliced
    into 16 cells, and the 32x32 vent sliced into 4. Orientation variants come free at stamp time
    (flips + 90-degree rotation), never as extra art.
  - vantium_sprites.png -- uniform 48x24 SpriteSeries cells, art top-left-anchored, index-0
    padded: Manta level / bank-1 / bank-2 / side-on (48x24), fighter (32x16), mine (16x16),
    player bolt (16x8), enemy shot (8x8), explosion frames x4 (24x24).
  - vantium_font.png    -- RICH 16x16 cells (each a 2x2 tile stamp): digits 0-9, A-Z, space, and
    a border rule -- doubled 5x7 cores under a 4-step vertical gradient with a 1px outline and a
    drop shadow, the Amiga-HUD look; the gradient recolors per palette (white/gold/cyan).

Engine-original art (an original homage -- no Graftgold / Hewson content). One-time authoring
tool, kept committed so the PNGs stay regenerable and auditable; stdlib zlib/struct only.

Run from the engine repo root:  python3 examples/vantium/assets/gen_vantium_assets.py
"""

from __future__ import annotations

import math
import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent  # engine/examples/vantium/assets

# Neutral viewable PLTE (the demo overrides with its own <=8-entry palettes): a gray ramp.
PALETTE = [(0, 0, 0), (40, 44, 56), (72, 78, 92), (104, 112, 128), (136, 144, 160),
           (168, 176, 192), (204, 210, 224), (240, 244, 252)]

MAX_INDEX = 7  # the 8-color lock -- every emitted pixel must be 0..7


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _png_indexed8(width: int, height: int, plane: list[list[int]]) -> bytes:
    for row in plane:  # the 8-color lock, enforced at the door
        for idx in row:
            assert 0 <= idx <= MAX_INDEX, f"index {idx} exceeds the 8-colour palette lock"
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)
    plte = b"".join(struct.pack(">BBB", r, g, b) for (r, g, b) in PALETTE)
    raw = bytearray()
    for row in plane:
        raw.append(0)
        raw.extend(row)
    return (sig + _chunk(b"IHDR", ihdr) + _chunk(b"PLTE", plte)
            + _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _chunk(b"IEND", b""))


def _blank(w: int, h: int) -> list[list[int]]:
    return [[0 for _ in range(w)] for _ in range(h)]


def _stamp(plane, ox, oy, art) -> None:
    for y, row in enumerate(art):
        for x, idx in enumerate(row):
            if idx:
                plane[oy + y][ox + x] = idx


def _dither(art, x, y, lo, hi) -> int:
    """The 50% checker between two adjacent ramp steps -- the large-surface shading workhorse."""
    return hi if (x + y) % 2 == 0 else lo


# A tiny fixed LCG so scatter details (stars, speckle) are deterministic across runs.
class Lcg:
    def __init__(self, seed: int) -> None:
        self.s = seed & 0xFFFFFFFF

    def next(self) -> int:
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.s


# ---- 16x16 deck-tile arts --------------------------------------------------------------------


def _bevel16(art, face_lo=3, face_hi=4, light=6, dark=2) -> None:
    """Fill a 16x16 cell as a bevelled plate: dithered face, light top/left, dark bottom/right,
    and a 1px inner shadow under the light edge so the plate reads pressed, not flat."""
    for y in range(16):
        for x in range(16):
            art[y][x] = _dither(art, x, y, face_lo, face_hi)
    for i in range(16):
        art[0][i] = light
        art[i][0] = light
        art[15][i] = dark
        art[i][15] = dark
    for i in range(1, 15):
        art[1][i] = min(art[1][i], face_lo)  # the inner shadow line
        art[i][1] = min(art[i][1], face_lo)


def _plate_a() -> list[list[int]]:
    art = _blank(16, 16)
    _bevel16(art)
    return art


def _plate_b() -> list[list[int]]:
    """The riveted variant: plate A plus corner rivets (bright dot over a dark socket)."""
    art = _plate_a()
    for (rx, ry) in ((3, 3), (12, 3), (3, 12), (12, 12)):
        art[ry][rx] = 7
        art[ry + 1][rx] = 1
    return art


def _plate_c() -> list[list[int]]:
    """The seamed variant: plate A etched with a cross of panel seams (dark cut + light lip)."""
    art = _plate_a()
    for i in range(2, 14):
        art[7][i] = 1
        art[8][i] = 5
        art[i][7] = 1
        art[i][8] = 5
    return art


def _hazard() -> list[list[int]]:
    """Deck-edge hazard trim: diagonal amber/dark stripes under a bright lip. The hazard palette
    maps 6 = amber, 7 = bright lip, 1..4 = the dark steel between stripes."""
    art = _blank(16, 16)
    for y in range(16):
        for x in range(16):
            art[y][x] = 6 if ((x + y) // 4) % 2 == 0 else 2
    for i in range(16):
        art[0][i] = 7
        art[15][i] = 1
    return art


def _corner() -> list[list[int]]:
    """Corner trim (top-left orientation; the other three corners are flips at stamp time)."""
    art = _plate_a()
    for i in range(16):
        art[0][i] = 7
        art[i][0] = 7
    for i in range(2, 16):
        art[2][i] = 6
        art[i][2] = 6
    art[1][1] = 7
    return art


def _structure() -> list[list[int]]:
    """The TALL superstructure block -- the lethal one. A strong 3px bevel and a bright face so
    raised reads unmistakably against the deck's dimmer plates."""
    art = _blank(16, 16)
    for y in range(16):
        for x in range(16):
            art[y][x] = _dither(art, x, y, 4, 5)
    for t in range(3):
        for i in range(t, 16 - t):
            art[t][i] = 7 - t if t else 7
            art[i][t] = 7 - t if t else 7
            art[15 - t][i] = 1 + t
            art[i][15 - t] = 1 + t
    for i in range(5, 11):  # a small inset panel on the face
        art[5][i] = 2
        art[10][i] = 6
        art[i][5] = 2
        art[i][10] = 6
    return art


def _pipe_h() -> list[list[int]]:
    """A horizontal conduit run: a cylinder's vertical shading ramp + a bracket every 8px.
    The vertical variant is this art under Rot90 at stamp time."""
    art = _blank(16, 16)
    shade = [1, 2, 4, 6, 7, 6, 4, 2]  # the cylinder profile, centered
    for y in range(16):
        for x in range(16):
            art[y][x] = shade[abs(y - 8) if abs(y - 8) < 8 else 7] if 1 <= y <= 14 else 1
    for bx in (3, 11):
        for y in range(1, 15):
            art[y][bx] = 1
            art[y][bx + 1] = 5
    return art


def _pod(scorched: bool) -> list[list[list[int]]]:
    """The 16x32 fuel pod as its two 16x16 cells (top, bottom): a rounded capsule with a glowing
    core (palette 6/7 = the glow) -- or its burned-out husk after a hit."""
    tall = _blank(16, 32)
    for y in range(32):
        for x in range(16):
            dx, dy = x - 7.5, (y - 15.5) * 0.55
            r = math.hypot(dx, dy)
            if r <= 7.2:
                if scorched:
                    tall[y][x] = 1 if (x + y) % 2 else 2
                else:
                    tall[y][x] = _dither(tall, x, y, 4, 5)
                if r >= 6.2:
                    tall[y][x] = 2 if not scorched else 1
    for y in range(10, 22):  # the core
        for x in range(4, 12):
            dx, dy = x - 7.5, (y - 15.5) * 0.8
            if math.hypot(dx, dy) <= 3.4:
                tall[y][x] = 3 if scorched else 7
            elif math.hypot(dx, dy) <= 4.4 and not scorched:
                tall[y][x] = 6
    return [[row[:] for row in tall[0:16]], [row[:] for row in tall[16:32]]]


def _chevron() -> list[list[int]]:
    """A landing-strip chevron: a bright arrow etched into a dark plate."""
    art = _blank(16, 16)
    for y in range(16):
        for x in range(16):
            art[y][x] = _dither(art, x, y, 1, 2)
    for t in range(6):
        y0, y1 = 7 - t, 8 + t
        x = 12 - t * 2
        if 0 <= x < 15:
            for xx in (x, x + 1):
                art[y0][xx] = 7
                art[y1][xx] = 7
    return art


def _star(seed: int, count: int) -> list[list[int]]:
    """A sparse star cell for the parallax layer: transparent void, a few 1px stars in three
    brightnesses (7 bright / 5 mid / 3 dim). Deterministic scatter."""
    art = _blank(16, 16)
    rng = Lcg(seed)
    for _ in range(count):
        x = rng.next() % 16
        y = rng.next() % 16
        art[y][x] = (7, 5, 3, 3)[rng.next() % 4]
    return art


def _rotor64() -> list[list[int]]:
    """The signature set-piece: a 64x64 radially-shaded rotor disc -- dark rim ring, blade
    notches, a dithered steel face falling off with radius, and a glowing hub (palette entries
    6/7 = the glow accents). Sliced into 16 16x16 cells for stamping."""
    art = _blank(64, 64)
    for y in range(64):
        for x in range(64):
            dx, dy = x - 31.5, y - 31.5
            r = math.hypot(dx, dy)
            if r > 30.5:
                continue
            ang = math.degrees(math.atan2(dy, dx)) % 45.0
            if r > 27.5:
                art[y][x] = 1                                    # the rim ring
            elif r > 24.5:
                art[y][x] = 3 if (x + y) % 2 else 2              # rim inner lip
            elif r > 9.5:
                if ang < 7.0:                                     # 8 blade notches
                    art[y][x] = 2
                else:
                    hi = 5 if r < 16 else 4                      # face falls off with radius
                    art[y][x] = _dither(art, x, y, hi - 1, hi)
            elif r > 6.5:
                art[y][x] = 6                                    # hub glow ring
            else:
                art[y][x] = 7                                    # hub core
    return art


def _vent32() -> list[list[int]]:
    """A 32x32 louvred vent: dark slats behind a bevelled frame. Sliced into 4 cells."""
    art = _blank(32, 32)
    for y in range(32):
        for x in range(32):
            art[y][x] = 2 if (y % 4) < 2 else 5
    for t in range(2):
        for i in range(t, 32 - t):
            art[t][i] = 7 - t
            art[i][t] = 7 - t
            art[31 - t][i] = 1 + t
            art[i][31 - t] = 1 + t
    return art


def _slice16(art: list[list[int]], cells_w: int, cells_h: int) -> list[list[list[int]]]:
    """Slice a big art block into 16x16 cells, row-major -- the multi-cell set-piece door."""
    out = []
    for cy in range(cells_h):
        for cx in range(cells_w):
            out.append([row[cx * 16:(cx + 1) * 16] for row in art[cy * 16:(cy + 1) * 16]])
    return out


def build_tiles() -> tuple[bytes, int]:
    cells: list[list[list[int]]] = [
        _plate_a(), _plate_b(), _plate_c(), _hazard(), _corner(), _structure(), _pipe_h(),
    ]
    cells += _pod(scorched=False)   # pod top, bottom
    cells += _pod(scorched=True)    # scorched top, bottom
    cells += [_chevron(), _star(0xBEEF, 5), _star(0xCAFE, 3), _blank(16, 16)]
    cells += _slice16(_rotor64(), 4, 4)
    cells += _slice16(_vent32(), 2, 2)
    n = len(cells)
    plane = _blank(16 * n, 16)
    for i, cell in enumerate(cells):
        _stamp(plane, i * 16, 0, cell)
    return _png_indexed8(16 * n, 16, plane), n


# ---- Sprites ---------------------------------------------------------------------------------


def _manta(frame: int) -> list[list[int]]:
    """The player's Manta, top-down, nose to the RIGHT (flipX gives the left facing). A swept
    delta: per-row half-span profile builds the silhouette; shading splits light upper / dark
    lower halves with a bright spine, a cyan canopy (6), and amber engine cores (7) at the tail.
    frame 0 = level, 1/2 = progressive banks (span squashes toward the spine), 3 = side-on (the
    roll / turn-around blade)."""
    art = _blank(48, 24)
    if frame == 3:  # side-on: a thin full-length blade with canopy bump + engine glow
        for y in range(9, 15):
            for x in range(2, 46):
                art[y][x] = 5 if y in (9, 10) else (_dither(art, x, y, 3, 4) if y < 13 else 2)
        for x in range(26, 34):
            art[8][x] = 6
        for y in range(10, 14):
            art[y][2] = 7
            art[y][3] = 7
        return art
    squash = (1.0, 0.72, 0.45)[frame]
    for x in range(46):
        # Wing half-span at this column: broad at the tail, tapering to the nose point.
        t = x / 45.0
        half = (2.0 + 9.5 * (1.0 - t) ** 0.8) if x > 6 else (2.0 + 1.2 * x)
        half = max(1.5, half * squash + (1.0 - squash) * 2.0)
        # The fuselage keeps full depth so banking squashes wings, not the hull.
        if 18 <= x <= 40:
            half = max(half, 3.2)
        for dy in range(-int(half), int(half) + 1):
            y = 12 + dy
            if not (0 <= y < 24):
                continue
            edge = abs(dy) >= half - 1.0
            if dy < -1:
                art[y][x + 2] = 2 if edge else _dither(art, x, y, 5, 6)   # lit upper surface
            elif dy > 1:
                art[y][x + 2] = 1 if edge else _dither(art, x, y, 2, 3)   # shadowed lower
            else:
                art[y][x + 2] = 7 if x > 8 else 5                          # the bright spine
    for x in range(30, 37):  # canopy
        art[11][x] = 6
        art[12][x] = 6
    for y in (10, 12, 14):   # engine cores at the tail
        art[y][2] = 7
        art[y][3] = 7
    return art


def _fighter() -> list[list[int]]:
    """An enemy dart, 32x16, nose LEFT (they mostly cross against the player). One shape; the
    four wave liveries are palette swaps."""
    art = _blank(32, 16)
    for x in range(30):
        t = x / 29.0
        half = 1.5 + 5.5 * t
        for dy in range(-int(half), int(half) + 1):
            y = 8 + dy
            if not (0 <= y < 16):
                continue
            edge = abs(dy) >= half - 1.0
            art[y][x + 1] = 2 if edge else (_dither(art, x, y, 4, 5) if dy <= 0 else 3)
    for x in range(8, 13):
        art[7][x] = 6
        art[8][x] = 6
    art[7][30] = 7
    art[9][30] = 7
    return art


def _mine() -> list[list[int]]:
    """A drifting contact mine: a dark spiked ball around a baleful glow core (6/7)."""
    art = _blank(16, 16)
    for y in range(16):
        for x in range(16):
            r = math.hypot(x - 7.5, y - 7.5)
            if r <= 5.2:
                art[y][x] = _dither(art, x, y, 2, 3)
            if r <= 2.2:
                art[y][x] = 7
            elif r <= 3.2:
                art[y][x] = 6
    for (sx, sy) in ((7, 0), (7, 15), (0, 7), (15, 7), (2, 2), (13, 2), (2, 13), (13, 13)):
        art[sy][sx] = 4
    return art


def _bolt() -> list[list[int]]:
    """The Manta's twin-linked bolt: a bright core with a halo, tapered at the head (right)."""
    art = _blank(16, 8)
    for x in range(15):
        art[3][x] = 7
        art[4][x] = 7
        if x < 12:
            art[2][x] = 5
            art[5][x] = 5
    art[3][15] = 5
    art[4][15] = 5
    return art


def _eshot() -> list[list[int]]:
    art = _blank(8, 8)
    for y in range(8):
        for x in range(8):
            r = math.hypot(x - 3.5, y - 3.5)
            if r <= 1.6:
                art[y][x] = 7
            elif r <= 3.0:
                art[y][x] = 5
    return art


def _boom(i: int) -> list[list[int]]:
    """Explosion frames: a bright core that blows out to a fragment ring, then embers."""
    art = _blank(24, 24)
    rng = Lcg(0x600D + i)
    core = (5.0, 8.0, 3.0, 0.0)[i]
    ring = (0.0, 9.5, 11.0, 11.5)[i]
    for y in range(24):
        for x in range(24):
            r = math.hypot(x - 11.5, y - 11.5)
            if r <= core:
                art[y][x] = 7 if r <= core * 0.6 else 6
            if ring and abs(r - ring) <= 1.4:
                art[y][x] = (6, 5, 4, 3)[i] if (x * 7 + y * 3 + i) % 3 else 0  # ragged ring
    if i == 3:
        for _ in range(10):  # dying embers
            art[rng.next() % 24][rng.next() % 24] = 3
    return art


SPRITE_CELL_W, SPRITE_CELL_H = 48, 24


def build_sprites() -> tuple[bytes, int]:
    cells = [_manta(0), _manta(1), _manta(2), _manta(3), _fighter(), _mine(), _bolt(), _eshot(),
             _boom(0), _boom(1), _boom(2), _boom(3)]
    n = len(cells)
    plane = _blank(SPRITE_CELL_W * n, SPRITE_CELL_H)
    for i, cell in enumerate(cells):
        _stamp(plane, i * SPRITE_CELL_W, 0, cell)
    return _png_indexed8(SPRITE_CELL_W * n, SPRITE_CELL_H, plane), n


# ---- Font (the established 5x7-in-8x8 set: 0-9, A-Z, space, border rule) ----------------------

GLYPH_W, GLYPH_H, FONT_CELL = 5, 7, 8

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

FONT_ORDER = [str(d) for d in range(10)] + [chr(ord("A") + i) for i in range(26)] + [" "]
BORDER_SLOT = len(FONT_ORDER)  # 37

# Vantium's font is RICH: 16x16 cells (each stamps as a 2x2 tile group, like the deck blocks).
# Each glyph is the 5x7 core doubled to 10x14 — chunky, authentically pixel — then dressed the
# Amiga-HUD way: a 4-step vertical gradient fill (7 bright top → 4 at the base), a 1px dark
# outline (1), and a bottom-right drop shadow (2). The gradient recolors per palette, so one
# sheet yields white/gold/cyan liveries. Palette layout: 0 = HUD bar background, 1 = outline,
# 2 = shadow, 4..7 = the gradient ramp (3 reserved).
RICH_CELL = 16


def _rich_glyph(mask_rows: list[str]) -> list[list[int]]:
    art = _blank(RICH_CELL, RICH_CELL)
    filled = set()
    for gy in range(GLYPH_H):
        for gx in range(GLYPH_W):
            if mask_rows[gy][gx] == "#":
                for dy in range(2):          # double the 5x7 core to 10x14 at (2,1)
                    for dx in range(2):
                        filled.add((2 + gx * 2 + dx, 1 + gy * 2 + dy))
    for (x, y) in filled:                    # gradient fill by row band
        art[y][x] = 7 if y < 5 else (6 if y < 8 else (5 if y < 11 else 4))
    for (x, y) in list(filled):              # 1px outline, 8-neighbor
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                nx, ny = x + dx, y + dy
                if 0 <= nx < RICH_CELL and 0 <= ny < RICH_CELL and art[ny][nx] == 0:
                    art[ny][nx] = 1
    for y in range(RICH_CELL):               # drop shadow: below-right of the outlined shape
        for x in range(RICH_CELL):
            if art[y][x] == 0 and x > 0 and y > 0 and art[y - 1][x - 1] in (1, 4, 5, 6, 7):
                art[y][x] = 2
    return art


def _rich_rule() -> list[list[int]]:
    """The HUD's full-width border rule as a 16x16 cell: a bevelled gradient bar mid-cell."""
    art = _blank(RICH_CELL, RICH_CELL)
    for x in range(RICH_CELL):
        art[5][x] = 7
        art[6][x] = 6
        art[7][x] = 5
        art[8][x] = 4
        art[9][x] = 2
        art[10][x] = 1
    return art


def build_font() -> bytes:
    n = len(FONT_ORDER) + 1
    plane = _blank(RICH_CELL * n, RICH_CELL)
    for k, ch in enumerate(FONT_ORDER):
        _stamp(plane, k * RICH_CELL, 0, _rich_glyph(_GLYPHS[ch]))
    _stamp(plane, BORDER_SLOT * RICH_CELL, 0, _rich_rule())
    return _png_indexed8(RICH_CELL * n, RICH_CELL, plane)


# ---- Deck section map-PNGs (16-bit grayscale; one pixel per 16px deck cell) -------------------
#
# The deck sections are authored HERE, as glyph rows, and emitted as 20x18 16-BIT GRAYSCALE map
# PNGs the demo loads through the engine's map-import pipeline (loadMapPng -> IndexGrid). Ids are
# spread so a HUMAN can read the maps: Void is pure black (0) and every other class climbs from
# 25% gray to pure white in even bands — id = 16383 + (class - 1) * 3072, topping out at exactly
# 65535. The class ORDER matches vant::MapGlyph in layout.h.
#
# Glyphs: ' ' void  '.' plate  ',' riveted  ':' seamed  '-' hazard  '|' hazard-vertical
#         'C' corner  '#' TALL structure (lethal)  'p'/'i' pipe H/V  'F'/'f' fuel pod top/bottom
#         'M' mine spawn  '>' strip chevron  'R'+'r' rotor 4x4  'V'+'v' vent 2x2

MAP_FIRST_ID = 16383
MAP_ID_STEP = 3072
GLYPH_CLASS = {  # glyph -> MapGlyph class index (the layout.h order)
    " ": 0, ".": 1, ",": 2, ":": 3, "-": 4, "|": 5, "C": 6, "#": 7,
    "p": 8, "i": 9, "F": 10, "f": 11, "M": 12, ">": 13, "R": 14, "r": 15, "V": 16, "v": 17,
}


def _glyph_id(ch: str) -> int:
    ci = GLYPH_CLASS[ch]
    return 0 if ci == 0 else MAP_FIRST_ID + (ci - 1) * MAP_ID_STEP

SECTIONS: dict[str, list[str]] = {
    "bow": [
        "                 -.,",
        "               -....",
        "             -......",
        "           -........",
        "         -..........",
        "       -...,........",
        "     -.....,........",
        "   -.......#........",
        " -.........,......,.",
        " -........,....##...",
        "   -......#.........",
        "     -..............",
        "       -............",
        "         -..........",
        "           -........",
        "             -......",
        "               -....",
        "                 -.,",
    ],
    "stern": [
        "--------------C     ",
        ",.............|     ",
        "......####....|     ",
        "......#..#....|#### ",
        "......####....|#### ",
        "..............|     ",
        "...Rrrr.......|     ",
        "...rrrr....M..|     ",
        "...rrrr.......|#### ",
        "...rrrr.......|#### ",
        "..............|     ",
        "......pppppp..|     ",
        "..............|     ",
        "....##........|#### ",
        "....##....,...|#### ",
        ".....,........|     ",
        "..............|     ",
        "--------------C     ",
    ],
    "strip": [
        "--------------------",
        "....................",
        ".,....,......,......",
        "....................",
        "..##............##..",
        "..##............##..",
        "....................",
        "....>>>>>>>>>>>>....",
        "....>>>>>>>>>>>>....",
        "....................",
        "....M..........M....",
        "....................",
        "..##............##..",
        "..##............##..",
        "....................",
        "......,......,......",
        "....................",
        "--------------------",
    ],
    "mid1": [
        "--------------------",
        "....,......:........",
        "..####......####....",
        "..#..#......#..#....",
        "..####......#..#....",
        "............####....",
        ".....Rrrr...........",
        ".....rrrr...M.......",
        ".....rrrr......Vv...",
        ".....rrrr......vv...",
        "....................",
        "..pppppp............",
        "............####....",
        "..F.........#..#....",
        "..f.........####....",
        "....,........:......",
        "....................",
        "--------------------",
    ],
    "mid2": [
        "--------------------",
        "....................",
        "....############....",
        "....############....",
        "....................",
        "....############....",
        "....############....",
        "....................",
        "..M.....,......M....",
        "....................",
        "....####....####....",
        "....####....####....",
        "....................",
        "....####....####....",
        "....####....####....",
        "....................",
        ".,.......:........,.",
        "--------------------",
    ],
    "mid3": [
        "--------------------",
        ".,......,......,....",
        "...Vv......Vv.......",
        "...vv......vv.......",
        "....................",
        "..F....####....F....",
        "..f....#..#....f....",
        ".......####.........",
        "....................",
        "....M..........M....",
        "..####........####..",
        "..#..#........#..#..",
        "..####........####..",
        "....................",
        "...Vv......Vv.......",
        "...vv......vv.......",
        "....................",
        "--------------------",
    ],
    "mid4": [
        "--------------------",
        "....................",
        "..pppppppppppppp....",
        "....................",
        "..##..##..##..##....",
        "..##..##..##..##....",
        "....................",
        "..pppppppppppppp....",
        ".........M..........",
        "....................",
        "....i....i....i.....",
        "....i....i....i.....",
        "....i....i....i.....",
        "....................",
        "..F..,..F..,...F....",
        "..f.....f......f....",
        "....................",
        "--------------------",
    ],
    "mid5": [
        "--------------------",
        "......,......,......",
        "..Rrrr......Rrrr....",
        "..rrrr......rrrr....",
        "..rrrr......rrrr....",
        "..rrrr......rrrr....",
        "....................",
        "......########......",
        "........F....#......",
        "........f..M.#......",
        "......########......",
        "....................",
        "..,....:......,.....",
        "....####....####....",
        "....####....####....",
        "....................",
        "....................",
        "--------------------",
    ],
    "mid6": [
        "--------------------",
        "..,......:.......,..",
        "......####..........",
        "..Vv..#..#..pppp....",
        "..vv..####..........",
        "....................",
        "..####......Rrrr....",
        "..#..#..M...rrrr....",
        "..####......rrrr....",
        "............rrrr....",
        "....................",
        "..F...####.....Vv...",
        "..f...####.....vv...",
        "....................",
        "....pppppp..........",
        ".........,....,.....",
        "....................",
        "--------------------",
    ],
}


def _png_gray16(width: int, height: int, values: list[list[int]]) -> bytes:
    """A 16-bit GRAYSCALE PNG — the engine map-import's headline format. The stored sample IS the
    id, big-endian, never derived from a color."""
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 16, 0, 0, 0, 0)  # bit depth 16, grayscale
    raw = bytearray()
    for row in values:
        raw.append(0)
        for v in row:
            assert 0 <= v <= 0xFFFF
            raw += struct.pack(">H", v)
    return (sig + _chunk(b"IHDR", ihdr)
            + _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _chunk(b"IEND", b""))


def build_deck_maps() -> None:
    for name, rows in SECTIONS.items():
        assert len(rows) == 18, f"{name}: {len(rows)} rows"
        values: list[list[int]] = []
        for r, row in enumerate(rows):
            assert len(row) == 20, f"{name} row {r}: {len(row)} cols"
            values.append([_glyph_id(ch) for ch in row])
        for r, row in enumerate(rows):  # every pod top must pair with a bottom directly below
            for c, ch in enumerate(row):
                if ch == "F":
                    assert r + 1 < 18 and rows[r + 1][c] == "f", f"{name}: F at ({c},{r}) unpaired"
        (HERE / f"vantium_deck_{name}.png").write_bytes(_png_gray16(20, 18, values))
    print(f"vantium_deck_*.png: {len(SECTIONS)} sections, 20x18 16-bit grayscale, "
          f"void=0 + classes {MAP_FIRST_ID}..{MAP_FIRST_ID + 16 * MAP_ID_STEP} "
          f"in steps of {MAP_ID_STEP}")


def main() -> None:
    tiles, ntiles = build_tiles()
    (HERE / "vantium_tiles.png").write_bytes(tiles)
    print(f"vantium_tiles.png: {16 * ntiles}x16, {ntiles} 16x16 art cells "
          f"(plates, hazard, corner, structure, pipe, pod x2+scorched, chevron, stars, empty, "
          f"rotor 4x4, vent 2x2)")
    sprites, nsprites = build_sprites()
    (HERE / "vantium_sprites.png").write_bytes(sprites)
    print(f"vantium_sprites.png: {SPRITE_CELL_W * nsprites}x{SPRITE_CELL_H}, {nsprites} cells "
          f"(manta x4, fighter, mine, bolt, eshot, boom x4)")
    (HERE / "vantium_font.png").write_bytes(build_font())
    print(f"vantium_font.png: {RICH_CELL * (len(FONT_ORDER) + 1)}x{RICH_CELL}, "
          f"37 rich 16x16 glyphs (gradient + outline + shadow) + rule")
    build_deck_maps()


if __name__ == "__main__":
    main()
