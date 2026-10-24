#!/usr/bin/env python3
"""Author the image-layer example's two PNGs.

  planet.png  96×96, 8-bit truecolor-alpha: a banded sphere lit from the upper left, shaded to a soft
              terminator, with a two-pixel alpha limb and a fully transparent background. Every pixel
              is its own color, which is what an image layer carries and indexed art cannot.
  stars.png   32×32, 8-bit indexed: four 8×8 tiles along the top row — empty sky, and one, two and three
              stars — and empty sky in the rest. The example uploads its own palette for it.

Dependency-free: a minimal PNG encoder over the standard library (zlib + struct), the same writers
tests/fixtures/gen_fixtures.py uses. Run from the repo root:

    python3 examples/image_layer/assets/gen_image_layer_assets.py
"""

from __future__ import annotations

import math
import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _png(width: int, height: int, bitdepth: int, colortype: int,
         scanlines: bytes, palette: list[tuple[int, int, int]] | None) -> bytes:
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, bitdepth, colortype, 0, 0, 0)
    out = sig + _chunk(b"IHDR", ihdr)
    if palette is not None:
        out += _chunk(b"PLTE", b"".join(struct.pack(">BBB", r, g, b) for (r, g, b) in palette))
    out += _chunk(b"IDAT", zlib.compress(scanlines, 9))
    out += _chunk(b"IEND", b"")
    return out


def write_rgba8(path: Path, width: int, height: int,
                pixels: list[list[tuple[int, int, int, int]]]) -> None:
    """8-bit truecolor-alpha PNG (colortype 6): four bytes (R, G, B, A) per pixel."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter type 0 (none)
        for (r, g, b, a) in row:
            raw.extend((r & 0xFF, g & 0xFF, b & 0xFF, a & 0xFF))
    path.write_bytes(_png(width, height, 8, 6, bytes(raw), None))


def write_indexed8(path: Path, width: int, height: int,
                   indices: list[list[int]], palette: list[tuple[int, int, int]]) -> None:
    """8-bit palette PNG (colortype 3): one index byte per pixel."""
    raw = bytearray()
    for row in indices:
        raw.append(0)  # filter type 0 (none)
        raw.extend(row)
    path.write_bytes(_png(width, height, 8, 3, bytes(raw), palette))


PLANET = 96
RADIUS = 46.0
LIMB = 2.0                                  # the alpha ramp at the edge, in pixels
LIGHT = (-0.55, -0.5, 0.67)                 # toward the upper left, out of the screen
WARM = (232.0, 158.0, 92.0)                 # band colors the latitude mixes between
COOL = (64.0, 142.0, 168.0)


def _normalize(v: tuple[float, float, float]) -> tuple[float, float, float]:
    n = math.sqrt(sum(c * c for c in v))
    return (v[0] / n, v[1] / n, v[2] / n)


def planet_pixel(x: int, y: int) -> tuple[int, int, int, int]:
    center = PLANET / 2.0
    dx = (x + 0.5 - center) / RADIUS
    dy = (y + 0.5 - center) / RADIUS
    d = math.sqrt(dx * dx + dy * dy) * RADIUS
    if d >= RADIUS:
        return (0, 0, 0, 0)
    alpha = 255 if d <= RADIUS - LIMB else int(round(255.0 * (RADIUS - d) / LIMB))

    nz = math.sqrt(max(0.0, 1.0 - dx * dx - dy * dy))
    lx, ly, lz = _normalize(LIGHT)
    lambert = max(0.0, dx * lx + dy * ly + nz * lz)
    # A soft terminator: the light falls off across a band at the edge of the lit side.
    lit = 0.06 + 0.94 * (lambert ** 0.8)

    band = 0.5 + 0.5 * math.sin(dy * 11.0 + 0.7 * math.sin(dx * 5.0))
    base = tuple(WARM[i] * band + COOL[i] * (1.0 - band) for i in range(3))
    specular = 70.0 * (max(0.0, lambert) ** 24)
    r, g, b = (int(round(min(255.0, c * lit + specular))) for c in base)
    return (r, g, b, alpha)


def planet() -> list[list[tuple[int, int, int, int]]]:
    return [[planet_pixel(x, y) for x in range(PLANET)] for y in range(PLANET)]


SKY, DIM, BRIGHT = 0, 1, 2
STAR_PALETTE = [(10, 12, 24), (120, 130, 170), (240, 240, 255)]


def stars() -> list[list[int]]:
    rows = [[SKY] * 32 for _ in range(32)]
    # Tile 0 is empty sky; tiles 1-3 hold one, two and three stars.
    for tile, points in ((1, [(3, 4, BRIGHT)]),
                         (2, [(1, 2, DIM), (6, 5, BRIGHT)]),
                         (3, [(2, 1, BRIGHT), (5, 3, DIM), (1, 6, DIM)])):
        for (px, py, index) in points:
            rows[py][tile * 8 + px] = index
    return rows


def main() -> None:
    write_rgba8(HERE / "planet.png", PLANET, PLANET, planet())
    write_indexed8(HERE / "stars.png", 32, 32, stars(), STAR_PALETTE)
    colors = {p for row in planet() for p in row if p[3] > 0}
    print(f"planet.png: {PLANET}x{PLANET}, {len(colors)} distinct colors")
    print("stars.png: 32x32 indexed, tiles 0-3 on the top row")


if __name__ == "__main__":
    main()
