#!/usr/bin/env python3
"""Generate the render-stats demo's art: indexed sheets + 16-entry palette images.

Every sheet is a standard 8-wide grid of equal cells. Every palette ships as a 16x1 RGBA image the
engine loads with loadPaletteImage. Shading uses ordered (Bayer) dithering between ramp steps so a
gradient reads smooth rather than banded, which is also what pushes each cell's distinct-index count
past the richness floor asserted at the bottom of this file.

Layout it writes:
    art/       indexed sheets, each a standard 8-wide grid (font.png is authored, not generated)
    palettes/  one 16x1 RGBA palette image per livery

Run from the repo root:  python3 examples/render_stats/assets/gen_render_stats_assets.py
Then re-CONFIGURE before building — embedded asset bytes are baked at configure time:
    cmake -S . -B build && cmake --build build
"""

import math
import os
import struct
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(HERE, 'art')            # indexed sheets the engine loads
PAL = os.path.join(HERE, 'palettes')       # 16-entry palette images
PREVIEW = os.path.join(tempfile.gettempdir(), 'render_stats_preview')  # eyeballing only, not shipped

# ── PNG writing ────────────────────────────────────────────────────────────────────────────────

def _chunk(tag, body):
    return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body) & 0xFFFFFFFF)


def write_indexed(path, w, h, pixels, palette):
    """An 8-bit palette-indexed PNG. `palette` is only for previewing the file itself — at runtime the
    engine colors these indices through a palette image."""
    plte = b''.join(bytes(c[:3]) for c in palette)
    plte += b'\x00\x00\x00' * (256 - len(palette))
    raw = b''.join(b'\x00' + bytes(pixels[y * w:(y + 1) * w]) for y in range(h))
    png = (b'\x89PNG\r\n\x1a\n'
           + _chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 3, 0, 0, 0))
           + _chunk(b'PLTE', plte)
           + _chunk(b'tRNS', bytes([0] + [255] * (len(palette) - 1)))
           + _chunk(b'IDAT', zlib.compress(raw, 9))
           + _chunk(b'IEND', b''))
    open(path, 'wb').write(png)


def write_rgba(path, w, h, rgba):
    raw = b''.join(b'\x00' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    png = (b'\x89PNG\r\n\x1a\n'
           + _chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
           + _chunk(b'IDAT', zlib.compress(raw, 9))
           + _chunk(b'IEND', b''))
    open(path, 'wb').write(png)


# ── Palette construction ───────────────────────────────────────────────────────────────────────

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def ramp(stops, n):
    """`n` colors interpolated through a list of (r,g,b) stops."""
    out = []
    for i in range(n):
        t = i / (n - 1) * (len(stops) - 1)
        a, b = int(math.floor(t)), min(int(math.floor(t)) + 1, len(stops) - 1)
        f = t - a
        out.append(tuple(round(stops[a][c] + (stops[b][c] - stops[a][c]) * f) for c in range(3)))
    return out


def palette_image(name, stops, hole=True, backing=None):
    """A 16-entry palette image. Entry 0 is an alpha-0 hole when `hole`, so art can carry structural
    transparency through the palette rather than a color key. Pass `backing` instead to make entry 0 an
    opaque color — the same art then draws its own background, which is how the readout gets a panel
    behind it without a second layer."""
    cols = ramp(stops, 15) if (hole or backing) else ramp(stops, 16)
    rgba = []
    if backing:
        rgba += [backing[0], backing[1], backing[2], 255]
    elif hole:
        rgba += [0, 0, 0, 0]
    for c in cols:
        rgba += [c[0], c[1], c[2], 255]
    write_rgba(os.path.join(PAL, name), 16, 1, rgba)
    return cols


def dither_index(v, lo, hi, x, y):
    """Map a 0..1 value onto indices [lo,hi] with 4x4 ordered dithering between neighboring steps."""
    span = hi - lo
    t = max(0.0, min(1.0, v)) * span
    base = int(math.floor(t))
    frac = t - base
    if frac * 16 > BAYER[y & 3][x & 3]:
        base += 1
    return lo + max(0, min(span, base))


# ── Sheets ─────────────────────────────────────────────────────────────────────────────────────

def sheet(cells, cell_px, cols=8):
    """Lay `cells` (each a cell_px*cell_px index list) into a grid at most `cols` wide, wrapping onto
    new rows. Eight is the wrap width, not a minimum — a sheet with fewer cells is only as wide as it
    needs to be rather than padded out to eight."""
    cols = min(cols, max(1, len(cells)))
    rows = (len(cells) + cols - 1) // cols
    w, h = cols * cell_px, rows * cell_px
    buf = [0] * (w * h)
    for i, cell in enumerate(cells):
        cx, cy = (i % cols) * cell_px, (i // cols) * cell_px
        for y in range(cell_px):
            for x in range(cell_px):
                buf[(cy + y) * w + cx + x] = cell[y * cell_px + x]
    return w, h, buf


def knob_cell(px, notch_deg):
    """A rotary knob: a dark rim, a domed body lit from above, and a pointer notch. The engine turns
    the sprite, so this is drawn at rest and the pointer a viewer sees is the value."""
    c = (px - 1) / 2.0
    cell = [0] * (px * px)
    for y in range(px):
        for x in range(px):
            dx, dy = x - c, y - c
            d = math.hypot(dx, dy)
            if d > px * 0.48:
                continue
            if d > px * 0.44:                                    # rim: a dark bevel
                cell[y * px + x] = dither_index(0.15 + 0.25 * (1 - (d - px * 0.44) / (px * 0.04)),
                                                1, 4, x, y)
                continue
            # A dome: brightest up-left, falling away with radius.
            lam = (-dx * 0.5 - dy * 0.85) / (px * 0.45)
            v = 0.52 + 0.42 * lam - 0.22 * (d / (px * 0.44)) ** 2
            cell[y * px + x] = dither_index(v, 3, 13, x, y)
            ang = math.degrees(math.atan2(dx, -dy)) - notch_deg
            ang = (ang + 180) % 360 - 180
            if abs(ang) < 7 and d > px * 0.10:                   # the pointer, cut to the rim
                cell[y * px + x] = dither_index(0.80 + 0.20 * (d / (px * 0.44)), 12, 15, x, y)
    return cell


def lamp_cell(px):
    """An indicator lamp: a bezel around a lens with a hot filament. Lit and unlit differ only by the
    palette bound to it."""
    c = (px - 1) / 2.0
    cell = [0] * (px * px)
    for y in range(px):
        for x in range(px):
            dx, dy = x - c, y - c
            d = math.hypot(dx, dy)
            if d > px * 0.48:
                continue
            if d > px * 0.36:                                    # bezel
                cell[y * px + x] = dither_index(0.55 - 0.5 * (d - px * 0.36) / (px * 0.12), 1, 5, x, y)
                continue
            v = 1.0 - (d / (px * 0.36)) ** 1.7                   # lens, hottest at the center
            v += 0.10 * (-dy / (px * 0.36))                      # a touch of top-lit glass
            cell[y * px + x] = dither_index(v, 4, 15, x, y)
    return cell


def _hash(ix, iy, seed):
    """A stable pseudo-random value in [0,1) for a lattice point."""
    h = (ix * 374761393 + iy * 668265263 + seed * 2147483647) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def _smooth(t):
    return t * t * (3.0 - 2.0 * t)


def value_noise(x, y, period, px, seed):
    """Value noise on a lattice that WRAPS at `period` cells across the tile, so a cell is seamless
    against itself and against its neighbors — no edge shows where the deck repeats."""
    step = px / period
    fx, fy = x / step, y / step
    ix, iy = int(math.floor(fx)), int(math.floor(fy))
    tx, ty = _smooth(fx - ix), _smooth(fy - iy)
    x0, x1 = ix % period, (ix + 1) % period
    y0, y1 = iy % period, (iy + 1) % period
    a = _hash(x0, y0, seed) + (_hash(x1, y0, seed) - _hash(x0, y0, seed)) * tx
    b = _hash(x0, y1, seed) + (_hash(x1, y1, seed) - _hash(x0, y1, seed)) * tx
    return a + (b - a) * ty


def deck_cell(px, kind, seed):
    """The faceplate: a fine bead-blasted anodised finish. Two octaves of wrapping value noise give the
    mottle, a per-pixel grain gives the tooth, and neither has a direction — nothing here reads as a
    line. The value sweeps the whole index range; it is the PALETTE that is narrow, which is what keeps
    a fully-used ramp reading as satin metal rather than as contrast.

    `kind` picks the trim: the plain face, the lit top rail, or the shaded bottom lip."""
    cell = [0] * (px * px)
    for y in range(px):
        for x in range(px):
            # Fine octaves only — 4px and 2px features. Anything coarser reads as blotches rather than
            # as tooth, and the trim gets its contrast from its own palette instead of from amplitude.
            v = 0.50
            v += 0.22 * (value_noise(x, y, 8, px, seed) - 0.5) * 2.0
            v += 0.18 * (value_noise(x, y, 16, px, seed + 91) - 0.5) * 2.0
            v += 0.24 * (_hash(x + seed * 31, y + seed * 17, seed + 7) - 0.5) * 2.0
            if kind == 'rail':
                v += 0.42 * max(0.0, 1.0 - y / (px * 0.22))
            elif kind == 'shade':
                v -= 0.38 * max(0.0, 1.0 - (px - 1 - y) / (px * 0.32))
            cell[y * px + x] = dither_index(v, 1, 15, x, y)
    return cell


def backdrop_cell(px, seed):
    """The field behind the scene: a dark, softly dithered wash so the load sprites read against it."""
    cell = [0] * (px * px)
    for y in range(px):
        for x in range(px):
            # The palette is dark end to end, so spanning the whole index range keeps the wash subtle
            # while still using every entry.
            v = 0.50
            v += 0.28 * math.sin((x * 0.21 + y * 0.17 + seed) * 1.3)
            v += 0.16 * math.sin((x * 0.05 - y * 0.09 + seed * 2) * 2.1)
            v += 0.09 * math.sin((x * 0.61 + y * 0.53 + seed * 4) * 0.9)
            cell[y * px + x] = dither_index(v, 1, 15, x, y)
    return cell


def blob_cell(px, seed):
    """A load sprite: a soft radial emitter with a hot core, so a Bloom has something to key on."""
    c = (px - 1) / 2.0
    cell = [0] * (px * px)
    for y in range(px):
        for x in range(px):
            dx, dy = x - c, y - c
            d = math.hypot(dx, dy) * (1.0 + 0.05 * math.sin(math.atan2(dy, dx) * 3 + seed))
            if d > px * 0.47:
                continue
            v = 1.0 - (d / (px * 0.47)) ** 1.5
            v += 0.08 * math.sin((x * 0.9 + y * 0.7 + seed * 3) * 1.1)
            cell[y * px + x] = dither_index(v, 1, 15, x, y)
    return cell


# ── Richness ───────────────────────────────────────────────────────────────────────────────────

def check(name, cells, floor=12):
    """Every solid cell must actually USE its palette's range. A cell that clears the count can still
    look flat, so previews are rendered too — the count is the floor, the eye is the bar."""
    worst, worst_i = 999, -1
    for i, cell in enumerate(cells):
        n = len(set(v for v in cell if v != 0))
        if n < worst:
            worst, worst_i = n, i
    status = 'ok ' if worst >= floor else 'LOW'
    print(f'  [{status}] {name:12s} {len(cells)} cells, fewest distinct indices: {worst} (cell {worst_i})')
    return worst >= floor


def preview(name, w, h, buf, cols):
    """Render a sheet through its palette so the art can be looked at, not just counted."""
    rgba = []
    for v in buf:
        if v == 0:
            rgba += [20, 20, 26, 255]
        else:
            c = cols[min(v, len(cols)) - 1]
            rgba += [c[0], c[1], c[2], 255]
    write_rgba(os.path.join(PREVIEW, name), w, h, rgba)


def main():
    for d in (ART, PAL, PREVIEW):
        os.makedirs(d, exist_ok=True)
    ok = True

    pal_knob = palette_image('knob.png', [(18, 18, 22), (52, 50, 58), (120, 116, 126),
                                              (186, 182, 190), (245, 243, 250)])
    pal_lamp_off = palette_image('lamp_off.png', [(14, 12, 10), (46, 30, 16), (86, 54, 22),
                                                      (120, 76, 30), (150, 100, 44)])
    pal_lamp_on = palette_image('lamp_on.png', [(40, 20, 6), (128, 60, 12), (214, 118, 24),
                                                    (255, 186, 76), (255, 246, 214)])
    # A tight satin band: every index is used, but they sit close together, so a fully-exercised ramp
    # still reads as brushed metal rather than as contrast. The trim carries its own ramps.
    pal_deck = palette_image('deck.png', [(114, 116, 122), (128, 130, 136), (142, 145, 151),
                                              (155, 158, 165), (168, 172, 180)])
    palette_image('deck_rail.png', [(150, 154, 162), (172, 176, 184), (190, 194, 202),
                                    (204, 208, 217), (218, 223, 232)])
    palette_image('deck_shade.png', [(52, 53, 58), (65, 66, 71), (78, 80, 85),
                                     (91, 93, 99), (104, 107, 113)])
    pal_backdrop = palette_image('backdrop.png', [(8, 9, 14), (14, 16, 24), (20, 23, 34),
                                                      (28, 32, 46), (36, 41, 58)])
    pal_blob = palette_image('blob.png', [(10, 24, 48), (24, 62, 120), (52, 126, 200),
                                              (140, 204, 246), (238, 250, 255)])
    # The font sheet is authored art; only its palettes are generated here.
    palette_image('font.png', [(30, 33, 42), (72, 78, 92), (128, 136, 152),
                                   (190, 198, 214), (244, 248, 255)])
    palette_image('font_pick.png', [(46, 26, 8), (120, 66, 16), (196, 118, 34),
                                        (240, 176, 80), (255, 240, 206)])
    # The readout's own liveries: identical text ramps over an opaque plate, so the panel comes from
    # the glyph cells themselves rather than from a layer underneath.
    palette_image('mono.png', [(30, 33, 42), (72, 78, 92), (128, 136, 152),
                               (190, 198, 214), (244, 248, 255)], backing=(9, 11, 16))
    palette_image('mono_pick.png', [(46, 26, 8), (120, 66, 16), (196, 118, 34),
                                    (240, 176, 80), (255, 240, 206)], backing=(9, 11, 16))

    knobs = [knob_cell(96, 0.0)]
    w, h, buf = sheet(knobs, 96)
    write_indexed(os.path.join(ART, 'knobs.png'), w, h, buf, [(0, 0, 0)] + list(pal_knob))
    ok &= check('knobs', knobs)
    preview('preview_knobs.png', w, h, buf, pal_knob)

    lamps = [lamp_cell(48)]
    w, h, buf = sheet(lamps, 48)
    write_indexed(os.path.join(ART, 'lamps.png'), w, h, buf, [(0, 0, 0)] + list(pal_lamp_on))
    ok &= check('lamps', lamps)
    preview('preview_lamps.png', w, h, buf, pal_lamp_on)

    deck = ([deck_cell(32, 'face', s) for s in range(4)]
            + [deck_cell(32, 'rail', 0), deck_cell(32, 'shade', 1)])
    w, h, buf = sheet(deck, 32)
    write_indexed(os.path.join(ART, 'deck.png'), w, h, buf, [(0, 0, 0)] + list(pal_deck))
    ok &= check('deck', deck)
    preview('preview_deck.png', w, h, buf, pal_deck)

    back = [backdrop_cell(32, s) for s in range(8)]
    w, h, buf = sheet(back, 32)
    write_indexed(os.path.join(ART, 'backdrop.png'), w, h, buf, [(0, 0, 0)] + list(pal_backdrop))
    ok &= check('backdrop', back)
    preview('preview_backdrop.png', w, h, buf, pal_backdrop)

    blobs = [blob_cell(32, s) for s in range(8)]
    w, h, buf = sheet(blobs, 32)
    write_indexed(os.path.join(ART, 'blobs.png'), w, h, buf, [(0, 0, 0)] + list(pal_blob))
    ok &= check('blobs', blobs)
    preview('preview_blobs.png', w, h, buf, pal_blob)

    print(f'previews written to {PREVIEW}')
    print('richness floor met' if ok else 'RICHNESS FLOOR NOT MET')
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
