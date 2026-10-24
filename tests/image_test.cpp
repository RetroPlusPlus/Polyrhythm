#include "retropp/image.h"

#include "retropp/asset_registry.h"  // setAssetRoot — resolve a logical asset path for the disk-read test

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// PNG decode + indexed-source extraction. Pure CPU (lodepng), so this suite is fully headless: it
// decodes the committed engine-authored fixtures and asserts the exact index plane + embedded
// palette. The fixtures and their known index planes are authored by tests/fixtures/gen_fixtures.py;
// RETROPP_FIXTURES_DIR points at that directory at build time.

namespace retropp {
namespace {

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in) << "could not open fixture: " << path;
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::string fixture(const char* name) { return std::string{RETROPP_FIXTURES_DIR} + "/" + name; }

// The known 4×4 diagonal index plane shared by indexed4.png and gray2.png (gen_fixtures.py).
constexpr std::uint8_t kDiagonalPlane[16] = {
    0, 1, 2, 3,
    1, 2, 3, 0,
    2, 3, 0, 1,
    3, 0, 1, 2,
};

// indexed4.png's embedded 4-entry palette (RGBA), authored by gen_fixtures.py.
constexpr Rgba8 kExpectedPalette[4] = {
    {10, 20, 30, 255}, {40, 50, 60, 255}, {70, 80, 90, 255}, {100, 110, 120, 255},
};

// 77 bytes — 2×2 8-bit truecolor (RGB) PNG, generated once with the gen_fixtures encoder. Inline so
// the truecolor seam can be exercised (art path decodes it; map path rejects it) without committing a
// fixture. Exact-value RGBA decode coverage lives in image_rgba_decode_test.cpp.
constexpr std::uint8_t kTruecolorPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x02, 0x00, 0x00, 0x00, 0xfd, 0xd4, 0x9a, 0x73, 0x00, 0x00, 0x00,
    0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xc0,
    0x00, 0xc2, 0x0c, 0xff, 0xff, 0xff, 0x67, 0x00, 0x00, 0x1e, 0xef, 0x04,
    0xfc, 0x73, 0x1c, 0x53, 0xcc, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e,
    0x44, 0xae, 0x42, 0x60, 0x82,
};

// ── Palette (PLTE) PNG: indexed kind, exact index plane, exact embedded palette ──────────────

TEST(Image, PaletteDecodeIndexPlaneAndPalette) {
    const std::vector<std::uint8_t> bytes = readFile(fixture("indexed4.png"));
    const LoadedImage img = loadPngFromMemory(bytes);

    EXPECT_EQ(img.kind, ImageColorKind::Indexed);
    EXPECT_EQ(img.width, 4);
    EXPECT_EQ(img.height, 4);

    ASSERT_EQ(img.indices.size(), 16u);
    for (std::size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(img.indices[i], kDiagonalPlane[i]) << "index mismatch at pixel " << i;
    }

    ASSERT_EQ(img.palette.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(img.palette[i], kExpectedPalette[i]) << "palette mismatch at entry " << i;
    }
}

// ── Grayscale-2 PNG: indexed kind, sample-value-as-index plane, NO embedded palette ──────────

TEST(Image, GrayscaleDecodeSampleAsIndexNoPalette) {
    const std::vector<std::uint8_t> bytes = readFile(fixture("gray2.png"));
    const LoadedImage img = loadPngFromMemory(bytes);

    EXPECT_EQ(img.kind, ImageColorKind::Indexed);
    EXPECT_EQ(img.width, 4);
    EXPECT_EQ(img.height, 4);

    ASSERT_EQ(img.indices.size(), 16u);
    for (std::size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(img.indices[i], kDiagonalPlane[i]) << "grey index mismatch at pixel " << i;
    }

    EXPECT_TRUE(img.palette.empty()) << "grayscale carries no embedded colour";
}

// A 2-bit grayscale unpack must never leak bits from neighboring samples: every value stays in
// 0..3. (Failable: a wrong shift/mask in readSample would surface values > 3 here.)
TEST(Image, GrayscaleValuesStayWithinTwoBitRange) {
    const LoadedImage img = loadPngFromMemory(readFile(fixture("gray2.png")));
    for (const std::uint8_t v : img.indices) {
        EXPECT_LE(v, 3u) << "2-bit grey sample out of range — sub-byte unpack leaked bits";
    }
}

// ── Map import: a map PNG decodes to a uint16 IndexGrid of raw index values ───────────────────

// map16.png's exact 4×4 uint16 plane (tests/fixtures/gen_fixtures.py MAP16_4x4) — several values
// ABOVE 255 so an 8-bit decode would truncate them; this is what proves the wide map path.
constexpr std::uint16_t kMap16Plane[16] = {
    0,     1,     255,   256,
    257,   300,   1000,  4095,
    4096,  20000, 40000, 60000,
    65535, 2,     513,   128,
};

TEST(Image, MapPngDecodes16BitValuesAboveByteRange) {
    const IndexGrid grid = loadMapPngFromMemory(readFile(fixture("map16.png")));

    EXPECT_EQ(grid.width, 4);
    EXPECT_EQ(grid.height, 4);
    ASSERT_EQ(grid.values.size(), 16u);
    for (std::size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(grid.values[i], kMap16Plane[i]) << "map value mismatch at pixel " << i;
    }
    // The headline: a value an 8-bit path could not carry, addressed via at(x, y).
    EXPECT_EQ(grid.at(3, 2), 60000) << "16-bit sample must survive widened, not truncate to a byte";
    EXPECT_EQ(grid.at(0, 3), 65535) << "the full uint16 range must round-trip";
}

TEST(Image, MapPngFilePathMatchesMemory) {
    // loadMapPng's LoadFromPath disk read: point the asset root at the fixtures dir and load by a literal
    // logical path. The result must match decoding the same file's bytes directly.
    setAssetRoot(RETROPP_FIXTURES_DIR);
    const std::vector<std::uint8_t> bytes = readFile(fixture("map16.png"));
    const IndexGrid fromMem  = loadMapPngFromMemory(bytes);
    const IndexGrid fromPath = loadMapPng("map16.png", AssetPolicy::LoadFromPath);

    EXPECT_EQ(fromPath.width, fromMem.width);
    EXPECT_EQ(fromPath.height, fromMem.height);
    EXPECT_EQ(fromPath.values, fromMem.values);
}

// A sub-byte grayscale map widens into the same uint16 grid (the map path is not 16-bit-only — a
// small index space decodes too). gray2.png is the 2-bit diagonal plane; as a map its values are
// the same 0..3 indices, widened.
TEST(Image, MapPngEightBitOrSubByteGrayscaleWidens) {
    const IndexGrid grid = loadMapPngFromMemory(readFile(fixture("gray2.png")));
    EXPECT_EQ(grid.width, 4);
    EXPECT_EQ(grid.height, 4);
    ASSERT_EQ(grid.values.size(), 16u);
    for (std::size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(grid.values[i], static_cast<std::uint16_t>(kDiagonalPlane[i]))
            << "sub-byte grey map value mismatch at pixel " << i;
    }
}

// A map carries indices, not color — a truecolor source is rejected (distinct message from the
// art path's RGBA-deferred seam: the map error names "truecolor", not "B.3.b").
TEST(Image, MapPngTruecolorRejected) {
    const std::span<const std::uint8_t> bytes{kTruecolorPng, sizeof(kTruecolorPng)};
    try {
        (void)loadMapPngFromMemory(bytes);
        FAIL() << "expected a truecolour map PNG to be rejected";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string{e.what()}.find("truecolour"), std::string::npos)
            << "map error should name the rejected truecolour source: " << e.what();
    }
}

TEST(Image, MapPngMissingFileThrows) {
    setAssetRoot(RETROPP_FIXTURES_DIR);
    EXPECT_THROW((void)loadMapPng("does_not_exist_map.png", AssetPolicy::LoadFromPath),
                 std::runtime_error);
}

// ── The shipped demo asset decodes as an indexed image with index-0 holes ─────────────────────

TEST(Image, DemoTilesAssetDecodesWithIndexZeroHoles) {
    const LoadedImage img = loadPng(std::string{RETROPP_ASSETS_DIR} + "/demo_tiles.png");

    EXPECT_EQ(img.kind, ImageColorKind::Indexed);
    EXPECT_EQ(img.width, 16);
    EXPECT_EQ(img.height, 16);
    ASSERT_EQ(img.indices.size(), 256u);
    ASSERT_EQ(img.palette.size(), 4u);

    // The asset's central diamond is the transparent index 0 (the hole the demo punches through to
    // the lower layer); the corners are opaque colored bands. Pin both so the asset can't silently
    // change out from under the transparent-index demo.
    EXPECT_EQ(img.indices[8 * 16 + 8], 0u) << "centre pixel should be the index-0 hole";
    EXPECT_NE(img.indices[0], 0u) << "corner pixel should be an opaque coloured band";
    EXPECT_NE(std::find(img.indices.begin(), img.indices.end(), std::uint8_t{0}), img.indices.end())
        << "the asset must contain index-0 holes for the transparent-index demo";
}

// ── loadPng(path) delegates to the same bytes loadPngFromMemory decodes ───────────────────────

TEST(Image, LoadPngFilePathMatchesMemory) {
    const std::vector<std::uint8_t> bytes = readFile(fixture("indexed4.png"));
    const LoadedImage fromMem  = loadPngFromMemory(bytes);
    const LoadedImage fromPath = loadPng(fixture("indexed4.png"));

    EXPECT_EQ(fromPath.kind, fromMem.kind);
    EXPECT_EQ(fromPath.width, fromMem.width);
    EXPECT_EQ(fromPath.height, fromMem.height);
    EXPECT_EQ(fromPath.indices, fromMem.indices);
    EXPECT_EQ(fromPath.palette, fromMem.palette);
}

// ── Routing / error seams ─────────────────────────────────────────────────────────────────────

// The art path decodes truecolor into the RGBA pixel plane (kind == Rgba); an RGB source is opaque
// (alpha widens to the 16-bit max). Exact per-pixel values are pinned in image_rgba_decode_test.cpp
// against known fixtures; this is the routing smoke check.
TEST(Image, TruecolorDecodesToRgbaKind) {
    const LoadedImage img = loadPngFromMemory(std::span<const std::uint8_t>(kTruecolorPng, sizeof(kTruecolorPng)));
    EXPECT_EQ(img.kind, ImageColorKind::Rgba);
    EXPECT_EQ(img.width, 2);
    EXPECT_EQ(img.height, 2);
    ASSERT_EQ(img.pixels.size(), 4u);
    EXPECT_TRUE(img.indices.empty()) << "an RGBA source carries no index plane";
    for (const Rgba16& px : img.pixels) {
        EXPECT_EQ(px.a, 65535u) << "an RGB (no-alpha) source decodes opaque";
    }
}

TEST(Image, CorruptBytesThrow) {
    const std::uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    EXPECT_THROW((void)loadPngFromMemory(std::span<const std::uint8_t>(garbage, sizeof(garbage))),
                 std::runtime_error);
}

TEST(Image, MissingFileThrows) {
    EXPECT_THROW((void)loadPng(fixture("does_not_exist.png")), std::runtime_error);
}

}  // namespace
}  // namespace retropp
