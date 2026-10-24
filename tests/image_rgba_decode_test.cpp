#include "retropp/image.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// Truecolor PNG decode → the RGBA pixel plane (LoadedImage::pixels, kind == Rgba), 16-bit per
// channel. Pure CPU (lodepng), so the suite is fully headless: it decodes the committed truecolor
// fixtures and asserts the exact 16-bit pixel plane. The fixtures and their planes are authored by
// tests/fixtures/gen_fixtures.py; RETROPP_FIXTURES_DIR points at that directory at build time.

namespace retropp {
namespace {

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in) << "could not open fixture: " << path;
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::string fixture(const char* name) { return std::string{RETROPP_FIXTURES_DIR} + "/" + name; }

// rgba8.png's exact 2×2 (R,G,B,A) plane, widened ×257 into 16-bit channels (gen_fixtures.py RGBA8_2x2).
// Includes a fully transparent pixel (a == 0) so the decode is proven to carry alpha.
constexpr Rgba16 kRgba8Widened[4] = {
    {0, 0, 0, 65535},                    // (0,0,0,255)
    {65535, 65535, 65535, 65535},        // (255,255,255,255)
    {2570, 5140, 7710, 10280},           // (10,20,30,40) × 257
    {51400, 25700, 12850, 0},            // (200,100,50,0) × 257 — transparent
};

// rgb8.png's exact 2×2 (R,G,B) plane, widened ×257; an RGB (no-alpha) source decodes OPAQUE (a == 65535).
constexpr Rgba16 kRgb8Widened[4] = {
    {0, 0, 0, 65535},                    // (0,0,0)
    {65535, 32896, 0, 65535},            // (255,128,0) × 257
    {3084, 8738, 14392, 65535},          // (12,34,56) × 257
    {20046, 23130, 65535, 65535},        // (78,90,255) × 257
};

// rgba16.png's exact 2×2 (R,G,B,A) uint16 plane — direct, no widening (gen_fixtures.py RGBA16_2x2).
// Several channels are above 0xFF00 (65281, 65535), which an 8-bit crush could not represent.
constexpr Rgba16 kRgba16Exact[4] = {
    {0, 65535, 257, 32896},
    {65535, 65281, 256, 255},
    {1000, 2000, 3000, 4000},
    {65280, 60000, 65535, 0},
};

// ── 8-bit truecolor-alpha (RGBA8): kind Rgba, channels widened ×257, alpha carried ──────────────

TEST(ImageRgbaDecode, Rgba8WidensEachChannel) {
    const LoadedImage img = loadPngFromMemory(readFile(fixture("rgba8.png")));

    EXPECT_EQ(img.kind, ImageColorKind::Rgba);
    EXPECT_EQ(img.width, 2);
    EXPECT_EQ(img.height, 2);
    EXPECT_TRUE(img.indices.empty()) << "an RGBA source carries no index plane";
    EXPECT_TRUE(img.palette.empty()) << "an RGBA source carries no embedded palette";

    ASSERT_EQ(img.pixels.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(img.pixels[i], kRgba8Widened[i]) << "pixel " << i << " mismatch";
    }
    // The transparent pixel's alpha survives the widen (0 stays 0), proving alpha is decoded.
    EXPECT_EQ(img.pixels[3].a, 0u) << "a fully transparent 8-bit pixel must decode to alpha 0";
}

// ── 8-bit truecolor without alpha (RGB8): decode synthesizes opaque alpha (65535) ───────────────

TEST(ImageRgbaDecode, Rgb8WithoutAlphaDecodesOpaque) {
    const LoadedImage img = loadPngFromMemory(readFile(fixture("rgb8.png")));

    EXPECT_EQ(img.kind, ImageColorKind::Rgba);
    EXPECT_EQ(img.width, 2);
    EXPECT_EQ(img.height, 2);

    ASSERT_EQ(img.pixels.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(img.pixels[i], kRgb8Widened[i]) << "pixel " << i << " mismatch";
        EXPECT_EQ(img.pixels[i].a, 65535u) << "an RGB source must decode opaque at pixel " << i;
    }
}

// ── 16-bit truecolor-alpha (RGBA16): decoded direct, no precision loss above 0xFF00 ─────────────

TEST(ImageRgbaDecode, Rgba16DecodesDirectWithoutCrush) {
    const LoadedImage img = loadPngFromMemory(readFile(fixture("rgba16.png")));

    EXPECT_EQ(img.kind, ImageColorKind::Rgba);
    EXPECT_EQ(img.width, 2);
    EXPECT_EQ(img.height, 2);

    ASSERT_EQ(img.pixels.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(img.pixels[i], kRgba16Exact[i]) << "pixel " << i << " mismatch";
    }
    // The headline: a 16-bit channel above 0xFF00 survives exactly — an 8-bit path would crush it.
    EXPECT_EQ(img.pixels[1].g, 65281u) << "0xFF01 must survive, not truncate to a byte";
    EXPECT_GT(img.pixels[1].g, 0xFF00u) << "the value must exceed the 8-bit-crush ceiling";
}

// ── loadPng(path) routes truecolor identically to loadPngFromMemory ─────────────────────────────

TEST(ImageRgbaDecode, FilePathMatchesMemory) {
    const std::vector<std::uint8_t> bytes = readFile(fixture("rgba16.png"));
    const LoadedImage fromMem  = loadPngFromMemory(bytes);
    const LoadedImage fromPath = loadPng(fixture("rgba16.png"));

    EXPECT_EQ(fromPath.kind, fromMem.kind);
    EXPECT_EQ(fromPath.width, fromMem.width);
    EXPECT_EQ(fromPath.height, fromMem.height);
    EXPECT_EQ(fromPath.pixels, fromMem.pixels);
}

}  // namespace
}  // namespace retropp
