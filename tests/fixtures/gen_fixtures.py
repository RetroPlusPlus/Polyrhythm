#!/usr/bin/env python3
"""Author the engine's tiny, license-clean PNG and audio fixtures + the demo asset.

The headless image-decode tests need committed PNGs with a KNOWN pixel/index plane so they can
assert exact values, plus a small indexed tileset with index-0 holes for the window demo's
transparent-index check. These are engine-original images (not third-party art). This is a
one-time authoring tool kept committed so the binary fixtures stay regenerable and their exact
planes are auditable.

Dependency-free: a minimal PNG encoder over the standard library (zlib + struct), so it
runs anywhere Python does — no Pillow. Emits non-interlaced, filter-0 PNGs:
  - 8-bit PLTE (palette) images        → colortype 3
  - 2-/16-bit grayscale images         → colortype 0  (the stored sample value IS the index)
  - 8-/16-bit truecolor(-alpha) images → colortype 2 / 6  (one color per pixel)

Run from the engine repo root:  python3 tests/fixtures/gen_fixtures.py
"""

from __future__ import annotations

import math
import struct
import wave
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent          # engine/tests/fixtures
ENGINE = HERE.parent.parent                      # engine/
ASSETS = ENGINE / "examples" / "assets"


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _png(width: int, height: int, bitdepth: int, colortype: int,
         scanlines: bytes, palette: list[tuple[int, int, int, int]] | None) -> bytes:
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, bitdepth, colortype, 0, 0, 0)
    out = sig + _chunk(b"IHDR", ihdr)
    if palette is not None:
        plte = b"".join(struct.pack(">BBB", r, g, b) for (r, g, b, _a) in palette)
        out += _chunk(b"PLTE", plte)
        # tRNS carries per-index alpha; we keep palettes opaque, so it is omitted.
    out += _chunk(b"IDAT", zlib.compress(scanlines, 9))
    out += _chunk(b"IEND", b"")
    return out


def write_indexed8(path: Path, width: int, height: int,
                   indices: list[list[int]], palette: list[tuple[int, int, int, int]]) -> None:
    """8-bit palette PNG: one index byte per pixel."""
    raw = bytearray()
    for row in indices:
        raw.append(0)  # filter type 0 (none)
        raw.extend(row)
    path.write_bytes(_png(width, height, 8, 3, bytes(raw), palette))


def write_gray16(path: Path, width: int, height: int, values: list[list[int]]) -> None:
    """16-bit grayscale PNG: each sample is one big-endian uint16 (colortype 0, bitdepth 16). The
    stored sample value IS the index — used by the map-import path, which must carry indices above
    255 (a tilemap may reference >256 catalog entries)."""
    raw = bytearray()
    for row in values:
        raw.append(0)  # filter type 0 (none)
        for v in row:
            raw.extend(struct.pack(">H", v & 0xFFFF))  # PNG samples are big-endian
    path.write_bytes(_png(width, height, 16, 0, bytes(raw), None))


def write_gray2(path: Path, width: int, height: int, indices: list[list[int]]) -> None:
    """2-bit grayscale PNG: sample values 0..3 packed MSB-first, 4 px/byte."""
    raw = bytearray()
    for row in indices:
        raw.append(0)  # filter type 0 (none)
        acc = 0
        nbits = 0
        for v in row:
            acc = (acc << 2) | (v & 3)
            nbits += 2
            if nbits == 8:
                raw.append(acc)
                acc = 0
                nbits = 0
        if nbits:                       # flush a partial trailing byte (left-justified)
            raw.append(acc << (8 - nbits))
    path.write_bytes(_png(width, height, 2, 0, bytes(raw), None))


def write_rgb8(path: Path, width: int, height: int,
               pixels: list[list[tuple[int, int, int]]]) -> None:
    """8-bit truecolor PNG (colortype 2): three bytes (R, G, B) per pixel, no alpha channel."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter type 0 (none)
        for (r, g, b) in row:
            raw.extend((r & 0xFF, g & 0xFF, b & 0xFF))
    path.write_bytes(_png(width, height, 8, 2, bytes(raw), None))


def write_rgba8(path: Path, width: int, height: int,
                pixels: list[list[tuple[int, int, int, int]]]) -> None:
    """8-bit truecolor-alpha PNG (colortype 6): four bytes (R, G, B, A) per pixel."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter type 0 (none)
        for (r, g, b, a) in row:
            raw.extend((r & 0xFF, g & 0xFF, b & 0xFF, a & 0xFF))
    path.write_bytes(_png(width, height, 8, 6, bytes(raw), None))


def write_rgba16(path: Path, width: int, height: int,
                 pixels: list[list[tuple[int, int, int, int]]]) -> None:
    """16-bit truecolor-alpha PNG (colortype 6, bitdepth 16): four big-endian uint16 samples per
    pixel. Carries channel values above 0xFF00 so a decode that crushed to 8 bits would be caught."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter type 0 (none)
        for (r, g, b, a) in row:
            raw.extend(struct.pack(">HHHH", r & 0xFFFF, g & 0xFFFF, b & 0xFFFF, a & 0xFFFF))
    path.write_bytes(_png(width, height, 16, 6, bytes(raw), None))


# A clear 4×4 diagonal pattern — every index appears, trivially hand-verifiable.
DIAGONAL_4x4 = [
    [0, 1, 2, 3],
    [1, 2, 3, 0],
    [2, 3, 0, 1],
    [3, 0, 1, 2],
]

# Four distinct opaque palette entries (consumer may ignore these — the demo hand-builds color).
PALETTE4 = [(10, 20, 30, 255), (40, 50, 60, 255), (70, 80, 90, 255), (100, 110, 120, 255)]

# A 4×4 16-bit map plane: raw index values, several ABOVE 255 to prove the wide decode
# (an 8-bit path would truncate these). Spans 0 .. 65535 (the uint16 range), trivially hand-verifiable.
MAP16_4x4 = [
    [0,     1,     255,   256],
    [257,   300,   1000,  4095],
    [4096,  20000, 40000, 60000],
    [65535, 2,     513,   128],
]


# A 2×2 16-bit map plane for the asset-registry linkage probe (tests/registry_linkage/). It is baked by
# the build scan and asserted present at runtime; the decode itself is never exercised, so the values only
# need to be distinct. Its own file, referenced by no other target, so the probe proves this target's
# registry rather than a bake some other target performed.
LINKAGE_PROBE_2x2 = [
    [1, 2],
    [3, 4],
]


# A 2×2 8-bit truecolor-alpha plane: includes a fully transparent pixel (a=0) so the decode is
# proven to carry alpha, and an opaque white so the channel endpoints (0 and 255) both appear.
RGBA8_2x2 = [
    [(0,  0,  0,  255), (255, 255, 255, 255)],
    [(10, 20, 30, 40),  (200, 100, 50,  0)],
]

# A 2×2 8-bit truecolor (no alpha) plane: decode must synthesize opaque alpha (→ 65535 widened).
RGB8_2x2 = [
    [(0,  0,  0),  (255, 128, 0)],
    [(12, 34, 56), (78,  90,  255)],
]

# A 2×2 16-bit truecolor-alpha plane: exact uint16 samples, several ABOVE 0xFF00 (e.g. 0xFF01,
# 0xFFFF) so an 8-bit crush would be caught, plus a transparent pixel (a=0). Hand-verifiable.
RGBA16_2x2 = [
    [(0,      65535,  257,    32896), (0xFFFF, 0xFF01, 0x0100, 0x00FF)],
    [(1000,   2000,   3000,   4000),  (65280,  60000,  65535,  0)],
]

# A 4×2 8-bit truecolor strip for the palette-image slicer: each pixel's color ENCODES its grid
# position (r = col·10, g = row·10), so the entry order a given ReadOrder produces is verifiable from
# the colors alone. 8 entries; an 8-bit source, so each channel widens ×257 in the decoded Rgba16.
PALETTE_STRIP_4x2 = [[(c * 10, r * 10, 0, 255) for c in range(4)] for r in range(2)]

# A 2×2 16-bit truecolor-alpha strip for the palette slicer: exact uint16 channels (several above
# 0xFF00 / not 8-bit-representable) and varied alpha incl. a fully transparent entry — so the slicer is
# proven to carry 16-bit precision AND alpha into the entries (the whole point of palette images).
PALETTE_STRIP16_2x2 = [
    [(65535, 0,     0,     65535), (0,     65535, 0,     32768)],
    [(0,     0,     65535, 0),     (40000, 50000, 60000, 12345)],
]


def sheet16x32_indices() -> list[list[int]]:
    """16×32 indexed sheet for the atlas-slot / animation-art tests: a 2×2 grid of 8×16-px assets,
    each quadrant filled with its LeftRightThenDown slot number (0..3). Sliced at 8×16 the carve's
    atlas cells are {0, 1, 4, 5} (cellsAcross = 2), so slot index ≠ atlas cell from slot 2 on — a
    correct read must resolve THROUGH the sheet's geometry, never echo the index."""
    return [[(x // 8) + 2 * (y // 16) for x in range(16)] for y in range(32)]


def demo_tiles_indices() -> list[list[int]]:
    """16×16 (2×2-tile) indexed tileset with a central diamond of index 0 (holes) ringed by a
    colored band — so the transparent-index demo shows the lower layer through an obvious shape."""
    rows = []
    for y in range(16):
        row = []
        for x in range(16):
            d = abs(x - 8) + abs(y - 8)         # Manhattan distance from center
            if d <= 5:
                row.append(0)                   # central diamond → transparent hole
            else:
                row.append(1 + ((x // 2 + y // 2) % 3))  # 1..3 colored bands
        rows.append(row)
    return rows


def write_wav8_mono(path: Path, rate: int, samples: list[int]) -> None:
    """8-bit mono WAV: unsigned samples, 128 the midpoint."""
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1)
        w.setframerate(rate)
        w.writeframes(bytes(max(0, min(255, 128 + s)) for s in samples))


def sweep(rate: int, seconds: float, start_hz: float, end_hz: float) -> list[int]:
    """A sine whose pitch moves linearly from start_hz to end_hz, its level rising from a third of full
    scale to all of it — so no stretch of it repeats another, in pitch, phase or level."""
    count = int(rate * seconds)
    out = []
    phase = 0.0
    for i in range(count):
        t = i / count
        phase += 2.0 * math.pi * (start_hz + (end_hz - start_hz) * t) / rate
        level = 40.0 + 80.0 * t
        out.append(int(round(level * math.sin(phase))))
    return out


def write_long_brr(path: Path, blocks: int) -> None:
    """A sample already in the SNES sound chip's format: `blocks` nine-byte blocks on filter 0 at shift 11,
    each block's eight data bytes derived from its index so no two blocks are alike, the last block
    carrying END without LOOP."""
    out = bytearray()
    for i in range(blocks):
        header = 0xB0 | (0x01 if i == blocks - 1 else 0x00)
        data = [(i & 0xFF), (i >> 8) & 0xFF] + [((i * 37 + k * 59) ^ (k << 4)) & 0xFF for k in range(6)]
        out.append(header)
        out.extend(data)
    path.write_bytes(bytes(out))


def main() -> None:
    ASSETS.mkdir(parents=True, exist_ok=True)

    # Audio for the SNES sampler's streaming cases: a 6 s sweep, encoded far larger than audio RAM; a 1 s
    # sweep, larger than a streamed file's window and small enough to load whole; and a 600-block sample in
    # the chip's own format that ends without looping.
    write_wav8_mono(HERE / "long.wav", 11025, sweep(11025, 6.0, 110.0, 2200.0))
    write_wav8_mono(HERE / "mid.wav", 11025, sweep(11025, 1.0, 1800.0, 300.0))
    write_long_brr(HERE / "long.brr", 600)

    write_indexed8(HERE / "indexed4.png", 4, 4, DIAGONAL_4x4, PALETTE4)
    write_gray2(HERE / "gray2.png", 4, 4, DIAGONAL_4x4)
    write_gray16(HERE / "map16.png", 4, 4, MAP16_4x4)
    (HERE / "linkage").mkdir(parents=True, exist_ok=True)
    write_gray16(HERE / "linkage" / "probe.png", 2, 2, LINKAGE_PROBE_2x2)
    write_rgba8(HERE / "rgba8.png", 2, 2, RGBA8_2x2)
    write_rgb8(HERE / "rgb8.png", 2, 2, RGB8_2x2)
    write_rgba16(HERE / "rgba16.png", 2, 2, RGBA16_2x2)
    write_rgba8(HERE / "palette_strip.png", 4, 2, PALETTE_STRIP_4x2)
    write_rgba16(HERE / "palette_strip16.png", 2, 2, PALETTE_STRIP16_2x2)
    write_indexed8(HERE / "sheet16x32.png", 16, 32, sheet16x32_indices(), PALETTE4)
    write_indexed8(ASSETS / "demo_tiles.png", 16, 16, demo_tiles_indices(), PALETTE4)

    # Print the exact index planes so image_test.cpp's expected values are transcribed, not guessed.
    print("indexed4.png / gray2.png index plane (row-major):")
    for r in DIAGONAL_4x4:
        print("  ", r)
    print("indexed4.png palette (RGBA):")
    for i, c in enumerate(PALETTE4):
        print(f"   {i}: {c}")
    print("demo_tiles.png 16x16 index plane:")
    for r in demo_tiles_indices():
        print("  ", "".join(str(v) for v in r))
    print("map16.png 4x4 uint16 value plane (row-major):")
    for r in MAP16_4x4:
        print("  ", r)
    print("rgba8.png 2x2 (R,G,B,A) plane:")
    for r in RGBA8_2x2:
        print("  ", r)
    print("rgb8.png 2x2 (R,G,B) plane:")
    for r in RGB8_2x2:
        print("  ", r)
    print("palette_strip.png 4x2 (R,G,B,A) plane:")
    for r in PALETTE_STRIP_4x2:
        print("  ", r)
    print("palette_strip16.png 2x2 (R,G,B,A) 16-bit plane:")
    for r in PALETTE_STRIP16_2x2:
        print("  ", r)
    print("rgba16.png 2x2 (R,G,B,A) uint16 plane:")
    for r in RGBA16_2x2:
        print("  ", r)


if __name__ == "__main__":
    main()
