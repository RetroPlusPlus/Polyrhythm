#include "assets.h"

#include <chrono>
#include <span>

#include "retropp/asset_policy.h"  // AssetPolicy (Embed / LoadFromPath)
#include "retropp/geometry.h"      // AssetDimensions

namespace polter {

using namespace retropp;
using namespace std::chrono_literals;

namespace {

// The one dark background every tile/text palette shares as entry 0 — the court, the HUD field, and
// every corridor floor read as the same void, so the playfield is a single surface, not patches.
constexpr Rgba8 kBg{14, 16, 30};

}  // namespace

PolterAssets loadPolterAssets(Renderer& renderer) {
    PolterAssets a;

    // ── Load the committed indexed PNGs. ALL Embed (baked into the binary by the build scan) — the
    //    policy is decided HERE, per loadAtlas call, as a literal token; no build rule, no copy rule.
    //    Polterball sets no engine-wide default and relies on none.
    a.font  = renderer.loadAtlas("examples/polterball/assets/polterball_font.png",
                                 AssetDimensions{kTile, kTile}, ContentKind::Tileset,
                                 ReadOrder::LeftRightThenDown, /*count=*/0, TransparentIndices::None,
                                 /*framesPerAnimation=*/0, AssetPolicy::Embed);
    a.tiles = renderer.loadAtlas("examples/polterball/assets/polterball_tiles.png",
                                 AssetDimensions{kCell, kCell}, ContentKind::Tileset,
                                 ReadOrder::LeftRightThenDown, /*count=*/0, TransparentIndices::None,
                                 /*framesPerAnimation=*/0, AssetPolicy::Embed);
    a.sheet = renderer.loadAtlas("examples/polterball/assets/polterball_sprites.png",
                                 AssetDimensions{80, 24}, ContentKind::SpriteSeries,
                                 ReadOrder::LeftRightThenDown, /*count=*/0, TransparentIndices::GameBoy,
                                 /*framesPerAnimation=*/0, AssetPolicy::Embed);

    // ── Sprite palettes, in the Pal-enum order. One ghost SHAPE serves the whole cast: art index
    //    1 = body, 2 = eye white, 3 = pupil, so a ghost's color — and the frightened blue — is
    //    nothing but palette selection.
    const Rgba8 eyeWhite{240, 240, 250};
    const Rgba8 pupil{40, 40, 90};
    auto up4 = [&](Rgba8 c1, Rgba8 c2, Rgba8 c3) {
        const std::array<Rgba8, 4> p{{{0, 0, 0}, c1, c2, c3}};
        a.spritePals.push_back(renderer.uploadPalette(std::span<const Rgba8>(p)));
    };
    up4({230, 60, 50}, eyeWhite, pupil);            // PAL_GHOST_0 — the Chaser, red
    up4({240, 130, 190}, eyeWhite, pupil);          // PAL_GHOST_1 — the Ambusher, pink
    up4({240, 160, 60}, eyeWhite, pupil);           // PAL_GHOST_2 — the Wanderer, orange
    up4({50, 70, 200}, {210, 190, 160}, eyeWhite);  // PAL_FRIGHT — deep blue, washed-out face
    up4(eyeWhite, eyeWhite, {60, 80, 200});         // PAL_EYES — white ovals, blue pupils
    up4({245, 240, 190}, {255, 255, 255}, {150, 120, 60});  // PAL_BALL — warm white
    up4({255, 150, 40}, {255, 230, 120}, {200, 60, 20});    // PAL_BALL_FIRE — ignited
    up4({90, 150, 235}, {170, 210, 255}, {40, 80, 160});    // PAL_PADDLE — cyan-blue
    up4({255, 210, 90}, {255, 255, 220}, {0, 0, 0});        // PAL_POW_A — the pulse's dim phase
    up4({255, 245, 190}, {255, 255, 255}, {0, 0, 0});       // PAL_POW_B — its bright phase

    // ── Tile palettes (entry 0 is the shared dark background — the tile path DRAWS entry 0).
    auto tp = [&](Rgba8 c1, Rgba8 c2, Rgba8 c3) {
        const std::array<Rgba8, 4> p{{kBg, c1, c2, c3}};
        return renderer.uploadPalette(std::span<const Rgba8>(p));
    };
    a.tilePals[TP_WALL]   = tp({60, 90, 200}, {130, 160, 240}, {25, 40, 110});   // cold blue masonry
    a.tilePals[TP_SOFT]   = tp({170, 110, 70}, {210, 160, 110}, {90, 50, 30});   // warm crackable rust
    a.tilePals[TP_PELLET] = tp({240, 220, 170}, {255, 250, 220}, kBg);           // cream dots
    a.tilePals[TP_GATE]   = tp({240, 200, 220}, {255, 240, 250}, {150, 110, 140});  // the pen bar

    // ── Text palettes: every font cell shares the dark background (entry 0); entry 1 is the lit
    //    glyph color. Blank cells render as the background, so the text layer is the backdrop too.
    const std::array<Rgba8, 2> textWhite{{kBg, {235, 238, 248}}};
    const std::array<Rgba8, 2> textGold{{kBg, {236, 196, 96}}};
    const std::array<Rgba8, 2> textCyan{{kBg, {130, 220, 230}}};
    a.textPals = {
        renderer.uploadPalette(std::span<const Rgba8>(textWhite)),
        renderer.uploadPalette(std::span<const Rgba8>(textGold)),
        renderer.uploadPalette(std::span<const Rgba8>(textCyan)),
    };

    // ── The shared clips. The power pellet's pulse is PALETTE animation — the same art cell under
    //    two alternating palettes, the engine's palette-cycling idiom (vary .palette, hold the art).
    //    The ghosts' walk varies the ART (frame A/B skirt wave) and ignores the frame palette — the
    //    renderer picks each ghost's palette by role/state and reads only WHICH frame is current.
    a.powerPulse = Animation{{
        {.label = "dim",    .sheet = a.sheet.atlasId, .tileIndex = S_POWER, .palette = a.spritePals[PAL_POW_A], .duration = 400ms},
        {.label = "bright", .sheet = a.sheet.atlasId, .tileIndex = S_POWER, .palette = a.spritePals[PAL_POW_B], .duration = 400ms},
    }};
    a.ghostWalk = Animation{{
        {.label = "stepA", .sheet = a.sheet.atlasId, .tileIndex = S_GHOST_A, .palette = a.spritePals[PAL_GHOST_0], .duration = 250ms},
        {.label = "stepB", .sheet = a.sheet.atlasId, .tileIndex = S_GHOST_B, .palette = a.spritePals[PAL_GHOST_0], .duration = 250ms},
    }};

    return a;
}

}  // namespace polter
