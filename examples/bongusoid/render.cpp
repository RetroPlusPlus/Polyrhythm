#include "render.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#include "retropp/geometry.h"   // AssetDimensions / PixelSize
#include "retropp/transform.h"  // Transform

namespace bong {

using namespace retropp;

BongRenderer::BongRenderer()
    : cells_(static_cast<std::size_t>(kMapW) * kMapH) {}

void BongRenderer::render(Renderer& renderer, const BongGame& game, const BongAssets& assets,
                          const BongFeel& feel) {
    // ── Text layer: clear to the dark background, then draw the title or the HUD. ────────────────────
    // Each cell names the font atlas + its palette directly; the TextPal indices select into
    // assets.textPals to resolve the actual PaletteId handle a cell carries.
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

    const std::uint16_t blank = assets.glyphTile(' ');
    for (TileCell& c : cells_) { c.tile = blank; c.atlas = assets.fontAtlas(); c.palette = assets.textPals[TXT_WHITE]; }
    if (game.state == GameState::Title) {
        centred(22, "BONGUSOID", TXT_GOLD);
        centred(30, "PRESS ENTER TO PLAY", TXT_CYAN);
        centred(40, "MOVE PADDLE   A OR CLICK SERVES", TXT_WHITE);
    } else {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%06d", game.score);
        stampText(2, 1, "SCORE", TXT_WHITE);
        stampText(8, 1, buf, TXT_WHITE);
        std::snprintf(buf, sizeof buf, "%d", game.lives);
        stampText(70, 1, "LIVES", TXT_WHITE);
        stampText(76, 1, buf, TXT_WHITE);
        // Bottom border of the status strip — a full-width rule just above the play field.
        for (int x = 0; x < kMapW; ++x) {
            TileCell& bcell = cells_[static_cast<std::size_t>(kStatusBorderRow) * kMapW + x];
            bcell.tile    = assets.borderTile();
            bcell.atlas   = assets.fontAtlas();
            bcell.palette = assets.textPals[TXT_WHITE];
        }
    }

    // ── Play layer: standing bricks, the paddle (squash), the ball (spin). ───────────────────────────
    sprites_.clear();
    // Every sprite needs a STABLE identity key (a brick's grid cell, the paddle / ball / popup names) — an
    // empty or index-derived key makes the interpolator cross-fade or skip the sprite, flashing it at the
    // wrong place as the population changes. ObjectKey owns its bytes, so a key assembled per frame passes
    // straight through.
    // Each sprite names the sprite sheet + its palette directly; the Pal / brick-color index selects
    // into assets.spritePals to resolve the actual PaletteId handle.
    auto placeSprite = [&](float x, float y, AssetDimensions size, Slot s, int pal,
                           const Transform& xf = Transform{}, ObjectKey label = "unlabelled") {
        sprites_.push_back(Sprite{
            .key = label,
            .x = static_cast<int>(x), .y = static_cast<int>(y), .size = size,
            .atlas = assets.spriteAtlas(), .tile = assets.slotTile(s),
            .palette = assets.spritePals[static_cast<std::size_t>(pal)], .transform = xf});
    };
    if (game.state == GameState::Playing) {
        for (int r = 0; r < kBrickRows; ++r) {
            for (int c = 0; c < kBrickCols; ++c) {
                const Cell& cell = game.grid[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
                Slot slot = S_BRICK; int pal = cell.colour;
                if (cell.type == Brick::None) continue;
                if (cell.type == Brick::Silver) { slot = S_SILVER; pal = cell.hp >= 2 ? PAL_SILVER : PAL_SILVER_CRACK; }
                else if (cell.type == Brick::Gold) { slot = S_GOLD; pal = PAL_GOLD; }
                placeSprite(brickX(c), brickY(r), AssetDimensions{static_cast<int>(kBrickW), static_cast<int>(kBrickH)}, slot, pal,
                            Transform{}, "brick_" + std::to_string(r) + "_" + std::to_string(c));
            }
        }
        // Paddle: a brief vertical squash on a bounce (scaleY about its center); identity when at rest.
        const float sy = feel.paddleScaleY();
        const Transform paddleXf = (sy == 1.0f)
            ? Transform{}
            : Transform::scale(1.0f, sy, kPaddleW / 2.0f, kPaddleH / 2.0f);
        placeSprite(game.paddleX, kPaddleY, AssetDimensions{static_cast<int>(kPaddleW), static_cast<int>(kPaddleH)}, S_VAUS, PAL_PADDLE, paddleXf, "paddle");
        // Ball: tumbles about its center in the direction of its english.
        const Transform ballXf = Transform::rotation(feel.ballSpinDegrees(), kBallSz / 2.0f, kBallSz / 2.0f);
        placeSprite(game.ballX, game.ballY, AssetDimensions{static_cast<int>(kBallSz), static_cast<int>(kBallSz)}, S_BALL, PAL_BALL, ballXf, "ball");
    }

    // ── Popup layer: the floating "+N" score numbers (font glyphs, drawn as sprites so index 0 is ──────
    //    transparent). Each rises and shrinks away over its life; progress drives both.
    popupSprites_.clear();
    for (const ScorePopup& p : feel.popups()) {
        const float prog = p.progress.value();   // 0 → 1
        const float scl  = 1.0f - prog;          // shrink to nothing
        if (scl <= 0.02f) continue;
        char buf[8];
        std::snprintf(buf, sizeof buf, "%d", p.points);
        const int n = static_cast<int>(std::strlen(buf));
        const float gx = p.x + kBrickW / 2.0f - static_cast<float>(n) * kTile / 2.0f;  // centered on the brick
        const float gy = p.y - prog * 26.0f;                                           // rise
        for (int i = 0; i < n; ++i) {
            const Transform xf = Transform::scale(scl, scl, kTile / 2.0f, kTile / 2.0f);
            popupSprites_.push_back(Sprite{
                .key = "pop_" + std::to_string(p.id) + "_" + std::to_string(i),
                .x = static_cast<int>(gx + static_cast<float>(i) * kTile), .y = static_cast<int>(gy),
                .size = AssetDimensions{kTile, kTile}, .atlas = assets.fontAtlas(),
                .tile = assets.glyphTile(buf[i]), .palette = assets.textPals[TXT_GOLD], .transform = xf});
        }
    }

    // ── Assemble the frame: text/backdrop (z=0) → play (z=10) → popups (z=20). ────────────────────────
    FrameDrawState frame;
    DrawLayer bg{.key = "backdrop"}; bg.z = 0; bg.size = PixelSize{kViewW, kViewH};
    bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                             .cells = std::span<const TileCell>(cells_)};
    frame.layers.push_back(bg);

    DrawLayer play{.key = "play"}; play.z = 10; play.size = PixelSize{kViewW, kViewH};
    play.content = SpriteContent{.sprites = std::span<const Sprite>(sprites_)};
    frame.layers.push_back(play);

    DrawLayer pops{.key = "popups"}; pops.z = 20; pops.size = PixelSize{kViewW, kViewH};
    pops.content = SpriteContent{.sprites = std::span<const Sprite>(popupSprites_)};
    frame.layers.push_back(pops);

    // Gentle, brief screen shake on impact (a tween-decayed RowDisplacement); absent while idle.
    if (const std::optional<ScreenSpaceEffect> shake = feel.screenShake()) {
        frame.postEffects.push_back(*shake);
    }

    renderer.renderFrame(frame);  // no alpha: the engine owns interpolation, easing each layer/sprite by its key
}

}  // namespace bong
