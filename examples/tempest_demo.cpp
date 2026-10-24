// ============================================================================================
//  Tempest — a vector-tube shooter on Polyrhythm, written as a heavily documented teaching example.
//  Tempest is a VECTOR game (everything is lines) and the engine has no line primitive, so every line
//  is a 1×1 solid sprite STRETCHED + ROTATED into a thin quad by a per-sprite Transform. And Tempest
//  levels are not just a circular tube — they are arbitrary "webs": closed shapes (circle, square,
//  plus/cross) AND open paths (a flat line, a V) the claw runs along. We model that generally.
// ============================================================================================
//
//  WHAT THIS DEMONSTRATES (engine features):
//    • EngineConfig with a non-default viewport + timing: ViewportResolution::Snes (256×224) +
//      TimingProfile{TickPeriodNs::Hz60} (60 Hz built from the period enum — no named ::Hz60 preset).
//    • SteadyClock + RunLoop                   — fixed-step sim (simTick) decoupled from render (renderLoop).
//    • SdlPlatform + Renderer                  — the live window + GPU device + draw API.
//    • Sprite::transform AS A LINE TOOL          — `line(A,B)` emits a 1×1 solid sprite whose transform is
//                                                scale(length, thickness) THEN rotation(angle): the texture
//                                                stays solid while the transform draws a thin quad from A to
//                                                B (corner-mapping verified against the real Transform).
//                                                The whole web, the claw, the flippers and bolts are lines.
//    • A TILE layer for the HUD                  — score / lives, on the 8px grid.
//    • POINTER / ANALOG input                    — the capture toggle (Backspace / pad Select) toggles
//                                                relative-pointer CAPTURE so the mouse becomes the rotary
//                                                SPINNER: InputState::rawDeltaX is integrated into the
//                                                claw's lane (the authentic Tempest knob).
//
//  THE WEB MODEL (the heart of the fix):
//    • A LEVEL is a list of near-rim "spoke" points (ANY shape) + a `closed` flag. The FAR end of every
//      spoke is just that point scaled toward the screen center (kFarScale), so ONE formula draws a
//      circle, a square, a plus, a flat line, or a V — the only thing that changes is the rim points.
//    • CLOSED webs (circle/square/plus): the claw wraps around; flippers flip the short way round.
//    • OPEN webs (line/V): the claw CLAMPS at the two ends; flippers flip toward the player without
//      wrapping. Lane count = (#spokes) for closed, (#spokes − 1) for open.
//
//  THE GAME:
//    • Left/Right walk the claw along the rim; Fire (X / pad south) shoots up to 3 bolts per press
//      (release + press to fire again — not auto-fire) down your lane; the Superzapper (Z / pad east)
//      is one-per-life and clears the web. Return / pad Start cycles to the next level shape (and it
//      auto-advances as you rack up kills) so you can see every web.
//    • FLIPPERS crawl up the lanes; shoot them before they reach the rim. One that reaches the rim flips
//      along it toward your lane — if it reaches you, you lose one of 3 lives. Lose all → the game resets.
//
// The demo never auto-launches a window; you run it.
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
#include <string_view>
#include <vector>

#include "retropp/clock.h"          // SteadyClock
#include "retropp/draw_state.h"     // FrameDrawState / DrawLayer / TileContent / SpriteContent / Sprite
#include "retropp/engine_config.h"  // EngineConfig
#include "retropp/input.h"          // InputState (isHeld / justPressed / rawDeltaX)
#include "retropp/input_actions.h"  // ActionMap + PadButton — the demo's bindings
#include "retropp/palette.h"        // Rgba8 / PaletteId
#include "retropp/renderer.h"       // Renderer — uploadAtlas/uploadPalette + renderFrame
#include "retropp/run_loop.h"       // RunLoop — simTick / renderLoop
#include "retropp/sdl_platform.h"   // SdlPlatform
#include "retropp/timing.h"         // TimingProfile, TickPeriodNs (Hz60)
#include "retropp/transform.h"      // Transform — the line-as-quad tool
#include "retropp/viewport.h"       // ViewportResolution — the Snes preset
#include "retropp/windowed_host.h"  // WindowedHost — pumps OS events + drives the loop

namespace {

using namespace retropp;
using namespace std::chrono_literals;

constexpr int kTile = 8;

// The demo's input vocabulary: claw movement, firing, the Superzapper, the level cycler, and the
// mouse-spinner capture toggle.
enum class Action : std::uint8_t { Left, Right, Fire, Superzapper, NextLevel, CaptureToggle };

// Gameplay tuning (depths are 0 = far end of the web, 1 = the near rim where the player sits).
constexpr float kEnemySpeed   = 0.0045f;  // flipper depth gained per tick (≈3.7 s far→rim at 60 Hz)
constexpr float kBulletSpeed  = 0.05f;    // bolt depth lost per tick (fast)
constexpr int   kMoveEvery    = 5;        // ticks per lane while a direction is held
constexpr int   kFireEvery     = 5;       // ticks between the bolts within one press's burst
constexpr int   kShotsPerPress = 3;       // a press fires up to this many; release + press to fire more
constexpr int   kFlipEvery     = 26;      // ticks between a rim flipper's hops toward the player
constexpr int   kMaxEnemies    = 10;
constexpr int   kMaxBullets     = 6;      // concurrent bolts in flight
constexpr int   kLives        = 3;
constexpr int   kScorePerKill = 30;
constexpr float kHitWindow    = 0.06f;    // bolt/enemy depth overlap that counts as a hit
constexpr int   kInvulnTicks  = 50;       // grace after losing a life
constexpr int   kKillsPerLevel = 14;      // auto-advance to the next web after this many kills
constexpr float kFarScale     = 0.16f;    // the far ring is the rim scaled this far toward the center
constexpr int   kRimSpokes    = 16;       // points sampled around a closed rim (≈ lane count)

// A point on the playfield.
struct Pt { float x = 0, y = 0; };
// A LEVEL: the near-rim spoke points (the shape) + whether the rim is a closed loop.
struct Web { std::string_view name; bool closed; std::vector<Pt> spokes; };

// Sample `count` points evenly along a polyline/polygon `verts`. Closed → around the loop (count points,
// the wrap edge included); open → from the first vertex to the last inclusive. This turns any shape's
// corner list into evenly-spaced spokes, so square / plus / line / V all come from the one routine.
std::vector<Pt> sampleAlong(const std::vector<Pt>& verts, int count, bool closed) {
    const int n = static_cast<int>(verts.size());
    const int segs = closed ? n : n - 1;
    auto dist = [](Pt a, Pt b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); };
    std::vector<float> segLen(static_cast<std::size_t>(segs));
    float total = 0;
    for (int i = 0; i < segs; ++i) { segLen[static_cast<std::size_t>(i)] = dist(verts[static_cast<std::size_t>(i)],
                                                                                 verts[static_cast<std::size_t>((i + 1) % n)]);
                                     total += segLen[static_cast<std::size_t>(i)]; }
    std::vector<Pt> out;
    for (int k = 0; k < count; ++k) {
        const float t = closed ? total * k / count : total * k / (count - 1);
        float acc = 0;
        for (int i = 0; i < segs; ++i) {
            const float L = segLen[static_cast<std::size_t>(i)];
            if (t <= acc + L || i == segs - 1) {
                const float f = L > 0 ? (t - acc) / L : 0.0f;
                const Pt a = verts[static_cast<std::size_t>(i)], b = verts[static_cast<std::size_t>((i + 1) % n)];
                out.push_back(Pt{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f});
                break;
            }
            acc += L;
        }
    }
    return out;
}

// ── 5×7 digit font (0–9) for the HUD ──────────────────────────────────────────────────────────────
constexpr std::array<std::array<std::uint8_t, 7>, 10> kDigits{{
    {0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110},{0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110},
    {0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111},{0b11111,0b00010,0b00100,0b00010,0b00001,0b10001,0b01110},
    {0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010},{0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110},
    {0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110},{0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000},
    {0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110},{0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100},
}};
constexpr int kTileSolid = 0, kTileDigit0 = 1, kBgTiles = 11;

std::vector<std::uint8_t> buildBgAtlas() {
    const int w = kTile * kBgTiles;
    std::vector<std::uint8_t> a(static_cast<std::size_t>(w) * kTile, 0);
    for (int p = 0; p < kTile * kTile; ++p) a[p] = 1;
    for (int d = 0; d < 10; ++d)
        for (int row = 0; row < 7; ++row) {
            const std::uint8_t bits = kDigits[static_cast<std::size_t>(d)][static_cast<std::size_t>(row)];
            for (int col = 0; col < 5; ++col)
                if ((bits >> (4 - col)) & 1)
                    a[static_cast<std::size_t>(1 + row) * w + ((kTileDigit0 + d) * kTile + 1 + col)] = 1;
        }
    return a;
}

std::uint32_t nextRand(std::uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

// Each flipper / bolt carries a per-spawn `id` so its vector-line sprites keep a stable reconciliation
// key for the object's whole life. This matters doubly here: the interpolator eases a sprite's TRANSFORM
// by key, and an index-derived key (which shifts as enemies spawn/despawn) would cross-fade one line's
// transform toward an unrelated line's — collapsing the thin quads to degenerate slivers (a blanked web).
struct Enemy  { int lane; float depth; bool flipping; int flipTimer; bool alive = true; int id = 0; };
struct Bullet { int lane; float depth; bool alive = true; int id = 0; };

}  // namespace

int main() {

    // ── 1. Startup configuration — an SNES game at 60 Hz ────────────────────────────────────────────
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Tempest Demo"},
        .window   = {.title = "Polyrhythm — Tempest (SNES, 60 Hz)"},
        .viewport = ViewportResolution::Snes,            // 256×224
        .timing   = TimingProfile{TickPeriodNs::Hz60}};  // 60 Hz from the period enum
    EngineConfig::setActive(config);

    // ── 2. Core engine objects ──────────────────────────────────────────────────────────────────────
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::Fire,          {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::Superzapper,   {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::NextLevel,     {SDL_SCANCODE_RETURN, PadButton::Start}},
        {Action::CaptureToggle, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
        {Action::Left,          {SDL_SCANCODE_LEFT, SDL_SCANCODE_A, PadButton::DpadLeft}},
        {Action::Right,         {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D, PadButton::DpadRight}},
    };
    platform.actions(map);

    // ── 3. The web library (built once) ─────────────────────────────────────────────────────────────
    const int   kViewW = config.viewport.width;    // 256
    const int   kViewH = config.viewport.height;   // 224
    const int   kMapW  = kViewW / kTile;
    const int   kMapH  = kViewH / kTile;
    const float kCx = kViewW / 2.0f, kCy = kViewH / 2.0f;
    const float R   = std::min(kViewW, kViewH) / 2.0f - 14.0f;   // rim "radius" (≈ 98)
    auto at = [&](float nx, float ny) { return Pt{kCx + nx * R, kCy + ny * R}; };  // normalized → screen

    std::vector<Web> webs;
    // CLOSED — circle: 16 spokes on a ring.
    { std::vector<Pt> s; for (int i = 0; i < kRimSpokes; ++i) {
          const float a = static_cast<float>(i) / kRimSpokes * 6.2831853f;
          s.push_back(at(std::cos(a), std::sin(a))); }
      webs.push_back(Web{"circle", true, std::move(s)}); }
    // CLOSED — square: sample the 4 corners.
    webs.push_back(Web{"square", true, sampleAlong({at(-1,-1), at(1,-1), at(1,1), at(-1,1)}, 16, true)});
    // CLOSED — plus / cross (the iconic Tempest field): a 12-corner plus, sampled into 24 spokes.
    { const float a = 0.40f;  // arm half-width (normalized)
      const std::vector<Pt> v{at(-a,-1), at(a,-1), at(a,-a), at(1,-a), at(1,a), at(a,a),
                              at(a,1), at(-a,1), at(-a,a), at(-1,a), at(-1,-a), at(-a,-a)};
      webs.push_back(Web{"plus", true, sampleAlong(v, 24, true)}); }
    // OPEN — flat line across the lower field: 16 spokes → 15 lanes; the lanes fan UP toward the center.
    webs.push_back(Web{"line", false, sampleAlong({at(-1, 0.55f), at(1, 0.55f)}, 16, false)});
    // OPEN — shallow V: ends low, middle high.
    webs.push_back(Web{"vee", false, sampleAlong({at(-1, 0.30f), at(0, 0.75f), at(1, 0.30f)}, 16, false)});

    int level = 0;  // current web

    // Level-shape MORPH. Each web line is reconciled by a role key ("spoke_i" / "rim_i" / "far_i"), so the
    // interpolator already eases a line from wherever it was last tick to wherever it is this tick — for
    // free. All the demo adds is a ~0.5s timer that blends each spoke's RIM point from the previous shape
    // to the new one (rimAt below), spreading the change over many ticks so you SEE the circle unfold into
    // a square instead of it snapping in one frame. Spokes the old shape lacked just snap in (the extra
    // role keys mount fresh); it still reads as a morph. It is "animation" nobody wrote a keyframe for.
    constexpr float kMorphTicks = 30.0f;  // morph duration in sim ticks (~0.5s at 60 Hz)
    int   prevLevel = 0;    // the shape being morphed away from
    float morphT    = 1.0f; // 0 → 1 over the morph; 1 = settled on `level`

    auto numSpokes = [&] { return static_cast<int>(webs[static_cast<std::size_t>(level)].spokes.size()); };
    auto isClosed  = [&] { return webs[static_cast<std::size_t>(level)].closed; };
    auto numLanes  = [&] { return isClosed() ? numSpokes() : numSpokes() - 1; };
    auto neighbour = [&](int spoke) { return isClosed() ? (spoke + 1) % numSpokes() : spoke + 1; };

    // ── 4. Geometry: depth maps a spoke / lane to a screen point. depth² so the lanes "rush" near the
    //       rim (far end compressed), and the far end of every spoke is the rim scaled toward center. ──
    // The spoke's RIM point, blended from the previous shape to the current one while a morph runs. A spoke
    // index the previous shape did not have snaps straight to the new shape (there is nothing to ease from).
    auto rimAt = [&](int i) -> Pt {
        const Pt cur = webs[static_cast<std::size_t>(level)].spokes[static_cast<std::size_t>(i)];
        if (morphT >= 1.0f) return cur;
        const auto& prev = webs[static_cast<std::size_t>(prevLevel)].spokes;
        if (i >= static_cast<int>(prev.size())) return cur;
        const Pt p = prev[static_cast<std::size_t>(i)];
        return Pt{p.x + (cur.x - p.x) * morphT, p.y + (cur.y - p.y) * morphT};
    };
    auto spokeAt = [&](int i, float depth) {
        const Pt rim = rimAt(i);
        const Pt far{kCx + (rim.x - kCx) * kFarScale, kCy + (rim.y - kCy) * kFarScale};
        const float t = depth * depth;
        return Pt{far.x + (rim.x - far.x) * t, far.y + (rim.y - far.y) * t};
    };
    auto laneAt = [&](int lane, float depth) {  // lane center = midpoint of its two bounding spokes
        const Pt a = spokeAt(lane, depth), b = spokeAt(neighbour(lane), depth);
        return Pt{(a.x + b.x) / 2, (a.y + b.y) / 2};
    };
    // One step from `lane` toward `player`: closed wraps the short way; open just clamps toward it.
    auto stepToward = [&](int lane, int player) {
        if (isClosed()) { int d = player - lane;
                          while (d >  numLanes() / 2) d -= numLanes();
                          while (d < -numLanes() / 2) d += numLanes();
                          return (d > 0) - (d < 0); }
        return (player > lane) - (player < lane);
    };

    // ── 5. Art ───────────────────────────────────────────────────────────────────────────────────────
    const std::vector<std::uint8_t> bgPx = buildBgAtlas();
    const AtlasId bgAtlas = renderer.uploadAtlas(bgPx.data(), kTile * kBgTiles, kTile).atlasId;
    const std::array<Rgba8, 2> palBlack{{ {6, 6, 10}, {6, 6, 10} }};
    const std::array<Rgba8, 2> palHud{{ {6, 6, 10}, {220, 220, 235} }};
    const PaletteId blackPal = renderer.uploadPalette(std::span<const Rgba8>(palBlack));
    const PaletteId hudPal   = renderer.uploadPalette(std::span<const Rgba8>(palHud));
    const std::array<PaletteId, 2> bgSet{blackPal, hudPal};

    // One 8×8 tile of solid index-1 pixels. It must be at least kTilePx (8) wide: the atlas-region stride
    // is width / kTilePx, so a sub-tile 4×4 sheet resolves to 0 columns and every sprite reading it is
    // discarded (a blank vec layer). The line/box sprites read a 1×1 region at tile 0 (the top-left pixel).
    std::array<std::uint8_t, 8 * 8> solidPx{};
    solidPx.fill(1);
    const AtlasId solidAtlas = renderer.uploadAtlas(solidPx.data(), 8, 8).atlasId;
    const std::array<std::array<Rgba8, 2>, 5> vectorColours{{
        {{ {0,0,0}, {70, 120, 230} }},   // 0 web — blue
        {{ {0,0,0}, {34, 54, 110} }},    // 1 far ring — dim blue
        {{ {0,0,0}, {245, 230, 80} }},   // 2 claw — yellow
        {{ {0,0,0}, {235, 70, 200} }},   // 3 flipper — magenta
        {{ {0,0,0}, {245, 245, 245} }},  // 4 bolt — white
    }};
    std::array<PaletteId, 5> vecPals{};
    for (std::size_t i = 0; i < vectorColours.size(); ++i)
        vecPals[i] = renderer.uploadPalette(std::span<const Rgba8>(vectorColours[i]));
    enum VecPal { V_WEB = 0, V_FAR, V_CLAW, V_ENEMY, V_BOLT };

    // ── 6. Game state ────────────────────────────────────────────────────────────────────────────────
    int   player = 0;
    std::vector<Enemy>  enemies;
    std::vector<Bullet> bullets;
    int                 nextId = 1;  // monotonic per-spawn id for flippers / bolts (their key identity)
    int   score = 0, lives = kLives, levelKills = 0;
    int   moveTimer = 0, fireTimer = 0, invuln = 0, spawnTimer = 0, shotsThisPress = 0;
    bool  zapReady = true;

    // ── Pointer / spinner state ─────────────────────────────────────────────────────────────────────
    // The capture toggle switches relative-pointer CAPTURE: with capture ON the mouse is the rotary
    // SPINNER — raw horizontal motion integrates into `spin` and steps the claw lane (the authentic
    // Tempest knob). Left/Right always walk the claw regardless.
    bool  captured = false;
    float spin = 0.0f;
    constexpr float kSpinPerLane = 14.0f;  // raw device units of horizontal motion per lane step
    std::uint32_t rng = static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const int kSpawnEvery = static_cast<int>(config.timing.ticksForDuration(900ms));
    spawnTimer = kSpawnEvery;

    // Switch to web `next`: clear the web and clamp the claw to a valid lane (lane counts differ).
    auto gotoLevel = [&](int next) {
        prevLevel = level;  // the shape we leave — the morph blends away from it
        level = (next % static_cast<int>(webs.size()) + static_cast<int>(webs.size())) % static_cast<int>(webs.size());
        morphT = (prevLevel == level) ? 1.0f : 0.0f;  // start the morph (skip if it is the same shape)
        enemies.clear(); bullets.clear(); levelKills = 0; zapReady = true;
        player = std::clamp(player, 0, numLanes() - 1);
        std::printf("level: %s (%d lanes, %s)\n", webs[static_cast<std::size_t>(level)].name.data(),
                    numLanes(), isClosed() ? "closed" : "open");
    };
    auto loseLife = [&] {
        enemies.clear(); bullets.clear(); zapReady = true;
        if (--lives <= 0) { std::printf("game over — score %d — new game\n", score);
                            lives = kLives; score = 0; gotoLevel(0); }
        invuln = kInvulnTicks;
    };
    auto onKill = [&] { score += kScorePerKill; if (++levelKills >= kKillsPerLevel) gotoLevel(level + 1); };

    // ── 7. Simulation step (60 Hz) ───────────────────────────────────────────────────────────────────
    loop.simTick([&](const InputState& in) {
        if (moveTimer > 0) --moveTimer;
        if (fireTimer > 0) --fireTimer;
        if (invuln > 0) --invuln;
        if (morphT < 1.0f) morphT = std::min(1.0f, morphT + 1.0f / kMorphTicks);  // advance the shape morph

        // 7a. Walk the claw: closed wraps, open clamps at the two ends. NextLevel cycles the web.
        if ((in.isHeld(Action::Left) || in.isHeld(Action::Right)) && moveTimer == 0) {
            const int dir = in.isHeld(Action::Right) ? 1 : -1;
            player = isClosed() ? (player + dir + numLanes()) % numLanes()
                                : std::clamp(player + dir, 0, numLanes() - 1);
            moveTimer = kMoveEvery;
        }
        // 7a′. Mouse spinner: CaptureToggle switches relative-pointer capture; while captured, integrate
        //      raw horizontal mouse motion into a rotary position and step the claw a lane each
        //      kSpinPerLane of travel — the authentic Tempest knob. Left/Right still walk the claw.
        if (in.justPressed(Action::CaptureToggle)) { captured = !captured; platform.pointerCaptured(captured); }
        if (captured) {
            spin += in.rawDeltaX();
            while (spin >= kSpinPerLane) {
                player = isClosed() ? (player + 1 + numLanes()) % numLanes() : std::clamp(player + 1, 0, numLanes() - 1);
                spin -= kSpinPerLane;
            }
            while (spin <= -kSpinPerLane) {
                player = isClosed() ? (player - 1 + numLanes()) % numLanes() : std::clamp(player - 1, 0, numLanes() - 1);
                spin += kSpinPerLane;
            }
        }

        if (in.justPressed(Action::NextLevel)) gotoLevel(level + 1);
        // Firing: a PRESS fires a burst of up to kShotsPerPress bolts (held, they stream out at the fire
        // cadence) — then it stops. To fire again you must RELEASE and press Fire again, which resets
        // the per-press allowance. NOT indefinite auto-fire.
        if (in.justPressed(Action::Fire)) shotsThisPress = 0;
        if (in.isHeld(Action::Fire) && shotsThisPress < kShotsPerPress && fireTimer == 0 &&
            static_cast<int>(bullets.size()) < kMaxBullets) {
            bullets.push_back(Bullet{player, 1.0f, true, nextId++});
            ++shotsThisPress;
            fireTimer = kFireEvery;
        }
        if (in.justPressed(Action::Superzapper) && zapReady && !enemies.empty()) {  // one per life
            for (Enemy& e : enemies) { e.alive = false; onKill(); }
            zapReady = false;
            std::printf("ZAP! — score %d\n", score);
        }

        // 7b. Bolts: advance toward the far end; kill any enemy in the same lane at a near-enough depth.
        for (Bullet& b : bullets) {
            if (!b.alive) continue;
            for (Enemy& e : enemies)
                if (e.alive && e.lane == b.lane && std::abs(e.depth - b.depth) <= kHitWindow) {
                    e.alive = false; b.alive = false; onKill(); break;
                }
            if (!b.alive) continue;
            b.depth -= kBulletSpeed;
            if (b.depth <= 0.0f) b.alive = false;
        }

        // 7c. Flippers: crawl up the lane; on reaching the rim, hop along it toward the player's lane.
        for (Enemy& e : enemies) {
            if (!e.alive) continue;
            if (!e.flipping) {
                e.depth += kEnemySpeed;
                if (e.depth >= 1.0f) { e.depth = 1.0f; e.flipping = true; e.flipTimer = kFlipEvery; }
            } else if (--e.flipTimer <= 0) {
                e.lane = (e.lane + stepToward(e.lane, player) + numLanes()) % numLanes();
                e.flipTimer = kFlipEvery;
            }
        }

        // 7d. Threat: a live enemy at the rim on the player's lane gets the player (unless invuln).
        if (invuln == 0)
            for (const Enemy& e : enemies)
                if (e.alive && e.depth >= 1.0f && e.lane == player) { loseLife(); break; }

        // 7e. Spawn a flipper at the far end on a random lane.
        if (invuln == 0 && --spawnTimer <= 0 && static_cast<int>(enemies.size()) < kMaxEnemies) {
            enemies.push_back(Enemy{static_cast<int>(nextRand(rng) % static_cast<unsigned>(numLanes())),
                                    0.0f, false, 0, true, nextId++});
            spawnTimer = kSpawnEvery;
        }

        // 7f. Reap dead.
        bullets.erase(std::remove_if(bullets.begin(), bullets.end(),
                                     [](const Bullet& b) { return !b.alive; }), bullets.end());
        enemies.erase(std::remove_if(enemies.begin(), enemies.end(),
                                     [](const Enemy& e) { return !e.alive; }), enemies.end());
    });

    std::vector<TileCell> bgCells(static_cast<std::size_t>(kMapW) * kMapH);
    std::vector<Sprite>   vec;
    // Every vector-line sprite takes a STABLE identity key (a web spoke/rim/ring role, a claw part, a
    // per-flipper id + part, a per-bolt id) — NEVER its emission index, which shifts as flippers and bolts
    // spawn/despawn and would cross-fade one line's transform toward another's (see the Enemy/Bullet note
    // above). ObjectKey owns its bytes, so a key assembled per frame moves straight into the sprite.

    // THE LINE TOOL: a thin quad from A to B (a 1×1 solid scaled to (length,thickness) then rotated about
    // its start corner → corner sits at A, far corner at B; verified against the real Transform pipeline).
    auto line = [&](std::string key, Pt a, Pt b, float thick, int pal) {
        const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.5f) return;
        const float deg = std::atan2(dy, dx) * 57.29577951f;
        vec.push_back(Sprite{
            .key = std::move(key),
            .x = static_cast<int>(a.x), .y = static_cast<int>(a.y), .size = AssetDimensions{1, 1}, .atlas = solidAtlas,
            .tile = 0, .palette = vecPals[static_cast<std::size_t>(pal)],
            .transform = Transform::scale(len, thick, 0.0f, 0.0f).then(Transform::rotation(deg, 0.0f, 0.0f))});
    };
    auto box = [&](std::string key, Pt c, float s, int pal) {
        vec.push_back(Sprite{
            .key = std::move(key),
            .x = static_cast<int>(c.x - s / 2), .y = static_cast<int>(c.y - s / 2),
            .size = AssetDimensions{1, 1}, .atlas = solidAtlas,
            .tile = 0, .palette = vecPals[static_cast<std::size_t>(pal)],
            .transform = Transform::scale(s, s, 0.0f, 0.0f)});
    };

    // ── 8. Render step ───────────────────────────────────────────────────────────────────────────────
    loop.renderLoop([&]() {
        // 8a. HUD: score (left) / lives (right) on row 1 over the black void.
        for (auto& c : bgCells) { c.tile = kTileSolid; c.atlas = bgAtlas; c.palette = bgSet[0]; }
        auto putNum = [&](int v, int endCol) { int x = v, col = endCol;
            do { bgCells[static_cast<std::size_t>(kMapW) + col].tile =
                     static_cast<std::uint16_t>(kTileDigit0 + x % 10);
                 bgCells[static_cast<std::size_t>(kMapW) + col].palette = bgSet[1]; x /= 10; --col;
            } while (x > 0 && col >= 0); };
        putNum(score, 7);
        putNum(lives, kMapW - 2);

        // 8b. The web: each spoke (far→rim), the rim edges, the far-ring edges (dim). For an OPEN web
        //     there is no edge past the last spoke (no wrap).
        vec.clear();
        // The web is static within a level, so each line takes a role-based key ("spoke_i"/"rim_i"/
        // "far_i") — stable frame to frame, so its transform eases from itself (a no-op) instead of from
        // some other line. Emission order no longer decides identity.
        const int N = numSpokes();
        const int edges = isClosed() ? N : N - 1;
        for (int i = 0; i < N; ++i)
            line("spoke_" + std::to_string(i), spokeAt(i, 0.0f), spokeAt(i, 1.0f), 1.0f, V_WEB);  // spokes
        for (int i = 0; i < edges; ++i) {
            const int j = (i + 1) % N;
            line("rim_" + std::to_string(i), spokeAt(i, 1.0f), spokeAt(j, 1.0f), 1.0f, V_WEB);   // near rim
            line("far_" + std::to_string(i), spokeAt(i, 0.0f), spokeAt(j, 0.0f), 1.0f, V_FAR);   // far ring
        }

        // 8c. Highlight the player's lane the Tempest way: the TWO SPOKES bounding it (the lines running
        //     INTO the screen) are redrawn in yellow over the blue web, and the claw straddles the rim
        //     between them. (Flippers travel down lane CENTERS — exactly midway between two spokes,
        //     `laneAt()` — and spawn on ANY lane, never specifically the player's; see 7e.)
        {
            const int sL = player, sR = neighbour(player);
            line("claw_spokeL", spokeAt(sL, 0.0f), spokeAt(sL, 1.0f), 2.0f, V_CLAW);  // left bounding spoke
            line("claw_spokeR", spokeAt(sR, 0.0f), spokeAt(sR, 1.0f), 2.0f, V_CLAW);  // right bounding spoke
            const Pt a = spokeAt(sL, 1.0f), b = spokeAt(sR, 1.0f), tip = laneAt(player, 0.82f);
            line("claw_bar",  a, b,   2.0f, V_CLAW);  // the rim bar between the two spokes
            line("claw_prong1", a, tip, 2.0f, V_CLAW);  // claw prong 1
            line("claw_prong2", b, tip, 2.0f, V_CLAW);  // claw prong 2
        }

        // 8d. Flippers: a BOWTIE that SPANS its lane — left/right tips ride the lane's two edge spokes at
        //     the flipper's depth, far/near tips on the lane center — so it fills the lane and scales with
        //     it (tiny at the far end, large at the rim), making which lane it's in unmistakable.
        for (const Enemy& e : enemies) {
            const float df = std::max(0.0f, e.depth - 0.06f), dn = std::min(1.0f, e.depth + 0.06f);
            const Pt a = spokeAt(e.lane, e.depth), b = spokeAt(neighbour(e.lane), e.depth);  // lane edges
            const Pt f = laneAt(e.lane, df), n = laneAt(e.lane, dn);                          // far/near tips
            const std::string k = "flip_" + std::to_string(e.id);
            line(k + "_af", a, f, 1.5f, V_ENEMY); line(k + "_an", a, n, 1.5f, V_ENEMY);
            line(k + "_bf", b, f, 1.5f, V_ENEMY); line(k + "_bn", b, n, 1.5f, V_ENEMY);
        }
        // 8e. Bolts.
        for (const Bullet& b : bullets) box("bolt_" + std::to_string(b.id), laneAt(b.lane, b.depth), 3.0f, V_BOLT);

        // 8f. Assemble: HUD (z=0) → vectors (z=10).
        FrameDrawState frame;
        DrawLayer bg{.key = "hud"}; bg.z = 0; bg.size = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                 .cells = std::span<const TileCell>(bgCells)};
        frame.layers.push_back(bg);

        DrawLayer v{.key = "vectors"}; v.z = 10; v.size = PixelSize{kViewW, kViewH};
        v.content = SpriteContent{.sprites = std::span<const Sprite>(vec)};
        frame.layers.push_back(v);

        renderer.renderFrame(frame);
    });

    std::printf("Tempest (SNES, 60 Hz) — Left/Right (arrows / A·D / d-pad) walk the claw, X (pad south) "
                "fires up to 3 per press (release + press to fire again), Z (pad east) = Superzapper "
                "(one per life), Return (pad Start) cycles the level shape (circle / square / plus / "
                "line / V). Backspace (pad Select) toggles the MOUSE SPINNER (capture on = the mouse is "
                "the rotary knob). Shoot flippers before they reach you. Close to quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
