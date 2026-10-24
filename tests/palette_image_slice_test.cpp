#include "retropp/image.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// Palette-image slicing: readOrderCells (the shared read-order walk) + slicePaletteImage (a color PNG
// read one-pixel-per-entry into Rgba16 palette colors). Pure CPU, fully headless. The palette_strip.png
// fixture encodes each pixel's grid position in its color (r = col·10, g = row·10), so the entry order
// a ReadOrder produces is verifiable from the colors alone. Authored by tests/fixtures/gen_fixtures.py.

namespace retropp {
namespace {

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in) << "could not open fixture: " << path;
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::string fixture(const char* name) { return std::string{RETROPP_FIXTURES_DIR} + "/" + name; }

// The strip encodes position in color: r = col·10, g = row·10, each widened ×257 at 8-bit decode. So a
// decoded entry's grid cell is recoverable — col = r / 2570, row = g / 2570 (2570 == 10·257).
GridCell cellOf(const Rgba16& e) {
    return GridCell{static_cast<int>(e.r) / 2570, static_cast<int>(e.g) / 2570};
}

LoadedImage strip() { return loadPngFromMemory(readFile(fixture("palette_strip.png"))); }

// ── readOrderCells: the shared walk ──────────────────────────────────────────────────────────────

TEST(ReadOrderCells, RowMajorIsDefaultOrder) {
    const std::vector<GridCell> cells = readOrderCells(2, 2, ReadOrder::LeftRightThenDown);
    const std::vector<GridCell> expected{{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    EXPECT_EQ(cells, expected);
}

TEST(ReadOrderCells, ColumnsFillWalksDownThenAcross) {
    const std::vector<GridCell> cells = readOrderCells(2, 2, ReadOrder::TopBottomThenRight);
    const std::vector<GridCell> expected{{0, 0}, {0, 1}, {1, 0}, {1, 1}};
    EXPECT_EQ(cells, expected);
}

TEST(ReadOrderCells, RightToLeftReversesTheRow) {
    const std::vector<GridCell> cells = readOrderCells(3, 1, ReadOrder::RightLeftThenDown);
    const std::vector<GridCell> expected{{2, 0}, {1, 0}, {0, 0}};
    EXPECT_EQ(cells, expected);
}

TEST(ReadOrderCells, CountCapsAndClampsAboveCapacity) {
    EXPECT_EQ(readOrderCells(4, 2, ReadOrder::LeftRightThenDown, 3).size(), 3u);   // first 3 only
    EXPECT_EQ(readOrderCells(4, 2, ReadOrder::LeftRightThenDown, 100).size(), 8u); // clamp to capacity
    EXPECT_EQ(readOrderCells(4, 2, ReadOrder::LeftRightThenDown, 0).size(), 8u);   // 0 = all
}

TEST(ReadOrderCells, NonPositiveGridIsEmpty) {
    EXPECT_TRUE(readOrderCells(0, 4, ReadOrder::LeftRightThenDown).empty());
    EXPECT_TRUE(readOrderCells(4, -1, ReadOrder::LeftRightThenDown).empty());
}

// ── slicePaletteImage: pixels → Rgba16 entries in read order ──────────────────────────────────────

TEST(PaletteImageSlice, DefaultOrderIsRowMajorWithWidenedChannels) {
    const std::vector<Rgba16> entries = slicePaletteImage(strip());  // LeftRightThenDown

    // The full 4×2 strip in row-major order, each 8-bit channel widened ×257 (10→2570, 20→5140, …).
    const std::vector<Rgba16> expected{
        {0, 0, 0, 65535},    {2570, 0, 0, 65535}, {5140, 0, 0, 65535}, {7710, 0, 0, 65535},
        {0, 2570, 0, 65535}, {2570, 2570, 0, 65535}, {5140, 2570, 0, 65535}, {7710, 2570, 0, 65535},
    };
    EXPECT_EQ(entries, expected);
}

TEST(PaletteImageSlice, ReadOrderMapsPositionToEntry) {
    // Columns fill (down a column before stepping across): the cell sequence reorders, and every entry's
    // color-encoded position must match that sequence — proving slicePaletteImage rides readOrderCells.
    const std::vector<Rgba16> entries = slicePaletteImage(strip(), ReadOrder::TopBottomThenRight);
    ASSERT_EQ(entries.size(), 8u);
    const std::vector<GridCell> expectedCells{{0, 0}, {0, 1}, {1, 0}, {1, 1},
                                              {2, 0}, {2, 1}, {3, 0}, {3, 1}};
    for (std::size_t i = 0; i < entries.size(); ++i) {
        EXPECT_EQ(cellOf(entries[i]), expectedCells[i]) << "entry " << i << " out of read order";
    }
}

TEST(PaletteImageSlice, CountCapsEntries) {
    const std::vector<Rgba16> entries = slicePaletteImage(strip(), ReadOrder::LeftRightThenDown, 3);
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(cellOf(entries[0]), (GridCell{0, 0}));
    EXPECT_EQ(cellOf(entries[2]), (GridCell{2, 0}));
}

// 16-bit precision + alpha carry through the slicer — the whole point of palette images. A 16-bit
// source slices to entries with exact channels (incl. values above the 8-bit ceiling) and alpha (incl.
// a fully transparent entry). palette_strip16.png is 2×2, row-major (gen_fixtures.py PALETTE_STRIP16_2x2).
TEST(PaletteImageSlice, SixteenBitAndAlphaCarryThrough) {
    const std::vector<Rgba16> entries =
        slicePaletteImage(loadPngFromMemory(readFile(fixture("palette_strip16.png"))));
    const std::vector<Rgba16> expected{
        {65535, 0, 0, 65535},
        {0, 65535, 0, 32768},
        {0, 0, 65535, 0},
        {40000, 50000, 60000, 12345},
    };
    EXPECT_EQ(entries, expected);
    EXPECT_EQ(entries[1].a, 32768u) << "partial alpha must survive the slice";
    EXPECT_EQ(entries[2].a, 0u) << "a fully transparent entry must survive the slice";
    // 60000 is not a multiple of 257, so it cannot be an 8-bit channel widened ×257 — a genuine 16-bit
    // value that an 8-bit decode path could never produce.
    EXPECT_NE(entries[3].b % 257u, 0u) << "a genuine 16-bit channel (not an 8-bit ×257 widen) must survive";
}

TEST(PaletteImageSlice, IndexedSourceThrows) {
    // An indexed image carries indices, not colors — handing it to the palette slicer is a misuse.
    const LoadedImage indexed = loadPngFromMemory(readFile(fixture("indexed4.png")));
    ASSERT_EQ(indexed.kind, ImageColorKind::Indexed);
    EXPECT_THROW((void)slicePaletteImage(indexed), std::runtime_error);
}

}  // namespace
}  // namespace retropp
