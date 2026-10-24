// ============================================================================================
//  Centipede — a sprite + palette showcase on Polyrhythm, written as a heavily documented teaching
//  example. The earlier demos (Pong/Breakout/Missile Command) drew solid rectangles, and Tempest
//  drew lines; THIS one is about real INDEXED SPRITE ART and the PALETTE system: multi-color
//  hand-pixelled sprites whose colors come entirely from palettes, with per-sprite palette
//  SELECTION, animated palette CYCLING, and palette SWAPS for state + level changes.
// ============================================================================================
//
//  WHAT THIS DEMONSTRATES (engine features — the focus is sprites + palettes):
//    • Indexed sprite art: ONE atlas of 8×8 tiles where each pixel is a PALETTE INDEX (0 =
//      transparent hole, 1..13 = color slots). The centipede, mushrooms, blaster, bullet and spider
//      are all hand-pixelled multi-color shapes — not solid blocks.
//    • Palette SELECTION (the core idea): a sprite's COLORS come from the palette its `palette` field
//      selects out of the layer's palette set. The SAME centipede-segment art is drawn in 6 different
//      hues just by choosing a different palette per segment.
//    • Palette CYCLING (animation with no art change): the rainbow MARCHES along the centipede and
//      shifts over time — purely by advancing which palette each segment selects each frame. (This is
//      the "palette is a per-frame field" idea, here as a live color wave.)
//    • Palette SWAP for state + level: a poisoned mushroom selects a purple palette (same art); each
//      new wave swaps the mushroom palette so the whole field recolors.
//    • One SpriteContent layer carries the multi-tile atlas + a 12-entry palette set; every game
//      object is a sprite that picks (tile, palette, position, flipX). Dynamic sprite count.
//    • EngineConfig viewport + timing (ViewportResolution::Nes 256×240 + TimingProfile{TickPeriodNs::Hz60}).
//
//  THE GAME:
//    • A centipede winds down through a field of mushrooms. Your blaster (bottom band) moves with the
//      d-pad / arrows / WASD; HOLD X (pad A) to fire upward. Shoot a body segment and it SPLITS into two centipedes and drops a
//      mushroom; shoot mushrooms to clear lanes (4 hits each). A roaming spider eats mushrooms — shoot
//      it for points. If the centipede (or spider) reaches your blaster you lose one of 3 lives; clear
//      the centipede for the next (faster, recolored) wave.
//
// The demo never auto-launches a window; you run it.
//
//  CI: like the other example hosts it instantiates SdlPlatform + Renderer for real, so the live GPU
//  path keeps compiling/linking on every CI platform — but CI never opens the window.
// ============================================================================================

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "retropp/clock.h"          // SteadyClock
#include "retropp/draw_state.h"     // FrameDrawState / DrawLayer / TileContent / SpriteContent / Sprite
#include "retropp/engine_config.h"  // EngineConfig
#include "retropp/input.h"          // InputState (isHeld / justPressed)
#include "retropp/input_actions.h"  // ActionMap + PadButton + presets — the demo's bindings
#include "retropp/palette.h"        // Rgba8 / PaletteId
#include "retropp/renderer.h"       // Renderer — uploadAtlas/uploadPalette + renderFrame
#include "retropp/run_loop.h"       // RunLoop — simTick / renderLoop
#include "retropp/sdl_platform.h"   // SdlPlatform
#include "retropp/timing.h"         // TimingProfile, TickPeriodNs (Hz60)
#include "retropp/transform.h"      // Transform — the hit-burst pops by scaling over its short life
#include "retropp/viewport.h"       // ViewportResolution — the Nes preset
#include "retropp/windowed_host.h"  // WindowedHost — pumps OS events + drives the loop

namespace {

using namespace retropp;
using namespace std::chrono_literals;

// The game's input vocabulary: blaster movement + fire.
enum class Action : std::uint8_t { Up, Down, Left, Right, Fire };

constexpr int kCell = 8;  // 8×8 px cell — the grid unit AND the sprite tile size

// ── Indexed sprite art ──────────────────────────────────────────────────────────────────────────────
// Each tile is 8 rows of 8 chars; a char is a PALETTE INDEX: '.' = 0 (transparent), '1'..'9' = 1..9,
// 'a'..'d' = 10..13. The color for an index is supplied by whichever palette a sprite selects — so the
// art is color-AGNOSTIC: index 2 is "the body color", whatever the chosen palette makes it.
//   centipede body/head: 1 outline, 2 body, 3 highlight, 4 eyes
//   mushroom:            5 cap,     6 spots, 7 stem
//   blaster:             8 hull,    9 cockpit
//   bullet:             10 bolt
//   spider:             11 legs,   12 body, 13 eyes
constexpr int kTileBody = 0, kTileHead = 1, kTileMushFull = 2, kTileMushMid = 3, kTileMushLow = 4,
              kTileBlaster = 5, kTileBullet = 6, kTileSpider = 7, kTileScorpion = 8, kTileBurst = 9;
constexpr int kSpriteTiles = 10;
constexpr std::array<std::array<std::string_view, 8>, kSpriteTiles> kArt{{
    {{ "..1111..", ".122221.", "12222221", "12322321", "12222221", ".122221.", "1.2222.1", ".1.11.1." }},  // body
    {{ "..1111..", ".122221.", "12422421", "12222221", "12233221", ".122221.", "1.2222.1", ".1.11.1." }},  // head (eyes)
    {{ "..5555..", ".556655.", "55655655", "55556555", ".555555.", "...77...", "...77...", "..7777.." }},   // mushroom full
    {{ "........", "..5555..", ".556655.", ".555555.", "...77...", "...77...", "..7777..", "........" }},    // mushroom mid
    {{ "........", "........", "..5555..", ".556655.", "...77...", "..7777..", "........", "........" }},     // mushroom low
    {{ "...88...", "...88...", "..8888..", ".889988.", "88999988", "88888888", "8.8888.8", "8......8" }},    // blaster
    {{ "........", "...aa...", "...aa...", "...aa...", "........", "........", "........", "........" }},     // bullet
    {{ "b......b", ".b.cc.b.", "..cccc..", "bcdccdcb", "..cccc..", ".b.cc.b.", "b.b..b.b", "........" }},     // spider
    {{ "........", "...cccc.", "..ccccdc", "bcccccc.", "..cccc..", ".b.b.b.b", "........", "........" }},   // scorpion (legs b/body c/eye d)
    {{ "...2....", ".2.1.2..", "..212...", "2211122.", "..212...", ".2.1.2..", "...2....", "........" }},   // burst (hit spark: 1 core, 2 rays)
}};

std::uint8_t artIndex(char c) {
    if (c == '.') return 0;
    if (c >= '1' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    return static_cast<std::uint8_t>(10 + (c - 'a'));  // 'a'..'d' → 10..13
}

// Lay the 8 tiles left-to-right in one 8-px-tall strip; each pixel is its art index.
std::vector<std::uint8_t> buildSpriteAtlas() {
    const int w = kCell * kSpriteTiles;
    std::vector<std::uint8_t> a(static_cast<std::size_t>(w) * kCell, 0);
    for (int t = 0; t < kSpriteTiles; ++t)
        for (int r = 0; r < kCell; ++r) {
            const std::string_view row = kArt[static_cast<std::size_t>(t)][static_cast<std::size_t>(r)];
            for (int c = 0; c < kCell; ++c) {
                const char ch = c < static_cast<int>(row.size()) ? row[static_cast<std::size_t>(c)] : '.';
                a[static_cast<std::size_t>(r) * w + (t * kCell + c)] = artIndex(ch);
            }
        }
    return a;
}

// A palette as a 16-slot color table indexed by the pixel index. makePal fills only the slots a sprite
// type uses; the rest stay black (those indices never appear in that sprite's art, so they're never
// sampled). Slot 0 is the transparent hole — its color is irrelevant.
std::array<Rgba8, 16> makePal(std::initializer_list<std::pair<int, Rgba8>> entries) {
    std::array<Rgba8, 16> p{};
    for (const auto& [i, c] : entries) p[static_cast<std::size_t>(i)] = c;
    return p;
}

// ── HUD digit font (5×7) + its tile atlas (for score / lives over the backdrop) ─────────────────────
constexpr std::array<std::array<std::uint8_t, 7>, 10> kDigits{{
    {0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110},{0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110},
    {0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111},{0b11111,0b00010,0b00100,0b00010,0b00001,0b10001,0b01110},
    {0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010},{0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110},
    {0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110},{0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000},
    {0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110},{0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100},
}};
constexpr int kTileSolid = 0, kTileDigit0 = 1, kBgTiles = 11;

std::vector<std::uint8_t> buildBgAtlas() {
    const int w = kCell * kBgTiles;
    std::vector<std::uint8_t> a(static_cast<std::size_t>(w) * kCell, 0);
    for (int p = 0; p < kCell * kCell; ++p) a[p] = 1;
    for (int d = 0; d < 10; ++d)
        for (int row = 0; row < 7; ++row) {
            const std::uint8_t bits = kDigits[static_cast<std::size_t>(d)][static_cast<std::size_t>(row)];
            for (int col = 0; col < 5; ++col)
                if ((bits >> (4 - col)) & 1)
                    a[static_cast<std::size_t>(1 + row) * w + ((kTileDigit0 + d) * kCell + 1 + col)] = 1;
        }
    return a;
}

std::uint32_t nextRand(std::uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

// Tuning.
constexpr int kCentStartLen = 10;
constexpr int kMushMaxHp    = 4;
constexpr int kMoveEvery    = 4;     // blaster: ticks per cell while held
constexpr int kFireEvery    = 7;     // ticks between bolts (hold Fire to stream)
constexpr float kBulletSpeed = 0.55f;// bullet rows/tick upward
constexpr int kLives        = 3;
constexpr int kRainbowRate  = 8;     // frames per rainbow hue step (palette cycling speed)
constexpr int kRainbowCount = 6;     // the 6 rainbow palettes

struct Cell { int cx, cy; };
struct Centipede { std::vector<Cell> segs; int dx; int vy; int stepTimer; };  // segs[0] = head
// A bullet carries a per-spawn `id` so its stable reconciliation key tracks THIS bolt as it flies (the
// interpolator eases its motion by key); a burst carries one so overlapping sparks stay distinct. The
// stationary grid objects (mushrooms, centipede segments) key by their cell position instead — see render.
struct Bullet    { int col; float row; bool alive = true; int id = 0; };
struct Spider    { float x, y, vx, vy; bool alive = false; int life = 0; };
struct Scorpion  { float x; int row; float vx; bool alive = false; };  // crosses a row, poisoning mushrooms
struct Burst     { int cx, cy; int age = 0; int id = 0; };             // a short hit spark at a kill cell
constexpr int kBurstLife = 12;  // ticks a hit burst lives (≈0.2 s) — a quick pop, not a strobe

}  // namespace

int main() {

    // ── 1. Config — NES at 60 Hz ────────────────────────────────────────────────────────────────────
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Centipede Demo"},
        .window   = {.title = "Polyrhythm — Centipede (NES, 60 Hz)"},
        .viewport = ViewportResolution::Nes,             // 256×240
        .timing   = TimingProfile{TickPeriodNs::Hz60}};
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // Bindings: movement on the directional preset (arrows + WASD + d-pad); X (pad A / Sony ✕) fires.
    ActionMap map{
        {Action::Fire, {SDL_SCANCODE_X, PadButton::FaceSouth, SDL_SCANCODE_P}},
    };
    map.add(presets::directional(Action::Up, Action::Down, Action::Left, Action::Right));
    platform.actions(map);

    const int kViewW = config.viewport.width, kViewH = config.viewport.height;  // 256, 240
    const int kCols = kViewW / kCell, kRows = kViewH / kCell;                    // 32, 30
    const int kFieldTop = 2;                  // rows 0..1 are the HUD
    const int kBlasterTop = kRows - 6;        // the bottom band the blaster roams

    // ── 2. Upload art + palettes ────────────────────────────────────────────────────────────────────
    // 2a. Backdrop (solid + digits) for the HUD.
    const std::vector<std::uint8_t> bgPx = buildBgAtlas();
    const AtlasId bgAtlas = renderer.uploadAtlas(bgPx.data(), kCell * kBgTiles, kCell).atlasId;
    const std::array<Rgba8, 2> palVoid{{ {8, 10, 20}, {8, 10, 20} }};
    const std::array<Rgba8, 2> palHud{{ {8, 10, 20}, {230, 230, 245} }};
    const PaletteId voidPal = renderer.uploadPalette(std::span<const Rgba8>(palVoid));
    const PaletteId hudPal  = renderer.uploadPalette(std::span<const Rgba8>(palHud));

    // 2b. THE SPRITE ATLAS — one indexed atlas of all 8 sprite tiles. Color comes from palettes, below.
    const std::vector<std::uint8_t> spritePx = buildSpriteAtlas();
    const AtlasId spriteAtlas = renderer.uploadAtlas(spritePx.data(), kCell * kSpriteTiles, kCell, TransparentIndices::GameBoy).atlasId;

    // 2c. THE PALETTE SET — every palette any sprite might select, in ONE set. A sprite's `palette` field
    //     is an index into this set; that's the whole coloring mechanism. 6 rainbow hues for the
    //     centipede, two mushroom schemes (swapped per wave) + a poisoned scheme, blaster, bullet, spider.
    const Rgba8 outline{20, 22, 34}, eyes{245, 245, 255};
    const std::array<std::pair<Rgba8, Rgba8>, kRainbowCount> hues{{   // {body, highlight}
        {{220, 60, 60}, {255, 130, 130}},  {{232, 132, 44}, {255, 196, 120}},
        {{226, 210, 64}, {255, 242, 150}}, {{74, 202, 96}, {158, 244, 170}},
        {{64, 200, 214}, {158, 244, 248}}, {{210, 84, 204}, {246, 168, 236}},
    }};
    std::vector<PaletteId> palSet;
    auto upload = [&](const std::array<Rgba8, 16>& p) {
        const PaletteId id = renderer.uploadPalette(std::span<const Rgba8>(p));
        palSet.push_back(id);
        return static_cast<int>(palSet.size()) - 1;  // its index in the set
    };
    int palRainbow0 = -1;
    for (int i = 0; i < kRainbowCount; ++i) {
        const int idx = upload(makePal({{1, outline}, {2, hues[static_cast<std::size_t>(i)].first},
                                        {3, hues[static_cast<std::size_t>(i)].second}, {4, eyes}}));
        if (i == 0) palRainbow0 = idx;  // the rainbow palettes are contiguous from here
    }
    const int palMushA = upload(makePal({{5, {200, 64, 64}},  {6, {250, 248, 230}}, {7, {198, 168, 120}}}));  // red
    const int palMushB = upload(makePal({{5, {86, 158, 222}}, {6, {232, 242, 252}}, {7, {150, 172, 196}}}));  // blue (wave B)
    const int palMushP = upload(makePal({{5, {154, 72, 204}}, {6, {224, 184, 250}}, {7, {120, 92, 152}}}));   // poisoned purple
    const int palBlaster = upload(makePal({{8, {120, 210, 140}}, {9, {246, 246, 150}}}));   // green hull, yellow cockpit
    const int palBullet  = upload(makePal({{10, {255, 250, 170}}}));                        // pale bolt
    const int palSpider  = upload(makePal({{11, {186, 126, 86}}, {12, {206, 86, 168}}, {13, {255, 255, 255}}}));
    const int palScorpion = upload(makePal({{11, {200, 160, 60}}, {12, {232, 200, 92}}, {13, {40, 40, 40}}}));  // yellow
    const int palBurst    = upload(makePal({{1, {255, 250, 200}}, {2, {255, 170, 60}}}));  // bright core + orange rays

    // ── 3. Game state ────────────────────────────────────────────────────────────────────────────────
    std::vector<std::vector<int>> mush(static_cast<std::size_t>(kRows),
                                       std::vector<int>(static_cast<std::size_t>(kCols), 0));     // HP per cell
    std::vector<std::vector<bool>> poisoned(static_cast<std::size_t>(kRows),
                                            std::vector<bool>(static_cast<std::size_t>(kCols), false));
    std::vector<Centipede> cents;
    std::vector<Bullet>    bullets;
    std::vector<Burst>     bursts;
    int                    nextId = 1;  // monotonic per-spawn id for bullets / bursts (their key identity)
    Spider                 spider;
    Scorpion               scorpion;
    int                    scorpionTimer = 0;
    std::uint32_t rng = static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    int px = kCols / 2, py = kRows - 1;  // blaster cell
    int score = 0, lives = kLives, wave = 0, invuln = 0;
    int moveTimer = 0, fireTimer = 0, spiderTimer = 0;
    int frame = 0;

    auto seedMushrooms = [&] {
        for (auto& row : mush) std::fill(row.begin(), row.end(), 0);
        for (auto& row : poisoned) std::fill(row.begin(), row.end(), false);
        for (int r = kFieldTop + 1; r < kBlasterTop; ++r)
            for (int c = 0; c < kCols; ++c)
                if (nextRand(rng) % 100 < 14) mush[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = kMushMaxHp;
    };
    auto spawnCentipede = [&] {
        Centipede c; c.dx = 1; c.vy = 1; c.stepTimer = 0;
        for (int i = 0; i < kCentStartLen; ++i) c.segs.push_back(Cell{kCols / 2 - i, kFieldTop});
        cents.push_back(std::move(c));
    };
    auto startWave = [&] { ++wave; cents.clear(); spawnCentipede(); };
    auto newGame   = [&] { score = 0; lives = kLives; wave = 0; cents.clear(); bullets.clear(); bursts.clear();
                           spider.alive = false; scorpion.alive = false; seedMushrooms(); startWave();
                           invuln = 0; };
    newGame();

    const int kStepTicks = 7;  // base centipede step cadence; faster per wave below
    auto stepTicksForWave = [&] { return std::max(2, kStepTicks - wave / 2); };

    auto loseLife = [&] {
        cents.clear(); bullets.clear(); bursts.clear(); spider.alive = false; scorpion.alive = false;
        if (--lives <= 0) { std::printf("game over — score %d — new game\n", score); newGame(); return; }
        spawnCentipede();
        invuln = 90;
    };

    // ── 4. Simulation step (60 Hz) ───────────────────────────────────────────────────────────────────
    loop.simTick([&](const InputState& in) {
        ++frame;
        if (moveTimer > 0) --moveTimer;
        if (fireTimer > 0) --fireTimer;
        if (invuln > 0) --invuln;

        // 4a. Blaster: movement steps it a cell at a time within the bottom band; HOLD Fire to stream bolts.
        if (moveTimer == 0) {
            int nx = px, ny = py;
            if (in.isHeld(Action::Left))  --nx;
            if (in.isHeld(Action::Right)) ++nx;
            if (in.isHeld(Action::Up))    --ny;
            if (in.isHeld(Action::Down))  ++ny;
            nx = std::clamp(nx, 0, kCols - 1);
            ny = std::clamp(ny, kBlasterTop, kRows - 1);
            if (nx != px || ny != py) { px = nx; py = ny; moveTimer = kMoveEvery; }
        }
        if (in.isHeld(Action::Fire) && fireTimer == 0 && static_cast<int>(bullets.size()) < 2) {
            bullets.push_back(Bullet{px, static_cast<float>(py) - 1.0f, true, nextId++});
            fireTimer = kFireEvery;
        }

        // 4b. Bullets: travel up; the cell they reach is checked for a mushroom (HP--, destroy at 0) or a
        //     centipede segment (destroy → drop a mushroom + SPLIT the centipede into two).
        for (Bullet& b : bullets) {
            if (!b.alive) continue;
            b.row -= kBulletSpeed;
            const int r = static_cast<int>(b.row + 0.5f);
            if (r < kFieldTop) { b.alive = false; continue; }
            // mushroom?
            if (mush[static_cast<std::size_t>(r)][static_cast<std::size_t>(b.col)] > 0) {
                if (--mush[static_cast<std::size_t>(r)][static_cast<std::size_t>(b.col)] == 0)
                    poisoned[static_cast<std::size_t>(r)][static_cast<std::size_t>(b.col)] = false;
                score += 1; b.alive = false; continue;
            }
            // centipede segment?
            bool hit = false;
            for (std::size_t ci = 0; ci < cents.size() && !hit; ++ci) {
                Centipede& c = cents[ci];
                for (std::size_t i = 0; i < c.segs.size(); ++i) {
                    if (c.segs[i].cx == b.col && c.segs[i].cy == r) {
                        mush[static_cast<std::size_t>(r)][static_cast<std::size_t>(b.col)] = kMushMaxHp;  // → mushroom
                        bursts.push_back(Burst{b.col, r, 0, nextId++});  // hit spark at the destroyed segment
                        score += (i == 0) ? 10 : 5;
                        std::vector<Cell> front(c.segs.begin(), c.segs.begin() + static_cast<long>(i));
                        std::vector<Cell> rear(c.segs.begin() + static_cast<long>(i) + 1, c.segs.end());
                        const int dx = c.dx, vy = c.vy;
                        cents.erase(cents.begin() + static_cast<long>(ci));
                        if (!front.empty()) cents.push_back(Centipede{std::move(front), dx, vy, 0});
                        if (!rear.empty())  cents.push_back(Centipede{std::move(rear), dx, vy, 0});
                        b.alive = false; hit = true; break;
                    }
                }
            }
        }

        // 4c. Centipede: each centipede steps on its own cadence — the head advances horizontally, and on
        //     hitting a wall or a mushroom it drops one row and reverses (bouncing off the field top/bottom
        //     in vy). The body snake-follows. Reaching the blaster cell costs a life.
        const int step = stepTicksForWave();
        for (Centipede& c : cents) {
            if (c.segs.empty()) continue;
            if (++c.stepTimer < step) continue;
            c.stepTimer = 0;
            const Cell head = c.segs.front();
            int nx = head.cx + c.dx, ny = head.cy;
            const bool wall = nx < 0 || nx >= kCols;
            const bool blocked = wall || (!wall && mush[static_cast<std::size_t>(ny)][static_cast<std::size_t>(nx)] > 0);
            Cell nextHead;
            if (blocked) {
                int ty = head.cy + c.vy;
                if (ty < kFieldTop || ty >= kRows) { c.vy = -c.vy; ty = head.cy + c.vy; }
                nextHead = Cell{head.cx, std::clamp(ty, kFieldTop, kRows - 1)};
                c.dx = -c.dx;
            } else {
                nextHead = Cell{nx, ny};
            }
            // snake-follow: push the new head, drop the tail.
            c.segs.insert(c.segs.begin(), nextHead);
            c.segs.pop_back();
        }
        // collision with the blaster
        if (invuln == 0)
            for (const Centipede& c : cents)
                for (const Cell& s : c.segs)
                    if (s.cx == px && s.cy == py) { loseLife(); goto after_cent; }
        after_cent:;
        // wave clear → next (faster, recolored) wave
        if (cents.empty()) startWave();

        // 4d. Spider: spawns occasionally, zig-zags through the lower field eating mushrooms; shoot it for
        //     points, it costs a life on contact. (Another multi-color sprite + a moving target.)
        if (!spider.alive && --spiderTimer <= 0) {
            spider.alive = true; spider.life = 600;
            spider.x = (nextRand(rng) & 1) ? 0.0f : static_cast<float>(kViewW - kCell);
            spider.y = static_cast<float>((kBlasterTop) * kCell);
            spider.vx = (spider.x < 1.0f ? 1.0f : -1.0f) * 1.1f;
            spider.vy = 0.8f;
            spiderTimer = 480;
        }
        if (spider.alive) {
            spider.x += spider.vx; spider.y += spider.vy;
            if (spider.y < kBlasterTop * kCell || spider.y > (kRows - 1) * kCell) spider.vy = -spider.vy;
            const int scx = static_cast<int>(spider.x) / kCell, scy = static_cast<int>(spider.y) / kCell;
            if (scy >= 0 && scy < kRows && scx >= 0 && scx < kCols)
                mush[static_cast<std::size_t>(scy)][static_cast<std::size_t>(scx)] = 0;  // eats mushrooms
            if (spider.x < -kCell || spider.x > kViewW || --spider.life <= 0) spider.alive = false;
            if (invuln == 0 && scx == px && scy == py) { loseLife(); }
            // bullet vs spider
            for (Bullet& b : bullets)
                if (b.alive && spider.alive && b.col == scx &&
                    std::abs(b.row - scy) < 1.0f) { spider.alive = false; b.alive = false; score += 30;
                                                    bursts.push_back(Burst{scx, scy, 0, nextId++}); }
        }

        // 4e. Scorpion: periodically crosses a random field row, POISONING every mushroom it passes — a
        //     state-driven PALETTE SWAP (a poisoned mushroom selects the purple palette, same art). Shoot
        //     it for points.
        if (!scorpion.alive && --scorpionTimer <= 0) {
            scorpion.alive = true;
            scorpion.row = kFieldTop + 2 + static_cast<int>(nextRand(rng) % static_cast<unsigned>(kBlasterTop - kFieldTop - 3));
            const bool fromLeft = (nextRand(rng) & 1) != 0;
            scorpion.x = fromLeft ? -static_cast<float>(kCell) : static_cast<float>(kViewW);
            scorpion.vx = fromLeft ? 1.3f : -1.3f;
            scorpionTimer = 720;
        }
        if (scorpion.alive) {
            scorpion.x += scorpion.vx;
            const int scx = static_cast<int>(scorpion.x) / kCell;
            if (scx >= 0 && scx < kCols && mush[static_cast<std::size_t>(scorpion.row)][static_cast<std::size_t>(scx)] > 0)
                poisoned[static_cast<std::size_t>(scorpion.row)][static_cast<std::size_t>(scx)] = true;
            if (scorpion.x < -kCell || scorpion.x > kViewW) scorpion.alive = false;
            for (Bullet& b : bullets)
                if (b.alive && scorpion.alive && b.col == scx &&
                    std::abs(b.row - scorpion.row) < 1.0f) { scorpion.alive = false; b.alive = false; score += 50;
                                                             bursts.push_back(Burst{scx, scorpion.row, 0, nextId++}); }
        }

        // 4f. Age the hit bursts; drop the spent ones.
        for (Burst& b : bursts) ++b.age;
        bursts.erase(std::remove_if(bursts.begin(), bursts.end(),
                                    [](const Burst& b) { return b.age >= kBurstLife; }), bursts.end());
        bullets.erase(std::remove_if(bullets.begin(), bullets.end(),
                                     [](const Bullet& b) { return !b.alive; }), bullets.end());
    });

    std::vector<TileCell> bgCells(static_cast<std::size_t>(kCols) * kRows);
    std::vector<Sprite>   sprites;
    // Every sprite gets a STABLE identity key — a grid cell for a stationary mushroom / centipede segment,
    // a per-spawn id for a bullet / burst, a fixed name for a singleton — NEVER its emission index, which
    // shifts as the sprite population changes and would make the interpolator cross-fade unrelated sprites.
    // ObjectKey owns its bytes, so a key assembled per frame moves straight into the sprite.

    auto put = [&](std::string key, int cx, int cy, int tile, int pal, bool flipX = false) {
        sprites.push_back(Sprite{
            .key = std::move(key),
            .x = cx * kCell, .y = cy * kCell, .size = AssetDimensions{kCell, kCell},
            .atlas = spriteAtlas, .tile = static_cast<std::uint16_t>(tile),
            .palette = palSet[static_cast<std::size_t>(pal)], .flipX = flipX});
    };

    // ── 5. Render step ───────────────────────────────────────────────────────────────────────────────
    loop.renderLoop([&]() {
        // 5a. HUD backdrop + score (left) / lives (right).
        for (auto& c : bgCells) { c.tile = kTileSolid; c.atlas = bgAtlas; c.palette = voidPal; }
        auto putNum = [&](int v, int endCol) { int x = v, col = endCol;
            do { bgCells[static_cast<std::size_t>(col)].tile = static_cast<std::uint16_t>(kTileDigit0 + x % 10);
                 bgCells[static_cast<std::size_t>(col)].atlas = bgAtlas;
                 bgCells[static_cast<std::size_t>(col)].palette = hudPal; x /= 10; --col;
            } while (x > 0 && col >= 0); };
        putNum(score, 6);
        putNum(lives, kCols - 2);

        // 5b. Sprites: mushrooms (HP → art state; poisoned → palette swap; otherwise the wave's mushroom
        //     palette), the centipede(s) with the RAINBOW palette marching along + cycling over time
        //     (head tile + flipX by direction), the bullets, the blaster, and the spider.
        sprites.clear();
        // Every sprite below carries a STABLE identity key (see the stable-key note above): a grid cell for a
        // stationary mushroom or centipede segment (they move by mounting a new head / unmounting the tail,
        // never by shifting an existing cell), a per-spawn id for a bullet or burst, a fixed name for a
        // singleton. Emission order no longer decides identity.
        const int mushPal = (wave % 2 == 0) ? palMushA : palMushB;  // per-WAVE palette swap (recolor)
        for (int r = kFieldTop; r < kRows; ++r)
            for (int c = 0; c < kCols; ++c) {
                const int hp = mush[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
                if (hp <= 0) continue;
                const int tile = hp >= 3 ? kTileMushFull : (hp == 2 ? kTileMushMid : kTileMushLow);
                const int pal  = poisoned[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] ? palMushP : mushPal;
                put("mush_" + std::to_string(r) + "_" + std::to_string(c), c, r, tile, pal);
            }
        for (const Centipede& cp : cents) {
            for (std::size_t i = 0; i < cp.segs.size(); ++i) {
                // PALETTE CYCLING: the hue marches along the body (+i) and animates over time (+frame),
                // selected per segment out of the 6 contiguous rainbow palettes.
                const int hue = palRainbow0 + ((static_cast<int>(i) + frame / kRainbowRate) % kRainbowCount);
                const Cell& seg = cp.segs[i];
                put("seg_" + std::to_string(seg.cx) + "_" + std::to_string(seg.cy),
                    seg.cx, seg.cy, i == 0 ? kTileHead : kTileBody, hue, cp.dx < 0);
            }
        }
        for (const Bullet& b : bullets)
            put("bullet_" + std::to_string(b.id), b.col, static_cast<int>(b.row + 0.5f), kTileBullet, palBullet);
        if (spider.alive) put("spider", static_cast<int>(spider.x) / kCell, static_cast<int>(spider.y) / kCell,
                              kTileSpider, palSpider);
        if (scorpion.alive) put("scorpion", static_cast<int>(scorpion.x) / kCell, scorpion.row, kTileScorpion,
                                palScorpion, scorpion.vx < 0);
        // hit bursts: the spark pops OUTWARD over its short life (scaled about its center via Transform).
        for (const Burst& bu : bursts) {
            const float s = 0.6f + 1.3f * (static_cast<float>(bu.age) / kBurstLife);
            sprites.push_back(Sprite{
                .key = "burst_" + std::to_string(bu.id),
                .x = bu.cx * kCell, .y = bu.cy * kCell, .size = AssetDimensions{kCell, kCell},
                .atlas = spriteAtlas, .tile = static_cast<std::uint16_t>(kTileBurst),
                .palette = palSet[static_cast<std::size_t>(palBurst)],
                .transform = Transform::scale(s, s, kCell / 2.0f, kCell / 2.0f)});
        }
        // blaster (blink while invulnerable so it's clearly in respawn grace — slow, not a strobe)
        if (invuln == 0 || (frame / 8) % 2 == 0) put("blaster", px, py, kTileBlaster, palBlaster);

        FrameDrawState frame_;
        DrawLayer bg{.key = "hud"}; bg.z = 0; bg.size = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles = kCols, .heightInTiles = kRows,
                                 .cells = std::span<const TileCell>(bgCells)};
        frame_.layers.push_back(bg);

        DrawLayer sp{.key = "sprites"}; sp.z = 10; sp.size = PixelSize{kViewW, kViewH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame_.layers.push_back(sp);

        renderer.renderFrame(frame_);
    });

    std::printf("Centipede (NES, 60 Hz) — arrows/WASD/d-pad move the blaster (bottom band), HOLD X (pad A) "
                "to fire. Shoot the centipede (it splits) and the mushrooms; watch the rainbow march along "
                "its body. Close to quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
