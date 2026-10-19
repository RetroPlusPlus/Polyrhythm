// Sub-byte PNG rows whose width is not a whole number of bytes.
//
// A PNG's own scanlines end on byte boundaries, and the decoder's raw output does not: samples under
// eight bits are packed tightly across the whole image, so a row begins where the one before it ended.
// These cases pin that every row of such an image decodes to the samples it was written with, through
// the art path and the map path, at each sub-byte depth — and that a byte-aligned image reads the same
// under either stride. The PNGs are built here, byte by byte, so what is decoded is known exactly.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/image.h"

namespace retropp {
namespace {

// ── A minimal PNG writer: one IHDR, one stored (uncompressed) IDAT, IEND ────────────────────────

std::uint32_t crc32Of(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::uint8_t b : bytes) {
        crc ^= b;
        for (int i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t adler32Of(std::span<const std::uint8_t> bytes) {
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t byte : bytes) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void putBigEndian32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void putChunk(std::vector<std::uint8_t>& out, std::string_view type, std::span<const std::uint8_t> data) {
    putBigEndian32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type.begin(), type.end());
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    putBigEndian32(out, crc32Of(body));
}

// A grayscale PNG of `width` × rows.size() at `bitDepth`, each row given as its packed scanline bytes
// (byte-aligned at the row's end, as the format requires), filter 0 on every row.
std::vector<std::uint8_t> grayscalePng(std::uint32_t width, std::uint8_t bitDepth,
                                       const std::vector<std::vector<std::uint8_t>>& rows) {
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<std::uint8_t> ihdr;
    putBigEndian32(ihdr, width);
    putBigEndian32(ihdr, static_cast<std::uint32_t>(rows.size()));
    ihdr.insert(ihdr.end(), {bitDepth, /*grayscale*/ 0, /*deflate*/ 0, /*filter*/ 0, /*no interlace*/ 0});
    putChunk(png, "IHDR", ihdr);

    std::vector<std::uint8_t> scanlines;
    for (const std::vector<std::uint8_t>& row : rows) {
        scanlines.push_back(0);  // filter type 0: the row as it is
        scanlines.insert(scanlines.end(), row.begin(), row.end());
    }
    // A zlib stream holding one stored deflate block: header, BFINAL + BTYPE 00, LEN, NLEN, the bytes,
    // then the Adler-32 of the bytes.
    std::vector<std::uint8_t> idat{0x78, 0x01, 0x01};
    const auto len = static_cast<std::uint16_t>(scanlines.size());
    idat.push_back(static_cast<std::uint8_t>(len & 0xFF));
    idat.push_back(static_cast<std::uint8_t>(len >> 8));
    idat.push_back(static_cast<std::uint8_t>(~len & 0xFF));
    idat.push_back(static_cast<std::uint8_t>((~len >> 8) & 0xFF));
    idat.insert(idat.end(), scanlines.begin(), scanlines.end());
    putBigEndian32(idat, adler32Of(scanlines));
    putChunk(png, "IDAT", idat);

    putChunk(png, "IEND", {});
    return png;
}

std::vector<std::uint16_t> widened(const std::vector<std::uint8_t>& samples) {
    return std::vector<std::uint16_t>(samples.begin(), samples.end());
}

// Both decode paths, against the samples the image was written with, row by row in one plane.
void expectBothPathsDecode(const std::vector<std::uint8_t>& png, int width, int height,
                           const std::vector<std::uint8_t>& samples) {
    const LoadedImage image = loadPngFromMemory(png);
    EXPECT_EQ(image.kind, ImageColorKind::Indexed);
    EXPECT_EQ(image.width, width);
    EXPECT_EQ(image.height, height);
    EXPECT_EQ(image.indices, samples);

    const IndexGrid grid = loadMapPngFromMemory(png);
    EXPECT_EQ(grid.width, width);
    EXPECT_EQ(grid.height, height);
    EXPECT_EQ(grid.values, widened(samples));
}

// ── Widths that are not a whole number of bytes ─────────────────────────────────────────────────

TEST(ImageSubByteRows, SevenWideOneBitRowsDecodeToTheirOwnSamples) {
    // 0101010 / 1010101 / 0101010 — each row reads differently from its neighbours, so a row read at
    // the wrong offset is never mistaken for the right one.
    const std::vector<std::uint8_t> png = grayscalePng(7, 1, {{0x54}, {0xAA}, {0x54}});
    expectBothPathsDecode(png, 7, 3,
                          {0, 1, 0, 1, 0, 1, 0,
                           1, 0, 1, 0, 1, 0, 1,
                           0, 1, 0, 1, 0, 1, 0});
}

TEST(ImageSubByteRows, SixWideTwoBitRowsDecodeToTheirOwnSamples) {
    // 0 1 2 3 0 1 / 3 2 1 0 3 2 — twelve bits a row, a byte and a half.
    const std::vector<std::uint8_t> png = grayscalePng(6, 2, {{0x1B, 0x10}, {0xE4, 0xE0}});
    expectBothPathsDecode(png, 6, 2,
                          {0, 1, 2, 3, 0, 1,
                           3, 2, 1, 0, 3, 2});
}

TEST(ImageSubByteRows, ThreeWideFourBitRowsDecodeToTheirOwnSamples) {
    // 1 2 3 / 4 5 6 — twelve bits a row at four bits a sample.
    const std::vector<std::uint8_t> png = grayscalePng(3, 4, {{0x12, 0x30}, {0x45, 0x60}});
    expectBothPathsDecode(png, 3, 2,
                          {1, 2, 3,
                           4, 5, 6});
}

// ── A byte-aligned width reads the same under either stride ─────────────────────────────────────

TEST(ImageSubByteRows, AByteAlignedWidthDecodesToTheSameBytes) {
    // 10110010 / 01001101 — a row is exactly one byte, so the tight and the byte-aligned readings of the
    // buffer are the same bytes.
    const std::vector<std::uint8_t> png = grayscalePng(8, 1, {{0xB2}, {0x4D}});
    expectBothPathsDecode(png, 8, 2,
                          {1, 0, 1, 1, 0, 0, 1, 0,
                           0, 1, 0, 0, 1, 1, 0, 1});
}

}  // namespace
}  // namespace retropp
