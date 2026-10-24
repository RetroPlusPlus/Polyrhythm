#!/usr/bin/env python3
"""Author the hello-world example's text image: an indexed PNG that reads "Hello, world!".

examples/hello_world.cpp is the engine's bare-minimum program — open a window and show this image as
one sprite. The text is baked into a committed indexed PNG (index 0 = background, index 1 = ink) so
the example itself stays tiny (load image -> one sprite); the little 5x7 font lives here, not there.

Engine-original art (no third-party content). One-time authoring tool, kept committed so the
binary stays regenerable and its index plane is auditable — same posture as the other gen_*.py tools;
the minimal dependency-free PNG encoder mirrors theirs so those frozen tools stay untouched.

Run from the engine repo root:  python3 examples/assets/gen_hello_world.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent  # engine/examples/assets

GW, GH, GAP = 5, 7, 1  # glyph width/height + inter-glyph gap, in pixels

# A tiny 5x7 font, only the glyphs "Hello, world!" needs. '#' = ink (index 1), ' ' = background.
FONT: dict[str, list[str]] = {
    "H": ["#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"],
    "e": ["     ", "     ", " ### ", "#   #", "#####", "#    ", " ### "],
    "l": [" ##  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "],
    "o": ["     ", "     ", " ### ", "#   #", "#   #", "#   #", " ### "],
    "w": ["     ", "     ", "#   #", "#   #", "# # #", "# # #", " # # "],
    "r": ["     ", "     ", "# ## ", "##  #", "#    ", "#    ", "#    "],
    "d": ["    #", "    #", " ## #", "#  ##", "#   #", "#  ##", " ## #"],
    ",": ["     ", "     ", "     ", "     ", "     ", "  ## ", " ##  "],
    "!": ["  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  "],
    " ": ["     ", "     ", "     ", "     ", "     ", "     ", "     "],
}

TEXT = "Hello, world!"
PALETTE = [(0, 0, 0), (235, 235, 245)]  # 0 = background (transparent on the sprite path), 1 = ink


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _png_indexed8(width: int, height: int, indices: list[list[int]]) -> bytes:
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)  # 8-bit, colortype 3 (palette)
    plte = b"".join(struct.pack(">BBB", r, g, b) for (r, g, b) in PALETTE)
    raw = bytearray()
    for row in indices:
        raw.append(0)  # filter type 0 (none)
        raw.extend(row)
    return (sig + _chunk(b"IHDR", ihdr) + _chunk(b"PLTE", plte)
            + _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _chunk(b"IEND", b""))


def _round8(n: int) -> int:
    return (n + 7) // 8 * 8


def render(text: str) -> tuple[int, int, list[list[int]]]:
    text_w = len(text) * (GW + GAP) - GAP
    # The atlas upload path needs image dimensions that are a multiple of the 8px cell, so pad the
    # canvas up and center the text (the padding is background = index 0, transparent on the sprite).
    width, height = _round8(text_w), _round8(GH)
    ox, oy = (width - text_w) // 2, (height - GH) // 2
    plane = [[0 for _ in range(width)] for _ in range(height)]
    for i, ch in enumerate(text):
        glyph = FONT[ch]
        gx0 = ox + i * (GW + GAP)
        for gy in range(GH):
            for gx in range(GW):
                if glyph[gy][gx] == "#":
                    plane[oy + gy][gx0 + gx] = 1
    return width, height, plane


def main() -> None:
    width, height, plane = render(TEXT)
    (HERE / "hello_world.png").write_bytes(_png_indexed8(width, height, plane))
    print(f'hello_world.png: {width}x{height}, "{TEXT}"')


if __name__ == "__main__":
    main()
