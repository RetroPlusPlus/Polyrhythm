#include "assets.h"

#include <algorithm>
#include <chrono>
#include <span>

#include "retropp/asset_policy.h"  // AssetPolicy (Embed)
#include "retropp/geometry.h"      // AssetDimensions

namespace vant {

using namespace retropp;
using namespace std::chrono_literals;

namespace {

// Scale a color channel-wise (the one helper behind every "same ramp, darker" palette variant).
Rgba8 scale(Rgba8 c, float f) {
    auto ch = [&](std::uint8_t v) {
        return static_cast<std::uint8_t>(std::clamp(static_cast<int>(static_cast<float>(v) * f), 0, 255));
    };
    return Rgba8{ch(c.r), ch(c.g), ch(c.b)};
}

}  // namespace

VantAssets loadVantAssets(Renderer& renderer) {
    VantAssets a;

    // ── The three sheets, ALL Embed as literal per-call tokens — the binary is self-contained.
    a.font  = renderer.loadAtlas("examples/vantium/assets/vantium_font.png",
                                 AssetDimensions{16, 16}, ContentKind::Tileset,
                                 ReadOrder::LeftRightThenDown, /*count=*/0, TransparentIndices::None,
                                 /*framesPerAnimation=*/0, AssetPolicy::Embed);
    a.tiles = renderer.loadAtlas("examples/vantium/assets/vantium_tiles.png",
                                 AssetDimensions{16, 16}, ContentKind::Tileset,
                                 ReadOrder::LeftRightThenDown, /*count=*/0,
                                 TransparentIndices::of({0}),   // deck holes reveal the starfield
                                 /*framesPerAnimation=*/0, AssetPolicy::Embed);
    a.sheet = renderer.loadAtlas("examples/vantium/assets/vantium_sprites.png",
                                 AssetDimensions{48, 24}, ContentKind::SpriteSeries,
                                 ReadOrder::LeftRightThenDown, /*count=*/0, TransparentIndices::GameBoy,
                                 /*framesPerAnimation=*/0, AssetPolicy::Embed);

    // ── Sprite palettes (≤8 entries each), in Pal-enum order. ─────────────────────────────────
    auto up = [&](std::initializer_list<Rgba8> cs) {
        std::array<Rgba8, 8> p{};
        std::size_t i = 0;
        for (Rgba8 c : cs) p[i++] = c;
        a.spritePals.push_back(renderer.uploadPalette(std::span<const Rgba8>(p.data(), i)));
    };
    // The Manta: steel ramp, cyan canopy (6), white spine (7).
    up({{0, 0, 0}, {32, 40, 54}, {56, 68, 92}, {76, 92, 120}, {102, 120, 143},
        {132, 148, 172}, {82, 216, 232}, {240, 244, 255}});
    // Four fighter liveries: one dart shape, four tinted ramps + a contrasting canopy glow.
    up({{0, 0, 0}, {52, 22, 24}, {96, 40, 40}, {140, 58, 52}, {184, 84, 64},
        {216, 120, 88}, {255, 214, 92}, {255, 240, 210}});     // rust red
    up({{0, 0, 0}, {18, 46, 40}, {32, 82, 66}, {48, 118, 92}, {70, 152, 116},
        {104, 188, 144}, {255, 170, 80}, {230, 255, 230}});    // viridian
    up({{0, 0, 0}, {44, 28, 60}, {74, 46, 104}, {106, 66, 146}, {140, 92, 184},
        {172, 128, 214}, {120, 255, 200}, {244, 230, 255}});   // violet
    up({{0, 0, 0}, {58, 40, 14}, {102, 70, 22}, {148, 102, 30}, {190, 136, 44},
        {222, 172, 66}, {110, 220, 255}, {255, 244, 200}});    // amber
    // Mine: dark husk, red glow core.
    up({{0, 0, 0}, {26, 28, 36}, {46, 50, 60}, {64, 68, 80}, {88, 92, 104},
        {110, 116, 128}, {190, 40, 32}, {255, 96, 64}});
    // Bolt: cyan-white beam. Enemy shot: amber. Booms: a fire ramp.
    up({{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
        {60, 160, 190}, {140, 230, 245}, {235, 255, 255}});
    up({{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
        {180, 110, 30}, {235, 170, 60}, {255, 230, 150}});
    up({{0, 0, 0}, {70, 20, 12}, {120, 36, 16}, {170, 60, 20}, {210, 96, 28},
        {240, 140, 40}, {252, 196, 72}, {255, 245, 200}});

    // ── Tile palettes: the steel-teal hull family + accents (all ≤8 entries). ────────────────
    const std::array<Rgba8, 8> steel{{{0, 0, 0}, {16, 20, 31}, {35, 44, 64}, {53, 66, 92},
                                      {74, 90, 118}, {98, 116, 144}, {130, 150, 176}, {184, 200, 220}}};
    auto tp = [&](const std::array<Rgba8, 8>& p) {
        return renderer.uploadPalette(std::span<const Rgba8>(p));
    };
    auto scaled = [&](const std::array<Rgba8, 8>& p, float f) {
        std::array<Rgba8, 8> out{};
        for (std::size_t i = 1; i < 8; ++i) out[i] = scale(p[i], f);
        return out;
    };
    a.tilePals[TP_DECK]        = tp(steel);
    a.tilePals[TP_DECK_SHADOW] = tp(scaled(steel, 0.55f));   // the same art, in the structures' shade
    a.tilePals[TP_HAZARD] = tp({{{0, 0, 0}, {10, 12, 18}, {24, 26, 32}, {60, 66, 80}, {90, 96, 110},
                                 {120, 126, 140}, {224, 160, 40}, {248, 240, 216}}});
    a.tilePals[TP_STRUCT] = tp({{{0, 0, 0}, {22, 28, 42}, {46, 58, 82}, {70, 86, 116}, {96, 114, 146},
                                 {126, 146, 178}, {166, 186, 214}, {232, 240, 255}}});
    a.tilePals[TP_ROTOR]  = tp({{{0, 0, 0}, {14, 18, 28}, {32, 40, 58}, {54, 66, 90}, {78, 92, 118},
                                 {104, 120, 148}, {56, 192, 208}, {160, 240, 248}}});
    a.tilePals[TP_VENT]   = tp(scaled(steel, 0.8f));
    a.tilePals[TP_POD_A]  = tp({{{0, 0, 0}, {20, 24, 34}, {40, 48, 64}, {58, 70, 92}, {80, 96, 120},
                                 {104, 122, 148}, {176, 116, 36}, {232, 176, 80}}});   // glow, dim
    a.tilePals[TP_POD_B]  = tp({{{0, 0, 0}, {20, 24, 34}, {40, 48, 64}, {58, 70, 92}, {80, 96, 120},
                                 {104, 122, 148}, {224, 156, 52}, {255, 226, 140}}});  // glow, bright
    a.tilePals[TP_STRIP]  = tp({{{0, 0, 0}, {14, 18, 26}, {30, 38, 52}, {48, 58, 76}, {70, 84, 104},
                                 {94, 110, 132}, {130, 148, 170}, {240, 248, 252}}});
    a.tilePals[TP_STAR_A] = tp({{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {76, 90, 128}, {0, 0, 0},
                                 {150, 168, 208}, {0, 0, 0}, {240, 244, 255}}});       // twinkle, bright
    a.tilePals[TP_STAR_B] = tp({{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {56, 66, 96}, {0, 0, 0},
                                 {180, 196, 230}, {0, 0, 0}, {200, 208, 230}}});       // twinkle, shifted

    // ── Rich-font palettes: 0 = the HUD bar, 1 = outline, 2 = shadow, 4..7 = the gradient. ────
    auto fp = [&](Rgba8 g4, Rgba8 g5, Rgba8 g6, Rgba8 g7) {
        const std::array<Rgba8, 8> p{{{16, 20, 32}, {6, 10, 16}, {20, 28, 44}, {0, 0, 0},
                                      g4, g5, g6, g7}};
        return renderer.uploadPalette(std::span<const Rgba8>(p));
    };
    a.textPals[TXT_WHITE] = fp({136, 148, 168}, {170, 182, 200}, {208, 218, 232}, {244, 248, 255});
    a.textPals[TXT_GOLD]  = fp({138, 100, 32}, {180, 136, 48}, {220, 174, 72}, {248, 220, 136});
    a.textPals[TXT_CYAN]  = fp({42, 120, 136}, {63, 160, 176}, {100, 200, 212}, {168, 236, 244});

    // ── The shared clips. The boom is FRAME animation (art changes); the pod pulse and the star
    //    twinkle are PALETTE animation (the art holds, the colors breathe) — both idioms, live.
    a.boomClip = Animation{{
        {.label = "flash",  .sheet = a.sheet.atlasId, .tileIndex = S_BOOM0, .palette = a.spritePals[PAL_BOOM], .duration = 70ms},
        {.label = "blast",  .sheet = a.sheet.atlasId, .tileIndex = S_BOOM1, .palette = a.spritePals[PAL_BOOM], .duration = 70ms},
        {.label = "ring",   .sheet = a.sheet.atlasId, .tileIndex = S_BOOM2, .palette = a.spritePals[PAL_BOOM], .duration = 70ms},
        {.label = "embers", .sheet = a.sheet.atlasId, .tileIndex = S_BOOM3, .palette = a.spritePals[PAL_BOOM], .duration = 90ms},
    }};
    a.podPulse = Animation{{
        {.label = "dim",    .sheet = a.tiles.atlasId, .tileIndex = T_POD_TOP, .palette = a.tilePals[TP_POD_A], .duration = 500ms},
        {.label = "bright", .sheet = a.tiles.atlasId, .tileIndex = T_POD_TOP, .palette = a.tilePals[TP_POD_B], .duration = 500ms},
    }};
    a.starTwinkle = Animation{{
        {.label = "phaseA", .sheet = a.tiles.atlasId, .tileIndex = T_STAR_A, .palette = a.tilePals[TP_STAR_A], .duration = 800ms},
        {.label = "phaseB", .sheet = a.tiles.atlasId, .tileIndex = T_STAR_A, .palette = a.tilePals[TP_STAR_B], .duration = 800ms},
    }};

    return a;
}

}  // namespace vant
