#!/usr/bin/env python3
"""Author Ferryman's art: an indexed sprite sheet, an indexed 32x32 terrain tile sheet, a rich
16x16 font sheet, and REAL PALETTE IMAGES -- one small color PNG per palette, loaded at runtime
with Renderer::loadPaletteImage (one pixel per entry, alpha included).

The indexed sheets are colortype-3 PNGs -- the pixels are palette INDICES; every color lives in
the palette images under palettes/ (the demo never hand-uploads a color table). One colonist
shape family recolors to three ramps, each vehicle livery's running lights blink by palette
phase, the water shimmer and the beacon glow are palette selection alone, and one font sheet
yields the white/gold/cyan liveries in both floating (alpha-0-backed) and HUD (opaque-backed)
forms -- all of it palette-image data.

THE RICHNESS LOCKS (asserted, not aspirational):
  * every palette has exactly 16 entries (indices 0..15) -- the CPS2 4bpp floor;
  * every emitted index is 0..15 (asserted at the PNG writer's door);
  * every SOLID tile and every sprite uses AT LEAST 12 DISTINCT indices (asserted per cell/tile
    at build) -- the full ramp is in the art, not just in the palette;
  * NO 8x8 art anywhere: terrain tiles are 32x32 (stamped as 4x4 groups of the engine's 8px
    cells), sprites run 16x16 .. 48x48, font glyphs are 16x16.

Index vocabulary (sprites + terrain): 0 = hole/background, 1 = outline, 2..11 = a 10-step
luminance ramp (dark -> bright), 12 = cool shadow, 13 = rim light, 14 = light A, 15 = light B /
hot spark. Font: 0 = background, 1 = outline, 2 = shadow, 3 reserved, 4..11 = an 8-step vertical
gradient (11 at the top).

Outputs (committed; regenerate any time -- byte-identical):
  ferryman_sprites.png   -- uniform 48x48 SpriteSeries: ferry A/B (32x22), colonists x3 looks x2
                            bob frames (16x16), dart (28x14), sweeper (36x18), hauler (48x18),
                            abductor A/B (32x22), mutant A/B (18x18), boom x3 (32x32).
  ferryman_terrain.png   -- 22 tiles of 32x32: hole, water x4, sparkle/foam x2, shore x3 + buoy +
                            mooring, causeway fresh/worn + dash + oil, median x2 + lamp,
                            sanctuary + beacon + trim.
  ferryman_font.png      -- 37 rich 16x16 glyphs (8-step gradient + outline + shadow) + rule.
  palettes/<name>.png    -- 16x1 RGBA palette images, one per palette (27 of them), consumed by
                            loadPaletteImage. Alpha is data: floating-text backgrounds are
                            alpha-0 entries, everything else opaque.

Engine-original art (an original homage -- no Konami / Williams content). Stdlib zlib/struct only.
Run from the engine repo root:  python3 examples/ferryman/assets/gen_ferryman_assets.py
"""

from __future__ import annotations

import math
import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent  # engine/examples/ferryman/assets
PAL_DIR = HERE / "palettes"

MAX_INDEX = 15   # the 16-entry lock
MIN_DISTINCT = 12  # every solid tile / sprite must use at least this many distinct indices

# Index vocabulary.
OUT = 1
R = list(range(2, 12))  # R[0]..R[9] -- the 10-step ramp, dark -> bright
SHADOW, RIM, LIGHT_A, LIGHT_B = 12, 13, 14, 15

# Neutral viewable PLTE for the indexed sheets (the demo colors everything via palette images).
VIEW_PLTE = [(0, 0, 0), (20, 22, 30)] + \
    [(40 + i * 20, 44 + i * 20, 56 + i * 19) for i in range(10)] + \
    [(52, 58, 76), (228, 232, 244), (244, 240, 220), (255, 255, 255)]


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _png_indexed8(width: int, height: int, plane: list[list[int]]) -> bytes:
    for row in plane:  # the 16-entry lock, enforced at the door
        for idx in row:
            assert 0 <= idx <= MAX_INDEX, f"index {idx} exceeds the 16-entry palette lock"
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)
    plte = b"".join(struct.pack(">BBB", r, g, b) for (r, g, b) in VIEW_PLTE)
    raw = bytearray()
    for row in plane:
        raw.append(0)
        raw.extend(row)
    return (sig + _chunk(b"IHDR", ihdr) + _chunk(b"PLTE", plte)
            + _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _chunk(b"IEND", b""))


def _png_rgba8(width: int, height: int, pixels: list[list[tuple[int, int, int, int]]]) -> bytes:
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    raw = bytearray()
    for row in pixels:
        raw.append(0)
        for (r, g, b, a) in row:
            raw.extend((r, g, b, a))
    return (sig + _chunk(b"IHDR", ihdr)
            + _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _chunk(b"IEND", b""))


def _blank(w: int, h: int) -> list[list[int]]:
    return [[0 for _ in range(w)] for _ in range(h)]


def _stamp(plane, ox, oy, art) -> None:
    for y, row in enumerate(art):
        for x, idx in enumerate(row):
            if idx:
                plane[oy + y][ox + x] = idx


def _dither(x: int, y: int, lo: int, hi: int) -> int:
    """The 50% checker between two adjacent ramp steps -- the large-surface shading workhorse."""
    return hi if (x + y) % 2 == 0 else lo


def _band(u: float, x: int, y: int) -> int:
    """Map u in [0,1] (lit -> shaded) across the 10-step ramp with dithered transitions."""
    b = (1.0 - max(0.0, min(1.0, u))) * 9.0  # 9 lit -> 0 shaded
    lo = R[max(0, min(9, int(b)))]
    hi = R[max(0, min(9, int(b) + 1))]
    return lo if b - int(b) < 0.5 else _dither(x, y, lo, hi)


def _outline(art, light_rim: bool = True) -> None:
    """Silhouette pass: edge pixels become the outline; the top-left limb catches a rim light."""
    h, w = len(art), len(art[0])
    for y in range(h):
        for x in range(w):
            if art[y][x] == 0:
                continue
            edge = any(not (0 <= y + dy < h and 0 <= x + dx < w) or art[y + dy][x + dx] == 0
                       for dy in (-1, 0, 1) for dx in (-1, 0, 1))
            if edge:
                art[y][x] = RIM if (light_rim and x + y < (w + h) // 2) else OUT


def _assert_rich(art, name: str, minimum: int = MIN_DISTINCT) -> None:
    used = {idx for row in art for idx in row if idx != 0}
    assert len(used) >= minimum, (
        f"'{name}' uses only {len(used)} distinct indices ({sorted(used)}) -- "
        f"the richness lock demands >= {minimum}")


class Lcg:
    """A tiny fixed LCG so scatter details are deterministic across runs."""

    def __init__(self, seed: int) -> None:
        self.s = seed & 0xFFFFFFFF

    def next(self) -> int:
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.s

    def frac(self) -> float:
        return self.next() / 0xFFFFFFFF


# ==================================================================================================
# Palette images -- the color authority. One 16x1 RGBA PNG per palette; entry k is pixel k.
# ==================================================================================================


def _lerp(a: tuple[int, int, int], b: tuple[int, int, int], t: float) -> tuple[int, int, int]:
    return (round(a[0] + (b[0] - a[0]) * t),
            round(a[1] + (b[1] - a[1]) * t),
            round(a[2] + (b[2] - a[2]) * t))


def _family(dark: tuple[int, int, int], bright: tuple[int, int, int],
            *, hole: bool, outline: tuple[int, int, int] | None = None,
            shadow: tuple[int, int, int] | None = None,
            rim: tuple[int, int, int] | None = None,
            light_a: tuple[int, int, int] = (255, 224, 140),
            light_b: tuple[int, int, int] = (255, 255, 255)) -> list[tuple[int, int, int, int]]:
    """Build a standard 16-entry family palette: 0 hole/base, 1 outline, 2..11 the 10-step ramp,
    12 cool shadow, 13 rim, 14/15 the light pair. Non-linear ramp spacing (eased toward the dark
    end) so mid-tones stay distinct under dithering."""
    entries: list[tuple[int, int, int, int]] = []
    entries.append((0, 0, 0, 0) if hole else (*dark, 255))
    out = outline if outline else _lerp(dark, (0, 0, 0), 0.55)
    entries.append((*out, 255))
    for i in range(10):
        t = (i / 9.0) ** 0.85
        entries.append((*_lerp(dark, bright, t), 255))
    sh = shadow if shadow else _lerp(dark, (10, 12, 30), 0.5)
    entries.append((*sh, 255))
    rm = rim if rim else _lerp(bright, (255, 255, 255), 0.6)
    entries.append((*rm, 255))
    entries.append((*light_a, 255))
    entries.append((*light_b, 255))
    assert len(entries) == 16
    return entries


def _swap_lights(pal: list[tuple[int, int, int, int]]) -> list[tuple[int, int, int, int]]:
    """The livery's blink phase: identical family, entries 14 and 15 exchanged."""
    out = list(pal)
    out[14], out[15] = out[15], out[14]
    return out


def _font_pal(bg: tuple[int, int, int, int], grad_top: tuple[int, int, int],
              grad_base: tuple[int, int, int]) -> list[tuple[int, int, int, int]]:
    """Font layout: 0 bg (alpha-0 for floating text, opaque for the HUD bar), 1 outline,
    2 shadow, 3 reserved, 4..11 the 8-step gradient (11 = the bright top)."""
    entries = [bg, (5, 8, 15, 255), (19, 26, 44, 255), (0, 0, 0, 255)]
    for i in range(8):
        t = i / 7.0
        entries.append((*_lerp(grad_base, grad_top, t), 255))
    entries += [(0, 0, 0, 255)] * 4  # 12..15 unused by glyph art; opaque black placeholders
    assert len(entries) == 16
    return entries


kBar = (13, 17, 30, 255)     # the opaque HUD-bar background
kClear = (0, 0, 0, 0)        # the floating-text material hole

PALETTES: dict[str, list[tuple[int, int, int, int]]] = {
    # ── Sprites ────────────────────────────────────────────────────────────────────────────────
    "ferry": _family((30, 16, 8), (246, 198, 118), hole=True,
                     rim=(255, 240, 200), light_a=(255, 208, 110), light_b=(255, 250, 230)),
    "colonist_a": _family((6, 30, 22), (152, 232, 192), hole=True,
                          light_a=(220, 255, 236), light_b=(255, 255, 255)),
    "colonist_b": _family((6, 24, 34), (140, 220, 232), hole=True,
                          light_a=(210, 248, 255), light_b=(255, 255, 255)),
    "colonist_c": _family((26, 28, 8), (208, 214, 120), hole=True,
                          light_a=(244, 250, 190), light_b=(255, 255, 255)),
    "dart_a": _family((34, 6, 14), (252, 128, 112), hole=True,
                      light_a=(255, 232, 120), light_b=(120, 40, 40)),
    "sweeper_a": _family((14, 18, 30), (196, 214, 236), hole=True,
                         light_a=(140, 220, 255), light_b=(40, 70, 100)),
    "hauler_a": _family((36, 22, 4), (252, 196, 96), hole=True,
                        light_a=(255, 240, 150), light_b=(120, 80, 30)),
    "abductor": _family((36, 30, 48), (240, 236, 228), hole=True,
                        shadow=(30, 20, 50), rim=(255, 255, 255),
                        light_a=(206, 120, 255), light_b=(255, 90, 220)),
    "mutant": _family((22, 34, 4), (198, 244, 96), hole=True,
                      shadow=(16, 40, 20), light_a=(255, 250, 160), light_b=(255, 70, 70)),
    "boom": _family((44, 10, 4), (255, 244, 196), hole=True,
                    rim=(255, 255, 230), light_a=(255, 190, 70), light_b=(255, 255, 255)),
    "bolt_enemy": _family((52, 4, 40), (255, 150, 235), hole=True,
                          rim=(255, 214, 250), light_a=(255, 120, 240), light_b=(255, 255, 255)),
    "bolt_cargo": _family((50, 34, 4), (255, 226, 130), hole=True,
                          rim=(255, 246, 200), light_a=(255, 210, 80), light_b=(255, 255, 255)),
    # A flat dark silhouette for a flying craft's cast shadow: the craft's own art is redrawn
    # through THIS palette on a low layer — entry 0 stays the transparent hole, 1..15 all one dark
    # tone; the softness comes from the sprite's alpha at draw time.
    "shadow": [kClear] + [(6, 10, 22, 255)] * 15,
    # The boat's wake foam: a deep-blue → white ramp so the churn reads as white foam at the core
    # feathering to blue at the rim. The renderer trails it behind the moving hull at a soft alpha.
    "wake": _family((44, 96, 140), (250, 253, 255), hole=True,
                    rim=(230, 245, 255), light_a=(214, 238, 255), light_b=(255, 255, 255)),
    # ── Terrain (index 0 is the sheet's structural hole; the water planes show through it) ─────
    # The sea sits DEEP and dark — the whole cast pops off it; crests/foam carry the light.
    "water_a": _family((4, 14, 26), (72, 150, 168), hole=True,
                       shadow=(2, 8, 18), rim=(188, 232, 238),
                       light_a=(224, 248, 250), light_b=(255, 255, 255)),
    # The b phase is a GENTLE lift — light passing over the swell: the crests flare while the
    # deep base barely moves (a strong base pulse strobes the whole screen).
    "water_b": _family((5, 16, 29), (82, 162, 178), hole=True,
                       shadow=(2, 9, 20), rim=(206, 242, 246),
                       light_a=(238, 252, 254), light_b=(255, 255, 255)),
    "shore": _family((58, 38, 20), (240, 218, 162), hole=True,
                     shadow=(24, 30, 40), light_a=(240, 250, 250), light_b=(255, 255, 255)),
    "lane": _family((16, 12, 28), (188, 180, 214), hole=True,
                    shadow=(8, 8, 22), light_a=(255, 236, 140), light_b=(255, 250, 240)),
    "median": _family((18, 22, 30), (198, 210, 224), hole=True,
                      light_a=(255, 214, 130), light_b=(255, 240, 190)),
    "sanctuary": _family((10, 34, 16), (150, 216, 118), hole=True,
                         shadow=(6, 24, 20), rim=(220, 255, 200),
                         light_a=(255, 150, 190), light_b=(250, 240, 130)),
    "beacon_a": _family((40, 28, 6), (252, 214, 104), hole=True,
                        rim=(255, 244, 190), light_a=(255, 214, 90), light_b=(255, 250, 210)),
    "beacon_b": _family((46, 32, 8), (255, 226, 128), hole=True,
                        rim=(255, 250, 210), light_a=(255, 240, 160), light_b=(255, 255, 255)),
    # ── Font (dual liveries per color: floating alpha-0 back vs the opaque HUD bar) ───────────
    "text_white": _font_pal(kClear, (244, 248, 255), (128, 140, 160)),
    "text_gold": _font_pal(kClear, (248, 220, 136), (132, 96, 30)),
    "text_cyan": _font_pal(kClear, (168, 236, 244), (40, 116, 132)),
    "hud_white": _font_pal(kBar, (244, 248, 255), (128, 140, 160)),
    "hud_gold": _font_pal(kBar, (248, 220, 136), (132, 96, 30)),
    "hud_cyan": _font_pal(kBar, (168, 236, 244), (40, 116, 132)),
    # The TITLE set's palette: 1 outline, 2 the deep cast shadow, 4..7 the submerged teal ramp,
    # 8..11 the dry gold ramp, 13 rim, 14 foam, 15 the hot glint. Entry 0 is alpha-0 — the word
    # floats over the open sea.
    "title": [(0, 0, 0, 0), (10, 14, 26, 255), (5, 9, 18, 255), (0, 0, 0, 255),
              (18, 72, 86, 255), (28, 96, 110, 255), (44, 124, 138, 255), (66, 154, 166, 255),
              (152, 98, 26, 255), (192, 134, 40, 255), (226, 172, 62, 255), (250, 214, 110, 255),
              (0, 0, 0, 255), (255, 240, 190, 255), (222, 250, 252, 255), (255, 255, 255, 255)],
}
# The blink phases: identical families with the light pair exchanged.
PALETTES["dart_b"] = _swap_lights(PALETTES["dart_a"])
PALETTES["sweeper_b"] = _swap_lights(PALETTES["sweeper_a"])
PALETTES["hauler_b"] = _swap_lights(PALETTES["hauler_a"])


def build_palettes() -> None:
    PAL_DIR.mkdir(exist_ok=True)
    for name, entries in sorted(PALETTES.items()):
        (PAL_DIR / f"{name}.png").write_bytes(_png_rgba8(16, 1, [entries]))


# ==================================================================================================
# Sprites (uniform 48x48 cells; every sprite >= 16px on a side and >= 12 distinct indices).
# ==================================================================================================


def _ferry(frame: int, w: int = 32, h: int = 22) -> list[list[int]]:
    """The ferryman's boat, facing RIGHT and reading as a VESSEL: a pointed bow with a white
    bow-wave, a bright gunwale over a planked brass hull, an aft cabin whose window genuinely
    glows, a dark waterline, and a churned stern wake whose foam alternates between the frames."""
    art = _blank(w, h)
    # The hull side: deck line at y=8 down to the waterline at y=17; the bow tapers to a point
    # at the right, the stern rounds off at the left.
    for y in range(8, 18):
        x0 = 2 + max(0, (y - 13))                 # the stern's rounding, low rows tuck in
        for x in range(x0, w - 1):
            if x > w - 9 and abs(y - 11.5) > (w - 2 - x) * 0.75:
                continue                          # the bow comes to a point around y=11..12
            u = 0.12 + (y - 8) / 9.0 * 0.78       # lit gunwale → dark bilge
            art[y][x] = _band(u, x, y)
    for x in range(3, w - 7):                     # the gunwale: one bright brass rail line
        if art[8][x]:
            art[8][x] = R[9]
    for x in range(5, w - 10, 6):                 # plank seams down the hull side
        for y in range(9, 16):
            if art[y][x]:
                art[y][x] = R[2]
    for x in range(2, w - 4):                     # the waterline: dark wet strake at the bottom
        if art[16][x]:
            art[16][x] = R[1]
        if art[17][x]:
            art[17][x] = SHADOW
    for y in range(5, 8):                         # the deck planks above the gunwale
        for x in range(4, 22):
            art[y][x] = R[7] if (x % 5) else R[5]
    for y in range(1, 8):                         # the aft cabin (left)
        for x in range(5, 13):
            art[y][x] = _dither(x, y, R[5], R[6])
    for x in range(5, 13):
        art[1][x] = R[9]                          # roof lip
    for y in range(3, 6):                         # the window GLOWS (the palette's light pair)
        for x in range(7, 11):
            art[y][x] = LIGHT_B if (y == 4 and x in (8, 9)) else LIGHT_A
    art[0][6] = R[8]                              # a stub chimney
    art[0][7] = R[3]
    for y in range(5, 8):                         # a tiller post at the stern deck edge
        art[y][3] = R[3]
    # The boat's MOTION lives in the separate wake effect now (see `_wake`), never baked into the
    # hull — so no bow-wave or stern churn here. A faint per-frame cabin-glow flicker keeps the two
    # idle frames distinct and the vessel alive at rest.
    art[4][8] = LIGHT_B if frame == 0 else LIGHT_A
    art[4][9] = LIGHT_A if frame == 0 else LIGHT_B
    _outline(art)
    for x in range(6, 20, 4):                     # rail glints, placed AFTER the outline pass
        if art[8][x]:
            art[8][x] = RIM
    _assert_rich(art, f"ferry {frame}")
    return art


def _ferry_ns(south: bool, w: int = 18, h: int = 28) -> list[list[int]]:
    """The boat seen END-ON and NARROW, for sailing north/south (the wide side view can't thread a
    one-block channel). Bow and stern are NOT interchangeable, so the two headings are DISTINCT
    art: `south` (moving toward the viewer — you see the BOW: a pointed cutwater at the near/bottom
    edge, the lit windshield of the cabin above it) vs north (moving away — you see the STERN: a
    flat transom + motor at the near/bottom edge, the cabin's dark back above it). No wake is baked
    in; the separate wake effect trails it."""
    art = _blank(w, h)
    cx = w // 2
    bow_y, stern_y = (h - 2, 1) if south else (1, h - 2)
    length = abs(stern_y - bow_y)
    ystep = 1 if stern_y > bow_y else -1

    def hw_at(t: float) -> int:  # half-beam along the hull: 0 at the bow, ~4 transom at the stern
        if t < 0.16:
            v = t / 0.16 * 4.5
        elif t < 0.60:
            v = 4.5 + (t - 0.16) / 0.44 * 2.5
        else:
            v = 7.0 - (t - 0.60) / 0.40 * 3.0
        return int(round(v))

    def y_at(t: float) -> int:
        return bow_y + ystep * int(round(t * length))

    # The hull, shaded across its beam (bright centreline, dark toward the sheer) with a rail line.
    for i in range(length + 1):
        t = i / length
        y = bow_y + ystep * i
        hw = hw_at(t)
        for x in range(cx - hw, cx + hw + 1):
            if 0 <= x < w:
                centre = 1.0 - abs(x - cx) / max(1.0, hw)
                art[y][x] = _band(0.16 + centre * 0.74, x, y)   # wide ramp: dark sheer → lit center
        if hw >= 3 and 0.12 < t < 0.92:
            art[y][cx - hw] = R[8]                              # bright gunwale rails…
            art[y][cx + hw] = R[8]
        if hw >= 4:
            art[y][cx - hw + 1] = SHADOW                        # …over a shaded waterline tuck
            art[y][cx + hw - 1] = SHADOW
        if 0.2 < t < 0.9 and y % 3 == 0:
            art[y][cx] = R[7]                                   # deck-plank grain down the center
    # The cabin sits toward the stern; its near face reads differently per heading.
    ylo, yhi = sorted((y_at(0.5), y_at(0.8)))
    for y in range(ylo, yhi + 1):
        for x in range(cx - 3, cx + 4):
            art[y][x] = _dither(x, y, R[5], R[6])
    for x in range(cx - 3, cx + 4):
        art[ylo][x] = R[9]                          # the roof lip (far edge of the cabin)
    if south:
        for x in range(cx - 2, cx + 3):             # the windshield GLOWS toward you (the bow view)
            art[yhi][x] = LIGHT_A
        art[yhi][cx] = LIGHT_B
        art[bow_y][cx] = RIM                         # the cutwater tip at the near point
        art[bow_y - ystep][cx] = R[9]
    else:
        for x in range(cx - 2, cx + 3):             # the dark transom back faces you (the stern view)
            art[yhi][x] = R[3]
        art[stern_y][cx - 1] = R[8]                  # the transom + a central motor housing
        art[stern_y][cx] = R[9]
        art[stern_y][cx + 1] = R[8]
        art[yhi - ystep][cx] = LIGHT_B               # a white stern lamp glowing over the cabin
        art[yhi - ystep][cx - 1] = LIGHT_A
        art[yhi - ystep][cx + 1] = LIGHT_A
    _outline(art)
    _assert_rich(art, "ferry S" if south else "ferry N")
    return art


def _wake(frame: int, w: int = 16, h: int = 12) -> list[list[int]]:
    """The boat's WAKE — its OWN sprite + palette, trailed BEHIND the moving hull by the renderer,
    never baked into the boat art. A rounded churn of foam: bright white core feathering to a blue
    rim through the wake palette's ramp, with a few crest caps that breathe between the two frames
    so a moving wake shimmers."""
    art = _blank(w, h)
    cx, cy = (w - 1) / 2.0, (h - 1) / 2.0
    rng = Lcg(frame * 2661 + 7)
    for y in range(h):
        for x in range(w):
            dx, dy = (x - cx) / (w / 2.0), (y - cy) / (h / 2.0)
            d = (dx * dx + dy * dy) ** 0.5 + rng.frac() * 0.22
            if d <= 1.0:
                art[y][x] = _band(d, x, y)          # d=0 core → R[9] (white foam), d=1 rim → R[0]
    caps = [(int(cx), int(cy)), (int(cx) - 3, int(cy) - 1), (int(cx) + 3, int(cy)),
            (int(cx) - 1, int(cy) + 2), (int(cx) + 2, int(cy) - 2), (int(cx) - 2, int(cy) - 2)]
    hi = [LIGHT_B, LIGHT_A, RIM, R[9]]              # crest caps: bright foam highlights
    for i, (x, y) in enumerate(caps):
        if 0 <= x < w and 0 <= y < h and art[y][x]:
            art[y][x] = hi[(i + frame) % len(hi)]
    g = Lcg(frame * 911 + 3)                        # dark water-gaps punched through — foam isn't solid
    for _ in range(6):
        x, y = int(g.frac() * w), int(g.frac() * h)
        if 0 <= x < w and 0 <= y < h and art[y][x] and art[y][x] not in (LIGHT_A, LIGHT_B):
            art[y][x] = SHADOW
    _assert_rich(art, f"wake {frame}")
    return art


def _colonist(look: int, frame: int, d: int = 16) -> list[list[int]]:
    """A crossing soul at 16x16: a shaded robe, arms that swing with the two-frame bob, a lit
    head, and a per-look tell -- a hood, a backpack, a cap. Deep enough shading to hold the
    richness lock at this size."""
    art = _blank(d, d)
    bob = 1 if frame == 1 else 0
    for y in range(6 + bob, 15):             # robe: a tapering column, top-lit + side-shaded —
        halfw = 2.4 + (y - 6 - bob) * 0.42   # kept BRIGHT so a soul pops off the dark sea
        for x in range(d):
            if abs(x - 7.5) <= halfw:
                u = (y - 6 - bob) / 9.0 * 0.55 + (abs(x - 7.5) / halfw) * 0.22
                art[y][x] = _band(u, x, y)
    for y in range(9 + bob, 14):             # dark folds down the shaded side (the deep ramp steps)
        art[y][10] = R[1]
        if y % 2:
            art[y][9] = R[0]
    for x in range(6, 10):                   # the hem's cast shadow (interior — survives the outline)
        art[13][x] = SHADOW
    for x in range(5, 11):                   # a belt line
        if art[10 + bob][x]:
            art[10 + bob][x] = R[2]
    art[10 + bob][7] = LIGHT_A               # the belt buckle
    art[11 + bob][5] = LIGHT_B               # a canteen glint on the hip
    art[8 + bob][3] = R[5]                   # arms, swinging with the bob
    art[9 + bob][2] = R[4]
    art[9 - bob][12] = R[5]
    art[10 - bob][13] = R[4]
    for y in range(2 + bob, 7 + bob):        # head
        for x in range(5, 11):
            if (x in (5, 10)) and y in (2 + bob, 6 + bob):
                continue
            art[y][x] = R[8] if y <= 3 + bob else R[7]
    art[3 + bob][6] = R[9]                   # brow light
    if look == 0:      # hood: the head wrapped darker, a lit face slot
        for y in range(2 + bob, 7 + bob):
            for x in range(4, 12):
                if art[y][x] or x in (4, 11):
                    art[y][x] = _dither(x, y, R[4], R[5])
        for x in range(6, 10):
            art[4 + bob][x] = R[9]
        art[4 + bob][7] = LIGHT_B
    elif look == 1:    # pack: a strapped slab on the back
        for y in range(7 + bob, 12 + bob):
            for x in range(1, 5):
                art[y][x] = _dither(x, y, R[3], R[4])
        art[7 + bob][2] = R[6]
        for y in range(8 + bob, 11 + bob):
            art[y][3] = R[2]
        art[8 + bob][1] = RIM
    else:              # cap: a brim over the brow + a button
        for x in range(4, 12):
            art[2 + bob][x] = R[6]
        for x in range(5, 13):
            art[3 + bob][x] = R[3]
        art[1 + bob][7] = LIGHT_A
        art[1 + bob][8] = R[6]
    _outline(art, light_rim=False)
    art[2 + bob][6] = RIM                    # a rim-lit crown, placed after the outline pass
    _assert_rich(art, f"colonist look {look} frame {frame}")
    return art


def _dart(w: int = 28, h: int = 14) -> list[list[int]]:
    """The fast lane's wedge racer: needle nose right, a bright canopy, twin tail fins, an intake
    scoop, running lights the palette phases blink."""
    art = _blank(w, h)
    for y in range(2, 12):
        t = abs(y - 6.5) / 5.0
        x1 = w - 2 - int(t * 9.0)
        for x in range(1, x1):
            u = 0.12 + (y - 2) / 10.0 * 0.72
            art[y][x] = _band(u, x, y)
    for x in range(4, 14):                   # dorsal ridge highlight
        if art[4][x]:
            art[4][x] = R[9]
    for y in range(5, 9):                    # canopy glass
        for x in range(14, 20):
            art[y][x] = LIGHT_A if y == 5 else (R[9] if y == 6 else R[8])
    for y in (1, 12):                        # tail fins
        for x in range(1, 6):
            art[y][x] = _dither(x, y, R[5], R[6])
    for y in range(9, 11):                   # the intake scoop
        for x in range(6, 11):
            art[y][x] = SHADOW if y == 10 else R[2]
    art[6][1] = LIGHT_B                      # tail running light
    art[7][1] = LIGHT_B
    art[3][w - 7] = LIGHT_A                  # nose marker light
    art[10][w - 8] = LIGHT_A                 # belly marker
    _outline(art)
    _assert_rich(art, "dart")
    return art


def _sweeper(w: int = 36, h: int = 18) -> list[list[int]]:
    """The mid lane's rounded transit van: a long body, a segmented window band, wheel pods,
    a roof beacon, skirt shadow."""
    art = _blank(w, h)
    for y in range(3, 15):
        for x in range(1, w - 1):
            if (x < 3 or x > w - 4) and (y < 5 or y > 12):
                continue                     # rounded corners
            u = 0.08 + (y - 3) / 11.0 * 0.78
            art[y][x] = _band(u, x, y)
    for x in range(4, w - 5):                # the window band, segmented by pillars
        seg = (x // 6) % 2 == 0
        art[5][x] = R[9] if seg else R[8]
        art[6][x] = LIGHT_A if (seg and x % 6 == 2) else (R[7] if seg else R[6])
        art[7][x] = R[6] if seg else R[5]
    for x in range(4, w - 5, 6):             # window pillars
        for y in range(5, 8):
            art[y][x] = R[3]
    for x in range(3, w - 3):                # skirt shadow
        art[14][x] = SHADOW
    for cx in (7, w - 10):                   # wheel pods
        for x in range(cx, cx + 5):
            art[15][x] = R[1]
            art[16][x] = R[2]
        art[15][cx + 2] = R[4]
    art[2][w // 2] = LIGHT_A                 # roof beacon
    art[2][w // 2 + 1] = LIGHT_B
    art[9][w - 3] = LIGHT_A                  # nose light
    art[9][1] = LIGHT_B                      # tail light
    _outline(art)
    _assert_rich(art, "sweeper")
    return art


def _hauler(w: int = 48, h: int = 18) -> list[list[int]]:
    """The slow lane's road-train: a cab at the nose (right) hauling a ribbed container with a
    tarp strap, roofline marker lights, four wheel pods."""
    art = _blank(w, h)
    for y in range(3, 15):                   # the container
        for x in range(1, 37):
            u = 0.14 + (y - 3) / 11.0 * 0.72
            art[y][x] = _band(u, x, y)
    for x in range(1, 37):                   # container ribs + a tarp strap
        if x % 6 == 0:
            for y in range(3, 15):
                if art[y][x]:
                    art[y][x] = R[3]
    for y in range(3, 15):
        if art[y][18]:
            art[y][18] = SHADOW              # the strap
    for x in range(2, 36):                   # container top lip
        if art[3][x]:
            art[3][x] = R[9]
    for y in range(4, 15):                   # the cab
        for x in range(38, w - 1):
            if y < 7 and x > w - 5:
                continue                     # raked windshield corner
            u = 0.08 + (y - 4) / 10.0 * 0.62
            art[y][x] = _band(u, x, y)
    for y in range(6, 9):                    # windshield
        for x in range(42, w - 2):
            art[y][x] = LIGHT_A if y == 6 else (R[9] if y == 7 else R[8])
    for x in range(2, 36, 8):                # roofline marker lights (the palette blinks them)
        art[2][x] = LIGHT_A if (x // 8) % 2 == 0 else LIGHT_B
    for x in range(1, w - 1):                # skirt shadow
        if art[14][x]:
            art[14][x] = SHADOW
    for cx in (5, 16, 27, 41):               # wheel pods
        for x in range(cx, cx + 5):
            art[15][x] = R[1]
            art[16][x] = R[2]
        art[15][cx + 2] = R[4]
    art[9][w - 2] = LIGHT_A                  # nose light
    _outline(art)
    _assert_rich(art, "hauler")
    return art


def _abductor(frame: int, w: int = 32, h: int = 22) -> list[list[int]]:
    """The colonist-snatcher's saucer: a glass dome over a laterally-shaded bone-white hull disc,
    a rim of violet running lights that ALTERNATE between the frames, and the beam emitter."""
    art = _blank(w, h)
    cx = (w - 1) / 2.0
    for y in range(h):                       # the hull: a wide, FLAT lens — menace, not a cloud
        for x in range(w):
            if ((x - cx) / 15.2) ** 2 + ((y - 14.0) / 4.2) ** 2 <= 1.0:
                art[y][x] = _band(0.1 + (x / (w - 1.0)) * 0.75 + (y - 10.0) * 0.03, x, y)
    for x in range(4, w - 4):                # a hard specular line along the hull's top edge
        if art[11][x]:
            art[11][x] = R[9]
    for y in range(h):                       # the dome: small, tight glass over the hull center
        for x in range(w):
            if ((x - cx) / 5.0) ** 2 + ((y - 9.0) / 3.6) ** 2 <= 1.0:
                art[y][x] = (LIGHT_B if y <= 6 else (R[9] if y <= 8 else R[7]))
    art[7][int(cx) - 2] = RIM                # a glass glint off-crown
    for y in range(16, 19):                  # the under-keel, in shadow
        for x in range(w):
            if art[y][x] and art[y][x] != OUT:
                art[y][x] = SHADOW if y >= 17 else R[1]
    _outline(art)
    for i, x in enumerate((4, 10, 15, 20, 26)):  # the running lights, alternating per frame
        if x + 1 < w:
            art[14][x] = LIGHT_B if (i % 2 == frame % 2) else LIGHT_A
            art[14][x + 1] = art[14][x]
    art[16][int(cx)] = LIGHT_A               # the beam emitter, keel-center
    art[16][int(cx) + 1] = LIGHT_A
    _assert_rich(art, f"abductor {frame}")
    return art


def _mutant(frame: int, d: int = 18) -> list[list[int]]:
    """What comes back when a colonist is carried off: a pulsing spined blob, radially shaded,
    with hot eyes and a bright pulsing core. Frame B swells a step larger."""
    art = _blank(d, d)
    c = (d - 1) / 2.0
    base = 5.6 + (1.0 if frame == 1 else 0.0)
    for y in range(d):
        for x in range(d):
            dx, dy = x - c, y - c
            ang = math.atan2(dy, dx)
            r = base + math.sin(ang * 5.0) * 1.4
            dd = math.hypot(dx, dy)
            if dd <= r:
                art[y][x] = _band(0.1 + (dd / r) * 0.75, x, y)
    for y in range(d):                       # the ramp's extremes as explicit features (the
        for x in range(d):                   # outline pass would eat gradient tails at the edge)
            dx, dy = x - c, y - c
            ang = math.atan2(dy, dx)
            r = base + math.sin(ang * 5.0) * 1.4
            dd = math.hypot(dx, dy)
            if not art[y][x]:
                continue
            if dd <= 2.4:                    # the inner glow ring around the core
                art[y][x] = _dither(x, y, R[8], R[9])
            elif dd / r >= 0.62 and dy > 0.5:  # the dark under-crescent of the hide
                art[y][x] = _dither(x, y, R[0], R[1])
    for y in range(d):                       # a sick vein pass
        for x in range(d):
            if art[y][x] and (x * 3 + y * 5) % 11 == 0:
                art[y][x] = SHADOW
    _outline(art, light_rim=False)
    art[int(c) - 4][int(c)] = RIM            # wet-lit spine tips
    art[int(c)][int(c) - 4] = RIM
    core = LIGHT_A if frame == 0 else RIM
    art[int(c)][int(c)] = core               # the pulsing core
    art[int(c)][int(c) + 1] = core
    eye = LIGHT_B
    art[int(c) - 2][int(c) - 2] = eye
    art[int(c) - 2][int(c) + 2] = eye
    _assert_rich(art, f"mutant {frame}")
    return art


def _boom(stage: int, d: int = 32) -> list[list[int]]:
    """Three frames of an expanding blast at 32x32: a hot core with a corona, a dithered double
    ring with sparks, then thin breaking fragments and embers -- the fire palette's whole ramp."""
    art = _blank(d, d)
    c = (d - 1) / 2.0
    rng = Lcg(97 + stage)
    if stage == 0:
        for y in range(d):
            for x in range(d):
                dd = math.hypot(x - c, y - c)
                if dd <= 3.5:
                    art[y][x] = LIGHT_B
                elif dd <= 6.0:
                    art[y][x] = _dither(x, y, LIGHT_A, RIM)
                elif dd <= 8.0:
                    art[y][x] = _dither(x, y, R[8], R[9])
                elif dd <= 9.2:
                    art[y][x] = _dither(x, y, R[6], R[7])
                elif dd <= 10.4:
                    art[y][x] = _dither(x, y, R[4], R[5])
                elif dd <= 11.4:                      # the smoke shell
                    art[y][x] = _dither(x, y, R[2], R[3])
                elif dd <= 12.4 and (x + y) % 3 == 0:  # thinning wisps
                    art[y][x] = R[1]
        for _ in range(10):                            # flying sparks
            a = rng.frac() * 2.0 * math.pi
            r = 11.0 + rng.frac() * 3.0
            art[int(c + math.sin(a) * r)][int(c + math.cos(a) * r)] = R[9]
        for _ in range(5):                             # soot flecks in the smoke
            a = rng.frac() * 2.0 * math.pi
            r = 10.0 + rng.frac() * 1.5
            art[int(c + math.sin(a) * r)][int(c + math.cos(a) * r)] = \
                OUT if rng.frac() < 0.5 else SHADOW
    elif stage == 1:
        for y in range(d):
            for x in range(d):
                dd = math.hypot(x - c, y - c)
                if 8.0 <= dd <= 10.5:
                    art[y][x] = _dither(x, y, R[7], R[9])
                elif 10.5 < dd <= 12.0:
                    art[y][x] = _dither(x, y, R[8], LIGHT_A)
                elif 12.0 < dd <= 13.5:
                    art[y][x] = _dither(x, y, R[4], R[5])
                elif 13.5 < dd <= 14.6:                # the trailing smoke shell
                    art[y][x] = _dither(x, y, R[1], R[2])
                elif dd < 4.0:                         # the core still burns as the ring departs
                    art[y][x] = _dither(x, y, R[7], R[9])
                elif 4.0 <= dd < 6.0 and (x + y) % 3 == 0:
                    art[y][x] = R[3]                   # falling embers inside the ring
        for _ in range(12):
            a = rng.frac() * 2.0 * math.pi
            art[int(c + math.sin(a) * 14.0)][int(c + math.cos(a) * 14.0)] = LIGHT_B
        for _ in range(4):                             # soot in the shell
            a = rng.frac() * 2.0 * math.pi
            art[int(c + math.sin(a) * 13.8)][int(c + math.cos(a) * 13.8)] = OUT
        art[int(c)][int(c)] = LIGHT_A
        art[int(c) + 2][int(c) - 2] = RIM
    else:
        for y in range(d):
            for x in range(d):
                dd = math.hypot(x - c, y - c)
                ang = math.atan2(y - c, x - c)
                if 12.5 <= dd <= 14.5 and int((ang + math.pi) * 8 / math.pi) % 3 != 0:
                    art[y][x] = _dither(x, y, R[3], R[5])   # the breaking ring, gapped
                elif 14.5 < dd <= 15.4 and (x + y) % 3 == 0:
                    art[y][x] = _dither(x, y, R[0], R[1])   # the last smoke haze
        for i in range(10):                  # drifting embers: bright heads, sooty tails,
            a = 0.3 + i * 0.63               # deterministic angles so every hue lands
            r = 6.0 + (i % 4) * 2.4
            ex, ey = int(c + math.cos(a) * r), int(c + math.sin(a) * r)
            art[ey][ex] = (LIGHT_A, R[9], R[8], LIGHT_B, RIM)[i % 5]
            if 0 <= ex - 1 and 0 <= ey + 1 < d:
                art[ey + 1][ex - 1] = (R[2], OUT, SHADOW)[i % 3]
        art[int(c)][int(c)] = SHADOW
    _assert_rich(art, f"boom {stage}", minimum=8 if stage == 2 else MIN_DISTINCT)
    return art


def _bolt(d: int = 12) -> list[list[int]]:
    """An energy bolt (enemy fire and the cargo's return fire — same art, two palette liveries):
    a hot core under concentric corona rings walking the whole ramp, with a sparking tail."""
    art = _blank(d, d)
    c = (d - 1) / 2.0
    for y in range(d):
        for x in range(d):
            dd = math.hypot(x - c, y - c)
            if dd <= 1.2:
                art[y][x] = LIGHT_B
            elif dd <= 2.0:
                art[y][x] = LIGHT_A
            elif dd <= 2.7:
                art[y][x] = RIM
            elif dd <= 3.4:
                art[y][x] = R[9]
            elif dd <= 4.1:
                art[y][x] = _dither(x, y, R[7], R[8])
            elif dd <= 4.8:
                art[y][x] = _dither(x, y, R[5], R[6])
            elif dd <= 5.4:
                art[y][x] = _dither(x, y, R[3], R[4])
    # The sparking tail, trailing down-left (bolts render point-symmetric enough either way).
    art[10][1] = R[1]
    art[11][0] = R[0]
    art[9][2] = R[2]
    art[11][2] = SHADOW
    art[10][3] = OUT
    _assert_rich(art, "bolt")
    return art


SPRITE_CELL = 48
SPRITE_BUILDERS = [
    ("ferry A",    lambda: _ferry(0)),
    ("ferry B",    lambda: _ferry(1)),
    ("hood A",     lambda: _colonist(0, 0)),
    ("hood B",     lambda: _colonist(0, 1)),
    ("pack A",     lambda: _colonist(1, 0)),
    ("pack B",     lambda: _colonist(1, 1)),
    ("cap A",      lambda: _colonist(2, 0)),
    ("cap B",      lambda: _colonist(2, 1)),
    ("dart",       _dart),
    ("sweeper",    _sweeper),
    ("hauler",     _hauler),
    ("abductor A", lambda: _abductor(0)),
    ("abductor B", lambda: _abductor(1)),
    ("mutant A",   lambda: _mutant(0)),
    ("mutant B",   lambda: _mutant(1)),
    ("boom 0",     lambda: _boom(0)),
    ("boom 1",     lambda: _boom(1)),
    ("boom 2",     lambda: _boom(2)),
    ("bolt",       _bolt),
    ("ferry N",    lambda: _ferry_ns(False)),   # stern/back view — sailing north (narrow)
    ("ferry S",    lambda: _ferry_ns(True)),    # bow/front view — sailing south (narrow)
    ("wake 0",     lambda: _wake(0)),           # the boat's trailing foam (its own effect sprite)
    ("wake 1",     lambda: _wake(1)),
]


SHEET_COLS = 8  # every sheet is a standard GRID, 8 cells wide, wrapping onto new rows


def _grid_png(cell: int, builders) -> bytes:
    """Lay builders out as a standard 8-wide grid sheet (read order: left-right, then down)."""
    rows_n = (len(builders) + SHEET_COLS - 1) // SHEET_COLS
    plane = _blank(cell * SHEET_COLS, cell * rows_n)
    for i, (_, build) in enumerate(builders):
        _stamp(plane, (i % SHEET_COLS) * cell, (i // SHEET_COLS) * cell, build())
    return _png_indexed8(cell * SHEET_COLS, cell * rows_n, plane)


def build_sprites() -> bytes:
    return _grid_png(SPRITE_CELL, SPRITE_BUILDERS)


# ==================================================================================================
# Terrain tiles -- 32x32 each (NO 8x8 art; a tile covers one whole grid cell and is stamped as a
# 4x4 group of the engine's 8px cells). Solid tiles paint every pixel 1..15 and hold the >= 12
# distinct-index lock; the hole tile and the sparse sparkle overlays are the deliberate exceptions.
# ==================================================================================================

TERRAIN_TILE = 32


WATER_FIELD_PX = 128  # the sea is authored as ONE 4×4-tile seamless field per animation frame

_water_field_cache: dict[int, list[list[int]]] = {}


def _water_field(phase: int) -> list[list[int]]:
    """The WHOLE sea design, 128x128, rich AND seamless: two organic integer-wave swells shade
    the deep half of the ramp in broad curved bands (every term is periodic over the field, so
    the field wraps at its own edges); troughs dip into cool shadow, crest ridges lift bright;
    and every 32x32 cell carries its own guaranteed detail — two wave dashes, a drifting sparkle
    pair, a shadow dip, a crest dab, a micro-glint — so EVERY sliced tile holds the richness
    lock. The renderer places the 16 slices by grid position, so adjacent tiles are literal
    neighbors in this one design: full detail, no seam possible. Frames animate DETAILS only
    (the base is identical across all three): the bright crest rotates, dashes breathe a pixel,
    sparkles drift."""
    if phase in _water_field_cache:
        return _water_field_cache[phase]
    D = WATER_FIELD_PX
    tau = 2.0 * math.pi
    art = _blank(D, D)
    def swell(x: int, y: int) -> float:
        s1 = math.sin(tau * (2.0 * y + 9.0 * math.sin(tau * x / D)) / D)
        s2 = math.sin(tau * (3.0 * x - 2.0 * y + 12.0 * math.sin(tau * y / D)) / D)
        return 0.6 * s1 + 0.4 * s2
    # The base — static across all frames, LOW contrast, SOFT gradients: the swell spans only
    # the four deep steps, and transitions use ordered (Bayer) dithering at every level instead
    # of quantized bands, so the shapes read as water depth, never as topographic contours.
    bayer = ((0.03, 0.53, 0.16, 0.66),
             (0.78, 0.28, 0.91, 0.41),
             (0.22, 0.72, 0.09, 0.59),
             (0.97, 0.47, 0.84, 0.34))
    for y in range(D):
        for x in range(D):
            s = swell(x, y)
            b = (s + 1.0) * 1.6              # 0..3.2 — the deep four steps only
            lo = max(0, min(3, int(b)))
            hi = min(3, lo + 1)
            frac = b - int(b)
            art[y][x] = R[hi] if frac > bayer[y % 4][x % 4] else R[lo]
    for y in range(D):                       # troughs dip cool; crest ridges lift, sparsely
        for x in range(D):
            s = swell(x, y)
            if s < -0.88:
                art[y][x] = SHADOW if (x + y) % 3 == 0 else R[0]
            elif s > 0.9 and (x + y) % 2 == 0:
                art[y][x] = R[5]
    # Per-cell guaranteed detail, drawn with wrap-around (modulo) so a mark crossing the field's
    # edge continues on the far side — the field stays seamless even through its details.
    def putw(x: int, y: int, c: int) -> None:
        art[y % D][x % D] = c
    def dash(mx: int, my: int, c: int, ends: int) -> None:
        for k in range(3):
            putw(mx + 1 + k, my, c)
        putw(mx, my + 1, ends)
        putw(mx + 4, my + 1, ends)
    for cell in range(16):
        ty, tx = divmod(cell, 4)
        rng = Lcg(4200 + cell * 13)
        ox, oy = tx * 32, ty * 32
        j = lambda span: int(rng.frac() * span)  # noqa: E731 — deterministic per-cell jitter
        shimmer = (0, 1, 0)[phase]
        # Two wave dashes; WHICH cell's first dash burns bright rotates with the phase.
        mx1, my1 = ox + 3 + j(22) + shimmer, oy + 4 + j(10)
        mx2, my2 = ox + 3 + j(22), oy + 18 + j(10)
        if (cell + phase) % 3 == 0:
            dash(mx1, my1, R[8], R[5])
            putw(mx1 + 2, my1, RIM)
            putw(mx1 - 1, my1, LIGHT_A)      # foam breaking off the lit crest
        else:
            dash(mx1, my1, R[6], R[4])
        dash(mx2, my2, R[5] if cell % 2 else R[4], R[3])
        # The drifting sparkle pair (a hot point and its fading twin).
        sx, sy = ox + 6 + j(18) + phase * 2, oy + 10 + j(12)
        putw(sx, sy, LIGHT_B)
        putw(sx - 1, sy + 1, R[9])
        putw(sx - 2, sy + 1, LIGHT_A)
        # A static micro-glint, a shadow dip, and a crest dab — the per-tile richness floor.
        putw(ox + 26 - j(6), oy + 27, RIM)
        putw(ox + 27 - j(6), oy + 28, R[8])
        putw(ox + 9 + j(4), oy + 30, SHADOW)
        putw(ox + 20 + j(5), oy + 2, R[7])
    _water_field_cache[phase] = art
    return art


def _water_tile(cell: int, phase: int) -> list[list[int]]:
    """Slice cell (ty·4 + tx) of the seamless field's given frame."""
    f = _water_field(phase)
    ty, tx = divmod(cell, 4)
    art = [[f[ty * 32 + y][tx * 32 + x] for x in range(32)] for y in range(32)]
    _assert_rich(art, f"water cell {cell} p{phase}")
    return art


def _sparkle(kind: int, phase: int) -> list[list[int]]:
    """The swell plane's sparse overlay, 32x32, TWO FRAMES per kind: a couple of DRIFTING FOAM
    STREAKS -- short horizontal dashes, bright-cored with soft tips -- that slide and dim/brighten
    between the frames, so the plane reads as surface foam breathing over the deep water."""
    d = TERRAIN_TILE
    art = _blank(d, d)
    def streak(mx: int, my: int, ln: int, bright: bool) -> None:
        for k in range(ln):
            art[my][mx + k] = (LIGHT_A if bright else RIM) if 0 < k < ln - 1 else R[8]
        if bright:
            art[my][mx + ln // 2] = LIGHT_B
            art[my + 1][mx + 1] = R[7]       # a hint of its underside
    slide = phase * 3
    if kind == 0:
        streak(5 + slide, 9, 6, phase == 0)
        streak(19 - slide, 22, 5, phase == 1)
    else:
        streak(14 + slide, 5, 5, phase == 1)
        streak(4 + slide, 18, 7, phase == 0)
        art[28][24 - slide] = R[9]           # one fading fleck trailing the far streak
        art[28][25 - slide] = R[7]
    return art


def _shore(variant: int) -> list[list[int]]:
    """An islet's sand, 32x32, CAP-AWARE: islets stand free in open water, so each tile carries
    its own coastline — a dark wet-sand rim + a foam ring on every edge that meets the sea.
    variant 0 = a single free-standing islet (coast on all four edges); 1 = the LEFT cap of a
    two-tile islet (no coast on its right edge); 2 = the RIGHT cap (no coast on its left edge).
    The sand itself is calm: flat warm planes, a tide line, a few deliberate shells — no noise."""
    d = TERRAIN_TILE
    art = _blank(d, d)
    rng = Lcg(500 + variant * 31)
    coastLeft  = variant != 2
    coastRight = variant != 1
    for y in range(d):                       # the calm sand body: two close planes, top-lit
        for x in range(d):
            art[y][x] = R[6] if y < 12 else (R[5] if (x * 3 + y * 5) % 17 else R[6])
    ty = 13 + variant * 2                    # one dried tide line, gently broken
    for x in range(d):
        if x % 5 != 0:
            art[ty][x] = R[4]
    shells = ((7, 8), (21, 17), (12, 22))    # deliberate shells + pebbles, not speckle
    for k, (sx, sy) in enumerate(shells):
        art[sy][sx]     = RIM if k == 0 else R[8]
        art[sy][sx + 1] = R[9]
        art[sy + 1][sx] = R[2]
    art[9][25] = R[3]                        # two driftwood dabs
    art[10][26] = R[2]
    # The coastline: wet rim + foam, only on edges that face open water.
    def coast_px(x: int, y: int, depth: int) -> None:
        if depth == 0:
            art[y][x] = SHADOW               # the waterline itself
        elif depth == 1:
            art[y][x] = R[0]                 # wet dark sand
        else:
            art[y][x] = _dither(x, y, R[1], R[3])  # damp fade into the dry body
    for x in range(d):                       # top + bottom edges always face water
        for depth in range(3):
            coast_px(x, depth, depth)
            coast_px(x, d - 1 - depth, depth)
    for y in range(d):
        if coastLeft:
            for depth in range(3):
                coast_px(depth, y, depth)
        if coastRight:
            for depth in range(3):
                coast_px(d - 1 - depth, y, depth)
    # Foam dashes riding the waterline (LIGHT entries — white surf under any sand palette):
    for x in range(2, d - 2, 5):
        art[0][x] = LIGHT_A
        art[d - 1][(x + 2) % d] = LIGHT_A
        if x % 10 == 2:
            art[0][x + 1] = LIGHT_B
    for y in range(2, d - 2, 5):
        if coastLeft:
            art[y][0] = LIGHT_A
        if coastRight:
            art[y][d - 1] = LIGHT_A
    _assert_rich(art, f"shore {variant}")
    return art


def _buoy() -> list[list[int]]:
    """A moored buoy over the RIGHT-CAP shore base (props ride an islet's right block): a
    striped float with a lamp head and a ground shadow."""
    art = _shore(2)
    cx, cy = 15, 16
    for y in range(cy - 7, cy + 8):          # the float body
        halfw = 6.0 * math.sqrt(max(0.0, 1.0 - ((y - cy) / 8.0) ** 2))
        for x in range(32):
            if abs(x - cx) <= halfw:
                stripe = ((y - cy + 7) // 3) % 2 == 0
                u = 0.15 + (abs(x - cx) / 6.5) * 0.5 + (0.0 if stripe else 0.3)
                art[y][x] = _band(u, x, y)
    for x in range(cx - 2, cx + 3):          # the lamp head
        art[cy - 9][x] = R[3]
    art[cy - 10][cx] = LIGHT_A
    art[cy - 10][cx + 1] = LIGHT_B
    for x in range(cx - 6, cx + 7):          # ground shadow
        art[cy + 9][x] = SHADOW
    _assert_rich(art, "buoy")
    return art


def _mooring() -> list[list[int]]:
    """A mooring post with a coiled line, over the RIGHT-CAP shore base."""
    art = _shore(2)
    for y in range(6, 26):                   # the post, side-lit
        for x in range(13, 18):
            art[y][x] = _band(0.2 + (x - 13) / 4.0 * 0.6, x, y)
    for x in range(13, 18):                  # the lit cap
        art[5][x] = R[9]
        art[6][x] = RIM if x == 14 else art[6][x]
    for t in range(40):                      # the coiled line
        ang = t * 0.5
        r = 7.0 + t * 0.08
        x, y = int(15.5 + math.cos(ang) * r * 0.6), int(18 + math.sin(ang) * r * 0.25)
        if 0 <= x < 32 and 0 <= y < 32:
            art[y][x] = R[2] if t % 2 else R[6]
    for x in range(10, 22):                  # ground shadow
        art[26][x] = SHADOW
    _assert_rich(art, "mooring")
    return art


def _lane(worn: bool) -> list[list[int]]:
    """The causeway roadbed, 32x32: a bevelled deck slab (lit top edge, dark base) with plate
    seams, rivets, and -- on the worn variant -- patches and cracks."""
    d = TERRAIN_TILE
    art = _blank(d, d)
    rng = Lcg(900 if worn else 800)
    for y in range(d):
        for x in range(d):
            u = 0.42 + (y / (d - 1.0)) * 0.3 + (0.08 if worn else 0.0)
            if (x * 3 + y * 7) % 17 == 0:
                u += 0.07                    # plate mottle
            art[y][x] = _band(u, x, y)
    for x in range(d):                       # the slab bevel: lit top lip, dark base
        art[0][x] = R[9]
        art[1][x] = _dither(x, 1, R[7], R[8])
        art[d - 2][x] = R[2]
        art[d - 1][x] = SHADOW
    for sx in (10, 21):                      # vertical plate seams with dark gutters + rivets
        for y in range(2, d - 2):
            art[y][sx] = R[1]
            art[y][sx + 1] = R[0] if y % 2 else R[1]
        for y in (5, 15, 25):
            art[y][sx - 1] = RIM
            art[y][sx + 2] = R[3]
    for y in range(2, d - 2):                # a hairline expansion joint
        art[y][27] = OUT if y % 3 else R[0]
    for x in (5, 16, 26):                    # cats-eye reflector studs on the deck
        art[8][x] = LIGHT_A
        art[24][x] = LIGHT_B
    for _ in range(6):                       # surface grit
        x, y = int(rng.frac() * d), 3 + int(rng.frac() * (d - 6))
        art[y][x] = R[8] if rng.frac() < 0.5 else R[2]
    if worn:
        for (px, py) in ((6, 9), (17, 20), (26, 7)):  # patches
            for y in range(py, py + 4):
                for x in range(px, px + 5):
                    art[y][x] = _dither(x, y, R[2], R[3])
            art[py][px] = R[6]
        for t in range(9):                   # a crack
            x = 4 + t * 3
            y = 14 + int(math.sin(t * 1.3) * 3)
            art[y][x] = OUT
            art[y + 1][x + 1] = R[2]
    _assert_rich(art, "lane worn" if worn else "lane fresh")
    return art


def _lane_dash() -> list[list[int]]:
    """The lane's direction dash: the fresh roadbed with a bright, edge-shaded center stripe."""
    art = _lane(False)
    for x in range(4, 24):
        art[14][x] = LIGHT_B
        art[15][x] = LIGHT_A
        art[16][x] = R[9]
        art[17][x] = R[6]
    art[14][4] = RIM
    art[17][23] = R[4]
    return art


def _oil() -> list[list[int]]:
    """An iridescent oil stain on the worn roadbed."""
    art = _lane(True)
    for y in range(8, 26):
        for x in range(5, 27):
            dd = ((x - 16.0) / 10.0) ** 2 + ((y - 17.0) / 8.0) ** 2
            if dd <= 1.0:
                if dd > 0.75:
                    art[y][x] = R[2]
                elif (x + y) % 3 == 0:
                    art[y][x] = SHADOW      # the slick's cool sheen
                elif (x * 2 + y) % 7 == 0:
                    art[y][x] = R[5]        # iridescent flecks
                else:
                    art[y][x] = OUT
    return art


def _median(variant: int) -> list[list[int]]:
    """The mid-crossing rest platform, 32x32: bevelled stone slabs in a bond pattern, offset per
    variant, with worn high spots and moss flecks."""
    d = TERRAIN_TILE
    art = _blank(d, d)
    rng = Lcg(1100 + variant * 7)
    for y in range(d):
        for x in range(d):
            row = y // 8
            off = (row % 2) * 8 + variant * 4
            col = (x + off) // 16
            u = 0.28 + ((x + off) % 16) / 15.0 * 0.18 + (y % 8) / 7.0 * 0.22
            u += 0.05 * ((row + col) % 3)
            art[y][x] = _band(u, x, y)
    for y in range(0, d, 8):                 # slab joints: lit top lip, deep dark seam
        for x in range(d):
            art[y][x] = R[8] if y == 0 else _dither(x, y, R[0], R[1])
            if y + 7 < d:
                art[y + 7][x] = R[1] if x % 2 else R[2]
    art[15][2] = LIGHT_B                     # a wet glint in a seam
    art[23][29] = OUT                        # a chipped-out joint pocket
    art[24][29] = OUT
    for row in range(4):                     # vertical joints, offset per bond row
        off = (row % 2) * 8 + variant * 4
        for jx in range((16 - off % 16) % 16, d, 16):
            for y in range(row * 8, row * 8 + 8):
                art[y][jx] = R[3]
    for _ in range(8):                       # worn high spots + moss
        x, y = int(rng.frac() * d), int(rng.frac() * d)
        art[y][x] = R[9] if rng.frac() < 0.5 else SHADOW
    art[3][2] = RIM
    art[19][27] = RIM
    art[11][14] = LIGHT_A                    # a brass survey stud
    _assert_rich(art, f"median {variant}")
    return art


def _lamp() -> list[list[int]]:
    """A median lamp over the slab base: a shaded post, a glowing twin head, a light pool."""
    art = _median(0)
    for y in range(7, 27):                   # the post
        for x in range(14, 18):
            art[y][x] = _band(0.25 + (x - 14) / 3.0 * 0.55, x, y)
    for y in range(4, 7):                    # the head
        for x in range(11, 21):
            art[y][x] = R[3] if y == 6 else R[4]
    art[5][12] = LIGHT_A                     # the twin lamps
    art[5][13] = LIGHT_B
    art[5][18] = LIGHT_B
    art[5][19] = LIGHT_A
    for x in range(10, 22):                  # the light pool on the slabs
        if art[27][x] not in (OUT,):
            art[27][x] = _dither(x, 27, R[8], R[9])
    for x in range(12, 20):
        art[28][x] = R[8]
    art[7][14] = RIM                         # post cap glint
    _assert_rich(art, "lamp")
    return art


def _sanctuary() -> list[list[int]]:
    """The sanctuary's meadow, 32x32: CALM flat grass (two close greens, no stripes, no noise)
    with deliberate tuft clusters, a few dark blades, dew glints, and flower pixels (the palette's
    light entries). Edge rows stay uniform so the band tiles seamlessly."""
    d = TERRAIN_TILE
    art = _blank(d, d)
    for y in range(d):                       # the calm base: one tone, softly patched with a second
        for x in range(d):
            art[y][x] = R[5] if ((x // 5 + y // 6) % 3) else R[4]
    # Deliberate tuft clusters (bright crown, shadowed root), interior only:
    for (tx, ty) in ((5, 6), (14, 12), (24, 5), (9, 22), (20, 25), (27, 16)):
        art[ty][tx]     = R[8]
        art[ty][tx + 1] = R[7]
        art[ty - 1][tx] = R[9]
        art[ty + 1][tx] = R[2]
        art[ty + 1][tx + 1] = R[3]
    for (bx, by) in ((3, 17), (17, 3), (29, 27), (12, 28)):   # single dark blades
        art[by][bx] = R[1]
    art[7][19] = SHADOW                      # a patch of cool shade under the biggest tuft
    art[8][19] = SHADOW
    art[8][20] = R[2]
    # Flowers + dew — the palette's light entries as tiny deliberate accents:
    art[4][10] = LIGHT_A
    art[23][6] = LIGHT_B
    art[15][27] = LIGHT_A
    art[26][22] = RIM                        # a dew glint
    art[11][3] = RIM
    art[18][14] = R[6]                       # sun-warmed grass dabs
    art[27][10] = R[6]
    _assert_rich(art, "sanctuary")
    return art


def _beacon() -> list[list[int]]:
    """The sanctuary beacon on the pad: a radially-shaded gold heart with rays -- its glow cycles
    by palette (beacon_a / beacon_b)."""
    art = _sanctuary()
    cx = cy = 15.5
    for y in range(32):
        for x in range(32):
            dd = math.hypot(x - cx, y - cy)
            if dd <= 4.0:
                art[y][x] = LIGHT_B
            elif dd <= 6.5:
                art[y][x] = _dither(x, y, LIGHT_A, RIM)
            elif dd <= 9.0:
                art[y][x] = _dither(x, y, R[9], R[8])
            elif dd <= 10.5:
                art[y][x] = R[6]
    for k in range(8):                       # the rays
        ang = k * math.pi / 4.0
        for t in range(11, 15):
            x, y = int(cx + math.cos(ang) * t), int(cy + math.sin(ang) * t)
            if 0 <= x < 32 and 0 <= y < 32:
                art[y][x] = R[9] if t < 13 else R[7]
    _assert_rich(art, "beacon")
    return art


def _trim() -> list[list[int]]:
    """The sanctuary's SHORELINE tile (stamped under the meadow row, in the SAND palette): dry
    beach fading to wet sand, the dark waterline, and surf foam along the bottom edge — the
    natural grass → beach → sea transition. Horizontal edges uniform, so the band tiles clean."""
    d = TERRAIN_TILE
    art = _blank(d, d)
    for y in range(d):                       # dry beach, calm, faintly banded toward the water
        for x in range(d):
            art[y][x] = R[6] if y < 10 else (R[5] if y < 18 else _dither(x, y, R[4], R[5]))
    for x in range(0, d, 7):                 # a scatter of deliberate beach pebbles
        art[6 + (x % 3)][x] = R[8]
        art[13][(x + 3) % d] = R[3]
    art[8][9] = RIM                          # one shell glint
    art[8][10] = R[9]
    for y in range(22, 26):                  # wet sand
        for x in range(d):
            art[y][x] = _dither(x, y, R[1], R[2])
    for x in range(d):                       # the waterline: deep wet band + cool shadow
        art[26][x] = R[0]
        art[27][x] = SHADOW
        art[28][x] = SHADOW
    for y in range(29, d):                   # the surf: foam dashes over the shadow line
        for x in range(d):
            art[y][x] = SHADOW
    for x in range(d):
        if x % 4 != 3:
            art[29][x] = LIGHT_A
        if x % 6 == 1:
            art[30][x] = LIGHT_B
        if x % 5 == 2:
            art[31][x] = R[7]
    _assert_rich(art, "trim")
    return art


TERRAIN_BUILDERS = [
    ("blank/hole", lambda: _blank(TERRAIN_TILE, TERRAIN_TILE)),
    # The animated sea: the seamless 4×4 field sliced into 16 position-mapped cells × 3 frames
    # (slot = 1 + cell·3 + phase; the renderer places cell (blockY%4)·4 + (blockX%4))…
    *[(f"water c{c} p{p}", lambda c=c, p=p: _water_tile(c, p)) for c in range(16) for p in range(3)],
    # …and the foam overlay: 2 kinds × 2 frames (slot = 49 + kind·2 + phase).
    *[(f"sparkle {'AB'[k]}{p}", lambda k=k, p=p: _sparkle(k, p)) for k in range(2) for p in range(2)],
    ("shore A", lambda: _shore(0)),
    ("shore B", lambda: _shore(1)),
    ("shore C", lambda: _shore(2)),
    ("buoy", _buoy),
    ("mooring", _mooring),
    ("lane fresh", lambda: _lane(False)),
    ("lane worn", lambda: _lane(True)),
    ("lane dash", _lane_dash),
    ("oil", _oil),
    ("median A", lambda: _median(0)),
    ("median B", lambda: _median(1)),
    ("lamp", _lamp),
    ("sanctuary", _sanctuary),
    ("beacon", _beacon),
    ("trim", _trim),
]


def build_terrain() -> bytes:
    return _grid_png(TERRAIN_TILE, TERRAIN_BUILDERS)


# ==================================================================================================
# Rich 16x16 font (the established gradient-glyph generator, widened to an 8-step ramp).
# ==================================================================================================

GLYPH_W, GLYPH_H = 5, 7

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
    for (x, y) in filled:                    # 8-step gradient fill: 11 bright top -> 4 base
        band = min(7, max(0, (y - 1) // 2))
        art[y][x] = 11 - band
    for (x, y) in list(filled):              # 1px outline, 8-neighbor
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                nx, ny = x + dx, y + dy
                if 0 <= nx < RICH_CELL and 0 <= ny < RICH_CELL and art[ny][nx] == 0:
                    art[ny][nx] = 1
    for y in range(RICH_CELL):               # drop shadow: below-right of the outlined shape
        for x in range(RICH_CELL):
            if art[y][x] == 0 and x > 0 and y > 0 and art[y - 1][x - 1] not in (0, 2):
                art[y][x] = 2
    return art


def _rich_rule() -> list[list[int]]:
    """The HUD's full-width border rule as a 16x16 cell: a bevelled gradient bar mid-cell."""
    art = _blank(RICH_CELL, RICH_CELL)
    for x in range(RICH_CELL):
        for i, idx in enumerate((11, 10, 9, 8, 6, 4, 2, 1)):
            art[4 + i][x] = idx
    return art


def build_font() -> bytes:
    builders = [(ch, lambda ch=ch: _rich_glyph(_GLYPHS[ch])) for ch in FONT_ORDER]
    builders.append(("rule", _rich_rule))
    return _grid_png(RICH_CELL, builders)


# ==================================================================================================
# The TITLE tileset -- "FERRYMAN" as its own bespoke 32x32 glyph set (n×n, deliberately not 8),
# themed to the game: chunky arcade letterforms HALF-SUBMERGED in the sea. Above a flowing
# waterline the fill is a gold gradient; below it the letters read through sea-teal; foam breaks
# along the boundary with the odd bubble; the caps catch a rim light; a deep drop shadow anchors
# the word over the open water. One palette image (`title`) colors the whole set.
# ==================================================================================================

TITLE_CELL = 32
TITLE_WORD = "FERRYMAN"


def _title_glyph(ch: str, k: int) -> list[list[int]]:
    d = TITLE_CELL
    art = _blank(d, d)
    mask = _GLYPHS[ch]
    filled = set()
    for gy in range(GLYPH_H):                # the 5×7 core, quadrupled to a 20×28 slab at (5,1)
        for gx in range(GLYPH_W):
            if mask[gy][gx] == "#":
                for dy in range(4):
                    for dx in range(4):
                        filled.add((5 + gx * 4 + dx, 1 + gy * 4 + dy))

    def waterline(x: int) -> int:            # flows across the word (per-letter phase offset)
        return 18 + round(1.8 * math.sin(2.0 * math.pi * (x + k * 5) / 22.0))

    for (x, y) in filled:
        wy = waterline(x)
        if y < wy:                           # dry gold, bright cap → deep base
            t = (y - 1) / max(1, wy - 2)
            art[y][x] = (11, 10, 9, 8)[min(3, int(t * 4.0))]
        else:                                # submerged teal, surface-lit → deep keel
            t = (y - wy) / max(1, 28 - wy)
            art[y][x] = (7, 6, 5, 4)[min(3, int(t * 4.0))]
    for (x, y) in filled:                    # the foam line riding the waterline
        if y == waterline(x):
            art[y][x] = 14
        elif y == waterline(x) + 1 and (x + k) % 4 == 0:
            art[y][x] = 15                   # a bubble caught under the surface
    for (x, y) in list(filled):              # 1px outline, 8-neighbor
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                nx, ny = x + dx, y + dy
                if 0 <= nx < d and 0 <= ny < d and art[ny][nx] == 0:
                    art[ny][nx] = 1
    for y in range(d):                       # the deep drop shadow, cast two steps down-right
        for x in range(d):
            if art[y][x] == 0 and x >= 2 and y >= 2 and art[y - 2][x - 2] not in (0, 2):
                art[y][x] = 2
    for (x, y) in filled:                    # rim light along each stroke's lit top-left edge
        if (x - 1, y) not in filled and (x, y - 1) not in filled and y < waterline(x):
            art[y][x] = 13
    caps = [x for (x, y) in filled if y == 1]
    if caps:                                 # one hot glint on the cap
        art[1][sorted(caps)[len(caps) // 2]] = 15
    _assert_rich(art, f"title '{ch}' {k}")
    return art


def build_title() -> bytes:
    builders = [(ch, lambda ch=ch, k=k: _title_glyph(ch, k))
                for k, ch in enumerate(TITLE_WORD)]
    return _grid_png(TITLE_CELL, builders)


def main() -> None:
    build_palettes()
    print(f"palettes/: {len(PALETTES)} palette images, 16x1 RGBA "
          f"({', '.join(sorted(PALETTES))})")

    def dims(cell: int, n: int) -> str:
        rows_n = (n + SHEET_COLS - 1) // SHEET_COLS
        return f"{cell * SHEET_COLS}x{cell * rows_n} ({SHEET_COLS}-wide grid)"

    (HERE / "ferryman_sprites.png").write_bytes(build_sprites())
    print(f"ferryman_sprites.png: {dims(SPRITE_CELL, len(SPRITE_BUILDERS))}, "
          f"{len(SPRITE_BUILDERS)} cells ({', '.join(name for name, _ in SPRITE_BUILDERS)})")
    (HERE / "ferryman_terrain.png").write_bytes(build_terrain())
    print(f"ferryman_terrain.png: {dims(TERRAIN_TILE, len(TERRAIN_BUILDERS))}, "
          f"{len(TERRAIN_BUILDERS)} 32x32 tiles ({', '.join(name for name, _ in TERRAIN_BUILDERS)})")
    (HERE / "ferryman_font.png").write_bytes(build_font())
    print(f"ferryman_font.png: {dims(RICH_CELL, len(FONT_ORDER) + 1)}, "
          f"37 rich 16x16 glyphs (8-step gradient + outline + shadow) + rule")
    (HERE / "ferryman_title.png").write_bytes(build_title())
    print(f"ferryman_title.png: {dims(TITLE_CELL, len(TITLE_WORD))}, "
          f"{len(TITLE_WORD)} bespoke 32x32 title glyphs (gold over a foaming waterline)")


if __name__ == "__main__":
    main()
