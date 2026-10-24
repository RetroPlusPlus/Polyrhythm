// ============================================================================================
//  Pong — a small, COMPLETE game built on Polyrhythm, written as a teaching example.
//  Every section below is commented in full: not just what the game does, but which engine
//  API each line touches and why. If you are learning the engine, read this top to bottom.
// ============================================================================================
//
//  WHAT THIS DEMONSTRATES (engine features, in the order they appear):
//    • EngineConfig + EngineConfig::setActive  — the one startup bundle (window/viewport/timing);
//                                                bare-constructed engine objects inherit it.
//    • SteadyClock + RunLoop                   — the fixed-step sim: simTick() runs game logic at a
//                                                steady cadence, renderLoop() draws (decoupled).
//    • Keyed sprites + engine interpolation    — each mover carries a stable key, so the engine eases it
//                                                between tick states on its own; the sim writes positions
//                                                and the render submits them.
//    • SdlPlatform + Renderer                  — the live window + GPU device + the draw API.
//    • uploadAtlas / uploadPalette             — getting indexed art + color onto the GPU.
//    • TileContent  (a tilemap layer)          — the court: net + scoreboard, on an 8px tile grid.
//    • SpriteContent (free-moving sprites)     — the two paddles + the ball, at pixel positions.
//    • A whole-frame dim (a Multiply ColorFill region) — driven by a Tween for the "point scored" flash.
//    • Tween<float> + TweenPlayer<float>       — value animation: the engine resolves a
//                                                value over time; the GAME owns the player and
//                                                writes the value into draw state each frame.
//
//  RESOLUTION-AGNOSTIC: the playfield reads its size from the ACTIVE VIEWPORT (config.viewport) at
//  startup — so this one file is a 160×144 game as a Game Boy or a 240×160 game as a Game Boy Advance
//  (the court, AI reaction line, net and score all re-derive from the viewport). It ships configured as
//  a GBA game; change config.viewport (§1) to retarget it. See §4a for how the dimensions are derived.
//
//  THE GAME:
//    • You are the LEFT paddle. Up / Down (arrows, W/S, or the d-pad) move it. The RIGHT paddle is a
//      beatable AI.
//    • SERVING: the ball sits on the SERVER'S paddle. You serve with the Serve action (the X key or
//      the pad's south button; you aim first by moving your paddle — the ball rides its face until
//      you launch). The AI auto-serves after a short beat. The WINNER of each point serves the next
//      one; you serve first at match start.
//    • RALLYING: the ball bounces off the top/bottom walls and off the paddles, taking "english"
//      (vertical spin) from where on the paddle it struck, and speeds up a little each hit.
//    • SCORING: a ball that passes a paddle and leaves that side scores for the opponent.
//    • First to 9 wins; the match then resets and the winner serves.
//
//  INTERPOLATION: the sim moves the paddles and ball at its own fixed cadence and the render submits
//    whatever the latest tick produced. Each mover sprite carries a stable key ("leftPaddle",
//    "rightPaddle", "ball"), which is the whole contract — the engine matches each object to its own
//    previous tick state by that key and eases between them, so motion stays smooth on a display running
//    faster than the tick rate with no blending code here.
//
// Nothing else pulses. The demo never auto-launches a window; you run it yourself.
//
//  CI: like the other example hosts it instantiates SdlPlatform + Renderer for real, so the live
//  GPU path keeps compiling/linking on every CI platform — but CI never opens the window.
// ============================================================================================

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "retropp/clock.h"          // SteadyClock — the real wall-clock the RunLoop schedules against
#include "retropp/draw_state.h"     // FrameDrawState / DrawLayer / TileContent / SpriteContent / Sprite / Region
#include "retropp/engine_config.h"  // EngineConfig — the startup bundle
#include "retropp/input.h"          // InputState (isHeld / justPressed) — the game's Action enum keys the reads
#include "retropp/input_actions.h"  // ActionMap — binds the game's actions to keys/pad
#include "retropp/palette.h"        // Rgba8 / PaletteId — color upload
#include "retropp/renderer.h"       // Renderer — uploadAtlas/uploadPalette + renderFrame
#include "retropp/run_loop.h"       // RunLoop — simTick / renderLoop, the fixed-step driver
#include "retropp/sdl_platform.h"   // SdlPlatform — window + GPU device + input
#include "retropp/tween.h"          // Tween<T> / TweenPlayer<T> / Easing — value animation
#include "retropp/viewport.h"       // ViewportResolution — the GB/GBA/… internal-resolution presets
#include "retropp/windowed_host.h"  // WindowedHost — pumps OS events and drives the loop until quit

namespace {

using namespace retropp;
using namespace std::chrono_literals;

// ── Court geometry — the playfield SIZE is read from the ACTIVE VIEWPORT at startup (see main, §4a), so
// this one demo fills whatever resolution the EngineConfig selects: 160×144 as a Game Boy, 240×160 as a
// Game Boy Advance, etc. Only the size-INDEPENDENT constants live here; the viewport-derived dimensions
// (the view size, the tile-map dimensions, the AI paddle x, the AI reaction line, the score/net columns)
// are computed in main() from config.viewport. ─────────────────────────────────────────────────────────
constexpr int kTile = 8;             // one tile is 8×8 px — the era-wide addressing cell (every preset
                                     // viewport is a multiple of it, so the tile map covers it exactly)

// A thin strip across the top holds the score; the ball/paddles live below it. This offset is a fixed
// pixel height (two tiles), independent of how tall the viewport is.
constexpr float kPlayTop = 16.0f;

// Paddle + ball sizes, the fixed player-paddle x, and movement speeds (pixels per sim tick). These are
// absolute pixel sizes — they stay the same whatever the viewport, so a bigger viewport = a bigger court
// around the same-sized paddles and ball.
constexpr float kPaddleW = 5.0f, kPaddleH = 28.0f;
constexpr float kBallSz  = 4.0f;
constexpr float kLeftX   = 7.0f;          // player paddle's left edge (the AI's is viewport-relative)
constexpr float kPlayerSpeed = 2.2f;       // your paddle speed
constexpr float kServeSpeed  = 1.6f;       // ball's horizontal speed at serve (grows on hits)
constexpr int   kWinScore    = 9;          // first to this many points wins the match

// AI difficulty — tuned (via an offline simulation) to be beatable by a skilled player yet competent
// against a passive one: the AI only chases the ball once it has crossed past this FRACTION of the court
// toward the AI AND is approaching; otherwise it holds position. The short reaction window means a fast,
// steep shot can get past it — that is what makes scoring possible. Expressing the reaction line as a
// fraction (rather than a fixed x) keeps the AI balanced at any viewport width.
constexpr float kAiSpeed     = 1.6f;
constexpr float kAiReactFrac = 0.55f;

// ── A 5×7 pixel font for the digits 0–9, one byte per row, low 5 bits = the 5 columns (bit 4 = left) ──
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

// How the font atlas is laid out as tiles: index 0 is a blank tile, indices 1..10 are the digits 0..9,
// and index 11 is the dashed-net tile. A tilemap cell stores one of these indices.
constexpr int kTileBlank  = 0;
constexpr int kTileDigit0 = 1;   // digit d lives at tile (kTileDigit0 + d)
constexpr int kTileNet    = 11;
constexpr int kAtlasTiles = 12;

// Build the indexed font atlas: ONE 8×8 tile per entry laid out left-to-right in a single row, with
// pixel value 1 = "lit" and 0 = "background". The renderer's uploadAtlas takes exactly this: a flat
// row-major array of palette INDICES (not colors). Color is applied later, by a palette, on the GPU.
std::vector<std::uint8_t> buildFontAtlas() {
    const int width = kTile * kAtlasTiles;                          // all tiles in one 8-px-tall strip
    std::vector<std::uint8_t> atlas(static_cast<std::size_t>(width) * kTile, 0);  // start all-background
    auto lit = [&](int tileIndex, int col, int row) {              // set one pixel within a given tile
        atlas[static_cast<std::size_t>(row) * width + (tileIndex * kTile + col)] = 1;
    };
    for (int d = 0; d < 10; ++d) {                                  // stamp each digit glyph...
        for (int row = 0; row < 7; ++row) {
            const std::uint8_t bits = kDigits[static_cast<std::size_t>(d)][static_cast<std::size_t>(row)];
            for (int col = 0; col < 5; ++col) {
                if ((bits >> (4 - col)) & 1) lit(kTileDigit0 + d, 1 + col, 1 + row);  // 1px top/left margin
            }
        }
    }
    for (int row = 1; row <= 6; ++row) { lit(kTileNet, 5, row); lit(kTileNet, 6, row); }  // the net: a short bar
    return atlas;
}

// A tiny linear-congruential generator so the serve direction/angle vary without pulling in <random>.
// (This is a plain native game, not a Game Boy ROM — no need to run RNG through the VM here.)
std::uint32_t nextRand(std::uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

// The game's input vocabulary: paddle movement plus Serve. Each action is bound to keyboard AND pad
// sources in main (§2) — the reads below never mention a physical control.
enum class Action : std::uint8_t { Up, Down, Serve };

// Which paddle is serving / who the ball belongs to during a serve.
enum class Side { Left, Right };

// The two phases of play. SERVING: the ball rides the server's paddle, waiting to be launched.
// RALLYING: the ball is live, integrating its velocity and bouncing.
enum class Phase { Serving, Rallying };

}  // namespace

int main() {

    // ── 1. Startup configuration ──────────────────────────────────────────────────────────────────
    // EngineConfig bundles window + viewport + timing; every field defaults to the
    // faithful Game Boy Color baseline. We override the window title AND the viewport: this Pong is a
    // GAME BOY ADVANCE game (240×160 internal resolution). The game reads its size from this viewport
    // (§4a), so swapping this one line — ViewportResolution::GameBoy, ::Nes, ::Snes, … or a raw
    // {width, height} — retargets the whole game to that resolution. setActive() stores this as the
    // process-wide active config AND fans its fields into the per-type defaults, so the bare
    // RunLoop/Renderer constructors below inherit the right timing + viewport with nothing threaded.
    // Interpolation stays at its default (on), so the renderer eases each keyed object between its tick
    // states and the demo submits raw tick positions (§8).
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Pong Demo"},
        .window   = {.title = "Polyrhythm — Pong (GBA)"},
        .viewport = ViewportResolution::GameBoyAdvance};
    EngineConfig::setActive(config);

    // ── 2. Core engine objects ────────────────────────────────────────────────────────────────────
    SteadyClock clock;                                            // the real clock the loop runs against
    RunLoop     loop{clock};                                      // fixed-step scheduler (inherits timing)
    SdlPlatform platform;                                         // opens the window + GPU device + input
    Renderer    renderer{platform.device(), platform.sdlWindow()};  // the draw API, bound to that device

    // Bind the game's actions. Each row lists every physical source the action answers to — a
    // multi-source action is simply several sources on one row. Up/Down take the arrows, W/S, and
    // the d-pad; Serve takes the X key or the pad's south face button.
    ActionMap map{
        {Action::Up,    {SDL_SCANCODE_UP, SDL_SCANCODE_W, PadButton::DpadUp}},
        {Action::Down,  {SDL_SCANCODE_DOWN, SDL_SCANCODE_S, PadButton::DpadDown}},
        {Action::Serve, {SDL_SCANCODE_X, PadButton::FaceSouth}},
    };
    platform.actions(map);

    // ── 3. Upload art + color to the GPU ─────────────────────────────────────────────────────────
    // 3a. The font atlas (digits + net) and its palette. A palette is just an array of RGBA colors;
    //     a tile/sprite pixel's INDEX selects an entry. Here index 0 = the dark court, index 1 = the
    //     soft-white lines/digits. uploadAtlas returns an AtlasId handle; uploadPalette a PaletteId.
    const std::vector<std::uint8_t> fontPx = buildFontAtlas();
    const AtlasId fontAtlas = renderer.uploadAtlas(fontPx.data(), kTile * kAtlasTiles, kTile).atlasId;
    const std::array<Rgba8, 2> palText{{{14, 18, 24}, {196, 208, 224}}};   // [0]=court, [1]=lit
    const PaletteId textPal = renderer.uploadPalette(std::span<const Rgba8>(palText));  // each cell names it directly

    // 3b. A single solid 8×32 atlas (every pixel index 1) serves BOTH paddles and the ball: a sprite
    //     draws a `size`-sized region read from tile 0, so any rectangle up to 8×32 is just a crop of
    //     solid fill. Each mover then gets its OWN palette so the three differ in color; a sprite's
    //     `palette` field selects which palette in the layer's set, and the pixel index (1) picks the
    //     entry within it. (Index 0 is the sprite "transparent hole", unused here since all pixels are 1.)
    std::array<std::uint8_t, 8 * 32> solidPx{};
    solidPx.fill(1);
    const AtlasId solidAtlas = renderer.uploadAtlas(solidPx.data(), 8, 32).atlasId;
    const std::array<Rgba8, 2> palLeft{{{0, 0, 0}, {110, 200, 255}}};   // [1] = player paddle: cyan
    const std::array<Rgba8, 2> palRight{{{0, 0, 0}, {255, 150, 90}}};   // [1] = AI paddle: orange
    const std::array<Rgba8, 2> palBall{{{0, 0, 0}, {245, 240, 180}}};   // [1] = ball: warm white
    const PaletteId leftPal  = renderer.uploadPalette(std::span<const Rgba8>(palLeft));
    const PaletteId rightPal = renderer.uploadPalette(std::span<const Rgba8>(palRight));
    const PaletteId ballPal  = renderer.uploadPalette(std::span<const Rgba8>(palBall));
    const std::array<PaletteId, 3> moverSet{leftPal, rightPal, ballPal};  // each mover names its handle from here

    // ── 4a. Playfield dimensions, READ FROM THE ACTIVE VIEWPORT ─────────────────────────────────────
    //     This is what makes the demo resolution-agnostic: everything below sizes itself off the
    //     configured viewport, so changing config.viewport (above) to GameBoyAdvance turns the same
    //     code into a 240×160 game with a bigger court, the AI line and net/score re-centered, etc.
    //     Every preset viewport is a multiple of the 8px tile, so kMapW/kMapH cover it exactly.
    const int   kViewW = config.viewport.width;
    const int   kViewH = config.viewport.height;
    const int   kMapW  = kViewW / kTile;                  // tile-map dimensions covering the viewport
    const int   kMapH  = kViewH / kTile;
    const float kPlayBottom = static_cast<float>(kViewH); // ball/paddles stay above this (the bottom edge)
    const float kRightX     = kViewW - 7.0f - kPaddleW;   // AI paddle's left edge (mirror of kLeftX)
    const float kAiReactX   = kViewW * kAiReactFrac;      // the AI tracks once the ball is right of this x
    const int   kNetCol     = (kMapW - 1) / 2;            // center-ish column for the dashed net
    const int   kScoreLeftCol  = kMapW / 2 - 3;           // score-digit columns, symmetric about center
    const int   kScoreRightCol = kMapW / 2 + 2;

    // ── 4. Game state (all positions/velocities are floats for smooth motion; we round to int pixels
    //       only at the moment we hand them to the sprite layer — Sprite::x/y are integers). ─────────
    float leftY  = (kPlayTop + kPlayBottom) / 2 - kPaddleH / 2;  // player paddle top-y (centered)
    float rightY = leftY;                                         // AI paddle top-y
    float ballX = 0, ballY = 0, ballVx = 0, ballVy = 0;          // ball position + velocity
    int   leftScore = 0, rightScore = 0;

    Phase phase  = Phase::Serving;   // a match opens on a serve...
    Side  server = Side::Left;       // ...and YOU (the left paddle) serve first.
    std::uint64_t serveTicks = 0;    // counts ticks the AI has waited before its auto-serve

    // Seed the RNG from the clock so the serve angle/jitter differ every run (a constant seed would make
    // every launch identical). A game wants variety, not reproducibility.
    std::uint32_t rng = static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    // How long the AI "thinks" before serving its ball — expressed as a duration, converted to a tick
    // count by the active timing profile (so it's correct regardless of the configured cadence).
    const std::uint64_t kAiServeDelayTicks = config.timing.ticksForDuration(600ms);

    // ── 5. Value animation: a gentle, single dim-and-restore of the WHOLE frame on each point.
    //       The Tween is a start value (1.0 = full brightness) plus two eased segments: dim to 0.6,
    //       then back to 1.0 — a yoyo authored as a 2-segment track. The TweenPlayer is the cursor the
    //       game owns: we advance() it each tick and read value() into a Multiply ColorFill region's fill. The
    //       engine never writes the value itself — that's the immediate-mode contract. We stop() it at
    //       startup so it rests at 1.0 (no dim) until the first point; restart() replays it per point.
    const Tween<float> dimTween =
        Tween<float>::of(1.0f, 0.6f, 350ms, Easing::InOutSine).then(1.0f, 350ms, Easing::InOutSine);
    TweenPlayer<float> dimPlayer{.tween = &dimTween};
    dimPlayer.stop();

    // ── 6. Helpers ────────────────────────────────────────────────────────────────────────────────
    // Begin a serve: park the ball on `srv`'s paddle face (it will then ride that paddle each tick
    // until launched) and enter the Serving phase. The ball gets no velocity yet.
    auto beginServe = [&](Side srv) {
        server     = srv;
        phase      = Phase::Serving;
        serveTicks = 0;
    };

    // Launch the parked ball toward the opponent: horizontal speed away from the server, a small random
    // vertical component for variety, then switch to Rallying.
    auto launchServe = [&]() {
        ballVx = (server == Side::Left) ? kServeSpeed : -kServeSpeed;
        ballVy = (static_cast<float>(nextRand(rng) % 200) / 100.0f - 1.0f) * 1.4f;  // [-1.4, 1.4]
        phase  = Phase::Rallying;
    };

    // Award a point: bump the score, fire the dim flash, handle match win/reset, and set up the next
    // serve. THE WINNER SERVES — so the scorer becomes the next server.
    auto awardPoint = [&](Side scorer) {
        if (scorer == Side::Left) ++leftScore; else ++rightScore;
        std::printf("score: %d - %d\n", leftScore, rightScore);
        dimPlayer.restart();  // one gentle dim-and-restore acknowledging the point
        if (leftScore >= kWinScore || rightScore >= kWinScore) {
            std::printf("%s wins %d-%d — new match\n",
                        scorer == Side::Left ? "Left (you)" : "Right (AI)", leftScore, rightScore);
            leftScore = rightScore = 0;
        }
        beginServe(scorer);   // winner serves the next ball
    };

    // ── 7. Simulation step — runs once per fixed tick (the game logic lives here) ───────────────────
    loop.simTick([&](const InputState& in) {
        // 7a. Player paddle: Up/Down move it every tick (during BOTH serve and rally, so you can aim
        //     your serve). Clamp it to the play field.
        if (in.isHeld(Action::Up))   leftY -= kPlayerSpeed;
        if (in.isHeld(Action::Down)) leftY += kPlayerSpeed;
        if (leftY < kPlayTop)             leftY = kPlayTop;
        if (leftY > kPlayBottom - kPaddleH) leftY = kPlayBottom - kPaddleH;

        // 7b. AI paddle.
        const float aiCentre = rightY + kPaddleH / 2;
        if (phase == Phase::Rallying) {
            // During a rally the AI only chases once the ball is approaching AND has crossed kAiReactX
            // (its short reaction window — the source of beatability). Otherwise it HOLDS position.
            if (ballVx > 0 && ballX > kAiReactX) {
                const float target = ballY + kBallSz / 2;
                if (target < aiCentre - 2.0f) rightY -= kAiSpeed;
                else if (target > aiCentre + 2.0f) rightY += kAiSpeed;
            }
        } else {
            // While someone is serving, the AI eases back toward vertical center so each point starts
            // fairly (it isn't left stranded wherever the last rally ended).
            const float mid = (kPlayTop + kPlayBottom) / 2;
            if (aiCentre < mid - 2.0f) rightY += kAiSpeed;
            else if (aiCentre > mid + 2.0f) rightY -= kAiSpeed;
        }
        if (rightY < kPlayTop)             rightY = kPlayTop;
        if (rightY > kPlayBottom - kPaddleH) rightY = kPlayBottom - kPaddleH;

        // 7c. Serving vs rallying.
        if (phase == Phase::Serving) {
            // The ball rides the server's paddle face, vertically centered on it — so a player can aim
            // by moving their paddle before launching.
            if (server == Side::Left) {
                ballX = kLeftX + kPaddleW;
                ballY = leftY + kPaddleH / 2 - kBallSz / 2;
                if (in.justPressed(Action::Serve)) launchServe();  // YOU launch the serve
            } else {
                ballX = kRightX - kBallSz;
                ballY = rightY + kPaddleH / 2 - kBallSz / 2;
                if (++serveTicks >= kAiServeDelayTicks) launchServe();  // the AI serves after a beat
            }
        } else {
            // 7d. Rally physics: integrate the ball, bounce off the top/bottom walls.
            ballX += ballVx;
            ballY += ballVy;
            if (ballY < kPlayTop)            { ballY = kPlayTop;             ballVy = -ballVy; }
            if (ballY > kPlayBottom - kBallSz) { ballY = kPlayBottom - kBallSz; ballVy = -ballVy; }

            // 7e. Paddle collision: if the ball overlaps a paddle, reflect it AWAY from that paddle,
            //     nudge it clear so the next tick can't re-trigger the overlap, add "english" from how
            //     far up/down the paddle it struck, and speed it up a little (both axes capped). The
            //     `isLeftPaddle` flag picks the reflection direction: off the LEFT paddle the ball goes
            //     right (+vx); off the RIGHT paddle it goes left (−vx).
            auto bounce = [&](float paddleX, float paddleY, bool isLeftPaddle) {
                const bool overlapX = ballX < paddleX + kPaddleW && ballX + kBallSz > paddleX;
                const bool overlapY = ballY < paddleY + kPaddleH && ballY + kBallSz > paddleY;
                if (!overlapX || !overlapY) return;
                ballVx = isLeftPaddle ? std::abs(ballVx) : -std::abs(ballVx);
                ballX  = isLeftPaddle ? paddleX + kPaddleW : paddleX - kBallSz;
                const float offset = ((ballY + kBallSz / 2) - (paddleY + kPaddleH / 2)) / (kPaddleH / 2);
                ballVy += offset * 1.5f;     // english: edge hits add more vertical spin than center hits
                ballVx *= 1.06f;             // each rally hit speeds the ball up slightly...
                if (std::abs(ballVx) > 4.5f) ballVx = (ballVx < 0 ? -4.5f : 4.5f);  // ...up to a cap
                if (std::abs(ballVy) > 5.0f) ballVy = (ballVy < 0 ? -5.0f : 5.0f);
            };
            if (ballVx < 0) bounce(kLeftX, leftY, /*isLeftPaddle=*/true);    // heading toward you
            if (ballVx > 0) bounce(kRightX, rightY, /*isLeftPaddle=*/false); // heading toward the AI

            // 7f. Scoring: a ball that fully leaves a side scores for the opponent (the winner serves).
            if (ballX + kBallSz < 0)  awardPoint(Side::Right);  // off the LEFT edge → AI scores
            else if (ballX > kViewW)  awardPoint(Side::Left);   // off the RIGHT edge → you score
        }

        // 7g. Advance the point-flash tween. It only progresses while a flash is in flight (we stop()
        //     it at startup and restart() it on each point); single() mode holds the final value (1.0).
        dimPlayer.advance(PlaybackMode::single());
    });

    // The court tilemap (net + score), kept alive for the whole program and rebuilt each frame. Every
    // cell draws from the font atlas through the text palette; the per-frame rebuild only rewrites
    // `.tile`, so the sheet + palette handles are set once here and preserved across frames.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (TileCell& c : cells) { c.atlas = fontAtlas; c.palette = textPal; }
    auto setDigit = [&](int col, int row, int digit) {  // write one digit glyph into a tile cell
        cells[static_cast<std::size_t>(row) * kMapW + col].tile =
            static_cast<std::uint16_t>(kTileDigit0 + digit);
    };

    // ── 8. Render step — runs each display frame. It only draws: it submits the positions the latest
    //       tick produced and the engine eases each keyed object toward them, so this callback reads no
    //       timing and does no blending. ─────────────────────────────────────────────────────────────
    FrameDrawState frame;  // reused each frame; we clear() + refill it (immediate mode, no retained state)
    loop.renderLoop([&] {
        // 8a. Rebuild the court tile layer: clear to blank, lay the dashed net down the center column
        //     (every other row), then stamp each player's single-digit score near the top.
        for (auto& c : cells) c.tile = kTileBlank;
        for (int row = 2; row < kMapH; row += 2) {
            cells[static_cast<std::size_t>(row) * kMapW + kNetCol].tile =
                static_cast<std::uint16_t>(kTileNet);
        }
        setDigit(kScoreLeftCol, 0, leftScore % 10);
        setDigit(kScoreRightCol, 0, rightScore % 10);

        frame.layers.clear();
        frame.regions.clear();

        // z=0: the court. A TileContent layer references the atlas, the palette set, the map size, and
        // the cells. Lower z draws first (behind).
        DrawLayer court{.key = "court"};
        court.z       = 0;
        court.size    = PixelSize{kViewW, kViewH};
        court.content = TileContent{.widthInTiles  = kMapW,
                                    .heightInTiles = kMapH,
                                    .cells         = std::span<const TileCell>(cells)};
        frame.layers.push_back(court);

        // z=10: the movers. Three sprites in one SpriteContent layer — left paddle (palette 0), right
        // paddle (palette 1), ball (palette 2) — each a solid rectangle cropped from the solid atlas.
        // The sim's float positions are quantized to integer screen pixels HERE, at the write into
        // Sprite::x/y. Each sprite's `key` is what lets the engine ease it: the same key next frame is
        // the same object, so it blends from where that object was at the previous tick.
        const AssetDimensions paddleDim{static_cast<int>(kPaddleW), static_cast<int>(kPaddleH)};
        const AssetDimensions ballDim{static_cast<int>(kBallSz), static_cast<int>(kBallSz)};
        const std::array<Sprite, 3> moverSprites{{
            {.key = "leftPaddle",  .x = static_cast<int>(kLeftX),  .y = static_cast<int>(leftY),  .size = paddleDim, .atlas = solidAtlas, .tile = 0, .palette = moverSet[0]},
            {.key = "rightPaddle", .x = static_cast<int>(kRightX), .y = static_cast<int>(rightY), .size = paddleDim, .atlas = solidAtlas, .tile = 0, .palette = moverSet[1]},
            {.key = "ball",        .x = static_cast<int>(ballX),   .y = static_cast<int>(ballY),  .size = ballDim,   .atlas = solidAtlas, .tile = 0, .palette = moverSet[2]},
        }};
        DrawLayer moversLayer{.key = "movers"};
        moversLayer.z       = 10;
        moversLayer.size    = PixelSize{kViewW, kViewH};
        moversLayer.content = SpriteContent{.sprites = std::span<const Sprite>(moverSprites)};
        frame.layers.push_back(moversLayer);

        // 8b. The point-flash: a uniform whole-frame dim, expressed as an ordinary effect — a Multiply-
        // blended ColorFill region covering the viewport, its gray fill the multiplier. Between points the
        // tween rests at 1.0 (Multiply by white = no change), so the region is pushed only while dimming →
        // it costs a pass only during the dip. On a point it dips to 0.6 and eases back.
        const float m = dimPlayer.value();
        if (m < 1.0f) {
            const auto  g = static_cast<std::uint8_t>(m * 255.0f);
            frame.regions.push_back(Region{
                .key     = "pointDim",
                .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{g, g, g, 255}}},
                .blend   = BlendMode::Multiply});
        }

        // 8c. Submit the frame to the GPU (composite the layers, the dim region, scale + present).
        renderer.renderFrame(frame);
    });

    // ── 9. Run ────────────────────────────────────────────────────────────────────────────────────
    std::printf("Pong — Up/Down (arrows, W/S, or the d-pad) move your (left) paddle; X (or the pad's "
                "south button) serves. First to %d wins.\n", kWinScore);
    // WindowedHost pumps OS events (close button, input) and drives the loop's tick+render until quit.
    WindowedHost{loop, platform}.run();
    return 0;
}
