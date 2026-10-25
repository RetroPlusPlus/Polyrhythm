#!/usr/bin/env python3
"""Author the palette-image demo's color PNGs.

The palette-image demo (examples/palette_image_demo/main.cpp) reads a color image one pixel per
palette entry. These exercise the FEATURE, not a toy: 16-bit-per-channel truecolor with a real ALPHA
channel — the two reasons palette images exist (lossless color + material transparency). Two images,
one per asset policy the demo exercises:

  - palette_grid.png  (4x4, 16-bit RGBA) — loaded Embed (baked into the binary). 16 hues; alpha falls
                       per row (opaque → fully transparent) so the demo shows material transparency.
  - palette_ramp.png  (8x1, 16-bit RGBA) — loaded LoadFromPath (copied beside the binary). One hue,
                       alpha ramped 0 → full across the 8 entries: a pure transparency ramp.

These are engine-original images (no third-party art). One-time authoring tool, kept committed
so the binary assets stay regenerable and their exact color planes are auditable — the same posture
as tests/fixtures/gen_fixtures.py (whose minimal PNG encoder this mirrors).

Dependency-free: a minimal 16-bit truecolor-alpha PNG encoder over the standard library (zlib +
struct), so it runs anywhere Python does — no Pillow. Emits non-interlaced, filter-0, colortype-6,
bitdepth-16 PNGs (samples big-endian, per the PNG spec).

Run from the engine repo root:  python3 examples/palette_image_demo/assets/gen_palette_demo_assets.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent  # engine/examples/palette_image_demo/assets

MAX16 = 65535


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_rgba16(path: Path, width: int, height: int,
                 pixels: list[list[tuple[int, int, int, int]]]) -> None:
    """16-bit truecolor-alpha PNG (colortype 6, bitdepth 16): four big-endian uint16 samples/pixel."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter type 0 (none)
        for (r, g, b, a) in row:
            raw.extend(struct.pack(">HHHH", r & 0xFFFF, g & 0xFFFF, b & 0xFFFF, a & 0xFFFF))
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 16, 6, 0, 0, 0)
    out = sig + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _chunk(b"IEND", b"")
    path.write_bytes(out)


def hsv16(h: float, s: float, v: float, a: float) -> tuple[int, int, int, int]:
    """h,s,v,a in [0,1] → 16-bit RGBA. Pure-stdlib HSV→RGB scaled to the full 16-bit range (the values
    are generally NOT 8-bit-representable, so they prove the 16-bit decode/store path)."""
    i = int(h * 6.0) % 6
    f = h * 6.0 - int(h * 6.0)
    p, q, t = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
    r, g, b = [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i]
    return (round(r * MAX16), round(g * MAX16), round(b * MAX16), round(a * MAX16))


def main() -> None:
    # 4x4 hue grid, 16-bit. Hue marches across the wheel in natural (row-major) order; ALPHA falls per
    # row — row 0 opaque, row 3 fully transparent — so over a background the demo shows the palette's
    # material transparency, and the color values exercise the 16-bit path.
    row_alpha = [1.0, 0.66, 0.33, 0.0]
    grid = [[hsv16((row * 4 + col) / 16.0, 0.85, 0.95, row_alpha[row]) for col in range(4)]
            for row in range(4)]
    write_rgba16(HERE / "palette_grid.png", 4, 4, grid)

    # 8x1 transparency ramp, 16-bit: one hue (cyan), alpha 0 → full across the 8 entries — the
    # LoadFromPath companion, a pure alpha gradient.
    ramp = [[hsv16(0.5, 0.8, 0.95, j / 7.0) for j in range(8)]]
    write_rgba16(HERE / "palette_ramp.png", 8, 1, ramp)

    print("palette_grid.png 4x4 (R,G,B,A) 16-bit plane:")
    for r in grid:
        print("  ", r)
    print("palette_ramp.png 8x1 (R,G,B,A) 16-bit plane:")
    for r in ramp:
        print("  ", r)


if __name__ == "__main__":
    main()
