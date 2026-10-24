// ============================================================================================
//  Missile Command — a small, complete arcade game on Polyrhythm, written as a heavily documented
//  teaching example. Like the Pong demo, every block explains what it does AND which engine API it
//  touches. This one is meatier: many simultaneous moving objects, missile trails, and expanding
//  circular explosions, all drawn from a handful of hand-built atlases.
// ============================================================================================
//
//  WHAT THIS DEMONSTRATES (engine features, roughly in order):
//    • EngineConfig with a NON-DEFAULT viewport AND timing:
//        - ViewportResolution::Genesis  → a 320×224 internal resolution (the Sega Genesis/Mega Drive).
//        - TimingProfile{TickPeriodNs::Hz60} → a clean 60 Hz fixed-step cadence. Note: 60 Hz exists as
//          a TickPeriodNs ENUM value; there is no named TimingProfile::Hz60 preset, so you build the
//          profile from the enum like this. (The two GB presets are TimingProfile::GameBoy/GameBoyColor.)
//    • SteadyClock + RunLoop                   — fixed-step sim (simTick) decoupled from render (renderLoop).
//    • SdlPlatform + Renderer                  — the live window + GPU device + draw API.
//    • uploadAtlas / uploadPalette             — indexed art + color onto the GPU.
//    • A TILE layer for the static backdrop     — sky + ground + the score readout, on the 8px grid.
//    • SPRITE layers for everything dynamic      — cities, battery, missile heads, trail dots, crosshair,
//                                                and explosions; DYNAMIC sprite counts (a std::vector
//                                                rebuilt each frame → arbitrary N sprites per layer).
//    • Sprite::transform (a per-sprite scale)    — the expanding explosions are one 16×16 circle sprite
//                                                scaled about its center to the current blast radius.
//
//  THE GAME:
//    • Enemy missiles rain from the top toward your 6 cities, leaving trails.
//    • A CROSSHAIR (arrows / WASD / d-pad) aims your battery (bottom center). Fire (X key or the
//      pad's south button) launches a counter-missile to the crosshair; when it arrives it
//      detonates into an expanding circular blast.
//    • Any enemy missile caught in a blast is destroyed (+points). Blasts chain: a ground hit also
//      detonates, so explosions can clear several missiles at once.
//    • A missile that reaches a city destroys it. Lose all 6 cities and, after a short pause, the wave
//      resets and the cities rebuild (endless — it's a demo). Your score persists.
//
// Missile motion is slow. Nothing strobes, and the demo never auto-launches a window; you run it
// yourself.
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
#include "retropp/input.h"          // InputState (isHeld / justPressed) — the game's Action enum keys the reads
#include "retropp/input_actions.h"  // ActionMap + presets — binds the game's actions to keys/pad
#include "retropp/palette.h"        // Rgba8 / PaletteId
#include "retropp/renderer.h"       // Renderer — uploadAtlas/uploadPalette + renderFrame
#include "retropp/run_loop.h"       // RunLoop — simTick / renderLoop
#include "retropp/sdl_platform.h"   // SdlPlatform
#include "retropp/timing.h"         // TimingProfile, TickPeriodNs (Hz60 lives here)
#include "retropp/transform.h"      // Transform — per-sprite scale for the expanding explosions
#include "retropp/viewport.h"       // ViewportResolution — the Genesis preset
#include "retropp/windowed_host.h"  // WindowedHost — pumps OS events + drives the loop

namespace {

using namespace retropp;
using namespace std::chrono_literals;

constexpr int kTile = 8;  // 8×8 px tiles (the Genesis tile cell, like every console of the era)

// The game's input vocabulary: four crosshair directions plus Fire.
enum class Action : std::uint8_t { Up, Down, Left, Right, Fire };

// Gameplay tuning — all SIZE-INDEPENDENT (absolute pixels / pixels-per-tick at 60 Hz). The viewport-
// derived layout (screen size, ground line, city/battery positions) is computed in main from the
// active viewport, so the same logic would scale to another resolution.
constexpr float kGroundFromBottom = 24.0f;   // ground strip height (px)
constexpr int   kNumCities        = 6;
constexpr float kCityW = 18.0f, kCityH = 11.0f;
constexpr float kBatteryW = 18.0f, kBatteryH = 13.0f;

constexpr float kCrosshairSpeed = 3.5f;   // crosshair px/tick (d-pad)
constexpr float kEnemySpeed     = 0.7f;   // enemy missile px/tick along its path (slow descent)
constexpr float kCounterSpeed   = 5.0f;   // your counter-missile px/tick (fast)
constexpr float kBlastMaxRadius = 26.0f;  // explosion peak radius (px)
constexpr int   kFireCooldown   = 8;      // ticks between your shots (stops trail spam)
constexpr int   kMaxEnemies     = 7;      // concurrent enemy missiles cap
constexpr int   kScorePerKill   = 25;

// Explosion timeline (ticks at 60 Hz): grow → hold → shrink. ~0.67 s total. Lethal while radius > 1.
constexpr int kBlastExpand = 16, kBlastHold = 4, kBlastShrink = 20;
constexpr int kBlastLife   = kBlastExpand + kBlastHold + kBlastShrink;

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

// Background tile-atlas layout: tile 0 = a solid block (used for both sky and ground, recolored by the
// cell's palette), tiles 1..10 = the digits 0..9 (5×7 lit on a 1px margin) for the score.
constexpr int kTileSolid  = 0;
constexpr int kTileDigit0 = 1;
constexpr int kBgTiles    = 11;

// Build the background tile atlas: one 8×8 tile per entry in a single 8-px-tall strip. Solid tile = all
// index 1; each digit lights index 1 where its glyph is set (index 0 stays background).
std::vector<std::uint8_t> buildBgAtlas() {
    const int w = kTile * kBgTiles;
    std::vector<std::uint8_t> a(static_cast<std::size_t>(w) * kTile, 0);
    for (int p = 0; p < kTile * kTile; ++p) a[p] = 1;  // tile 0: solid (every pixel index 1)
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

// A 16×16 filled-disc atlas: index 1 inside the radius, 0 outside (0 = the sprite transparent hole, so
// the blast is a clean circle). Scaled up per-explosion via Sprite::transform.
constexpr int kCircleSz = 16;
std::array<std::uint8_t, kCircleSz * kCircleSz> buildCircleAtlas() {
    std::array<std::uint8_t, kCircleSz * kCircleSz> a{};
    const float c = (kCircleSz - 1) / 2.0f, r = kCircleSz / 2.0f - 0.5f;
    for (int y = 0; y < kCircleSz; ++y) {
        for (int x = 0; x < kCircleSz; ++x) {
            const float dx = x - c, dy = y - c;
            if (dx * dx + dy * dy <= r * r) a[static_cast<std::size_t>(y) * kCircleSz + x] = 1;
        }
    }
    return a;
}

// A tiny clock-seeded LCG (variety per run, no <random>; this is a native game, not a GB ROM).
std::uint32_t nextRand(std::uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

// ── Game entities ─────────────────────────────────────────────────────────────────────────────────
// Each dynamic object carries a per-spawn `id`, so its sprites' reconciliation keys are stable for the
// object's whole lifetime and unique to that instance. The interpolator matches an object to its own
// previous-tick state by key; a key derived from emission INDEX shifts as the sprite population changes,
// so a sprite would inherit an unrelated object's history and flash at that object's position for a tick.
struct EnemyMissile {
    float sx, sy;          // spawn point (top) — the fixed tail of the trail
    float x, y;            // current head
    float vx, vy;          // velocity toward the target
    bool  alive = true;
    int   id = 0;          // stable identity for this missile's head + trail keys
};
struct CounterMissile {
    float bx, by;          // launch point (the battery) — the fixed tail of the trail
    float x, y;            // current head
    float vx, vy;
    float tx, ty;          // detonation target (the crosshair position at fire time)
    bool  alive = true;
    int   id = 0;          // stable identity for this missile's head + trail keys
};
struct Explosion {
    float x, y;            // center
    int   age = 0;         // ticks since detonation
    bool  scoring = false; // YOUR blasts score kills; ground/enemy blasts don't
    bool  alive = true;
    int   id = 0;          // stable identity for this blast's sprite key
};
struct City {
    float x;               // center x (fixed)
    bool  alive = true;
};

// The blast radius at a given age along the expand → hold → shrink timeline.
float blastRadius(int age) {
    if (age < kBlastExpand)               return kBlastMaxRadius * (static_cast<float>(age) / kBlastExpand);
    if (age < kBlastExpand + kBlastHold)  return kBlastMaxRadius;
    const float t = static_cast<float>(age - kBlastExpand - kBlastHold) / kBlastShrink;  // 0→1 shrinking
    return kBlastMaxRadius * (1.0f - t);
}

}  // namespace

int main() {

    // ── 1. Startup configuration — a GENESIS game at 60 Hz ──────────────────────────────────────────
    // viewport → 320×224 (Genesis internal resolution); timing → a clean 60 Hz fixed step built from the
    // TickPeriodNs enum (there is no named TimingProfile::Hz60 — only GameBoy/GameBoyColor are named
    // statics, so 60 Hz is constructed from the period enum). setActive() makes this the process-wide
    // config so the bare RunLoop/Renderer ctors below inherit the 60 Hz timing + 320×224 viewport.
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Missile Command Demo"},
        .window   = {.title = "Polyrhythm — Missile Command (Genesis, 60 Hz)"},
        .viewport = ViewportResolution::Genesis,
        .timing   = TimingProfile{TickPeriodNs::Hz60}};
    EngineConfig::setActive(config);

    // ── 2. Core engine objects ──────────────────────────────────────────────────────────────────────
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // Bind the game's actions: the crosshair directions on arrows + WASD + d-pad (the directional
    // preset), Fire on the X key or the pad's south face button.
    ActionMap map{
        {Action::Fire, {SDL_SCANCODE_X, PadButton::FaceSouth}},
    };
    map.add(presets::directional(Action::Up, Action::Down, Action::Left, Action::Right));
    platform.actions(map);

    // ── 3. Playfield dimensions, read from the active viewport ──────────────────────────────────────
    const int   kViewW   = config.viewport.width;    // 320
    const int   kViewH   = config.viewport.height;   // 224
    const int   kMapW    = kViewW / kTile;           // 40 background tiles wide
    const int   kMapH    = kViewH / kTile;           // 28 tall
    const float kGroundY = kViewH - kGroundFromBottom;            // y of the ground surface (== 200)
    const int   kGroundRow = static_cast<int>(kGroundY) / kTile;  // first ground tile row
    const float kBatteryX  = kViewW / 2.0f;                       // battery sits at bottom center

    // ── 4. Upload art + palettes ─────────────────────────────────────────────────────────────────────
    // 4a. Background tile atlas (solid + digits) and its palette SET. A tile cell picks a palette from
    //     this set; the pixel index (0/1) picks the entry. We recolor the SAME solid tile as sky or
    //     ground just by choosing palette 0 vs 1; digits use palette 2 (lit on the sky color).
    const std::vector<std::uint8_t> bgPx = buildBgAtlas();
    const AtlasId bgAtlas = renderer.uploadAtlas(bgPx.data(), kTile * kBgTiles, kTile).atlasId;
    const std::array<Rgba8, 2> palSky{{ {12, 14, 34}, {12, 14, 34} }};       // solid → night sky
    const std::array<Rgba8, 2> palGround{{ {44, 30, 22}, {44, 30, 22} }};    // solid → brown ground
    const std::array<Rgba8, 2> palScore{{ {12, 14, 34}, {235, 225, 120} }};  // digit: sky bg + amber lit
    const PaletteId skyPal    = renderer.uploadPalette(std::span<const Rgba8>(palSky));
    const PaletteId groundPal = renderer.uploadPalette(std::span<const Rgba8>(palGround));
    const PaletteId scorePal  = renderer.uploadPalette(std::span<const Rgba8>(palScore));
    // (each tile cell now names skyPal / groundPal / scorePal directly — no per-layer palette set)

    // 4b. A solid 16×16 atlas (all index 1) → every solid rectangle (cities, battery, missile heads,
    //     trail dots, crosshair bars). A sprite reads its `size`-sized region from tile 0's top-left,
    //     so any rect up to 16×16 is a crop of solid fill; color comes from the sprite's palette.
    // A solid index-1 sheet the rect() sprites read a w×h region from. It must be at least as large as the
    // widest / tallest solid sprite (the 18×13 battery, the 18-wide cities): a sprite reads px in [0, size),
    // and any column/row past the sheet's extent reads index 0 → the city/battery showed a 2px black strip
    // where they overran the old 16-wide sheet.
    std::array<std::uint8_t, 24 * 24> solidPx{};
    solidPx.fill(1);
    const AtlasId solidAtlas = renderer.uploadAtlas(solidPx.data(), 24, 24).atlasId;
    // Solid-layer palette set — sprite.palette selects the color (entry 1 is the color; 0 is unused).
    const std::array<Rgba8, 2> pCity{{ {0,0,0}, {90, 200, 120} }};      // 0: city — green
    const std::array<Rgba8, 2> pBattery{{ {0,0,0}, {120, 180, 255} }};  // 1: battery — blue
    const std::array<Rgba8, 2> pEnemy{{ {0,0,0}, {235, 90, 70} }};      // 2: enemy missile + trail — red
    const std::array<Rgba8, 2> pCounter{{ {0,0,0}, {120, 235, 235} }};  // 3: your missile + trail — cyan
    const std::array<Rgba8, 2> pRubble{{ {0,0,0}, {70, 60, 55} }};      // 4: destroyed city — gray
    const std::array<Rgba8, 2> pCross{{ {0,0,0}, {245, 245, 245} }};    // 5: crosshair — white
    const PaletteId cityPal    = renderer.uploadPalette(std::span<const Rgba8>(pCity));
    const PaletteId batteryPal = renderer.uploadPalette(std::span<const Rgba8>(pBattery));
    const PaletteId enemyPal   = renderer.uploadPalette(std::span<const Rgba8>(pEnemy));
    const PaletteId counterPal = renderer.uploadPalette(std::span<const Rgba8>(pCounter));
    const PaletteId rubblePal  = renderer.uploadPalette(std::span<const Rgba8>(pRubble));
    const PaletteId crossPal   = renderer.uploadPalette(std::span<const Rgba8>(pCross));
    const std::array<PaletteId, 6> solidSet{cityPal, batteryPal, enemyPal, counterPal, rubblePal, crossPal};
    enum SolidPal { PAL_CITY = 0, PAL_BATTERY, PAL_ENEMY, PAL_COUNTER, PAL_RUBBLE, PAL_CROSS };

    // 4c. The 16×16 circle atlas (its own atlas/layer, since it's a different shape) + one amber palette.
    const std::array<std::uint8_t, kCircleSz * kCircleSz> circlePx = buildCircleAtlas();
    const AtlasId circleAtlas = renderer.uploadAtlas(circlePx.data(), kCircleSz, kCircleSz, TransparentIndices::GameBoy).atlasId;
    const std::array<Rgba8, 2> pBlast{{ {0,0,0}, {250, 210, 80} }};  // amber blast (steady — no flash)
    const PaletteId blastPal = renderer.uploadPalette(std::span<const Rgba8>(pBlast));
    // (the blast sprite names blastPal directly — no per-layer palette set)

    // ── 5. Game state ────────────────────────────────────────────────────────────────────────────────
    std::array<City, kNumCities> cities{};
    // Spread the 6 cities across the width in two groups of 3, leaving the center clear for the battery.
    auto layoutCities = [&] {
        const float slot[kNumCities] = {0.12f, 0.24f, 0.36f, 0.64f, 0.76f, 0.88f};
        for (int i = 0; i < kNumCities; ++i) { cities[static_cast<std::size_t>(i)].x = slot[i] * kViewW;
                                               cities[static_cast<std::size_t>(i)].alive = true; }
    };
    layoutCities();

    std::vector<EnemyMissile>   enemies;
    std::vector<CounterMissile> counters;
    std::vector<Explosion>      blasts;
    int                         nextId = 1;  // monotonic per-spawn id (each object's stable key identity)

    float crossX = kViewW / 2.0f, crossY = kViewH / 2.0f;  // crosshair (d-pad aim)
    int   score = 0;
    int   fireTimer = 0;        // counts down between shots
    int   spawnTimer = 0;       // counts down to the next enemy spawn
    int   gameOverTimer = 0;    // >0 while waiting to rebuild after losing all cities
    std::uint32_t rng = static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    // Spawn interval as a duration → ticks via the active (60 Hz) profile.
    const int kSpawnEvery = static_cast<int>(config.timing.ticksForDuration(1100ms));  // ~66 ticks
    spawnTimer = kSpawnEvery;

    auto rand01 = [&] { return static_cast<float>(nextRand(rng) % 10000) / 10000.0f; };

    // Spawn one enemy missile: from a random x at the top, aimed at a random ALIVE city (or a random
    // ground x if none remain), traveling at kEnemySpeed along that path.
    auto spawnEnemy = [&] {
        if (static_cast<int>(enemies.size()) >= kMaxEnemies) return;
        EnemyMissile m{};
        m.id = nextId++;
        m.sx = rand01() * kViewW;  m.sy = 0.0f;
        m.x = m.sx; m.y = m.sy;
        // pick a target x: a living city if any, else anywhere along the ground
        std::vector<float> targets;
        for (const City& c : cities) if (c.alive) targets.push_back(c.x);
        const float tx = targets.empty() ? rand01() * kViewW
                                         : targets[static_cast<std::size_t>(nextRand(rng) % targets.size())];
        const float ty = kGroundY;
        const float dx = tx - m.sx, dy = ty - m.sy, len = std::sqrt(dx * dx + dy * dy);
        m.vx = (len > 0 ? dx / len : 0) * kEnemySpeed;
        m.vy = (len > 0 ? dy / len : 1) * kEnemySpeed;
        enemies.push_back(m);
    };

    // Detonate: add a blast at (x,y). `scoring` blasts (yours) award points for kills.
    auto detonate = [&](float x, float y, bool scoring) {
        blasts.push_back(Explosion{.x = x, .y = y, .age = 0, .scoring = scoring, .alive = true, .id = nextId++});
    };

    // Fire a counter-missile from the battery toward the crosshair (its detonation target is fixed now).
    auto fire = [&] {
        if (fireTimer > 0) return;
        CounterMissile m{};
        m.id = nextId++;
        m.bx = kBatteryX; m.by = kGroundY;
        m.x = m.bx; m.y = m.by;
        m.tx = crossX; m.ty = crossY;
        const float dx = m.tx - m.bx, dy = m.ty - m.by, len = std::sqrt(dx * dx + dy * dy);
        m.vx = (len > 0 ? dx / len : 0) * kCounterSpeed;
        m.vy = (len > 0 ? dy / len : -1) * kCounterSpeed;
        counters.push_back(m);
        fireTimer = kFireCooldown;
    };

    // ── 6. Simulation step (60 Hz) ───────────────────────────────────────────────────────────────────
    loop.simTick([&](const InputState& in) {
        if (fireTimer > 0) --fireTimer;

        // 6a. Crosshair (the directional actions), clamped to the sky region (above the ground).
        if (in.isHeld(Action::Left))  crossX -= kCrosshairSpeed;
        if (in.isHeld(Action::Right)) crossX += kCrosshairSpeed;
        if (in.isHeld(Action::Up))    crossY -= kCrosshairSpeed;
        if (in.isHeld(Action::Down))  crossY += kCrosshairSpeed;
        crossX = std::clamp(crossX, 6.0f, kViewW - 6.0f);
        crossY = std::clamp(crossY, 6.0f, kGroundY - 6.0f);
        if (in.justPressed(Action::Fire)) fire();  // launch a counter-missile

        // 6b. Enemy spawning (paused during the post-wipeout pause).
        if (gameOverTimer > 0) {
            if (--gameOverTimer == 0) { layoutCities(); enemies.clear(); }  // rebuild the wave
        } else if (--spawnTimer <= 0) {
            spawnEnemy();
            spawnTimer = kSpawnEvery;
        }

        // 6c. Move enemy missiles; a missile reaching the ground destroys a city there (and detonates,
        //     non-scoring, so its blast can still chain other missiles).
        for (EnemyMissile& m : enemies) {
            if (!m.alive) continue;
            m.x += m.vx; m.y += m.vy;
            if (m.y >= kGroundY) {
                m.y = kGroundY;
                for (City& c : cities) {
                    if (c.alive && std::abs(c.x - m.x) <= kCityW / 2) { c.alive = false; break; }
                }
                detonate(m.x, kGroundY, /*scoring=*/false);
                m.alive = false;
            }
        }

        // 6d. Move counter-missiles; detonate (scoring) on reaching the target.
        for (CounterMissile& m : counters) {
            if (!m.alive) continue;
            m.x += m.vx; m.y += m.vy;
            if ((m.x - m.tx) * m.vx + (m.y - m.ty) * m.vy >= 0) {  // passed/reached the target
                detonate(m.tx, m.ty, /*scoring=*/true);
                m.alive = false;
            }
        }

        // 6e. Age blasts; each lethal blast kills enemy heads inside its current radius (scoring blasts
        //     award points). This is also where chain reactions happen.
        for (Explosion& b : blasts) {
            if (!b.alive) continue;
            const float r = blastRadius(b.age);
            if (r > 1.0f) {
                for (EnemyMissile& m : enemies) {
                    if (!m.alive) continue;
                    const float dx = m.x - b.x, dy = m.y - b.y;
                    if (dx * dx + dy * dy <= r * r) {
                        m.alive = false;
                        if (b.scoring) score += kScorePerKill;
                    }
                }
            }
            if (++b.age >= kBlastLife) b.alive = false;
        }

        // 6f. Reap dead entities (keeps the vectors — and the per-frame sprite lists — bounded).
        enemies.erase(std::remove_if(enemies.begin(), enemies.end(),
                                     [](const EnemyMissile& m) { return !m.alive; }), enemies.end());
        counters.erase(std::remove_if(counters.begin(), counters.end(),
                                      [](const CounterMissile& m) { return !m.alive; }), counters.end());
        blasts.erase(std::remove_if(blasts.begin(), blasts.end(),
                                    [](const Explosion& b) { return !b.alive; }), blasts.end());

        // 6g. Lost all cities → start the short rebuild pause (existing missiles/blasts still resolve).
        if (gameOverTimer == 0 &&
            std::none_of(cities.begin(), cities.end(), [](const City& c) { return c.alive; })) {
            std::printf("all cities lost — score %d — new wave\n", score);
            gameOverTimer = static_cast<int>(config.timing.ticksForDuration(2500ms));
        }
    });

    // Reused per-frame buffers (cleared + refilled each render — immediate mode, no retained state).
    std::vector<TileCell> bgCells(static_cast<std::size_t>(kMapW) * kMapH);
    std::vector<Sprite>   solidSprites;   // cities, battery, trails, heads
    std::vector<Sprite>   blastSprites;   // scaled circles
    std::vector<Sprite>   crossSprites;   // the two crosshair bars
    // Every sprite gets a STABLE identity key (a city index, a per-missile id + trail step, a per-blast id,
    // a fixed name) — NEVER its emission index, which shifts as the sprite population changes and would
    // make the interpolator cross-fade unrelated sprites (a one-tick flash at the wrong place). ObjectKey
    // owns its bytes, so a key assembled per frame moves straight into the sprite.
    static const std::vector<std::string> crossKeys{"cross0", "cross1"};

    // Append a solid rectangle (center-anchored) under the stable key `key`, in palette `pal` (an index
    // into solidSet — each sprite now names its sheet + palette handle directly).
    auto rect = [&](std::string key, float cx, float cy, float w, float h, int pal) {
        solidSprites.push_back(Sprite{
            .key = std::move(key),
            .x = static_cast<int>(cx - w / 2), .y = static_cast<int>(cy - h / 2),
            .size = AssetDimensions{static_cast<int>(w), static_cast<int>(h)}, .atlas = solidAtlas,
            .tile = 0, .palette = solidSet[static_cast<std::size_t>(pal)]});
    };
    // Append a missile's trail as a row of small dots from its fixed tail to its current head. Each dot
    // is keyed `<keyPrefix>_t<step>` — stable per (missile, step), so a growing/shrinking trail mounts or
    // unmounts dots at its head without disturbing the identity of the ones behind it.
    auto trail = [&](const std::string& keyPrefix, float tailX, float tailY, float headX, float headY, int pal) {
        const float dx = headX - tailX, dy = headY - tailY, len = std::sqrt(dx * dx + dy * dy);
        const int steps = static_cast<int>(len / 5.0f);  // a dot every 5 px
        for (int i = 1; i <= steps; ++i) {
            const float t = static_cast<float>(i) / (steps + 1);
            rect(keyPrefix + "_t" + std::to_string(i), tailX + dx * t, tailY + dy * t, 2.0f, 2.0f, pal);
        }
    };

    // ── 7. Render step ───────────────────────────────────────────────────────────────────────────────
    loop.renderLoop([&]() {
        // 7a. Background tile layer: sky everywhere, ground in the bottom rows, score digits up top.
        for (int row = 0; row < kMapH; ++row) {
            for (int col = 0; col < kMapW; ++col) {
                TileCell& c = bgCells[static_cast<std::size_t>(row) * kMapW + col];
                c.tile    = kTileSolid;
                c.atlas   = bgAtlas;
                c.palette = (row >= kGroundRow) ? groundPal : skyPal;  // ground vs sky
            }
        }
        // Score, left-aligned at the top, most-significant digit first (always at least one digit).
        { int s = score, col = 7;
          do { const int d = s % 10; s /= 10;
               bgCells[1 * static_cast<std::size_t>(kMapW) + col].tile =
                   static_cast<std::uint16_t>(kTileDigit0 + d);
               bgCells[1 * static_cast<std::size_t>(kMapW) + col].atlas   = bgAtlas;
               bgCells[1 * static_cast<std::size_t>(kMapW) + col].palette = scorePal;  // score palette
               --col;
          } while (s > 0 && col >= 0); }

        // 7b. Solid sprites: cities (or rubble), battery, then every missile's trail + head. Each sprite
        //     carries a STABLE identity key (see the stable-key note above); emission order no longer decides it.
        solidSprites.clear();
        for (std::size_t i = 0; i < cities.size(); ++i) {
            const City& c = cities[i];
            const std::string k = "city_" + std::to_string(i);  // a city keeps its identity as rubble
            if (c.alive) rect(k, c.x, kGroundY - kCityH / 2, kCityW, kCityH, PAL_CITY);
            else         rect(k, c.x, kGroundY - 3, kCityW, 6.0f, PAL_RUBBLE);  // flattened rubble
        }
        rect("battery", kBatteryX, kGroundY - kBatteryH / 2, kBatteryW, kBatteryH, PAL_BATTERY);
        for (const EnemyMissile& m : enemies) {
            const std::string k = "enemy_" + std::to_string(m.id);
            trail(k, m.sx, m.sy, m.x, m.y, PAL_ENEMY);
            rect(k + "_head", m.x, m.y, 3.0f, 3.0f, PAL_ENEMY);
        }
        for (const CounterMissile& m : counters) {
            const std::string k = "counter_" + std::to_string(m.id);
            trail(k, m.bx, m.by, m.x, m.y, PAL_COUNTER);
            rect(k + "_head", m.x, m.y, 3.0f, 3.0f, PAL_COUNTER);
        }

        // 7c. Explosions: ONE 16×16 circle sprite each, scaled about its center to the live radius via
        //     Sprite::transform (the circle's own radius in the sprite is 8 px, so scale = radius / 8).
        blastSprites.clear();
        for (const Explosion& b : blasts) {
            const float r = blastRadius(b.age);
            if (r <= 0.5f) continue;
            const float s = r / (kCircleSz / 2.0f);  // scale factor
            blastSprites.push_back(Sprite{
                .key = "blast_" + std::to_string(b.id),
                .x = static_cast<int>(b.x - kCircleSz / 2.0f), .y = static_cast<int>(b.y - kCircleSz / 2.0f),
                .size = AssetDimensions{kCircleSz, kCircleSz}, .atlas = circleAtlas,
                .tile = 0, .palette = blastPal,
                .transform = Transform::scale(s, s, kCircleSz / 2.0f, kCircleSz / 2.0f)});
        }

        // 7d. Crosshair: two thin solid bars forming a +, on top of everything.
        crossSprites.clear();
        crossSprites.push_back(Sprite{.key = crossKeys[crossSprites.size()],
                                      .x = static_cast<int>(crossX - 6), .y = static_cast<int>(crossY - 1),
                                      .size = AssetDimensions{12, 2}, .atlas = solidAtlas,
                                      .tile = 0, .palette = crossPal});
        crossSprites.push_back(Sprite{.key = crossKeys[crossSprites.size()],
                                      .x = static_cast<int>(crossX - 1), .y = static_cast<int>(crossY - 6),
                                      .size = AssetDimensions{2, 12}, .atlas = solidAtlas,
                                      .tile = 0, .palette = crossPal});

        // 7e. Assemble the frame: backdrop (z=0) → solids (z=10) → blasts (z=20) → crosshair (z=30).
        FrameDrawState frame;
        DrawLayer bg{.key = "backdrop"}; bg.z = 0; bg.size = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles  = kMapW,
                                 .heightInTiles = kMapH,
                                 .cells         = std::span<const TileCell>(bgCells)};
        frame.layers.push_back(bg);

        DrawLayer solids{.key = "solids"}; solids.z = 10; solids.size = PixelSize{kViewW, kViewH};
        solids.content = SpriteContent{.sprites = std::span<const Sprite>(solidSprites)};
        frame.layers.push_back(solids);

        DrawLayer expl{.key = "blasts"}; expl.z = 20; expl.size = PixelSize{kViewW, kViewH};
        expl.content = SpriteContent{.sprites = std::span<const Sprite>(blastSprites)};
        frame.layers.push_back(expl);

        DrawLayer cross{.key = "crosshair"}; cross.z = 30; cross.size = PixelSize{kViewW, kViewH};
        cross.content = SpriteContent{.sprites = std::span<const Sprite>(crossSprites)};
        frame.layers.push_back(cross);

        renderer.renderFrame(frame);
    });

    std::printf("Missile Command (Genesis, 60 Hz) — arrows/WASD/d-pad aim the crosshair; X (or the "
                "pad's south button) fires a counter-missile. Defend your 6 cities; blasts destroy "
                "missiles (and chain). Close to quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
