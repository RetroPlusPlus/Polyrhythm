// =====================================================================================================
//  Tempest — a vector-tube shooter on Polyrhythm, drawn as REAL colored vector lines.
//
//  Tempest is a vector game: the whole playfield is glowing line work — a tube of radial lanes seen in
//  perspective, a claw straddling the near rim, enemies crawling up the lanes, bolts streaking down. Every
//  one of those lines is a stroked Region filled with a color. A Region binds a ShapePoints (here a curve
//  of Bezier segments) to a ColorFill effect; giving the ShapePoints a strokeWidth makes the region a thin
//  BAND along the path rather than a filled interior, and the ColorFill paints that band a solid color. A
//  straight lane is a band along a Linear segment; a bolt's tracer is a band along a Quadratic — straight
//  and curved paths from one primitive. Because a stroke is the union of its segments' bands, many disjoint
//  segments share ONE region, so the whole tube is a handful of passes, not one per line. The bands paint
//  onto an opaque backdrop at frame level (ColorFill recolors existing pixels), composited in list order
//  so the dim far ring sits behind the bright tube and the bolts sit in front.
//
//  WHAT THIS DEMONSTRATES:
//    • Region + ScreenSpaceEffectKind::ColorFill as a VECTOR LINE TOOL — a stroked ShapePoints (a curve +
//      a strokeWidth) filled with a color is a drawn colored path. Disjoint segments batch into one
//      region (the stroke is the union of per-segment bands).
//    • Curve boundaries — Linear segments for the straight lanework, Quadratic for the bolts' curved
//      tracers, both evaluated exactly (no facets).
//    • A non-default viewport + 60 Hz timing (a raw 640x480 internal resolution).
//    • A tile layer for the HUD (score / lives) — also the opaque void the bands paint onto.
//    • Pointer / analog input — the mouse as a rotary SPINNER (relative-capture) steering the claw.
//
//  THE TUBE:
//    • A LEVEL is a ring of near-rim "spoke" points (any shape) + a closed/open flag, so one set of
//      formulas draws a circle, a square, a plus, a flat line, or a V. The far end of each spoke is the
//      rim point pulled toward the screen center; depth in [0,1] runs far->rim with a depth-squared rush.
//    • CLOSED tubes wrap (the claw circles the rim); OPEN tubes clamp at the two ends.
//
//  THE GAME:
//    • Left/Right (or the mouse spinner) walk the claw along the rim; A fires up to 3 bolts per press
//      (release + press to fire again) down your lane; B is a one-per-life Superzapper. START cycles the
//      tube shape, which also auto-advances as you rack up kills.
//    • FLIPPERS crawl up the lanes and, at the rim, hop along it toward you. SPIKERS crawl up leaving a
//      growing SPIKE in their lane — shoot the lane to erode the spike and kill the spiker; an un-cleared
//      spike eats your bolts. An enemy that reaches the rim in your lane costs a life.
//
// The demo never auto-launches a window; you run it.
//
//  CI: it instantiates SdlPlatform + Renderer for real, so the live GPU path keeps compiling/linking on
//  every platform — but CI never opens the window.
// =====================================================================================================

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include "retropp/clock.h"          // SteadyClock
#include "retropp/curve.h"          // Curve / CurveSegment / CurveDegree — the stroked-path boundary
#include "retropp/draw_state.h"     // FrameDrawState / DrawLayer / TileContent / Region / ShapePoints / ScreenSpaceEffect
#include "retropp/engine_config.h"  // EngineConfig
#include "retropp/geometry.h"       // Vec2 / Point
#include "retropp/input.h"          // InputState (isHeld / justPressed)
#include "retropp/input_actions.h"  // ActionMap / PadButton — the game's action bindings
#include "retropp/palette.h"        // Rgba8 / PaletteId
#include "retropp/renderer.h"       // Renderer — uploadAtlas/uploadPalette + renderFrame
#include "retropp/run_loop.h"       // RunLoop — simTick / renderLoop
#include "retropp/sdl_platform.h"   // SdlPlatform
#include "retropp/timing.h"         // TimingProfile, TickPeriodNs (Hz60)
#include "retropp/viewport.h"       // ViewportResolution — the Snes preset
#include "retropp/windowed_host.h"  // WindowedHost — pumps OS events + drives the loop

namespace {

using namespace retropp;
using namespace std::chrono_literals;

constexpr int kTile = 8;

// The game's input vocabulary: walk the claw along the rim, fire, the Superzapper, the tube cycle,
// and the mouse-spinner capture toggle.
enum class Action : std::uint8_t { Left, Right, Fire, Superzapper, CycleTube, ToggleSpinner };

// Gameplay tuning (depths are 0 = far end of the tube, 1 = the near rim where the player sits).
constexpr float kEnemySpeed    = 0.0045f;  // flipper depth gained per tick (~3.7 s far->rim at 60 Hz)
constexpr float kSpikeGrow     = 0.0030f;  // spiker crawl per tick (slower than a flipper)
constexpr float kSpikeErode    = 0.34f;    // spike depth a single bolt erodes (a few bolts clears it)
constexpr float kBulletSpeed   = 0.05f;    // bolt depth lost per tick (fast)
constexpr int   kMoveEvery     = 5;        // ticks per lane while a direction is held
constexpr int   kFireEvery     = 5;        // ticks between the bolts within one press's burst
constexpr int   kShotsPerPress = 3;        // a press fires up to this many; release + press to fire more
constexpr int   kFlipEvery     = 26;       // ticks between a rim flipper's hops toward the player
constexpr int   kMaxEnemies    = 10;
constexpr int   kMaxBullets    = 6;        // concurrent bolts in flight
constexpr int   kLives         = 3;
constexpr int   kScorePerKill  = 30;
constexpr float kHitWindow     = 0.06f;    // bolt/enemy depth overlap that counts as a hit
constexpr int   kInvulnTicks   = 50;       // grace after losing a life
constexpr int   kKillsPerLevel = 14;       // auto-advance to the next tube after this many kills
constexpr float kFarScale      = 0.16f;    // the far ring is the rim scaled this far toward the center
constexpr int   kRimSpokes     = 16;       // points sampled around a closed rim (~ lane count)
constexpr int   kSpikerPercent = 30;       // share of spawns that are spikers (vs flippers)

// Stroke widths (viewport px) for each kind of line.
constexpr float kWebWidth    = 1.5f;
constexpr float kFarWidth    = 1.0f;
constexpr float kClawWidth   = 2.5f;
constexpr float kFlipWidth   = 1.5f;
constexpr float kSpikeWidth  = 2.0f;
constexpr float kSpikerWidth = 1.5f;
constexpr float kBoltWidth   = 2.5f;

// A point on the playfield.
struct Pt { float x = 0, y = 0; };
// A LEVEL: the near-rim spoke points (the shape) + whether the rim is a closed loop.
struct Web { std::string_view name; bool closed; std::vector<Pt> spokes; };

// Sample `count` points evenly along a polyline/polygon `verts`. Closed -> around the loop (count points,
// the wrap edge included); open -> from the first vertex to the last inclusive. This turns any shape's
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

// ── 5x7 digit font (0-9) for the HUD ──────────────────────────────────────────────────────────────
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

enum class EKind : std::uint8_t { Flipper, Spiker };
struct Enemy  { EKind kind; int lane; float depth; bool flipping; int flipTimer; bool alive = true; };
struct Bullet { int lane; float depth; bool alive = true; };

}  // namespace

int main() {

    // ── 1. Startup configuration — an SNES game at 60 Hz ────────────────────────────────────────────
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Tempest Vector Demo"},
        .window   = {.title = "Polyrhythm — Tempest (real vector lines)"},
        .viewport = ViewportResolution{640, 480},        // raw (non-preset) internal resolution
        .timing   = TimingProfile{TickPeriodNs::Hz60}};  // 60 Hz from the period enum
    EngineConfig::setActive(config);

    // ── 2. Core engine objects ──────────────────────────────────────────────────────────────────────
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::Left,          {SDL_SCANCODE_LEFT, SDL_SCANCODE_A, PadButton::DpadLeft}},
        {Action::Right,         {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D, PadButton::DpadRight}},
        {Action::Fire,          {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::Superzapper,   {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::CycleTube,     {SDL_SCANCODE_RETURN, PadButton::Start}},
        {Action::ToggleSpinner, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // ── 3. The tube library (built once) ────────────────────────────────────────────────────────────
    const int   kViewW = config.viewport.width;    // 640
    const int   kViewH = config.viewport.height;   // 480
    const int   kMapW  = kViewW / kTile;
    const int   kMapH  = kViewH / kTile;
    const float kCx = kViewW / 2.0f, kCy = kViewH / 2.0f;
    const float R   = std::min(kViewW, kViewH) / 2.0f - 16.0f;   // rim "radius" (~224)
    auto at = [&](float nx, float ny) { return Pt{kCx + nx * R, kCy + ny * R}; };  // normalized -> screen

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
    // OPEN — flat line across the lower field: 16 spokes -> 15 lanes; the lanes fan UP toward the center.
    webs.push_back(Web{"line", false, sampleAlong({at(-1, 0.55f), at(1, 0.55f)}, 16, false)});
    // OPEN — shallow V: ends low, middle high.
    webs.push_back(Web{"vee", false, sampleAlong({at(-1, 0.30f), at(0, 0.75f), at(1, 0.30f)}, 16, false)});

    int level = 0;  // current tube
    auto numSpokes = [&] { return static_cast<int>(webs[static_cast<std::size_t>(level)].spokes.size()); };
    auto isClosed  = [&] { return webs[static_cast<std::size_t>(level)].closed; };
    auto numLanes  = [&] { return isClosed() ? numSpokes() : numSpokes() - 1; };
    auto neighbour = [&](int spoke) { return isClosed() ? (spoke + 1) % numSpokes() : spoke + 1; };

    // ── 4. Geometry: depth maps a spoke / lane to a screen point. depth^2 so the lanes "rush" near the
    //       rim (far end compressed), and the far end of every spoke is the rim scaled toward center. ──
    auto spokeAt = [&](int i, float depth) {
        const Pt rim = webs[static_cast<std::size_t>(level)].spokes[static_cast<std::size_t>(i)];
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

    // ── 5. HUD art (the opaque void the vector bands paint onto + the score/lives glyphs) ────────────
    const std::vector<std::uint8_t> bgPx = buildBgAtlas();
    const AtlasId bgAtlas = renderer.uploadAtlas(bgPx.data(), kTile * kBgTiles, kTile).atlasId;
    const std::array<Rgba8, 2> palBlack{{ {6, 6, 10}, {6, 6, 10} }};
    const std::array<Rgba8, 2> palHud{{ {6, 6, 10}, {220, 220, 235} }};
    const PaletteId blackPal = renderer.uploadPalette(std::span<const Rgba8>(palBlack));
    const PaletteId hudPal   = renderer.uploadPalette(std::span<const Rgba8>(palHud));

    // ── 6. Game state ────────────────────────────────────────────────────────────────────────────────
    int   player = 0;
    std::vector<Enemy>  enemies;
    std::vector<Bullet> bullets;
    std::vector<float>  laneSpike;  // per-lane spike depth in [0,1] (0 = none, grows far->rim)
    int   score = 0, lives = kLives, levelKills = 0;
    int   moveTimer = 0, fireTimer = 0, invuln = 0, spawnTimer = 0, shotsThisPress = 0;
    bool  zapReady = true;

    // ── Pointer / spinner state ─────────────────────────────────────────────────────────────────────
    // SELECT toggles relative-pointer CAPTURE: with capture ON the mouse is the rotary SPINNER — raw
    // horizontal motion integrates into `spin` and steps the claw lane (the authentic Tempest knob). The
    // d-pad always walks the claw regardless.
    bool  captured = false;
    float spin = 0.0f;
    constexpr float kSpinPerLane = 22.0f;  // raw device units of horizontal motion per lane step
    std::uint32_t rng = static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const int kSpawnEvery = static_cast<int>(config.timing.ticksForDuration(900ms));
    spawnTimer = kSpawnEvery;

    // Switch to tube `next`: clear the field and clamp the claw to a valid lane (lane counts differ).
    auto gotoLevel = [&](int next) {
        level = (next % static_cast<int>(webs.size()) + static_cast<int>(webs.size())) % static_cast<int>(webs.size());
        enemies.clear(); bullets.clear(); levelKills = 0; zapReady = true;
        laneSpike.assign(static_cast<std::size_t>(numLanes()), 0.0f);
        player = std::clamp(player, 0, numLanes() - 1);
        std::printf("level: %s (%d lanes, %s)\n", webs[static_cast<std::size_t>(level)].name.data(),
                    numLanes(), isClosed() ? "closed" : "open");
    };
    auto loseLife = [&] {
        enemies.clear(); bullets.clear(); zapReady = true;
        laneSpike.assign(static_cast<std::size_t>(numLanes()), 0.0f);
        if (--lives <= 0) { std::printf("game over — score %d — new game\n", score);
                            lives = kLives; score = 0; gotoLevel(0); }
        invuln = kInvulnTicks;
    };
    auto onKill = [&] { score += kScorePerKill; if (++levelKills >= kKillsPerLevel) gotoLevel(level + 1); };
    laneSpike.assign(static_cast<std::size_t>(numLanes()), 0.0f);

    // ── Viewport-independent motion ─────────────────────────────────────────────────────────────────
    // The per-tick steps below move a fraction of the TUBE's DEPTH, and the tube's pixel size scales with
    // R (the rim radius), which scales with the viewport. Left raw, a larger viewport would sweep more
    // pixels per second — everything would look faster on a bigger screen. Scaling the depth steps by
    // (reference R / R) and the tick cadences by the inverse holds the on-screen pace constant at any
    // viewport, so the feel is the same whether the internal resolution is small or large.
    constexpr float kRefR = 96.0f;             // the rim radius the speeds/cadences below were tuned at
    const float velScale    = kRefR / R;        // < 1 on a larger tube (slower depth steps, same screen pace)
    const float enemySpeed  = kEnemySpeed  * velScale;
    const float spikeGrow   = kSpikeGrow   * velScale;
    const float bulletSpeed = kBulletSpeed * velScale;
    const int   moveEvery   = std::max(1, static_cast<int>(std::lround(kMoveEvery / velScale)));
    const int   flipEvery   = std::max(1, static_cast<int>(std::lround(kFlipEvery / velScale)));

    // ── 7. Simulation step (60 Hz) ───────────────────────────────────────────────────────────────────
    loop.simTick([&](const InputState& in) {
        if (moveTimer > 0) --moveTimer;
        if (fireTimer > 0) --fireTimer;
        if (invuln > 0) --invuln;
        const int lanes = numLanes();

        // 7a. Walk the claw: closed wraps, open clamps at the two ends. Start cycles the tube.
        if ((in.isHeld(Action::Left) || in.isHeld(Action::Right)) && moveTimer == 0) {
            const int dir = in.isHeld(Action::Right) ? 1 : -1;
            player = isClosed() ? (player + dir + lanes) % lanes
                                : std::clamp(player + dir, 0, lanes - 1);
            moveTimer = moveEvery;
        }
        // 7a'. Mouse spinner: SELECT toggles relative-pointer capture; while captured, integrate raw
        //      horizontal mouse motion into a rotary position and step the claw a lane each kSpinPerLane
        //      of travel. The d-pad still walks the claw either way.
        if (in.justPressed(Action::ToggleSpinner)) { captured = !captured; platform.pointerCaptured(captured); }
        if (captured) {
            spin += in.rawDeltaX();
            while (spin >= kSpinPerLane) {
                player = isClosed() ? (player + 1 + lanes) % lanes : std::clamp(player + 1, 0, lanes - 1);
                spin -= kSpinPerLane;
            }
            while (spin <= -kSpinPerLane) {
                player = isClosed() ? (player - 1 + lanes) % lanes : std::clamp(player - 1, 0, lanes - 1);
                spin += kSpinPerLane;
            }
        }

        if (in.justPressed(Action::CycleTube)) gotoLevel(level + 1);
        // Firing (A or the left mouse button): a PRESS fires a burst of up to kShotsPerPress bolts (held,
        // they stream out at the fire cadence) — then it stops. To fire again you must RELEASE and press
        // again, which resets the per-press allowance. NOT indefinite auto-fire.
        const bool fireEdge = in.justPressed(Action::Fire) || in.mouseJustPressed(MouseButton::Left);
        const bool fireHeld = in.isHeld(Action::Fire)      || in.mouseHeld(MouseButton::Left);
        if (fireEdge) shotsThisPress = 0;
        if (fireHeld && shotsThisPress < kShotsPerPress && fireTimer == 0 &&
            static_cast<int>(bullets.size()) < kMaxBullets) {
            bullets.push_back(Bullet{player, 1.0f, true});
            ++shotsThisPress;
            fireTimer = kFireEvery;
        }
        if (in.justPressed(Action::Superzapper) && zapReady && !enemies.empty()) {  // Superzapper — one per life
            for (Enemy& e : enemies) { e.alive = false; onKill(); }
            zapReady = false;
            std::printf("ZAP! — score %d\n", score);
        }

        // 7b. Bolts: kill an enemy in the same lane at a near-enough depth; otherwise erode a spike tip in
        //     that lane (a spike eats the bolt); otherwise advance toward the far end.
        for (Bullet& b : bullets) {
            if (!b.alive) continue;
            for (Enemy& e : enemies)
                if (e.alive && e.lane == b.lane && std::abs(e.depth - b.depth) <= kHitWindow) {
                    e.alive = false; b.alive = false; onKill(); break;
                }
            if (!b.alive) continue;
            if (b.lane < static_cast<int>(laneSpike.size()) &&
                laneSpike[static_cast<std::size_t>(b.lane)] > 0.0f &&
                b.depth <= laneSpike[static_cast<std::size_t>(b.lane)] + kHitWindow) {
                laneSpike[static_cast<std::size_t>(b.lane)] =
                    std::max(0.0f, laneSpike[static_cast<std::size_t>(b.lane)] - kSpikeErode);
                b.alive = false;
                continue;
            }
            b.depth -= bulletSpeed;
            if (b.depth <= 0.0f) b.alive = false;
        }

        // 7c. Enemies. Flippers crawl up a lane then hop along the rim toward the player. Spikers crawl up
        //     more slowly, raising their lane's spike to their depth, then sit at the rim as a threat.
        for (Enemy& e : enemies) {
            if (!e.alive) continue;
            if (e.kind == EKind::Spiker) {
                if (e.depth < 1.0f) e.depth = std::min(1.0f, e.depth + spikeGrow);
                if (e.lane < static_cast<int>(laneSpike.size()))
                    laneSpike[static_cast<std::size_t>(e.lane)] =
                        std::max(laneSpike[static_cast<std::size_t>(e.lane)], e.depth);
                continue;
            }
            if (!e.flipping) {
                e.depth += enemySpeed;
                if (e.depth >= 1.0f) { e.depth = 1.0f; e.flipping = true; e.flipTimer = flipEvery; }
            } else if (--e.flipTimer <= 0) {
                e.lane = (e.lane + stepToward(e.lane, player) + lanes) % lanes;
                e.flipTimer = flipEvery;
            }
        }

        // 7d. Threat: a live enemy at the rim on the player's lane gets the player (unless invuln).
        if (invuln == 0)
            for (const Enemy& e : enemies)
                if (e.alive && e.depth >= 1.0f && e.lane == player) { loseLife(); break; }

        // 7e. Spawn a flipper or (kSpikerPercent of the time) a spiker at the far end on a random lane.
        if (invuln == 0 && --spawnTimer <= 0 && static_cast<int>(enemies.size()) < kMaxEnemies) {
            const int lane = static_cast<int>(nextRand(rng) % static_cast<unsigned>(lanes));
            const bool spiker = (nextRand(rng) % 100u) < static_cast<unsigned>(kSpikerPercent);
            enemies.push_back(Enemy{spiker ? EKind::Spiker : EKind::Flipper, lane, 0.0f, false, 0, true});
            spawnTimer = kSpawnEvery;
        }

        // 7f. Reap dead.
        bullets.erase(std::remove_if(bullets.begin(), bullets.end(),
                                     [](const Bullet& b) { return !b.alive; }), bullets.end());
        enemies.erase(std::remove_if(enemies.begin(), enemies.end(),
                                     [](const Enemy& e) { return !e.alive; }), enemies.end());
    });

    std::vector<TileCell> bgCells(static_cast<std::size_t>(kMapW) * kMapH);

    // ── Vector line tooling — a stroked Region + ColorFill is a drawn colored path ──────────────────
    // Each line is one Bezier segment; many disjoint segments of one color batch into stroked
    // curve-region(s) (the stroke is the union of the per-segment bands). The curve cbuffer carries up to
    // 32 segments per region, so a color's segments are emitted in 32-long chunks.
    constexpr std::size_t kSegsPerRegion = 32;
    auto lin  = [](Pt a, Pt b) {
        return CurveSegment{.p0 = Vec2{a.x, a.y}, .p1 = Vec2{b.x, b.y}, .degree = CurveDegree::Linear};
    };
    auto quad = [](Pt a, Pt c, Pt b) {
        return CurveSegment{.p0 = Vec2{a.x, a.y}, .p1 = Vec2{c.x, c.y}, .p2 = Vec2{b.x, b.y},
                            .degree = CurveDegree::Quadratic};
    };
    auto pushStroked = [&](std::vector<Region>& out, const std::vector<CurveSegment>& segs,
                           float width, Rgba8 colour) {
        for (std::size_t i = 0; i < segs.size(); i += kSegsPerRegion) {
            const std::size_t n = std::min(kSegsPerRegion, segs.size() - i);
            ShapePoints sp;
            sp.curve.assign(segs.begin() + static_cast<std::ptrdiff_t>(i),
                            segs.begin() + static_cast<std::ptrdiff_t>(i + n));
            sp.strokeWidth = width;
            out.push_back(Region{.key = "stroke",
                                 .shape = std::move(sp),
                                 .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                                               .fill = colour}}});
        }
    };

    // Per-style segment buckets, reused each frame.
    std::vector<CurveSegment> webSegs, farSegs, clawSegs, flipSegs, spikeSegs, spikerSegs, boltSegs;

    // ── 8. Render step ───────────────────────────────────────────────────────────────────────────────
    loop.renderLoop([&]() {
        // 8a. HUD: score (left) / lives (right) on row 1 over the black void. Each cell names the HUD sheet
        //     + its palette directly (black void vs lit glyph).
        for (auto& c : bgCells) { c.tile = kTileSolid; c.atlas = bgAtlas; c.palette = blackPal; }
        auto putNum = [&](int v, int endCol) { int x = v, col = endCol;
            do { bgCells[static_cast<std::size_t>(kMapW) + col].tile =
                     static_cast<std::uint16_t>(kTileDigit0 + x % 10);
                 bgCells[static_cast<std::size_t>(kMapW) + col].palette = hudPal; x /= 10; --col;
            } while (x > 0 && col >= 0); };
        putNum(score, 7);
        putNum(lives, kMapW - 2);

        const int N = numSpokes();
        const int edges = isClosed() ? N : N - 1;
        const int lanes = numLanes();

        // 8b. The tube: every spoke (far->rim) + the near-rim edges in bright blue; the far-ring edges in
        //     dim blue. For an OPEN tube there is no edge past the last spoke (no wrap).
        webSegs.clear(); farSegs.clear();
        for (int i = 0; i < N; ++i) webSegs.push_back(lin(spokeAt(i, 0.0f), spokeAt(i, 1.0f)));   // spokes
        for (int i = 0; i < edges; ++i) {
            const int j = (i + 1) % N;
            webSegs.push_back(lin(spokeAt(i, 1.0f), spokeAt(j, 1.0f)));   // near rim
            farSegs.push_back(lin(spokeAt(i, 0.0f), spokeAt(j, 0.0f)));   // far ring
        }

        // 8c. The player's lane the Tempest way: its TWO bounding spokes (the lines running INTO the
        //     screen) plus a rim bar and two prongs, in yellow over the blue tube.
        clawSegs.clear();
        {
            const int sL = player, sR = neighbour(player);
            clawSegs.push_back(lin(spokeAt(sL, 0.0f), spokeAt(sL, 1.0f)));   // left bounding spoke
            clawSegs.push_back(lin(spokeAt(sR, 0.0f), spokeAt(sR, 1.0f)));   // right bounding spoke
            const Pt a = spokeAt(sL, 1.0f), b = spokeAt(sR, 1.0f), tip = laneAt(player, 0.82f);
            clawSegs.push_back(lin(a, b));     // the rim bar between the two spokes
            clawSegs.push_back(lin(a, tip));   // claw prong 1
            clawSegs.push_back(lin(b, tip));   // claw prong 2
        }

        // 8d. Spikes: a line from the far end up to the spike's current tip, along the lane center.
        spikeSegs.clear();
        for (int l = 0; l < lanes && l < static_cast<int>(laneSpike.size()); ++l)
            if (laneSpike[static_cast<std::size_t>(l)] > 0.02f)
                spikeSegs.push_back(lin(laneAt(l, 0.0f), laneAt(l, laneSpike[static_cast<std::size_t>(l)])));

        // 8e. Enemies. Flippers are a BOWTIE spanning their lane (left/right tips on the two edge spokes at
        //     the flipper's depth, far/near tips on the lane center) so they scale with the lane. Spikers
        //     are a small diamond riding the lane center at the head of their spike.
        flipSegs.clear(); spikerSegs.clear();
        for (const Enemy& e : enemies) {
            if (e.kind == EKind::Spiker) {
                const Pt c = laneAt(e.lane, e.depth);
                const float s = 3.0f + e.depth * 3.0f;
                spikerSegs.push_back(lin(Pt{c.x - s, c.y}, Pt{c.x, c.y - s}));
                spikerSegs.push_back(lin(Pt{c.x, c.y - s}, Pt{c.x + s, c.y}));
                spikerSegs.push_back(lin(Pt{c.x + s, c.y}, Pt{c.x, c.y + s}));
                spikerSegs.push_back(lin(Pt{c.x, c.y + s}, Pt{c.x - s, c.y}));
                continue;
            }
            const float df = std::max(0.0f, e.depth - 0.06f), dn = std::min(1.0f, e.depth + 0.06f);
            const Pt a = spokeAt(e.lane, e.depth), b = spokeAt(neighbour(e.lane), e.depth);  // lane edges
            const Pt f = laneAt(e.lane, df), n = laneAt(e.lane, dn);                          // far/near tips
            flipSegs.push_back(lin(a, f)); flipSegs.push_back(lin(a, n));
            flipSegs.push_back(lin(b, f)); flipSegs.push_back(lin(b, n));
        }

        // 8f. Bolts: a curved tracer streak — a Quadratic from the trailing depth up to the head, bowed
        //     perpendicular to the lane so the path is visibly a curve (not a straight chord).
        boltSegs.clear();
        for (const Bullet& bl : bullets) {
            const Pt head = laneAt(bl.lane, bl.depth);
            const Pt tail = laneAt(bl.lane, std::min(1.0f, bl.depth + 0.13f));
            const float dx = head.x - tail.x, dy = head.y - tail.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            const float nx = len > 0.001f ? -dy / len : 0.0f, ny = len > 0.001f ? dx / len : 0.0f;
            const float bow = 5.0f;
            const Pt ctrl{(tail.x + head.x) * 0.5f + nx * bow, (tail.y + head.y) * 0.5f + ny * bow};
            boltSegs.push_back(quad(tail, ctrl, head));
        }

        // 8g. Assemble: HUD tile layer (the opaque source), then the vector bands painted onto it in
        //     back-to-front order (far ring -> tube -> claw -> spikes -> spikers -> flippers -> bolts).
        FrameDrawState frame;
        DrawLayer bg{.key = "hud"}; bg.z = 0; bg.size = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                 .cells = std::span<const TileCell>(bgCells)};
        frame.layers.push_back(bg);

        frame.regions.clear();
        pushStroked(frame.regions, farSegs,    kFarWidth,    Rgba8{34, 54, 110});    // far ring — dim blue
        pushStroked(frame.regions, webSegs,    kWebWidth,    Rgba8{70, 120, 230});   // tube — blue
        pushStroked(frame.regions, clawSegs,   kClawWidth,   Rgba8{245, 230, 80});   // claw — yellow
        pushStroked(frame.regions, spikeSegs,  kSpikeWidth,  Rgba8{255, 95, 40});    // spikes — orange
        pushStroked(frame.regions, spikerSegs, kSpikerWidth, Rgba8{90, 235, 120});   // spikers — green
        pushStroked(frame.regions, flipSegs,   kFlipWidth,   Rgba8{235, 70, 200});   // flippers — magenta
        pushStroked(frame.regions, boltSegs,   kBoltWidth,   Rgba8{245, 245, 245});  // bolts — white

        renderer.renderFrame(frame);
    });

    std::printf("Tempest (real vector lines) — Left/Right walk the claw, A or LEFT-CLICK fires up to 3 per "
                "press (release + press to fire again), B = Superzapper (one per life), START cycles the "
                "tube shape (circle / square / plus / line / V). SELECT toggles the MOUSE SPINNER (capture "
                "on = the mouse is the rotary knob). Shoot flippers before they reach you; shoot spiked "
                "lanes to clear the spikes. Close to quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
