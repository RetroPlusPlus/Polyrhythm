#include "render.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "retropp/geometry.h"   // AssetDimensions / PixelSize / Point
#include "retropp/transform.h"  // Transform

namespace polter {

using namespace retropp;

PolterRenderer::PolterRenderer()
    : cells_(static_cast<std::size_t>(kMapW) * kMapH) {}

void PolterRenderer::render(Renderer& renderer, const PolterGame& game, const PolterAssets& assets,
                            const PolterFeel& feel) {
    // ── Backdrop layer: clear to the dark background, then either the title or HUD + maze. ─────────
    auto stampText = [&](int col, int row, std::string_view s, std::size_t pal) {
        for (std::size_t i = 0; i < s.size() && col + static_cast<int>(i) < kMapW; ++i) {
            TileCell& cell = cells_[static_cast<std::size_t>(row) * kMapW + col + static_cast<int>(i)];
            cell.tile    = assets.glyphTile(s[i]);
            cell.atlas   = assets.fontAtlas();
            cell.palette = assets.textPals[pal];
        }
    };
    auto centred = [&](int row, std::string_view s, std::size_t pal) {
        stampText((kMapW - static_cast<int>(s.size())) / 2, row, s, pal);
    };
    // Stamp one 32×32 maze block: a 4×4 group of 8px tiles read from the tiles sheet. The block's
    // top-left 8px tile comes from the manifest slot; its neighbors sit at +dx and +stride·dy on
    // the sheet's own tile grid (kTileSheetCols8 — the sheet is 160px → 20 tile columns).
    auto stampBlock = [&](int cellC, int cellR, TileBlock block, std::size_t tilePal) {
        const std::uint16_t base = assets.blockTile(block);
        const int tx = cellC * kCellTiles;
        const int ty = kMazeTop / kTile + cellR * kCellTiles;
        for (int dy = 0; dy < kCellTiles; ++dy) {
            for (int dx = 0; dx < kCellTiles; ++dx) {
                TileCell& cell = cells_[static_cast<std::size_t>(ty + dy) * kMapW + tx + dx];
                cell.tile    = static_cast<std::uint16_t>(base + dy * kTileSheetCols8 + dx);
                cell.atlas   = assets.tileAtlas();
                cell.palette = assets.tilePals[tilePal];
            }
        }
    };

    const std::uint16_t blank = assets.glyphTile(' ');
    for (TileCell& c : cells_) {
        c.tile    = blank;
        c.atlas   = assets.fontAtlas();
        c.palette = assets.textPals[TXT_WHITE];
    }

    if (game.state == GameState::Title) {
        centred(18, "POLTERBALL", TXT_GOLD);
        centred(24, "PRESS ENTER TO PLAY", TXT_CYAN);
        centred(30, "MOVE PADDLE   A OR CLICK SERVES", TXT_WHITE);
        centred(33, "HERD THE BALL  THE GHOSTS HUNT IT", TXT_WHITE);
        centred(36, "POWER PELLETS TURN THE TABLES", TXT_WHITE);
        if (game.score > 0) {
            char buf[24];
            std::snprintf(buf, sizeof buf, "LAST SCORE %06d", game.score);
            centred(42, buf, TXT_GOLD);
        }
    } else {
        // The HUD line + its rule.
        char buf[16];
        std::snprintf(buf, sizeof buf, "%06d", game.score);
        stampText(2, kHudTextRow, "SCORE", TXT_WHITE);
        stampText(8, kHudTextRow, buf, TXT_WHITE);
        std::snprintf(buf, sizeof buf, "%02d", game.boardNum);
        stampText(36, kHudTextRow, "BOARD", TXT_CYAN);
        stampText(42, kHudTextRow, buf, TXT_CYAN);
        std::snprintf(buf, sizeof buf, "%d", game.lives);
        stampText(70, kHudTextRow, "LIVES", TXT_WHITE);
        stampText(76, kHudTextRow, buf, TXT_WHITE);
        for (int x = 0; x < kMapW; ++x) {
            TileCell& bcell = cells_[static_cast<std::size_t>(kHudBorderRow) * kMapW + x];
            bcell.tile    = assets.borderTile();
            bcell.atlas   = assets.fontAtlas();
            bcell.palette = assets.textPals[TXT_WHITE];
        }
        // The maze, from its CURRENT state — eaten pellets restamp as plain floor, broken soft
        // walls as corridor, so the backdrop always shows the board the sim is playing on.
        for (int r = 0; r < kMazeRows; ++r) {
            for (int c = 0; c < kMazeCols; ++c) {
                switch (game.board.kind[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)]) {
                    case CellKind::Hard:
                        stampBlock(c, r, T_HARD, TP_WALL);
                        break;
                    case CellKind::Soft:
                        stampBlock(c, r, T_SOFT, TP_SOFT);
                        break;
                    case CellKind::Floor:
                        stampBlock(c, r,
                                   game.board.pellet[static_cast<std::size_t>(r)]
                                                    [static_cast<std::size_t>(c)]
                                       ? T_PELLET
                                       : T_FLOOR,
                                   TP_PELLET);
                        break;
                    case CellKind::CourtGate:
                    case CellKind::PenFloor:
                        stampBlock(c, r, T_FLOOR, TP_PELLET);
                        break;
                    case CellKind::PenGate:
                        stampBlock(c, r, T_GATE, TP_GATE);
                        break;
                }
            }
        }
    }

    // ── Maze actors: the pulsing power pellets + the ghosts (or their homing eyes). ────────────────
    actorSprites_.clear();
    moverSprites_.clear();
    popupSprites_.clear();
    if (game.state == GameState::Playing) {
        // Power pellets: the SAME art under the pulse clip's current palette — palette cycling via
        // an AnimationPlayer, the engine's color-animation idiom (the art never changes).
        const AnimationFrame& pow = feel.powerFrame();
        for (int i = 0; i < game.board.powerCount; ++i) {
            if (!game.board.powerAlive[static_cast<std::size_t>(i)]) continue;
            const CellRC cell = game.board.power[static_cast<std::size_t>(i)];
            actorSprites_.push_back(Sprite{
                .key     = "pow_" + std::to_string(game.epoch) + "_" + std::to_string(i),
                .x       = static_cast<int>(cellPxX(cell.c)) + kCell / 2 - 8,
                .y       = static_cast<int>(cellPxY(cell.r)) + kCell / 2 - 8,
                .size    = pow.size(),
                .atlas   = pow.atlas(),
                .tile    = pow.tile(),
                .palette = pow.palette});
        }

        // Ghosts. Identity keys carry the game's epoch: a board/life reset TELEPORTS the cast back
        // to the pen, and the fresh key makes each ghost mount-snap there instead of streaking
        // across the maze. Body color is pure palette selection — role palette normally, the one
        // frightened blue while the ball is ignited; the skirt frame comes from the shared clip.
        const Slot step = feel.ghostStep() == 0 ? S_GHOST_A : S_GHOST_B;
        for (int i = 0; i < kGhostCount; ++i) {
            const Ghost& g  = game.squad.ghost(i);
            const Vec2   gc = game.squad.center(i);
            if (g.state == GhostState::Eyes) {
                actorSprites_.push_back(Sprite{
                    .key     = "eyes_" + std::to_string(i) + "_" + std::to_string(game.epoch),
                    .x       = static_cast<int>(gc.x) - 8,
                    .y       = static_cast<int>(gc.y) - 8,
                    .size    = AssetDimensions{16, 16},
                    .atlas   = assets.spriteAtlas(),
                    .tile    = assets.slotTile(S_EYES),
                    .palette = assets.spritePals[PAL_EYES]});
            } else {
                const std::size_t pal =
                    g.frightened ? static_cast<std::size_t>(PAL_FRIGHT)
                                 : static_cast<std::size_t>(PAL_GHOST_0) + static_cast<std::size_t>(i);
                actorSprites_.push_back(Sprite{
                    .key     = "ghost_" + std::to_string(i) + "_" + std::to_string(game.epoch),
                    .x       = static_cast<int>(gc.x) - 12,
                    .y       = static_cast<int>(gc.y) - 12,
                    .size    = AssetDimensions{24, 24},
                    .atlas   = assets.spriteAtlas(),
                    .tile    = assets.slotTile(step),
                    .palette = assets.spritePals[pal]});
            }
        }

        // The movers. The ball's key carries its serve number — every re-park is a teleport onto
        // the paddle, and the fresh key mount-snaps it there. Its palette flips fiery while
        // ignited. The paddle squashes about its own center on a bounce (a per-sprite transform).
        moverSprites_.push_back(Sprite{
            .key     = "ball_" + std::to_string(game.serveCount),
            .x       = static_cast<int>(game.ballX),
            .y       = static_cast<int>(game.ballY),
            .size    = AssetDimensions{static_cast<int>(kBallSz), static_cast<int>(kBallSz)},
            .atlas   = assets.spriteAtlas(),
            .tile    = assets.slotTile(S_BALL),
            .palette = assets.spritePals[game.ignited() ? PAL_BALL_FIRE : PAL_BALL]});
        const float     sy = feel.paddleScaleY();
        const Transform paddleXf =
            (sy == 1.0f) ? Transform{}
                         : Transform::scale(1.0f, sy, kPaddleW / 2.0f, kPaddleH / 2.0f);
        moverSprites_.push_back(Sprite{
            .key       = "paddle",
            .x         = static_cast<int>(game.paddleX),
            .y         = static_cast<int>(kPaddleY),
            .size      = AssetDimensions{static_cast<int>(kPaddleW), static_cast<int>(kPaddleH)},
            .atlas     = assets.spriteAtlas(),
            .tile      = assets.slotTile(S_PADDLE),
            .palette   = assets.spritePals[PAL_PADDLE],
            .transform = paddleXf});

        // Popups: the floating "+N" where a ghost went down (font glyphs drawn as sprites so index
        // 0 is transparent). Each rises and shrinks away over its life; progress drives both.
        for (const ScorePopup& p : feel.popups()) {
            const float prog = p.progress.value();  // 0 → 1
            const float scl  = 1.0f - prog;         // shrink to nothing
            if (scl <= 0.02f) continue;
            char buf[8];
            std::snprintf(buf, sizeof buf, "%d", p.points);
            const int   n  = static_cast<int>(std::strlen(buf));
            const float gx = p.x - static_cast<float>(n) * kTile / 2.0f;  // centered on the ghost
            const float gy = p.y - 12.0f - prog * 26.0f;                  // rise
            for (int i = 0; i < n; ++i) {
                const Transform xf = Transform::scale(scl, scl, kTile / 2.0f, kTile / 2.0f);
                popupSprites_.push_back(Sprite{
                    .key       = "pop_" + std::to_string(p.id) + "_" + std::to_string(i),
                    .x         = static_cast<int>(gx + static_cast<float>(i) * kTile),
                    .y         = static_cast<int>(gy),
                    .size      = AssetDimensions{kTile, kTile},
                    .atlas     = assets.fontAtlas(),
                    .tile      = assets.glyphTile(buf[i]),
                    .palette   = assets.textPals[TXT_GOLD],
                    .transform = xf});
            }
        }
    }

    // ── Assemble the frame: backdrop (z=0) → actors (z=10) → movers (z=20) → popups (z=30). ────────
    FrameDrawState frame;
    DrawLayer bg{.key = "backdrop"};
    bg.z       = 0;
    bg.size    = PixelSize{kViewW, kViewH};
    bg.content = TileContent{.widthInTiles  = kMapW,
                             .heightInTiles = kMapH,
                             .cells         = std::span<const TileCell>(cells_)};
    frame.layers.push_back(bg);

    if (game.state == GameState::Playing) {
        DrawLayer actors{.key = "actors"};
        actors.z       = 10;
        actors.size    = PixelSize{kViewW, kViewH};
        actors.content = SpriteContent{.sprites = std::span<const Sprite>(actorSprites_)};
        frame.layers.push_back(actors);

        DrawLayer movers{.key = "movers"};
        movers.z       = 20;
        movers.size    = PixelSize{kViewW, kViewH};
        movers.content = SpriteContent{.sprites = std::span<const Sprite>(moverSprites_)};
        frame.layers.push_back(movers);

        DrawLayer pops{.key = "popups"};
        pops.z       = 30;
        pops.size    = PixelSize{kViewW, kViewH};
        pops.content = SpriteContent{.sprites = std::span<const Sprite>(popupSprites_)};
        frame.layers.push_back(pops);

        // The ignite glow: an Add-blended ColorFill circle riding the ball — a warm halo lifted
        // over the composited scene, alive only while the ignite clock runs.
        if (game.ignited()) {
            frame.regions.push_back(Region{
                .key     = "igniteGlow",
                .shape   = ShapePoints::circle(Point{game.ballX + kBallSz / 2,
                                                     game.ballY + kBallSz / 2}, 18.0f),
                .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                              .fill = Rgba8{255, 150, 40}}},
                .alpha   = 0.35f,
                .blend   = BlendMode::Add});
        }
    }

    // Gentle, brief screen shake on a lost ball (a tween-decayed RowDisplacement); absent while idle.
    if (const std::optional<ScreenSpaceEffect> shake = feel.screenShake()) {
        frame.postEffects.push_back(*shake);
    }

    renderer.renderFrame(frame);  // no alpha: the engine owns interpolation, easing by each key
}

}  // namespace polter
