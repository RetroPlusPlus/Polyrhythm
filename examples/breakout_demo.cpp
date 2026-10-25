// ============================================================================================
//  Breakout — a small, complete game on Polyrhythm, written as a heavily documented teaching example.
//  Like the Pong and Missile Command demos, every block explains what it does AND which engine API
//  it touches. This one is a brick-breaker: a paddle, a bouncing ball, and a grid of bricks.
// ============================================================================================
//
//  WHAT THIS DEMONSTRATES (engine features, roughly in order):
//    • EngineConfig with a NON-DEFAULT viewport + timing:
//        - ViewportResolution::Nes → a 256×240 internal resolution (the NES).
//        - TimingProfile{TickPeriodNs::Hz60} → a clean 60 Hz fixed step (built from the period enum;
//          there is no named TimingProfile::Hz60 — only GameBoy/GameBoyColor are named statics).
//    • SteadyClock + RunLoop                   — fixed-step sim (simTick) decoupled from render (renderLoop).
//    • SdlPlatform + Renderer                  — the live window + GPU device + draw API.
//    • uploadAtlas / uploadPalette             — indexed art + color onto the GPU.
//    • A TILE layer for the static backdrop     — dark wall + the score / lives readout (8px grid).
//    • SPRITE layers for everything dynamic      — bricks (per-row color), paddle, ball; the bricks use
//                                                a DYNAMIC sprite count (a std::vector<Sprite> rebuilt
//                                                each frame, only for the bricks still standing).
//
//  THE GAME:
//    • Left / Right move the paddle. X (pad A) serves the ball off the paddle.
//    • The ball bounces off the walls, the paddle (taking sideways "english" from where it hits), and
//      bricks (which it destroys for points). The ball's speed stays roughly constant.
//    • Miss the ball and it falls past the paddle → you lose a life and re-serve. Lose all 3 lives →
//      the board, score, and lives reset.
//    • Clear every brick → a fresh board (score + lives carry over).
//
// The demo never auto-launches a window; you run it yourself.
//
//  CI: like the other example hosts it instantiates SdlPlatform + Renderer for real, so the live GPU
//  path keeps compiling/linking on every CI platform — but CI never opens the window.
// ============================================================================================

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "retropp/clock.h"          // SteadyClock
#include "retropp/draw_state.h"     // FrameDrawState / DrawLayer / TileContent / SpriteContent / Sprite
#include "retropp/engine_config.h"  // EngineConfig
#include "retropp/input.h"          // InputState (isHeld / justPressed)
#include "retropp/input_actions.h"  // ActionMap + PadButton — the demo's bindings
#include "retropp/palette.h"        // Rgba8 / PaletteId
#include "retropp/renderer.h"       // Renderer — uploadAtlas/uploadPalette + renderFrame
#include "retropp/run_loop.h"       // RunLoop — simTick / renderLoop
#include "retropp/sdl_platform.h"   // SdlPlatform
#include "retropp/timing.h"         // TimingProfile, TickPeriodNs (Hz60 lives here)
#include "retropp/viewport.h"       // ViewportResolution — the Nes preset
#include "retropp/windowed_host.h"  // WindowedHost — pumps OS events + drives the loop

namespace {

using namespace retropp;
using namespace std::chrono_literals;

// The game's input vocabulary: paddle movement + the serve.
enum class Action : std::uint8_t { Left, Right, Serve };

constexpr int kTile = 8;  // 8×8 px tiles

// Gameplay tuning (absolute pixels / px-per-tick at 60 Hz). Viewport-derived layout (screen size,
// paddle y, brick origin) is computed in main from the active viewport.
constexpr float kPaddleW = 36.0f, kPaddleH = 6.0f;
constexpr float kBallSz  = 4.0f;
constexpr float kPaddleSpeed = 3.5f;
constexpr float kBallSpeed   = 2.6f;   // constant ball speed magnitude
constexpr float kMaxEnglish  = 2.1f;   // max horizontal speed imparted by a paddle-edge hit
constexpr float kMinEnglish  = 0.8f;   // MINIMUM horizontal speed — the ball can never go perfectly
                                       // vertical, so it can't lock into an endless bounce inside an
                                       // already-cleared column (it always drifts to reach every brick)
constexpr int   kLives       = 3;
constexpr int   kScorePerBrick = 10;

// Brick grid.
constexpr int   kBrickCols = 12, kBrickRows = 7;
constexpr float kBrickAreaTop = 34.0f;  // y of the top brick row
constexpr float kBrickCellW = 20.0f, kBrickCellH = 11.0f;  // grid spacing (includes the gap)
constexpr float kBrickW = 18.0f, kBrickH = 8.0f;            // drawn brick size (cell minus the gap)
constexpr float kBrickMarginX = 8.0f;                       // left/right margin of the brick field

// ── 5×7 digit font (0–9): one byte per row, low 5 bits = the 5 columns (bit 4 = leftmost) ─────────────
constexpr std::array<std::array<std::uint8_t, 7>, 10> kDigits{{
    {0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110},  // 0
    {0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110},  // 1
    {0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111},  // 2
    {0b11111,0b00010,0b00100,0b00010,0b00001,0b10001,0b01110},  // 3
    {0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010},  // 4
    {0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110},  // 5
    {0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110},  // 6
    {0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000},  // 7
    {0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110},  // 8
    {0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100},  // 9
}};

constexpr int kTileSolid  = 0;   // backdrop tile-atlas: 0 = solid block, 1..10 = digits 0..9
constexpr int kTileDigit0 = 1;
constexpr int kBgTiles    = 11;

// Build the backdrop tile atlas: solid block (recolored per cell) + the digits for score / lives.
std::vector<std::uint8_t> buildBgAtlas() {
    const int w = kTile * kBgTiles;
    std::vector<std::uint8_t> a(static_cast<std::size_t>(w) * kTile, 0);
    for (int p = 0; p < kTile * kTile; ++p) a[p] = 1;  // tile 0: solid
    for (int d = 0; d < 10; ++d) {
        for (int row = 0; row < 7; ++row) {
            const std::uint8_t bits = kDigits[static_cast<std::size_t>(d)][static_cast<std::size_t>(row)];
            for (int col = 0; col < 5; ++col) {
                if ((bits >> (4 - col)) & 1) {
                    a[static_cast<std::size_t>(1 + row) * w + ((kTileDigit0 + d) * kTile + 1 + col)] = 1;
                }
            }
        }
    }
    return a;
}

// A tiny clock-seeded LCG (variety per run; native game, not a GB ROM).
std::uint32_t nextRand(std::uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

}  // namespace

int main() {

    // ── 1. Startup configuration — an NES game at 60 Hz ─────────────────────────────────────────────
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Breakout Demo"},
        .window   = {.title = "Polyrhythm — Breakout (NES, 60 Hz)"},
        .viewport = ViewportResolution::Nes,                  // 256×240
        .timing   = TimingProfile{TickPeriodNs::Hz60}};       // 60 Hz, built from the period enum
    EngineConfig::setActive(config);

    // ── 2. Core engine objects ──────────────────────────────────────────────────────────────────────
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // Bindings: the paddle steers on arrows / A+D / d-pad; X (pad A / Sony ✕) serves. Only Left and
    // Right exist as movement — the game has no vertical axis, so no directional preset.
    ActionMap map{
        {Action::Left,  {SDL_SCANCODE_LEFT, SDL_SCANCODE_A, PadButton::DpadLeft}},
        {Action::Right, {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D, PadButton::DpadRight}},
        {Action::Serve, {SDL_SCANCODE_X, PadButton::FaceSouth}},
    };
    platform.actions(map);

    // ── 3. Playfield dimensions, read from the active viewport ──────────────────────────────────────
    const int   kViewW = config.viewport.width;    // 256
    const int   kViewH = config.viewport.height;   // 240
    const int   kMapW  = kViewW / kTile;           // 32 backdrop tiles wide
    const int   kMapH  = kViewH / kTile;           // 30 tall
    const float kPaddleY = kViewH - 16.0f;          // paddle top-y
    const float kPlayTop = 16.0f;                   // ball can't go above this (room for the HUD)

    // ── 4. Upload art + palettes ─────────────────────────────────────────────────────────────────────
    // 4a. Backdrop tile atlas (solid + digits). Cell palette picks the color; pixel index picks the
    //     entry. The whole backdrop is one solid color (palette 0); the HUD digits use palette 1.
    const std::vector<std::uint8_t> bgPx = buildBgAtlas();
    const AtlasId bgAtlas = renderer.uploadAtlas(bgPx.data(), kTile * kBgTiles, kTile).atlasId;
    const std::array<Rgba8, 2> palWall{{ {18, 18, 26}, {18, 18, 26} }};     // solid → dark wall
    const std::array<Rgba8, 2> palHud{{ {18, 18, 26}, {230, 230, 240} }};   // digit: wall bg + white lit
    const PaletteId wallPal = renderer.uploadPalette(std::span<const Rgba8>(palWall));
    const PaletteId hudPal  = renderer.uploadPalette(std::span<const Rgba8>(palHud));

    // 4b. A solid 16×16 atlas (all index 1) → bricks, paddle, ball. Each sprite reads a `size`-sized
    //     region from tile 0's top-left, so any rect up to 16×16 is solid fill; color = the palette.
    //     Bricks taller/wider than 16 would exceed the atlas, so kBrickW/H stay ≤ 16 (18 > 16!) —
    //     see the note below; we use an atlas wide enough for the brick width.
    constexpr int kSolidW = 40, kSolidH = 16;  // wide enough for a 36px paddle / 18px brick row cell
    std::array<std::uint8_t, kSolidW * kSolidH> solidPx{};
    solidPx.fill(1);
    const AtlasId solidAtlas = renderer.uploadAtlas(solidPx.data(), kSolidW, kSolidH).atlasId;
    // Palette set: 7 brick-row colors, then paddle, then ball. sprite.palette selects (entry 1 = color).
    const std::array<std::array<Rgba8, 2>, 9> solidColours{{
        {{ {0,0,0}, {220, 60, 60} }},    // 0 row 0 — red
        {{ {0,0,0}, {230, 130, 50} }},   // 1 row 1 — orange
        {{ {0,0,0}, {225, 210, 70} }},   // 2 row 2 — yellow
        {{ {0,0,0}, {90, 200, 90} }},    // 3 row 3 — green
        {{ {0,0,0}, {70, 190, 210} }},   // 4 row 4 — cyan
        {{ {0,0,0}, {90, 130, 235} }},   // 5 row 5 — blue
        {{ {0,0,0}, {170, 110, 230} }},  // 6 row 6 — purple
        {{ {0,0,0}, {235, 235, 245} }},  // 7 paddle — white
        {{ {0,0,0}, {245, 230, 160} }},  // 8 ball   — warm white
    }};
    std::array<PaletteId, 9> solidPals{};
    for (std::size_t i = 0; i < solidColours.size(); ++i) {
        solidPals[i] = renderer.uploadPalette(std::span<const Rgba8>(solidColours[i]));
    }
    const int PAL_PADDLE = 7, PAL_BALL = 8;  // brick rows are palettes 0..6

    // ── 5. Game state ────────────────────────────────────────────────────────────────────────────────
    std::array<std::array<bool, kBrickCols>, kBrickRows> bricks{};
    auto resetBricks = [&] { for (auto& row : bricks) row.fill(true); };
    resetBricks();
    int bricksLeft = kBrickCols * kBrickRows;

    float paddleX = kViewW / 2.0f - kPaddleW / 2;       // paddle top-left x
    float ballX = 0, ballY = 0, ballVx = 0, ballVy = 0; // ball top-left + velocity
    int   score = 0, lives = kLives;
    bool  serving = true;                                // ball rides the paddle until A
    std::uint32_t rng = static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    // Brick rectangle (top-left x/y) for a given column/row.
    auto brickX = [&](int col) { return kBrickMarginX + col * kBrickCellW; };
    auto brickY = [&](int row) { return kBrickAreaTop + row * kBrickCellH; };

    // Re-center the ball on the paddle for a serve.
    auto parkBall = [&] {
        ballX = paddleX + kPaddleW / 2 - kBallSz / 2;
        ballY = kPaddleY - kBallSz - 1;
        ballVx = 0; ballVy = 0;
    };
    parkBall();

    // Aim the ball UPWARD with a desired horizontal velocity plus a tiny random jitter, renormalized so
    // |v| ≈ kBallSpeed. The jitter is the key robustness trick: it keeps the ball off any perfectly-
    // periodic orbit, so even a player holding the paddle dead-center can't trap the ball bouncing
    // forever in an already-cleared column — it always drifts and eventually reaches every brick.
    auto aimBallUp = [&](float desiredVx) {
        desiredVx += (static_cast<float>(nextRand(rng) % 1000) / 1000.0f - 0.5f) * 0.8f;  // ±0.4 jitter
        desiredVx = std::clamp(desiredVx, -kMaxEnglish, kMaxEnglish);
        if (std::abs(desiredVx) < kMinEnglish) desiredVx = (desiredVx < 0 ? -kMinEnglish : kMinEnglish);
        ballVx = desiredVx;
        ballVy = -std::sqrt(std::max(0.5f, kBallSpeed * kBallSpeed - ballVx * ballVx));
    };

    // Launch the ball off the paddle with a small random lean.
    auto serve = [&] {
        aimBallUp((static_cast<float>(nextRand(rng) % 200) / 100.0f - 1.0f) * 1.0f);  // base lean [-1,1]
        serving = false;
    };

    // Lose a life (or reset the whole game when out of lives), then re-serve.
    auto loseLife = [&] {
        if (--lives <= 0) {
            std::printf("game over — score %d — new game\n", score);
            lives = kLives; score = 0; resetBricks(); bricksLeft = kBrickCols * kBrickRows;
        }
        serving = true;
        parkBall();
    };

    // ── 6. Simulation step (60 Hz) ───────────────────────────────────────────────────────────────────
    loop.simTick([&](const InputState& in) {
        // 6a. Paddle (Left/Right), clamped to the screen.
        if (in.isHeld(Action::Left))  paddleX -= kPaddleSpeed;
        if (in.isHeld(Action::Right)) paddleX += kPaddleSpeed;
        paddleX = std::clamp(paddleX, 0.0f, kViewW - kPaddleW);

        // 6b. While serving the ball rides the paddle; Serve launches it.
        if (serving) {
            parkBall();
            if (in.justPressed(Action::Serve)) serve();
            return;
        }

        // 6c. Integrate the ball.
        ballX += ballVx;
        ballY += ballVy;

        // 6d. Walls: bounce off left/right/top; falling past the bottom loses a life.
        if (ballX < 0)                  { ballX = 0;                   ballVx = std::abs(ballVx); }
        if (ballX > kViewW - kBallSz)   { ballX = kViewW - kBallSz;    ballVx = -std::abs(ballVx); }
        if (ballY < kPlayTop)           { ballY = kPlayTop;            ballVy = std::abs(ballVy); }
        if (ballY > kViewH)             { loseLife(); return; }

        // 6e. Paddle: bounce up, and set horizontal english from where on the paddle it hit (center =
        //     straight up, edges = steep angle). Speed is renormalized so |v| stays ≈ kBallSpeed.
        if (ballVy > 0 &&
            ballX < paddleX + kPaddleW && ballX + kBallSz > paddleX &&
            ballY + kBallSz >= kPaddleY && ballY + kBallSz <= kPaddleY + kPaddleH + 4) {
            const float hit = ((ballX + kBallSz / 2) - (paddleX + kPaddleW / 2)) / (kPaddleW / 2);  // [-1,1]
            aimBallUp(hit * kMaxEnglish);  // english from the hit offset, + jitter, renormalized
            ballY = kPaddleY - kBallSz - 1;
        }

        // 6f. Bricks: find the first brick the ball overlaps, reflect off the nearer face (minimum-
        //     translation axis), destroy it, and score. One brick per tick (the ball is slow enough).
        for (int r = 0; r < kBrickRows && (ballVx != 0 || ballVy != 0); ++r) {
            for (int c = 0; c < kBrickCols; ++c) {
                if (!bricks[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)]) continue;
                const float bx = brickX(c), by = brickY(r);
                if (ballX < bx + kBrickW && ballX + kBallSz > bx &&
                    ballY < by + kBrickH && ballY + kBallSz > by) {
                    // Overlap depths on each axis → reflect the axis with the smaller penetration.
                    const float overlapL = (ballX + kBallSz) - bx;        // pushing left
                    const float overlapR = (bx + kBrickW) - ballX;        // pushing right
                    const float overlapT = (ballY + kBallSz) - by;        // pushing up
                    const float overlapB = (by + kBrickH) - ballY;        // pushing down
                    const float minX = std::min(overlapL, overlapR);
                    const float minY = std::min(overlapT, overlapB);
                    if (minX < minY) ballVx = -ballVx; else ballVy = -ballVy;
                    bricks[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = false;
                    score += kScorePerBrick;
                    if (--bricksLeft == 0) {                  // board cleared → fresh board, keep score
                        std::printf("board cleared — score %d — next board\n", score);
                        resetBricks(); bricksLeft = kBrickCols * kBrickRows;
                        serving = true; parkBall();
                    }
                    return;  // one brick per tick
                }
            }
        }
    });

    // Reused per-frame buffers.
    std::vector<TileCell> bgCells(static_cast<std::size_t>(kMapW) * kMapH);
    std::vector<Sprite>   solidSprites;
    // Every sprite gets a STABLE identity key — a grid cell for a brick, a fixed name for the paddle /
    // ball — NEVER its emission index: as bricks break the index shifts, and an index-derived key would
    // make the interpolator match the paddle or ball to a broken brick's history and flash it there for a
    // tick. ObjectKey owns its bytes, so a key assembled per frame moves straight into the sprite.

    auto rect = [&](std::string key, float x, float y, float w, float h, int pal) {
        solidSprites.push_back(Sprite{
            .key = std::move(key),
            .x = static_cast<int>(x), .y = static_cast<int>(y),
            .size = AssetDimensions{static_cast<int>(w), static_cast<int>(h)}, .atlas = solidAtlas,
            .tile = 0, .palette = solidPals[static_cast<std::size_t>(pal)]});
    };
    // Stamp a number into the HUD tile row, right-aligned ending at `endCol`.
    auto putNumber = [&](int value, int endCol) {
        int v = value, col = endCol;
        do { const int d = v % 10; v /= 10;
             bgCells[static_cast<std::size_t>(kMapW) + col].tile = static_cast<std::uint16_t>(kTileDigit0 + d);
             bgCells[static_cast<std::size_t>(kMapW) + col].atlas = bgAtlas;
             bgCells[static_cast<std::size_t>(kMapW) + col].palette = hudPal;  // HUD palette
             --col;
        } while (v > 0 && col >= 0);
    };

    // ── 7. Render step ───────────────────────────────────────────────────────────────────────────────
    loop.renderLoop([&]() {
        // 7a. Backdrop: solid wall everywhere, then the HUD — score (left) and lives (right) on row 1.
        for (auto& c : bgCells) { c.tile = kTileSolid; c.atlas = bgAtlas; c.palette = wallPal; }
        putNumber(score, 7);          // score, left side
        putNumber(lives, kMapW - 2);  // lives, right side

        // 7b. Bricks (only the standing ones), paddle, ball — each under a STABLE identity key (a brick's
        //     grid cell, the paddle / ball singleton names), so a broken brick never shifts the identity of
        //     the sprites emitted after it.
        solidSprites.clear();
        for (int r = 0; r < kBrickRows; ++r) {
            for (int c = 0; c < kBrickCols; ++c) {
                if (bricks[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)]) {
                    rect("brick_" + std::to_string(r) + "_" + std::to_string(c),
                         brickX(c), brickY(r), kBrickW, kBrickH, r);  // palette r = the row's color
                }
            }
        }
        rect("paddle", paddleX, kPaddleY, kPaddleW, kPaddleH, PAL_PADDLE);
        rect("ball", ballX, ballY, kBallSz, kBallSz, PAL_BALL);

        // 7c. Assemble the frame: backdrop (z=0) → bricks/paddle/ball (z=10).
        FrameDrawState frame;
        DrawLayer bg{.key = "wall"}; bg.z = 0; bg.size = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                 .cells = std::span<const TileCell>(bgCells)};
        frame.layers.push_back(bg);

        DrawLayer play{.key = "play"}; play.z = 10; play.size = PixelSize{kViewW, kViewH};
        play.content = SpriteContent{.sprites = std::span<const Sprite>(solidSprites)};
        frame.layers.push_back(play);

        renderer.renderFrame(frame);
    });

    std::printf("Breakout (NES, 60 Hz) — Left/Right move the paddle, X (pad A) serves. Clear the bricks; "
                "don't drop the ball (3 lives). Close to quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
