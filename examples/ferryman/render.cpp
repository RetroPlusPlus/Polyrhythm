#include "render.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <span>
#include <string>
#include <string_view>

#include "retropp/geometry.h"   // AssetDimensions / PixelSize / Point
#include "retropp/palette.h"    // Rgba8

namespace ferryman {

using namespace retropp;

namespace {

// A deterministic per-block hash — the sea's grain and the islet variants are identical every
// run and every frame; only the palettes animate.
std::uint32_t blockHash(int x, int y) {
    auto h = static_cast<std::uint32_t>(x) * 2654435761u ^ static_cast<std::uint32_t>(y) * 40503u;
    h ^= h >> 13;
    return h * 1274126177u;
}

constexpr int kFieldCellRows = kMapH - kHudBandRows;  // 52 — the terrain grid's 8px rows

}  // namespace

FerrymanRenderer::FerrymanRenderer()
    : driftCells_(static_cast<std::size_t>(kSeaCellCols) * kSeaCellRows),
      swellCells_(static_cast<std::size_t>(kSeaCellCols) * kSeaCellRows),
      terrainCells_(static_cast<std::size_t>(kMapW) * kFieldCellRows),
      hudCells_(static_cast<std::size_t>(kMapW) * kHudBandRows),
      titleCells_(static_cast<std::size_t>(kMapW) * kMapH),
      pauseCells_(static_cast<std::size_t>(kMapW) * kMapH) {}

void FerrymanRenderer::render(Renderer& renderer, const FerrymanGame& game,
                              const FerrymanAssets& assets, const FerrymanFeel& feel) {
    // ── Text stamping: a rich 16×16 glyph is a 2×2 group of 8px tiles read from the font sheet
    // at stride kFontStride8. Title text stamps into its own transparent layer; the in-game HUD
    // stamps into its band-sized grid, drawn on the top-most layer.
    auto stampRichInto = [&](std::vector<TileCell>& grid, int col16, int row8, std::string_view s,
                             PaletteId palette) {
        for (std::size_t i = 0; i < s.size(); ++i) {
            const int tx = (col16 + static_cast<int>(i)) * 2;
            if (tx + 1 >= kMapW) break;
            const std::uint16_t base = assets.glyphBase(s[i]);
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    TileCell& cell = grid[static_cast<std::size_t>(row8 + dy) * kMapW + tx + dx];
                    cell.atlas   = assets.fontAtlas();
                    cell.tile    = static_cast<std::uint16_t>(base + dx + kFontStride8 * dy);
                    cell.palette = palette;
                }
            }
        }
    };
    auto centred = [&](int row8, std::string_view s, std::size_t pal) {
        stampRichInto(titleCells_, (kHudCols - static_cast<int>(s.size())) / 2, row8, s,
                      assets.textPals[pal]);
    };
    auto stampHud = [&](int col16, int row8, std::string_view s, std::size_t pal) {
        stampRichInto(hudCells_, col16, row8, s, assets.hudPals[pal]);
    };

    // ── The two water planes, rebuilt each render. THE SEA IS ONE SEAMLESS FIELD: the generator
    // authored a rich 128×128 design (4×4 tiles) that wraps at its own edges, and the slices are
    // placed BY GRID POSITION — adjacent blocks are literal neighbors in that one design, so
    // full detail carries no seams. The water clip's index picks the field's animation frame
    // (crests twinkle, dashes breathe, sparkles drift over a static base) and its palette
    // carries the a/b breathing; the foam overlay swaps its own two frames on the same beat.
    const PaletteId   seaPal     = feel.waterFrame().palette;
    const int         waterPhase = static_cast<int>(feel.waterPhase());
    for (int by = 0; by < kSeaBlockRows; ++by) {
        for (int bx = 0; bx < kSeaBlockCols; ++bx) {
            const std::uint32_t h = blockHash(bx, by);
            const std::size_t cell = static_cast<std::size_t>(by % kWaterField) * kWaterField +
                                     static_cast<std::size_t>(bx % kWaterField);
            const std::size_t slot = static_cast<std::size_t>(T_WATER_0) +
                                     cell * kWaterPhases + static_cast<std::size_t>(waterPhase);
            const std::uint16_t base = assets.terrain[slot].tile;
            for (int dy = 0; dy < 4; ++dy) {
                for (int dx = 0; dx < 4; ++dx) {
                    TileCell& cell =
                        driftCells_[static_cast<std::size_t>(by * 4 + dy) * kSeaCellCols + bx * 4 +
                                    dx];
                    cell.atlas   = assets.terrainAtlas();
                    cell.tile    = static_cast<std::uint16_t>(base + dx + kTerrainStride8 * dy);
                    cell.palette = seaPal;
                }
            }
            // The swell plane: sparse foam streaks over hole tiles, at its own scroll rate,
            // breathing between their two frames.
            const bool sparkle = (h >> 8) % 5u == 0u;
            std::uint16_t swellBase = assets.terrainTile(T_BLANK);
            if (sparkle) {
                const std::size_t kind = (h >> 16) % 2u;
                swellBase = assets.terrain[static_cast<std::size_t>(T_SPARKLE_A) +
                                           kind * kSparklePhases +
                                           static_cast<std::size_t>(waterPhase % kSparklePhases)]
                                .tile;
            }
            for (int dy = 0; dy < 4; ++dy) {
                for (int dx = 0; dx < 4; ++dx) {
                    TileCell& cell =
                        swellCells_[static_cast<std::size_t>(by * 4 + dy) * kSeaCellCols + bx * 4 +
                                    dx];
                    cell.atlas   = assets.terrainAtlas();
                    cell.tile = static_cast<std::uint16_t>(swellBase + dx + kTerrainStride8 * dy);
                    cell.palette = seaPal;
                }
            }
        }
    }

    // ── The terrain: the sanctuary island band + the fixed islets, everything else open water
    // (the hole tile). Rebuilt each render because the beacon's palette breathes.
    auto stampBlock = [&](int blockX, int blockY, TerrainTile t, PaletteId pal) {
        const std::uint16_t base = assets.terrainTile(t);
        for (int dy = 0; dy < 4; ++dy) {
            for (int dx = 0; dx < 4; ++dx) {
                TileCell& cell = terrainCells_[static_cast<std::size_t>(blockY * 4 + dy) * kMapW +
                                               blockX * 4 + dx];
                cell.atlas   = assets.terrainAtlas();
                cell.tile    = static_cast<std::uint16_t>(base + dx + kTerrainStride8 * dy);
                cell.palette = pal;
            }
        }
    };
    {
        const std::uint16_t hole = assets.terrainTile(T_BLANK);
        for (TileCell& cell : terrainCells_) {
            cell.atlas   = assets.terrainAtlas();
            cell.tile    = hole;
            cell.palette = assets.terrainPals[TP_WATER_A];
        }
        // The sanctuary band: pads with the breathing beacon at center, lamps at the quay ends,
        // and the bannered trim edge facing the crossing.
        for (int bx = 0; bx < kBlockCols; ++bx) {
            if (bx == 9) {
                stampBlock(bx, 0, T_BEACON, feel.beaconFrame().palette);
            } else if (bx == 0 || bx == kBlockCols - 1) {
                stampBlock(bx, 0, T_LAMP, assets.terrainPals[TP_MEDIAN]);
            } else {
                stampBlock(bx, 0, T_SANCTUARY, assets.terrainPals[TP_SANCTUARY]);
            }
            // The band's second row is the SHORELINE tile — grass above, beach fading to a dark
            // waterline and surf below. It reads in the SAND palette (a beach, not green stone).
            stampBlock(bx, 1, T_TRIM, assets.terrainPals[TP_SHORE]);
        }
        // The islets — THIS RUN's rolled archipelago, read straight off the sim. The shore tiles
        // are CAP-AWARE (each carries its own coastline): a single free-standing block takes the
        // all-edges coast (T_SHORE_A); a two-block islet takes the left cap + right cap pair, so
        // foam rings the islet and never crosses its middle. The spec's prop replaces the right
        // block (props are authored over the right-cap base).
        for (const IsletSpec& islet : game.islets) {
            // A pure 2×1 horizontal islet keeps its seamless left-cap/right-cap coastline; every
            // other shape (single, VERTICAL, block) stamps the all-edges shore tile per block, so
            // the coast rings each block — reading as a little rocky isle or a vertical chain.
            const bool cleanHoriz = islet.tilesW == 2 && islet.tilesH == 1;
            for (int h = 0; h < islet.tilesH; ++h) {
                for (int w = 0; w < islet.tilesW; ++w) {
                    const bool isProp =
                        islet.prop != 0 && w == islet.tilesW - 1 && h == islet.tilesH - 1;
                    TerrainTile tile = T_SHORE_A;
                    if (isProp) {
                        tile = static_cast<TerrainTile>(islet.prop);
                    } else if (cleanHoriz) {
                        tile = w == 0 ? T_SHORE_B : T_SHORE_C;
                    }
                    const bool stone = tile == T_LAMP || tile == T_MEDIAN_A || tile == T_MEDIAN_B;
                    stampBlock(islet.blockX + w, islet.blockY + h, tile,
                               assets.terrainPals[stone ? TP_MEDIAN : TP_SHORE]);
                }
            }
        }
    }

    // ── The sprite lists. ─────────────────────────────────────────────────────────────────────
    groundSprites_.clear();
    wakeSprites_.clear();
    shadowSprites_.clear();
    boltSprites_.clear();
    actorSprites_.clear();
    flyerSprites_.clear();
    popupSprites_.clear();
    if (game.state == GameState::Playing) {
        // A flying craft casts a shadow on the sea: its OWN art, redrawn through the flat shadow
        // palette on the low shadow layer — squashed, softened by alpha, and offset down-right so
        // the gap between craft and shadow reads as altitude.
        auto pushCraftShadow = [&](int spriteX, int spriteY, int w, int h, retropp::AtlasId atlas,
                                   std::uint16_t tile, bool flipX, const std::string& key) {
            shadowSprites_.push_back(Sprite{
                .key       = key + "_sh",
                .x         = spriteX + 6,
                .y         = spriteY + 18,
                .size      = AssetDimensions{w, h},
                .atlas     = atlas,
                .tile      = tile,
                .palette   = assets.spritePals[PAL_SHADOW],
                .alpha     = 0.30f,
                .flipX     = flipX,
                .transform = Transform::scale(1.0f, 0.5f, static_cast<float>(w) / 2.0f,
                                              static_cast<float>(h) / 2.0f)});
        };
        // Grounded colonists: the idle-bob clip per look; a stunned soul treads water at 0.55
        // presence (per-sprite alpha as the stun tell). ONE key for its whole life.
        for (const Colonist& c : game.colonists) {
            if (c.state == ColonistState::Aboard) continue;
            const AnimationFrame& f = feel.bobFrame(c.look);
            groundSprites_.push_back(Sprite{
                .key     = "col_" + std::to_string(c.id),
                .x       = static_cast<int>(c.x) - 8,
                .y       = static_cast<int>(c.y) - 8,
                .size    = AssetDimensions{16, 16},
                .atlas   = f.atlas(),
                .tile    = f.tile(),
                .palette = f.palette,
                .alpha   = c.state == ColonistState::Stunned ? 0.55f : 1.0f});
        }

        // Every bolt in flight: one art, two liveries — gold is yours, magenta is theirs.
        for (const Bolt& b : game.bolts) {
            boltSprites_.push_back(Sprite{
                .key     = "b_" + std::to_string(b.id),
                .x       = static_cast<int>(b.x) - 6,
                .y       = static_cast<int>(b.y) - 6,
                .size    = AssetDimensions{12, 12},
                .atlas   = assets.spriteAtlas(),
                .tile    = assets.slotTile(S_BOLT),
                .palette = assets.spritePals[b.friendly ? PAL_BOLT_CARGO : PAL_BOLT_ENEMY]});
        }

        // The enemies split by nature: the flying CRAFT rise ABOVE the boat with a shadow on the
        // sea (airborne — they never touch you, only their bullets do); the MUTANT is a water
        // hunter at boat level (its pulse clip) — the one enemy whose touch is lethal. A just-hit
        // craft dims briefly (per-sprite alpha); liveries blink by palette phase.
        for (const Enemy& e : game.enemies) {
            const float w = kEnemyW[static_cast<std::size_t>(e.kind)];
            const float h = kEnemyH[static_cast<std::size_t>(e.kind)];
            Sprite s{.key = "en_" + std::to_string(e.id)};
            s.x     = static_cast<int>(e.x - w / 2.0f);
            s.y     = static_cast<int>(e.y - h / 2.0f);
            s.size  = AssetDimensions{static_cast<int>(w), static_cast<int>(h)};
            s.alpha = e.hitFlash > 0 ? 0.55f : 1.0f;
            if (e.kind == EK_MUTANT) {
                const AnimationFrame& f = feel.pulseFrame();
                s.atlas     = f.atlas();
                s.tile      = f.tile();
                s.palette   = f.palette;
                s.transform = Transform::scale(1.2f, 1.2f, w / 2.0f, h / 2.0f);  // a touch bigger,
                                                                                 // more prominent
                actorSprites_.push_back(s);  // the water hunter shares the boat's layer (z=30)
            } else {
                s.atlas   = assets.spriteAtlas();
                s.tile    = assets.slotTile(e.kind == EK_CORSAIR    ? S_DART
                                            : e.kind == EK_WARDEN   ? S_SWEEPER
                                                                    : S_HAULER);
                s.palette = assets.vehiclePal(e.kind, feel.lightsPhase());
                s.flipX   = e.vx < 0.0f;  // authored facing right
                pushCraftShadow(s.x, s.y, static_cast<int>(w), static_cast<int>(h), s.atlas,
                                s.tile, s.flipX, "en_" + std::to_string(e.id));
                flyerSprites_.push_back(s);  // airborne — above the boat (z=35)
            }
        }

        // The abductors: wing lights blink via the frame clip; each field visit is a fresh spawn
        // key, so an arrival mount-snaps at the curve's start instead of streaking from the exit.
        for (int i = 0; i < game.abductorCount(); ++i) {
            const Abductor& abd = game.abductors[static_cast<std::size_t>(i)];
            if (abd.state() == AbductorState::Away) continue;
            const AnimationFrame& f   = feel.wingsFrame();
            const Vec2            ap  = abd.pos();
            const int             abx = static_cast<int>(ap.x - kAbductorW / 2.0f);
            const int             aby = static_cast<int>(ap.y - kAbductorH / 2.0f);
            const std::string     akey =
                "abd" + std::to_string(i) + "_" + std::to_string(abd.spawn());
            // The saucer flies too: its own shadow on the sea, itself on the flyer layer.
            pushCraftShadow(abx, aby, static_cast<int>(kAbductorW), static_cast<int>(kAbductorH),
                            f.atlas(), f.tile(), false, akey);
            flyerSprites_.push_back(Sprite{
                .key     = akey,
                .x       = abx,
                .y       = aby,
                .size    = AssetDimensions{static_cast<int>(kAbductorW),
                                           static_cast<int>(kAbductorH)},
                .atlas   = f.atlas(),
                .tile    = f.tile(),
                .palette = f.palette});
            // The colonist in its clutches rides just under the keel — same key, same soul, and
            // rides the flyer layer with the saucer that carries it.
            if (const auto& held = game.carried[static_cast<std::size_t>(i)]) {
                const AnimationFrame& cf = feel.bobFrame(held->look);
                flyerSprites_.push_back(Sprite{
                    .key     = "col_" + std::to_string(held->id),
                    .x       = static_cast<int>(ap.x) - 8,
                    .y       = static_cast<int>(ap.y + kAbductorH / 2.0f) - 2,
                    .size    = AssetDimensions{16, 16},
                    .atlas   = cf.atlas(),
                    .tile    = cf.tile(),
                    .palette = cf.palette});
            }
        }

        // The wake: foam trailed BEHIND the moving hull on its own low layer (never baked into the
        // boat art). Two puffs — a near bright one, a fainter far one — offset opposite the
        // heading; keyed by heading so a turn mount-snaps the wake to the new stern rather than
        // sliding it around the boat.
        if (game.ferryMoving) {
            int bx = 0, by = 0;
            switch (game.ferryFacing) {
                case Facing::East:  bx = -1; break;  // heading right → wake astern (left)
                case Facing::West:  bx = +1; break;
                case Facing::North: by = +1; break;  // heading up → wake below
                case Facing::South: by = -1; break;
            }
            const int wakePhase = static_cast<int>(feel.waterPhase()) & 1;
            for (int t = 0; t < 2; ++t) {
                const int off = 13 + t * 9;
                wakeSprites_.push_back(Sprite{
                    .key     = "wake_" + std::to_string(static_cast<int>(game.ferryFacing)) + "_" +
                               std::to_string(t),
                    .x       = static_cast<int>(game.ferryX) - 8 + bx * off,
                    .y       = static_cast<int>(game.ferryY) - 6 + by * off,
                    .size    = AssetDimensions{16, 12},
                    .atlas   = assets.spriteAtlas(),
                    .tile    = assets.slotTile((t ^ wakePhase) ? S_WAKE_B : S_WAKE_A),
                    .palette = assets.spritePals[PAL_WAKE],
                    .alpha   = t == 0 ? 0.6f : 0.32f});
            }
        }

        // The ferry: the SPRITE and SIZE follow the heading — the wide side view (mirrored for
        // west) sailing E/W, the narrow bow (south) / stern (north) views sailing N/S. The side
        // view gently flickers via the thruster clip; the respawn invulnerability breathes its OWN
        // Sprite::alpha (a slow 1 Hz triangle).
        float ferryAlpha = 1.0f;
        if (game.invulnLeft > 0) {
            const int ph = game.invulnLeft % 60;
            ferryAlpha   = 0.55f + 0.45f * (ph < 30 ? static_cast<float>(ph) / 30.0f
                                                    : static_cast<float>(60 - ph) / 30.0f);
        }
        const AnimationFrame& ff = feel.thrusterFrame();
        std::uint16_t ferryTile = ff.tile();   // E/W: the side view's current thruster frame
        float         ferryW = kFerryW, ferryH = kFerryH;
        bool          ferryFlip = false;
        if (game.ferryFacing == Facing::North) {
            ferryTile = assets.slotTile(S_FERRY_NORTH);
            ferryW = kFerryNSW;
            ferryH = kFerryNSH;
        } else if (game.ferryFacing == Facing::South) {
            ferryTile = assets.slotTile(S_FERRY_SOUTH);
            ferryW = kFerryNSW;
            ferryH = kFerryNSH;
        } else if (game.ferryFacing == Facing::West) {
            ferryFlip = true;
        }
        actorSprites_.push_back(Sprite{
            .key     = "ferry_l" + std::to_string(game.lifeNum),
            .x       = static_cast<int>(game.ferryX - ferryW / 2.0f),
            .y       = static_cast<int>(game.ferryY - ferryH / 2.0f),
            .size    = AssetDimensions{static_cast<int>(ferryW), static_cast<int>(ferryH)},
            .atlas   = assets.spriteAtlas(),
            .tile    = ferryTile,
            .palette = ff.palette,
            .alpha   = ferryAlpha,
            .flipX   = ferryFlip});
        // The deck passengers: the same colonists, the SAME keys (identity travels field → deck
        // → field), huddled on the open deck.
        for (std::size_t k = 0; k < game.deck.size(); ++k) {
            const Colonist* c = game.colonistById(game.deck[k]);
            if (c == nullptr) continue;
            const AnimationFrame& cf = feel.bobFrame(c->look);
            const int             dx = static_cast<int>(k % 2) * 10 - 13;
            const int             dy = static_cast<int>(k / 2) * 7 - 12;
            actorSprites_.push_back(Sprite{
                .key     = "col_" + std::to_string(c->id),
                .x       = static_cast<int>(game.ferryX) + dx,
                .y       = static_cast<int>(game.ferryY) + dy,
                .size    = AssetDimensions{16, 16},
                .atlas   = cf.atlas(),
                .tile    = cf.tile(),
                .palette = cf.palette,
                .alpha   = ferryAlpha});
        }

        // Explosions: each pooled Boom plays the 3-frame clip once from its own cursor.
        for (const Boom& b : feel.booms()) {
            const AnimationFrame& f = b.player.current();
            actorSprites_.push_back(Sprite{
                .key     = "boom_" + std::to_string(b.id),
                .x       = static_cast<int>(b.x) - 16,
                .y       = static_cast<int>(b.y) - 16,
                .size    = f.size(),
                .atlas   = f.atlas(),
                .tile    = f.tile(),
                .palette = f.palette});
        }

        // Popups: rich-font digits rising where the points landed — shrinking via a per-sprite
        // transform AND fading via per-sprite alpha, the two composing on the same sprite.
        for (const ScorePopup& p : feel.popups()) {
            const float prog = p.progress.value();  // 0 → 1
            const float scl  = 1.0f - prog * 0.6f;
            const float fade = 1.0f - prog;
            if (fade <= 0.03f) continue;
            char buf[8];
            std::snprintf(buf, sizeof buf, "%d", p.points);
            const int   n  = static_cast<int>(std::strlen(buf));
            const float gx = p.x - static_cast<float>(n) * 8.0f;
            const float gy = p.y - 12.0f - prog * 26.0f;  // rise
            for (int i = 0; i < n; ++i) {
                popupSprites_.push_back(Sprite{
                    .key       = "pop_" + std::to_string(p.id) + "_" + std::to_string(i),
                    .x         = static_cast<int>(gx + static_cast<float>(i) * 16.0f),
                    .y         = static_cast<int>(gy),
                    .size      = AssetDimensions{16, 16},
                    .atlas     = assets.fontAtlas(),
                    .tile      = assets.glyphBase(buf[i]),
                    .palette   = assets.textPals[TXT_GOLD],
                    .alpha     = fade,
                    .transform = Transform::scale(scl, scl, 8.0f, 8.0f)});
            }
        }

        // The round card: "WAVE N" slides across on its OutBack track — the wave intro.
        if (feel.bannerActive()) {
            char card[20];
            std::snprintf(card, sizeof card, "WAVE %d", feel.bannerWave());
            const int   n  = static_cast<int>(std::strlen(card));
            const float cx = feel.bannerX01() * kViewW - static_cast<float>(n) * 8.0f;
            for (int i = 0; i < n; ++i) {
                if (card[i] == ' ') continue;
                popupSprites_.push_back(Sprite{
                    .key     = "card_" + std::to_string(i),
                    .x       = static_cast<int>(cx + static_cast<float>(i) * 16.0f),
                    .y       = 208,
                    .size    = AssetDimensions{16, 16},
                    .atlas   = assets.fontAtlas(),
                    .tile    = assets.glyphBase(card[i]),
                    .palette = assets.textPals[TXT_GOLD]});
            }
        }

        // The HUD band's grid: bar fill, the two text rows, the alert slot, the bevelled rule.
        const std::uint16_t barTile = assets.glyphBase(' ');
        for (TileCell& cell : hudCells_) {
            cell.atlas   = assets.fontAtlas();
            cell.tile    = barTile;
            cell.palette = assets.hudPals[TXT_WHITE];  // the opaque bar fill
        }
        char buf[24];
        std::snprintf(buf, sizeof buf, "%07d", game.score);
        stampHud(1, kHudRow1, "SCORE", TXT_WHITE);
        stampHud(7, kHudRow1, buf, TXT_WHITE);
        std::snprintf(buf, sizeof buf, "%02d", game.wave);
        stampHud(17, kHudRow1, "WAVE", TXT_CYAN);
        stampHud(22, kHudRow1, buf, TXT_CYAN);
        std::snprintf(buf, sizeof buf, "%d", game.lives);
        stampHud(27, kHudRow1, "LIVES", TXT_WHITE);
        stampHud(33, kHudRow1, buf, TXT_WHITE);
        // The greed dial: the crew aboard and what delivering RIGHT NOW would pay.
        std::snprintf(buf, sizeof buf, "%d", static_cast<int>(game.deck.size()));
        stampHud(1, kHudRow2, "CREW", TXT_GOLD);
        stampHud(6, kHudRow2, buf, TXT_GOLD);
        std::snprintf(buf, sizeof buf, "%04d", game.haulPays());
        stampHud(9, kHudRow2, "PAYS", TXT_GOLD);
        stampHud(14, kHudRow2, buf, TXT_GOLD);
        std::snprintf(buf, sizeof buf, "%02d OF %02d", std::min(game.rescued, game.quota()),
                      game.quota());
        stampHud(20, kHudRow2, "SAVED", TXT_WHITE);
        stampHud(26, kHudRow2, buf, TXT_WHITE);
        // The alert slot: what deserves your eyes right now, one word.
        if (game.waveLull > 0) {
            stampHud(35, kHudRow2, "CLEAR", TXT_CYAN);
        } else if (game.anyBeamLit()) {
            stampHud(35, kHudRow2, "ALARM", TXT_GOLD);
        } else if (game.anyMutant()) {
            stampHud(34, kHudRow2, "MUTANT", TXT_GOLD);
        }
        const std::uint16_t rule = assets.ruleBase();
        for (int x = 0; x < kMapW; x += 2) {
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    TileCell& cell =
                        hudCells_[static_cast<std::size_t>(kHudRuleRow + dy) * kMapW + x + dx];
                    cell.atlas   = assets.fontAtlas();
                    cell.tile    = static_cast<std::uint16_t>(rule + dx + kFontStride8 * dy);
                    cell.palette = assets.hudPals[TXT_CYAN];
                }
            }
        }
    }

    // ── Assemble the frame: drift (z=0) → swell (z=2) → terrain (z=5) → ground (z=10) → bolts
    //    (z=20) → actors (z=30) → popups (z=40) → the HUD band (z=100). ───────────────────────
    FrameDrawState frame;

    DrawLayer drift{.key = "drift"};
    drift.z       = 0;
    drift.size    = PixelSize{kViewW, kViewH};
    drift.scroll  = LayerScroll{static_cast<int>(driftScrollX_), static_cast<int>(driftScrollY_)};
    drift.content = TileContent{.widthInTiles  = kSeaCellCols,  // bigger-than-viewport, field-aligned
                                .heightInTiles = kSeaCellRows,   // wrap (see render.h)
                                .cells         = std::span<const TileCell>(driftCells_)};
    if (game.state == GameState::Title) {
        // The open sea breathes under the title: a slow, small Layer-scope wave on the drift
        // plane's own content (isolated — the text above rides steady). Paced like a swell, in
        // seconds — a quick or deep displacement reads as jiggle, not water.
        drift.effects.push_back(ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::RowDisplacement,
                                                  .amplitude = 1.6f,
                                                  .frequency = 1.5f,
                                                  .phase     = feel.titleWavePhase() * 0.15f,
                                                  .axis      = Axis::Horizontal});
    }
    frame.layers.push_back(drift);

    DrawLayer swell{.key = "swell"};
    swell.z       = 2;
    swell.size    = PixelSize{kViewW, kViewH};
    swell.scroll  = LayerScroll{static_cast<int>(swellScrollX_), static_cast<int>(swellScrollY_)};
    swell.content = TileContent{.widthInTiles  = kSeaCellCols,  // bigger-than-viewport, field-aligned
                                .heightInTiles = kSeaCellRows,   // wrap (see render.h)
                                .cells         = std::span<const TileCell>(swellCells_)};
    frame.layers.push_back(swell);

    if (game.state == GameState::Playing) {
        DrawLayer terrain{.key = "terrain"};
        terrain.z       = 5;
        terrain.size    = PixelSize{kViewW, kViewH};
        terrain.scroll  = LayerScroll{0, -kFieldTop};  // the band starts below the HUD
        terrain.content = TileContent{.widthInTiles  = kMapW,
                                      .heightInTiles = kFieldCellRows,
                                      .cells = std::span<const TileCell>(terrainCells_),
                                      .wrap  = TileWrap::Blank};  // finite — open sea beyond
        frame.layers.push_back(terrain);

        DrawLayer shadows{.key = "shadows"};
        shadows.z       = 8;  // on the sea, under the sprites — the flying craft's cast shadows
        shadows.size    = PixelSize{kViewW, kViewH};
        shadows.content = SpriteContent{.sprites = std::span<const Sprite>(shadowSprites_)};
        frame.layers.push_back(shadows);

        DrawLayer wake{.key = "wake"};
        wake.z       = 9;  // on the sea, just under the boat — the trailing foam
        wake.size    = PixelSize{kViewW, kViewH};
        wake.content = SpriteContent{.sprites = std::span<const Sprite>(wakeSprites_)};
        frame.layers.push_back(wake);

        DrawLayer ground{.key = "ground"};
        ground.z       = 10;
        ground.size    = PixelSize{kViewW, kViewH};
        ground.content = SpriteContent{.sprites = std::span<const Sprite>(groundSprites_)};
        frame.layers.push_back(ground);

        DrawLayer boltLayer{.key = "bolts"};
        boltLayer.z       = 20;
        boltLayer.size    = PixelSize{kViewW, kViewH};
        boltLayer.content = SpriteContent{.sprites = std::span<const Sprite>(boltSprites_)};
        frame.layers.push_back(boltLayer);

        DrawLayer actors{.key = "actors"};
        actors.z       = 30;
        actors.size    = PixelSize{kViewW, kViewH};
        actors.content = SpriteContent{.sprites = std::span<const Sprite>(actorSprites_)};
        frame.layers.push_back(actors);

        DrawLayer flyers{.key = "flyers"};
        flyers.z       = 35;  // ABOVE the boat — the craft fly over it (only their bullets bite)
        flyers.size    = PixelSize{kViewW, kViewH};
        flyers.content = SpriteContent{.sprites = std::span<const Sprite>(flyerSprites_)};
        frame.layers.push_back(flyers);

        // The mutant's REALITY-WARP COMET TAIL: a content-less layer JUST BELOW the mutant (z=29)
        // whose BELOW-scope custom shader refracts the whole view beneath it — traced as a chain of
        // TAPERING circles down the creature's path history (feel.mutantTrail), so the tail curves
        // lazily behind it and decays to a point, comet-style, while the mutant (z=30) floats on top
        // UNdistorted. The trail also cycles the hue for a psychedelic wake. (Kept generic — the
        // ferry can drive the same wake later; the boat's plain-distortion version was removed as it
        // read wrong on the hull.)
        warpRegions_.clear();
        for (const Enemy& e : game.enemies) {
            if (e.kind != EK_MUTANT) continue;
            const std::deque<Vec2>* trail = feel.mutantTrail(e.id);
            if (trail == nullptr || trail->empty()) continue;
            const auto n = static_cast<float>(trail->size());
            int        ci = 0;
            for (std::size_t i = 0; i < trail->size(); i += 3, ++ci) {
                const float tt     = static_cast<float>(i) / n;   // 0 at the head → ~1 at the tail
                const float radius = 15.0f * (1.0f - tt);         // taper the tail to a point
                if (radius < 2.5f) break;
                const Vec2 p = (*trail)[i];
                warpRegions_.push_back(Region{
                    .key   = "warp_" + std::to_string(e.id) + "_" + std::to_string(ci),
                    .shape = ShapePoints::circle(Point{p.x, p.y}, radius),
                    .effects = {ScreenSpaceEffect{
                        .kind            = ScreenSpaceEffectKind::Custom,
                        .customShader    = assets.wakeWarp,
                        .scope           = ScreenSpaceEffectScope::Below,
                        .warpPhase       = feel.warpPhase(),
                        .warpAmp         = 0.007f * (1.0f - tt * 0.6f),  // fainter toward the tip
                        .warpFreq        = 34.0f,
                        .warpChroma      = 0.85f,
                        .warpChromaSpeed = 0.35f}}});   // a lazy hue drift
            }
        }
        if (!warpRegions_.empty()) {
            DrawLayer warp{.key = "warpWake"};
            warp.z       = 29;  // below the mutant (z=30) — warps the view, never the mutant
            warp.size    = PixelSize{kViewW, kViewH};
            warp.regions = warpRegions_;
            frame.layers.push_back(warp);
        }

        DrawLayer pops{.key = "popups"};
        pops.z       = 40;
        pops.size    = PixelSize{kViewW, kViewH};
        pops.content = SpriteContent{.sprites = std::span<const Sprite>(popupSprites_)};
        frame.layers.push_back(pops);

        DrawLayer hud{.key = "hud"};
        hud.z       = 100;
        hud.size    = PixelSize{kViewW, kFieldTop};
        hud.content = TileContent{.widthInTiles  = kMapW,
                                  .heightInTiles = kHudBandRows,
                                  .cells         = std::span<const TileCell>(hudCells_),
                                  // FINITE: the opaque bar must not repeat down the frame.
                                  .wrap          = TileWrap::Blank};
        frame.layers.push_back(hud);

        // The tractor beam: an Add-blended ColorFill capsule from the saucer's keel toward its
        // prey — the threat is its own alarm light, breathing via the feel tween.
        for (int i = 0; i < game.abductorCount(); ++i) {
            const Abductor& abd = game.abductors[static_cast<std::size_t>(i)];
            if (!abd.beamLit()) continue;
            const Vec2  ap    = abd.pos();
            const float reach = abd.state() == AbductorState::Carrying ? 26.0f : 58.0f;
            frame.regions.push_back(Region{
                .key     = "beam" + std::to_string(i),
                .shape   = ShapePoints::capsule(Point{ap.x, ap.y + 10.0f},
                                                Point{ap.x, ap.y + 10.0f + reach}, 7.0f),
                .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                              .fill = Rgba8{216, 140, 255}}},
                .alpha   = feel.beamAlpha(),
                .blend   = BlendMode::Add});
        }

        // The sanctuary glow: home calls, harder the heavier you ride — the greed dial as light.
        if (!game.deck.empty()) {
            const float load = static_cast<float>(game.deck.size()) / kDeckCap;
            frame.regions.push_back(Region{
                .key     = "homeGlow",
                .shape   = ShapePoints::rectangle(Point{0.0f, static_cast<float>(kFieldTop)},
                                                  static_cast<float>(kViewW),
                                                  kSanctuaryBottom - kFieldTop),
                .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                              .fill = Rgba8{255, 214, 90}}},
                .alpha   = feel.glowAlpha() * 0.45f * load,
                .blend   = BlendMode::Add});
        }

        // The pause menu: a full-width opaque band across the middle — PAUSED over two choices,
        // the selected one gold. main freezes the sim, feel, and the parallax while this shows, so
        // the frozen field sits behind it. z=110 rides above the HUD.
        if (game.paused) {
            const std::uint16_t clearTile = assets.glyphBase(' ');
            for (TileCell& cell : pauseCells_) {
                cell.atlas   = assets.fontAtlas();
                cell.tile    = clearTile;
                cell.palette = assets.textPals[TXT_WHITE];  // alpha-0 entry 0 — transparent
            }
            constexpr int kBandTop = 22, kBandBot = 34;  // 8px rows 22..33 (y 176..272)
            for (int row = kBandTop; row < kBandBot; ++row)
                for (int x = 0; x < kMapW; ++x)
                    pauseCells_[static_cast<std::size_t>(row) * kMapW + x].palette =
                        assets.hudPals[TXT_WHITE];  // the opaque HUD-bar fill
            auto centredPause = [&](int row8, std::string_view s, std::size_t pal) {
                stampRichInto(pauseCells_, (kHudCols - static_cast<int>(s.size())) / 2, row8, s,
                              assets.hudPals[pal]);
            };
            centredPause(24, "PAUSED", TXT_GOLD);
            centredPause(28, "RESUME", game.pauseChoice == 0 ? TXT_GOLD : TXT_WHITE);
            centredPause(30, "QUIT TO TITLE", game.pauseChoice == 1 ? TXT_GOLD : TXT_WHITE);

            DrawLayer pause{.key = "pause"};
            pause.z       = 110;
            pause.size    = PixelSize{kViewW, kViewH};
            pause.content = TileContent{.widthInTiles  = kMapW,
                                        .heightInTiles = kMapH,
                                        .cells         = std::span<const TileCell>(pauseCells_),
                                        .wrap          = TileWrap::Blank};
            frame.layers.push_back(pause);
        }
    }

    if (game.state == GameState::Title) {
        // The title text floats on its own transparent layer over the open sea.
        const std::uint16_t clearTile = assets.glyphBase(' ');
        for (TileCell& cell : titleCells_) {
            cell.atlas   = assets.fontAtlas();
            cell.tile    = clearTile;
            cell.palette = assets.textPals[TXT_WHITE];
        }
        centred(24, "ARROWS OR WASD SAIL", TXT_WHITE);
        centred(27, "CARRY SOULS HOME TO THE SANCTUARY", TXT_WHITE);
        centred(30, "HEAVY DECKS PAY MORE  AND FIGHT BACK", TXT_WHITE);
        centred(33, "BUT A HEAVY FERRY IS A SLOW FERRY", TXT_WHITE);
        centred(36, "DODGE EVERY BOLT", TXT_WHITE);
        centred(39, "YOUR CREW GUNS DOWN THE ALIEN ABDUCTOR", TXT_WHITE);
        if (game.score > 0) {
            char buf[24];
            std::snprintf(buf, sizeof buf, "LAST SCORE %07d", game.score);
            centred(43, buf, TXT_GOLD);
        }
        DrawLayer title{.key = "title"};
        title.z       = 90;
        title.size    = PixelSize{kViewW, kViewH};
        title.content = TileContent{.widthInTiles  = kMapW,
                                    .heightInTiles = kMapH,
                                    .cells         = std::span<const TileCell>(titleCells_),
                                    .wrap          = TileWrap::Blank};
        frame.layers.push_back(title);

        // The FERRYMAN marquee: the BESPOKE title set — eight 32×32 glyphs, gold above a foaming
        // waterline, sea-teal below it — each a SPRITE scaling about its own center by a
        // phase-offset sine, so the crest rolls left-to-right across the word (the stadium wave).
        titleSprites_.clear();
        constexpr std::size_t kTitleLen = 8;   // the title sheet holds exactly "FERRYMAN"
        constexpr float       kBase     = 1.4f;
        constexpr float       kAmp      = 0.25f;
        constexpr float       kOffset   = 0.75f;
        constexpr int         kAdvance  = 42;
        constexpr int         kTitleY   = 82;
        const float phase = feel.titleWavePhase();
        const int   x0    = (kViewW - kAdvance * static_cast<int>(kTitleLen)) / 2;
        for (std::size_t i = 0; i < kTitleLen; ++i) {
            const float s = kBase + kAmp * std::sin(phase - static_cast<float>(i) * kOffset);
            titleSprites_.push_back(Sprite{
                .key       = "title_" + std::to_string(i),
                .x         = x0 + static_cast<int>(i) * kAdvance,
                .y         = kTitleY,
                .size      = AssetDimensions{32, 32},
                .atlas     = assets.titleAtlas(),
                .tile      = assets.title[i].tile,
                .palette   = assets.titlePal,
                .transform = Transform::scale(s, s, 16.0f, 16.0f)});
        }
        // The PRESS ENTER prompt: the classic attract-mode breath via per-sprite alpha.
        constexpr std::string_view kPrompt = "PRESS ENTER TO SET SAIL";
        const float promptAlpha = feel.promptAlpha();
        const int   px0 = (kViewW - 16 * static_cast<int>(kPrompt.size())) / 2;
        for (std::size_t i = 0; i < kPrompt.size(); ++i) {
            if (kPrompt[i] == ' ') continue;
            titleSprites_.push_back(Sprite{
                .key     = "prompt_" + std::to_string(i),
                .x       = px0 + static_cast<int>(i) * 16,
                .y       = 152,
                .size    = AssetDimensions{16, 16},
                .atlas   = assets.fontAtlas(),
                .tile    = assets.glyphBase(kPrompt[i]),
                .palette = assets.textPals[TXT_CYAN],
                .alpha   = promptAlpha});
        }
        DrawLayer marquee{.key = "titleWord"};
        marquee.z       = 95;
        marquee.size    = PixelSize{kViewW, kViewH};
        marquee.content = SpriteContent{.sprites = std::span<const Sprite>(titleSprites_)};
        frame.layers.push_back(marquee);
    }

    // Gentle, brief screen shake on ferry death (a tween-decayed RowDisplacement); absent idle.
    if (const std::optional<ScreenSpaceEffect> shake = feel.screenShake()) {
        frame.postEffects.push_back(*shake);
    }

    renderer.renderFrame(frame);  // no alpha: the engine owns interpolation, easing by each key
}

}  // namespace ferryman
