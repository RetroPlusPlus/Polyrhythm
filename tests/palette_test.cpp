#include "retropp/palette.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace retropp {

// Rgba8 is the engine's final output color: four named channels, opaque by default, 4 bytes
// so it uploads as a tightly-packed RGBA8 texture row with no padding.

TEST(Palette, Rgba8IsFourTightlyPackedBytes) {
    static_assert(sizeof(Rgba8) == 4, "Rgba8 must upload as packed RGBA8");
    EXPECT_EQ(sizeof(Rgba8), 4u);
}

TEST(Palette, Rgba8DefaultsToOpaqueBlack) {
    constexpr Rgba8 c{};
    EXPECT_EQ(c.r, 0);
    EXPECT_EQ(c.g, 0);
    EXPECT_EQ(c.b, 0);
    EXPECT_EQ(c.a, 255);  // opaque default — never accidentally transparent
}

TEST(Palette, Rgba8NamedFieldsAndEquality) {
    constexpr Rgba8 a{10, 20, 30, 40};
    constexpr Rgba8 b{10, 20, 30, 40};
    constexpr Rgba8 c{10, 20, 30, 41};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_EQ(a.r, 10);
    EXPECT_EQ(a.g, 20);
    EXPECT_EQ(a.b, 30);
    EXPECT_EQ(a.a, 40);
}

// Rgba16 is the 16-bit-per-channel output color: four named channels, opaque by default, 8 bytes
// so it uploads as a tightly-packed RGBA16 texture row with no padding.

TEST(Palette, Rgba16IsEightTightlyPackedBytes) {
    static_assert(sizeof(Rgba16) == 8, "Rgba16 must upload as packed RGBA16");
    EXPECT_EQ(sizeof(Rgba16), 8u);
}

TEST(Palette, Rgba16DefaultsToOpaqueBlack) {
    constexpr Rgba16 c{};
    EXPECT_EQ(c.r, 0u);
    EXPECT_EQ(c.g, 0u);
    EXPECT_EQ(c.b, 0u);
    EXPECT_EQ(c.a, 65535u);  // opaque default — the 16-bit max, never accidentally transparent
}

TEST(Palette, Rgba16NamedFieldsAndEquality) {
    constexpr Rgba16 a{1000, 2000, 3000, 4000};
    constexpr Rgba16 b{1000, 2000, 3000, 4000};
    constexpr Rgba16 c{1000, 2000, 3000, 4001};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_EQ(a.r, 1000u);
    EXPECT_EQ(a.g, 2000u);
    EXPECT_EQ(a.b, 3000u);
    EXPECT_EQ(a.a, 4000u);
}

// widen8 maps the 8-bit endpoints onto the 16-bit endpoints exactly (×257). This identity is what
// makes the 16-bit palette store behavior-preserving for 8-bit palettes: 255 → 65535, not 0xFF00.
TEST(Palette, Widen8MapsEndpointsExactly) {
    static_assert(widen8(0) == 0u, "0 must widen to 0");
    static_assert(widen8(255) == 65535u, "255 must widen to the 16-bit max, not 0xFF00");
    static_assert(widen8(128) == 32896u, "128 * 257 == 32896");
    EXPECT_EQ(widen8(0), 0u);
    EXPECT_EQ(widen8(255), 65535u);
    EXPECT_EQ(widen8(128), 32896u);
    EXPECT_EQ(widen8(1), 257u);
}

// widen(Rgba8) widens every channel, including alpha: an opaque 8-bit color stays opaque (65535).
TEST(Palette, WidenColorWidensEveryChannel) {
    constexpr Rgba16 w = widen(Rgba8{0, 128, 255, 255});
    EXPECT_EQ(w.r, 0u);
    EXPECT_EQ(w.g, 32896u);
    EXPECT_EQ(w.b, 65535u);
    EXPECT_EQ(w.a, 65535u);
}

// PaletteSize presets are count mnemonics: the enumerator VALUE is the entry count, so a caller
// can pass a preset or a raw integer interchangeably.

TEST(Palette, SizePresetValuesEqualEntryCounts) {
    EXPECT_EQ(static_cast<std::uint32_t>(PaletteSize::GameBoy), 4u);
    EXPECT_EQ(static_cast<std::uint32_t>(PaletteSize::GameBoyColor), 4u);
    EXPECT_EQ(static_cast<std::uint32_t>(PaletteSize::Nes), 4u);
    EXPECT_EQ(static_cast<std::uint32_t>(PaletteSize::MasterSystem), 16u);
    EXPECT_EQ(static_cast<std::uint32_t>(PaletteSize::Genesis), 16u);
    EXPECT_EQ(static_cast<std::uint32_t>(PaletteSize::Snes), 16u);
}

TEST(Palette, SizeRoundTripsRawInteger) {
    // A raw entry count constructs the matching preset value and back — the interchangeability.
    constexpr auto fromRaw = static_cast<PaletteSize>(4u);
    EXPECT_EQ(fromRaw, PaletteSize::GameBoy);
    EXPECT_EQ(static_cast<std::uint32_t>(static_cast<PaletteSize>(16u)), 16u);
}

}  // namespace retropp
