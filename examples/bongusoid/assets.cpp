#include "assets.h"

#include <algorithm>
#include <array>
#include <span>

#include "retropp/asset_policy.h"  // AssetPolicy (Embed / LoadFromPath)
#include "retropp/geometry.h"      // AssetDimensions
#include "retropp/palette.h"       // Rgba8

namespace bong {

using namespace retropp;

namespace {

// Scale a color channel-wise by f (clamped) — builds the light/dark shades of a brick palette.
Rgba8 scale(Rgba8 c, float f) {
    auto ch = [&](std::uint8_t v) {
        return static_cast<std::uint8_t>(std::clamp(static_cast<int>(static_cast<float>(v) * f), 0, 255));
    };
    return Rgba8{ch(c.r), ch(c.g), ch(c.b)};
}

// A {transparent, main, light, dark} sprite palette from a base color (index 0 transparent).
std::array<Rgba8, 4> brickPal(Rgba8 base) {
    return std::array<Rgba8, 4>{{ {0, 0, 0}, base, scale(base, 1.30f), scale(base, 0.62f) }};
}

}  // namespace

BongAssets loadBongAssets(Renderer& renderer) {
    BongAssets a;

    // ── Load the committed indexed PNGs. S2: BOTH Embed (baked into the binary by the build scan) —
    //    the policy is decided HERE, per loadAtlas call; no build rule, no copy rule. Bongusoid sets
    //    no EngineConfig default and relies on none: every asset states its policy at the call site.
    a.font  = renderer.loadAtlas("examples/bongusoid/assets/bongusoid_font.png", AssetDimensions{kTile, kTile},
                                 ContentKind::Tileset, ReadOrder::LeftRightThenDown,
                                 /*count=*/0, TransparentIndices::None, /*framesPerAnimation=*/0,
                                 AssetPolicy::Embed);
    a.sheet = renderer.loadAtlas("examples/bongusoid/assets/bongusoid_sprites.png", AssetDimensions{80, 24},
                                 ContentKind::SpriteSeries, ReadOrder::LeftRightThenDown,
                                 /*count=*/0, TransparentIndices::GameBoy, /*framesPerAnimation=*/0,
                                 AssetPolicy::Embed);

    // ── Sprite-layer palette set: 6 brick-row colors, then silver(full/cracked), gold, paddle, ball.
    const std::array<Rgba8, kBrickRowPals> rowColours{{
        {220, 70, 70}, {230, 140, 55}, {228, 210, 80}, {95, 200, 100}, {75, 185, 215}, {130, 130, 235},
    }};
    auto upPal = [&](const std::array<Rgba8, 4>& p) {
        a.spritePals.push_back(renderer.uploadPalette(std::span<const Rgba8>(p)));
    };
    for (Rgba8 c : rowColours) upPal(brickPal(c));     // 0..5
    upPal(brickPal({185, 190, 200}));                  // 6  silver
    upPal(brickPal({130, 134, 146}));                  // 7  silver cracked (dimmer)
    upPal(brickPal({226, 188, 70}));                   // 8  gold
    upPal(brickPal({90, 150, 235}));                   // 9  paddle
    upPal(brickPal({245, 230, 160}));                  // 10 ball

    // ── Text-layer set: every font cell shares a dark background (palette entry 0); entry 1 is the lit
    //    glyph color. Blank cells render as the dark background, so this one layer is the backdrop too.
    const Rgba8 kBg{16, 18, 28};
    const std::array<Rgba8, 2> textWhite{{ kBg, {235, 238, 248} }};
    const std::array<Rgba8, 2> textGold {{ kBg, {236, 196, 96} }};
    const std::array<Rgba8, 2> textCyan {{ kBg, {130, 220, 230} }};
    a.textPals = {
        renderer.uploadPalette(std::span<const Rgba8>(textWhite)),
        renderer.uploadPalette(std::span<const Rgba8>(textGold)),
        renderer.uploadPalette(std::span<const Rgba8>(textCyan)),
    };

    return a;
}

}  // namespace bong
