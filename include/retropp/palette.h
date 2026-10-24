#pragma once

#include <cstdint>

namespace retropp {

// Color resources for the indexed-tile / runtime-palette pipeline.
//
// The faithful GB/C color model: tile art is INDICES (0..N-1), and the color comes from a
// palette selected at render time — never baked into the art. An indexed atlas holds one
// palette index per pixel; a palette maps each index to a final output color; a tile picks
// which palette (within its layer's set) it draws from. Application happens per-pixel in the
// fragment shader, so palette swaps, day/night, and animation stay data/shader concerns —
// never a palette-RAM poke or scanline idiom.

// One palette entry: a final output color. Identity is the named channels — never a
// positional quad. Default is opaque black. The platform stores Rgba8 output regardless of the
// source console; a palette's colors come from the game's assets at upload time.
struct Rgba8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
    [[nodiscard]] constexpr bool operator==(const Rgba8&) const noexcept = default;
};
static_assert(sizeof(Rgba8) == 4);

// A 16-bit-per-channel output color, for color sources that carry more than 8 bits of precision
// (a 16-bit truecolor PNG). Same named-channel identity as Rgba8 — never a positional quad — and an
// aggregate (no user constructors) so it stays designated-init friendly. Default is opaque black. The
// palette store keeps 16-bit channels internally; an 8-bit source widens losslessly into it (see widen).
struct Rgba16 {
    std::uint16_t r = 0;
    std::uint16_t g = 0;
    std::uint16_t b = 0;
    std::uint16_t a = 65535;
    [[nodiscard]] constexpr bool operator==(const Rgba16&) const noexcept = default;
};
static_assert(sizeof(Rgba16) == 8);

// Widen an 8-bit channel to 16 bits losslessly: v * 257 maps 0 → 0 and 255 → 65535 exactly (0x101 is
// 65535/255), so the 8-bit endpoints land on the 16-bit endpoints and an 8-bit color round-trips
// byte-exact through the 16-bit store. (v << 8 would map 255 → 0xFF00, dropping the top end.)
[[nodiscard]] constexpr std::uint16_t widen8(std::uint8_t v) noexcept {
    return static_cast<std::uint16_t>(static_cast<unsigned>(v) * 257u);
}

// Widen an 8-bit color to 16-bit per channel, each channel via widen8 (so opaque 8-bit alpha 255 →
// 65535). The bridge from the public Rgba8 hand-authoring type to the 16-bit palette store.
[[nodiscard]] constexpr Rgba16 widen(Rgba8 c) noexcept {
    return Rgba16{widen8(c.r), widen8(c.g), widen8(c.b), widen8(c.a)};
}

// A handle to uploaded palette color data the renderer owns, and ALSO its flat offset into the
// palette store — a PaletteId's underlying value IS its offset, so content carries it directly and
// the shader reads it with no indirection. Identity is the typed handle. Sibling to AtlasId. 16-bit:
// palette data is hundreds to low-thousands of entries, so 65,536 is generous headroom and keeps a
// tilemap cell tight (these cells stream to the GPU every frame).
enum class PaletteId : std::uint16_t {};

// Named entry-count presets. The enumerator VALUE is the entry count, so a caller passes a
// raw integer or a preset interchangeably (static_cast<std::uint32_t>(PaletteSize::GameBoy) == 4).
// These are count mnemonics, not per-console color models — the platform stores Rgba8 output
// regardless; the preset sets only how many entries. The platform generalizes beyond GB, so the
// entry count is data (a span length), never a constant baked into the type.
enum class PaletteSize : std::uint32_t {
    GameBoy      = 4,
    GameBoyColor = 4,
    Nes          = 4,
    MasterSystem = 16,
    Genesis      = 16,
    Snes         = 16,
};

}  // namespace retropp
