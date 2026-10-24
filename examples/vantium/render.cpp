#include "render.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "retropp/geometry.h"
#include "retropp/transform.h"

namespace vant {

using namespace retropp;

VantRenderer::VantRenderer()
    : hudCells_(static_cast<std::size_t>(kMapW) * kMapH) {}

void VantRenderer::render(Renderer& renderer, VantGame& game, const VantAssets& assets,
                          const VantFeel& feel) {
    ++pulseTick_;

    // ── The deck's per-tick palette passes: the pod glow and the star twinkle each read their
    //    palette-cycling clip's CURRENT frame — animation.h driving tile color, no art churn.
    game.deck.setPodPalette(feel.podFrame().palette);
    game.deck.setStarPalette(feel.starFrame().palette);

    // ── The HUD / title layer (fixed, rich 16×16 glyphs stamped as 2×2 tile groups). ──────────
    auto stampRich = [&](int col16, int row8, std::string_view s, std::size_t pal) {
        for (std::size_t i = 0; i < s.size(); ++i) {
            const int tx = (col16 + static_cast<int>(i)) * 2;
            if (tx + 1 >= kMapW) break;
            const std::uint16_t base = assets.glyphBase(s[i]);
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    TileCell& cell = hudCells_[static_cast<std::size_t>(row8 + dy) * kMapW + tx + dx];
                    cell.tile    = static_cast<std::uint16_t>(base + dx + kFontStride8 * dy);
                    cell.atlas   = assets.fontAtlas();
                    cell.palette = assets.textPals[pal];
                }
            }
        }
    };
    auto centredRich = [&](int row8, std::string_view s, std::size_t pal) {
        stampRich((kMapW / 2 - static_cast<int>(s.size())) / 2, row8, s, pal);
    };

    // Clear to the space-glyph (its entry-0 background paints the HUD bar); only the HUD strip
    // and title rows carry cells with content, the rest of the layer is that flat bar color —
    // but we want the PLAY AREA transparent, so everything below the HUD strip clears to an
    // atlas hole instead (the deck sheet's fully-transparent empty cell).
    const std::uint16_t barTile  = assets.glyphBase(' ');
    const std::uint16_t holeTile = assets.artTile(T_EMPTY);
    for (int ty = 0; ty < kMapH; ++ty) {
        const bool hudStrip = ty < 6;   // y 0..47 — the framed HUD bar
        for (int tx = 0; tx < kMapW; ++tx) {
            TileCell& cell = hudCells_[static_cast<std::size_t>(ty) * kMapW + tx];
            if (hudStrip || game.state == GameState::Title) {
                cell.tile    = barTile;
                cell.atlas   = assets.fontAtlas();
                cell.palette = assets.textPals[TXT_WHITE];
            } else {
                cell.tile    = holeTile;
                cell.atlas   = assets.tileAtlas();
                cell.palette = assets.tilePals[TP_DECK];
            }
        }
    }

    if (game.state == GameState::Title) {
        centredRich(14, "VANTIUM", TXT_GOLD);
        centredRich(20, "PRESS ENTER TO FLY", TXT_CYAN);
        centredRich(26, "ARROWS FLY  A FIRES", TXT_WHITE);
        centredRich(29, "HOLD B TO ROLL THROUGH GAPS", TXT_WHITE);
        centredRich(32, "CLEAR THE WAVES THEN LAND", TXT_WHITE);
        if (game.score > 0) {
            char buf[24];
            std::snprintf(buf, sizeof buf, "LAST SCORE %06d", game.score);
            centredRich(38, buf, TXT_GOLD);
        }
    } else {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%06d", game.score);
        stampRich(1, kHudTextRow8, buf, TXT_WHITE);
        std::snprintf(buf, sizeof buf, "SHIP %d", game.shipNum);
        stampRich(10, kHudTextRow8, buf, TXT_CYAN);
        if (game.quotaMet()) {
            // The LAND NOW pulse: a slow (~1 Hz) gold/white palette alternation — a beckon,
            // never a strobe.
            stampRich(18, kHudTextRow8, "LAND NOW",
                      (pulseTick_ / 30) % 2 == 0 ? TXT_GOLD : TXT_WHITE);
        } else {
            std::snprintf(buf, sizeof buf, "WAVES %d", game.waves.quota() - game.waves.wavesCleared());
            stampRich(18, kHudTextRow8, buf, TXT_WHITE);
        }
        std::snprintf(buf, sizeof buf, "LIVES %d", game.lives);
        stampRich(32, kHudTextRow8, buf, TXT_WHITE);
        for (int tx = 0; tx < kMapW; tx += 2) {  // the rule under the readouts
            const std::uint16_t base = assets.ruleBase();
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    TileCell& cell = hudCells_[static_cast<std::size_t>(kHudRuleRow8 + dy) * kMapW + tx + dx];
                    cell.tile    = static_cast<std::uint16_t>(base + dx + kFontStride8 * dy);
                    cell.atlas   = assets.fontAtlas();
                    cell.palette = assets.textPals[TXT_WHITE];
                }
            }
        }
    }

    // ── The actor sprites (world space; the layer's scroll is the camera). ────────────────────
    actorSprites_.clear();
    popupSprites_.clear();
    if (game.state == GameState::Playing) {
        // The Manta: bank frames from vertical intent, the side-on frame while turning or rolled,
        // facing by flipX. Its key carries the life epoch — a respawn teleports, so it re-keys.
        const Slot mantaSlot = (game.rolled || game.turnTicks > 0)
                                   ? S_MANTA_SIDE
                                   : (game.bank == 0 ? S_MANTA_LEVEL
                                                     : (std::abs(game.bank) == 1 ? S_MANTA_BANK1
                                                                                 : S_MANTA_BANK2));
        // Respawn invulnerability: the Manta's OWN sprite alpha breathes while it is invulnerable — its
        // opacity alone, so the fighters, shots, and popups sharing the actor layer stay solid. The
        // interpolator eases this alpha between ticks (a fresh respawn re-keys, so it snaps clean). Default
        // 1.0 (opaque) when not invulnerable.
        float mantaAlpha = 1.0f;
        if (game.invulnTicks > 0) {
            const int ph = game.invulnTicks % 60;
            mantaAlpha   = 0.55f + 0.45f * (ph < 30 ? static_cast<float>(ph) / 30.0f
                                                    : static_cast<float>(60 - ph) / 30.0f);
        }
        actorSprites_.push_back(Sprite{
            .key     = "manta_" + std::to_string(game.lifeEpoch),
            .x       = static_cast<int>(game.shipX),
            .y       = static_cast<int>(game.shipY),
            .size    = AssetDimensions{48, 24},
            .atlas   = assets.spriteAtlas(),
            .tile    = assets.slotTile(mantaSlot),
            .palette = assets.spritePals[PAL_MANTA],
            .alpha   = mantaAlpha,
            .flipX   = game.facing < 0,
            .flipY   = game.bank < 0});   // bank art is authored downward; upward mirrors it

        // Fighters: livery palette per wave, flipped to their travel direction.
        const Wave& w = game.waves.wave();
        if (w.running && w.path) {
            for (int i = 0; i < kWaveSize; ++i) {
                if (!w.members[static_cast<std::size_t>(i)].alive) continue;
                const Vec2 p = game.waves.fighterPos(i);
                actorSprites_.push_back(Sprite{
                    .key     = "f_" + std::to_string(w.id) + "_" + std::to_string(i),
                    .x       = static_cast<int>(p.x) - 16,
                    .y       = static_cast<int>(p.y) - 8,
                    .size    = AssetDimensions{32, 16},
                    .atlas   = assets.spriteAtlas(),
                    .tile    = assets.slotTile(S_FIGHTER),
                    .palette = assets.spritePals[static_cast<std::size_t>(PAL_FIGHTER_0) +
                                                 static_cast<std::size_t>(w.livery)],
                    .flipX   = w.headsRight});   // art noses LEFT; heading right mirrors it
            }
        }

        // Mines: a slow per-sprite spin — the geometric Transform on live gameplay sprites.
        for (const Mine& m : game.waves.mines()) {
            actorSprites_.push_back(Sprite{
                .key       = "mine_" + std::to_string(m.id),
                .x         = static_cast<int>(m.x) - 8,
                .y         = static_cast<int>(m.y) - 8,
                .size      = AssetDimensions{16, 16},
                .atlas     = assets.spriteAtlas(),
                .tile      = assets.slotTile(S_MINE),
                .palette   = assets.spritePals[PAL_MINE],
                .transform = Transform::rotation(static_cast<float>((pulseTick_ * 2 + m.id * 45) % 360),
                                                 8.0f, 8.0f)});
        }

        // Bolts + enemy shots.
        for (const PlayerShot& s : game.shots) {
            if (!s.alive) continue;
            actorSprites_.push_back(Sprite{
                .key     = "shot_" + std::to_string(s.id),
                .x       = static_cast<int>(s.x),
                .y       = static_cast<int>(s.y),
                .size    = AssetDimensions{16, 8},
                .atlas   = assets.spriteAtlas(),
                .tile    = assets.slotTile(S_BOLT),
                .palette = assets.spritePals[PAL_BOLT],
                .flipX   = s.vx < 0});
        }
        for (const EnemyShot& s : game.waves.shots()) {
            actorSprites_.push_back(Sprite{
                .key     = "eshot_" + std::to_string(s.id),
                .x       = static_cast<int>(s.x) - 4,
                .y       = static_cast<int>(s.y) - 4,
                .size    = AssetDimensions{8, 8},
                .atlas   = assets.spriteAtlas(),
                .tile    = assets.slotTile(S_ESHOT),
                .palette = assets.spritePals[PAL_ESHOT]});
        }

        // Explosions: each pooled Boom plays the 4-frame clip once; the frame comes from its own
        // AnimationPlayer cursor.
        for (const Boom& b : feel.booms()) {
            const AnimationFrame& f = b.player.current();
            actorSprites_.push_back(Sprite{
                .key     = "boom_" + std::to_string(b.id),
                .x       = static_cast<int>(b.x) - 12,
                .y       = static_cast<int>(b.y) - 12,
                .size    = f.size(),
                .atlas   = f.atlas(),
                .tile    = f.tile(),
                .palette = f.palette});
        }

        // Popups: rich-font digits rising and shrinking where the points landed.
        for (const ScorePopup& p : feel.popups()) {
            const float prog = p.progress.value();
            const float scl  = 1.0f - prog;
            if (scl <= 0.02f) continue;
            char buf[8];
            std::snprintf(buf, sizeof buf, "%d", p.points);
            const int   n  = static_cast<int>(std::strlen(buf));
            const float gx = p.x - static_cast<float>(n) * 8.0f;
            const float gy = p.y - 10.0f - prog * 24.0f;
            for (int i = 0; i < n; ++i) {
                popupSprites_.push_back(Sprite{
                    .key       = "pop_" + std::to_string(p.id) + "_" + std::to_string(i),
                    .x         = static_cast<int>(gx + static_cast<float>(i) * 16.0f),
                    .y         = static_cast<int>(gy),
                    .size      = AssetDimensions{16, 16},
                    .atlas     = assets.fontAtlas(),
                    .tile      = assets.glyphBase(buf[i]),
                    .palette   = assets.textPals[TXT_GOLD],
                    .transform = Transform::scale(scl, scl, 8.0f, 8.0f)});
            }
        }
    }

    // ── Assemble the frame. ────────────────────────────────────────────────────────────────────
    FrameDrawState frame;
    const int cam = static_cast<int>(game.camX);

    DrawLayer stars{.key = "stars"};
    stars.z       = 0;
    stars.size    = PixelSize{kViewW, kViewH};
    stars.scroll  = LayerScroll{cam / 2, 0};   // half rate: the parallax depth cue
    stars.content = TileContent{.widthInTiles  = Deck::kStarCols8,
                                .heightInTiles = Deck::kStarRows8,
                                .cells         = std::span<const TileCell>(game.deck.starTiles()),
                                .wrap          = TileWrap::Repeat};
    frame.layers.push_back(stars);

    if (game.state == GameState::Playing) {
        DrawLayer deckL{.key = "deck"};
        deckL.z       = 10;
        deckL.size    = PixelSize{kViewW, kViewH};
        deckL.scroll  = LayerScroll{cam, -kDeckTop};   // the band sits at world y 96; Blank wrap
        deckL.content = TileContent{.widthInTiles  = kWorldW / kTile,
                                    .heightInTiles = kDeckRows * 2,
                                    .cells         = std::span<const TileCell>(game.deck.tiles()),
                                    .wrap          = TileWrap::Blank};
        frame.layers.push_back(deckL);

        DrawLayer actors{.key = "actors"};
        actors.z       = 20;
        actors.size    = PixelSize{kViewW, kViewH};
        actors.scroll  = LayerScroll{cam, 0};
        actors.content = SpriteContent{.sprites = std::span<const Sprite>(actorSprites_)};
        // The invulnerability breath lives on the Manta's own Sprite::alpha (built above), scoped to the
        // one sprite — the actor layer stays fully opaque.
        frame.layers.push_back(actors);

        DrawLayer pops{.key = "popups"};
        pops.z       = 30;
        pops.size    = PixelSize{kViewW, kViewH};
        pops.scroll  = LayerScroll{cam, 0};
        pops.content = SpriteContent{.sprites = std::span<const Sprite>(popupSprites_)};
        frame.layers.push_back(pops);
    }

    DrawLayer hud{.key = "hud"};
    hud.z       = 40;
    hud.size    = PixelSize{kViewW, kViewH};
    hud.content = TileContent{.widthInTiles  = kMapW,
                              .heightInTiles = kMapH,
                              .cells         = std::span<const TileCell>(hudCells_)};
    frame.layers.push_back(hud);

    if (game.state == GameState::Playing) {
        // The thrust glow: a small Add-blended halo behind the tail while the throttle is held.
        if (game.thrustDir != 0 && game.phase == FlightPhase::Flying) {
            const float tailX = game.facing > 0 ? game.shipX - game.camX + 4.0f
                                                : game.shipX - game.camX + kMantaW - 4.0f;
            frame.regions.push_back(Region{
                .key     = "thrustGlow",
                .shape   = ShapePoints::circle(Point{tailX, game.shipY + kMantaH / 2}, 10.0f),
                .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                              .fill = Rgba8{255, 170, 60}}},
                .alpha   = 0.28f,
                .blend   = BlendMode::Add});
        }
        // The destruct sequence: a gentle Ripple confined to the deck band (the hull groaning)
        // plus the feel layer's slow Multiply dim over the whole frame.
        if (game.phase == FlightPhase::Destruct) {
            frame.regions.push_back(Region{
                .key     = "destructRipple",
                .shape   = ShapePoints::rectangle(Point{0, static_cast<float>(kDeckTop)},
                                                  static_cast<float>(kViewW),
                                                  static_cast<float>(kDeckBottom - kDeckTop)),
                .effects = {ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Ripple,
                                              .amplitude = 2.0f,
                                              .frequency = 4.0f,
                                              .phase     = static_cast<float>(pulseTick_) * 0.01f,
                                              .center    = Point{game.shipX - game.camX,
                                                                 game.shipY + 12.0f},
                                              .decay     = 1.0f}}});
        }
        const float dim = feel.destructDim();
        if (dim < 1.0f) {
            const auto g = static_cast<std::uint8_t>(dim * 255.0f);
            frame.regions.push_back(Region{
                .key     = "destructDim",
                .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                              .fill = Rgba8{g, g, g, 255}}},
                .blend   = BlendMode::Multiply});
        }
        if (const std::optional<ScreenSpaceEffect> shake = feel.screenShake()) {
            frame.postEffects.push_back(*shake);
        }
    }

    renderer.renderFrame(frame);  // no alpha: the engine owns interpolation, easing by each key
}

}  // namespace vant
