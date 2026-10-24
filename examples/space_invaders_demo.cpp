// ============================================================================================
//  Space Invaders — a sprite + palette demo on Polyrhythm, using the ASSET-LOAD route. Where Centipede
//  built its indexed atlas from in-code byte grids (uploadAtlas), THIS demo loads a committed indexed
//  PNG sprite sheet via Renderer::loadAtlas(...) and slices it into addressable sprite slots.
//  Both routes are indexed (pixels are palette INDICES; color comes from palettes) — only the atlas
//  SOURCE differs. The art is authored by examples/assets/gen_space_invaders.py → space_invaders.png.
// ============================================================================================
//
//  WHAT THIS DEMONSTRATES (the focus is the asset-load route + color):
//    • Renderer::loadAtlas("space_invaders.png", AssetDimensions{8,8}, ContentKind::SpriteSeries) — load
//      a committed INDEXED PNG and slice it into a grid of 8×8 cells. The returned AtlasManifest gives
//      `atlas` + `slots[i]`; `slots[i].tile` is the i-th sprite's atlas cell — exactly Sprite::tile.
//      (Contrast Centipede's embedded-bytes route: same indexed-palette model, different atlas source.)
//    • Palette SELECTION over the loaded atlas: every sprite picks its palette out of one set; the five
//      alien rows are five colors, the cannon/bomb/UFO/bunker each their own — "nice colors" on the
//      colorless arcade original, with NO art change (only the chosen palette).
//    • EngineConfig viewport + timing (ViewportResolution::Snes 256×224 + TimingProfile{TickPeriodNs::Hz60}).
//
//  THE GAME:
//    • A 5×11 formation of invaders (squid / crab / octopus, two march frames each) sweeps left-right,
//      dropping + reversing at the edges and speeding up as you thin them. Left/Right move your cannon;
//      Fire shoots (one bolt on screen, like the arcade). Invaders drop bombs; four destructible bunkers
//      shield you; a mystery UFO crosses the top for bonus. Lose a life to a bomb or to the invaders
//      reaching you; clear the wave for a faster one. 3 lives.
//
// The demo never auto-launches a window; you run it.
//
//  CI: like the other example hosts it instantiates SdlPlatform + Renderer for real (so the live GPU +
//  image-load path keeps compiling on every CI platform) — but CI never opens the window.
// ============================================================================================

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "retropp/clock.h"          // SteadyClock
#include "retropp/draw_state.h"     // FrameDrawState / DrawLayer / TileContent / SpriteContent / Sprite
#include "retropp/engine_config.h"  // EngineConfig
#include "retropp/image.h"          // ContentKind, AssetDimensions, AtlasManifest (the slicer surface)
#include "retropp/input.h"          // InputState (isHeld / justPressed)
#include "retropp/input_actions.h"  // ActionMap + PadButton — the demo's bindings
#include "retropp/palette.h"        // Rgba8 / PaletteId
#include "retropp/renderer.h"       // Renderer — loadAtlas + uploadPalette + renderFrame
#include "retropp/run_loop.h"       // RunLoop — simTick / renderLoop
#include "retropp/sdl_platform.h"   // SdlPlatform
#include "retropp/timing.h"         // TimingProfile, TickPeriodNs (Hz60)
#include "retropp/viewport.h"       // ViewportResolution — the Snes preset
#include "retropp/windowed_host.h"  // WindowedHost — pumps OS events + drives the loop

namespace {

using namespace retropp;
using namespace std::chrono_literals;

constexpr int kCell = 8;

// The demo's input vocabulary: sideways cannon movement + fire.
enum class Action : std::uint8_t { Left, Right, Fire };

// The sprite-sheet cell order (matches gen_space_invaders.py's layout → the manifest's slot order).
enum Spr {
    SQUID_A = 0, SQUID_B, CRAB_A, CRAB_B, OCTO_A, OCTO_B,
    CANNON, BULLET, BOMB, UFO, EXPLO, BUNKER, BUNKER_X
};

// Formation.
constexpr int   kRowsF = 5, kColsF = 11;
constexpr float kSpacingX = 16.0f, kSpacingY = 14.0f;
constexpr float kStartX = 16.0f, kStartY = 26.0f;
constexpr float kStepX = 4.0f, kDrop = 8.0f;
constexpr float kMargin = 8.0f;

constexpr float kCannonSpeed = 2.4f;
constexpr float kBulletSpeed = 4.0f;
constexpr float kBombSpeed   = 1.7f;
constexpr float kUfoSpeed    = 1.2f;
constexpr int   kLives       = 3;
constexpr int   kMaxBombs    = 3;

// ── HUD digit font (5×7) + its tile atlas ───────────────────────────────────────────────────────────
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

std::array<Rgba8, 16> makePal(std::initializer_list<std::pair<int, Rgba8>> entries) {
    std::array<Rgba8, 16> p{};
    for (const auto& [i, c] : entries) p[static_cast<std::size_t>(i)] = c;
    return p;
}

std::uint32_t nextRand(std::uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

// Each transient object (bomb, explosion) carries a per-spawn `id` so its reconciliation key is stable
// for its whole lifetime and unique to that instance — the interpolator matches an object to its own
// previous-tick state by key, so a recycled or index-derived key would cross-fade unrelated objects.
struct Bomb  { float x, y; bool alive = true; int id = 0; };
struct Explo { float x, y; int age; int id = 0; };
struct Block { int cx, cy, hp; };

}  // namespace

int main() {

    // ── 1. Config — SNES at 60 Hz ───────────────────────────────────────────────────────────────────
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Space Invaders Demo"},
        .window   = {.title = "Polyrhythm — Space Invaders (SNES, 60 Hz)"},
        .viewport = ViewportResolution::Snes,            // 256×224
        .timing   = TimingProfile{TickPeriodNs::Hz60}};
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::Fire,  {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::Left,  {SDL_SCANCODE_LEFT, SDL_SCANCODE_A, PadButton::DpadLeft}},
        {Action::Right, {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D, PadButton::DpadRight}},
    };
    platform.actions(map);

    const int kViewW = config.viewport.width, kViewH = config.viewport.height;  // 256, 224
    const int kCols = kViewW / kCell, kRows = kViewH / kCell;

    // ── 2. THE ASSET-LOAD ROUTE: load the indexed PNG sheet + slice it into sprite slots ────────────
    AtlasManifest sheet;
    try {
        // SpriteSeries + AssetDimensions{8,8}: carve the 104×8 sheet into 13 8×8 cells, left-to-right.
        // sheet.atlas is the uploaded indexed atlas; sheet[Spr].tile is each sprite's atlas cell.
        sheet = renderer.loadAtlas("assets/space_invaders.png", AssetDimensions{kCell, kCell},
                                   ContentKind::SpriteSeries, ReadOrder::LeftRightThenDown,
                                   /*count=*/0, TransparentIndices::GameBoy);  // index 0 is the OBJ hole
    } catch (const std::exception& e) {
        std::printf("space_invaders: could not load space_invaders.png: %s\n", e.what());
        return 1;
    }
    const AtlasId spriteAtlas = sheet.atlasId;
    auto tileOf = [&](Spr s) { return sheet[static_cast<std::size_t>(s)].tile; };  // manifest slot → tile

    // ── 3. HUD backdrop ──────────────────────────────────────────────────────────────────────────────
    const std::vector<std::uint8_t> bgPx = buildBgAtlas();
    const AtlasId bgAtlas = renderer.uploadAtlas(bgPx.data(), kCell * kBgTiles, kCell).atlasId;
    const std::array<Rgba8, 2> palVoid{{ {6, 8, 16}, {6, 8, 16} }};
    const std::array<Rgba8, 2> palHud{{ {6, 8, 16}, {230, 230, 245} }};
    const PaletteId voidPal = renderer.uploadPalette(std::span<const Rgba8>(palVoid));
    const PaletteId hudPal  = renderer.uploadPalette(std::span<const Rgba8>(palHud));

    // ── 4. Palettes — color the loaded INDICES (index 1 = main, 2 = accent). The five alien rows get
    //       five colors; the same loaded art recolors purely by which palette a sprite selects. ─────
    std::vector<PaletteId> palSet;
    auto up = [&](const std::array<Rgba8, 16>& p) {
        palSet.push_back(renderer.uploadPalette(std::span<const Rgba8>(p)));
        return static_cast<int>(palSet.size()) - 1;
    };
    const std::array<Rgba8, kRowsF> rowColours{{ {120, 230, 235}, {235, 96, 86}, {236, 162, 64},
                                                 {118, 220, 126}, {220, 116, 220} }};
    std::array<int, kRowsF> palRow{};
    for (int r = 0; r < kRowsF; ++r) palRow[static_cast<std::size_t>(r)] = up(makePal({{1, rowColours[static_cast<std::size_t>(r)]}}));
    const int palCannon = up(makePal({{1, {110, 220, 130}}, {2, {206, 255, 214}}}));
    const int palBullet = up(makePal({{1, {245, 245, 245}}}));
    const int palBomb   = up(makePal({{1, {255, 168, 72}}}));
    const int palUfo    = up(makePal({{1, {236, 84, 84}}, {2, {245, 245, 255}}}));
    const int palExplo  = up(makePal({{1, {255, 232, 124}}}));
    const int palBunker = up(makePal({{1, {92, 210, 122}}}));

    // ── 5. Game state ────────────────────────────────────────────────────────────────────────────────
    std::array<std::array<bool, kColsF>, kRowsF> alien{};
    float fx = kStartX, fy = kStartY;
    int   dir = 1, animFrame = 0, stepTimer = 0;
    float cannonX = kViewW / 2.0f - kCell / 2.0f;
    const float cannonY = static_cast<float>(kViewH - 20);
    bool  bulletAlive = false; float bulletX = 0, bulletY = 0;
    std::vector<Bomb>  bombs;
    std::vector<Explo> explos;
    std::vector<Block> bunkers;
    bool  ufoAlive = false; float ufoX = 0; int ufoDir = 1;
    int   nextSpawnId = 1;   // monotonic per-spawn id for bombs / explosions (their stable key identity)
    int   score = 0, lives = kLives, wave = 0, bombTimer = 0, ufoTimer = 0, invuln = 0;
    std::uint32_t rng = static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    auto alienX = [&](int c) { return fx + c * kSpacingX; };
    auto alienY = [&](int r) { return fy + r * kSpacingY; };
    auto aliveCount = [&] { int n = 0; for (auto& row : alien) for (bool a : row) n += a; return n; };
    auto rowType = [&](int r) { return r == 0 ? SQUID_A : (r <= 2 ? CRAB_A : OCTO_A); };  // base frame A
    auto rowScore = [&](int r) { return r == 0 ? 30 : (r <= 2 ? 20 : 10); };

    auto buildBunkers = [&] {
        bunkers.clear();
        const int row = kRows - 7;  // a few cells above the cannon
        for (int b = 0; b < 4; ++b) {
            const int x0 = 3 + b * 7;  // cell columns; 4 bunkers spread across 32
            for (int cy = 0; cy < 4; ++cy)
                for (int cx = 0; cx < 5; ++cx) {
                    if (cy == 3 && cx >= 1 && cx <= 3) continue;  // doorway notch
                    if (cy == 0 && (cx == 0 || cx == 4)) continue;  // rounded top corners
                    bunkers.push_back(Block{x0 + cx, row + cy, 2});
                }
        }
    };
    auto startWave = [&] {
        ++wave;
        for (auto& row : alien) row.fill(true);
        fx = kStartX; fy = kStartY + std::min(wave - 1, 4) * 6.0f;  // each wave starts a little lower
        dir = 1; animFrame = 0; stepTimer = 0;
        bombs.clear(); bulletAlive = false;
    };
    auto newGame = [&] { score = 0; lives = kLives; wave = 0; explos.clear(); ufoAlive = false;
                         buildBunkers(); startWave(); invuln = 0; };
    newGame();

    auto loseLife = [&] {
        bombs.clear(); bulletAlive = false; ufoAlive = false;
        if (--lives <= 0) { std::printf("game over — score %d — new game\n", score); newGame(); return; }
        invuln = 90;
    };
    auto blockAt = [&](int cx, int cy) -> Block* {
        for (Block& b : bunkers) if (b.hp > 0 && b.cx == cx && b.cy == cy) return &b;
        return nullptr;
    };

    // ── 6. Simulation step (60 Hz) ───────────────────────────────────────────────────────────────────
    loop.simTick([&](const InputState& in) {
        if (invuln > 0) --invuln;

        // 6a. Cannon + fire (one bolt on screen, arcade-style: fire only when none is in flight).
        if (in.isHeld(Action::Left))  cannonX -= kCannonSpeed;
        if (in.isHeld(Action::Right)) cannonX += kCannonSpeed;
        cannonX = std::clamp(cannonX, 0.0f, static_cast<float>(kViewW - kCell));
        if (in.isHeld(Action::Fire) && !bulletAlive) {
            bulletAlive = true; bulletX = cannonX + kCell / 2.0f - 1.0f; bulletY = cannonY - kCell;
        }

        // 6b. Bullet: up; hit an invader (AABB) / the UFO / a bunker block.
        if (bulletAlive) {
            bulletY -= kBulletSpeed;
            const int bcx = static_cast<int>(bulletX) / kCell, bcy = static_cast<int>(bulletY) / kCell;
            if (bulletY < kCell) bulletAlive = false;
            else if (Block* blk = blockAt(bcx, bcy)) { --blk->hp; bulletAlive = false; }
            else if (ufoAlive && bulletY < 2 * kCell && std::abs(bulletX - (ufoX + kCell / 2.0f)) < kCell) {
                ufoAlive = false; score += 100; explos.push_back(Explo{ufoX, 1.0f * kCell, 0, nextSpawnId++}); bulletAlive = false;
            } else {
                for (int r = 0; r < kRowsF && bulletAlive; ++r)
                    for (int c = 0; c < kColsF; ++c)
                        if (alien[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)]) {
                            const float ax = alienX(c), ay = alienY(r);
                            if (bulletX >= ax && bulletX < ax + kCell && bulletY >= ay && bulletY < ay + kCell) {
                                alien[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = false;
                                explos.push_back(Explo{ax, ay, 0, nextSpawnId++}); score += rowScore(r);
                                bulletAlive = false; break;
                            }
                        }
            }
        }

        // 6c. Formation march: every step (faster as fewer remain), shift; at an edge drop + reverse;
        //     toggle the animation frame. Reaching the cannon row ends a life.
        const int n = std::max(1, aliveCount());
        const int step = std::clamp(n / 4 + 2, 2, 18);  // 55 invaders ≈ 15 ticks/step → 1 ≈ 2 (acceleration)
        if (++stepTimer >= step) {
            stepTimer = 0;
            animFrame ^= 1;
            int minC = kColsF, maxC = -1, maxR = -1;
            for (int r = 0; r < kRowsF; ++r)
                for (int c = 0; c < kColsF; ++c)
                    if (alien[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)]) {
                        minC = std::min(minC, c); maxC = std::max(maxC, c); maxR = std::max(maxR, r);
                    }
            const float leftPx = alienX(minC), rightPx = alienX(maxC) + kCell;
            if ((dir > 0 && rightPx + kStepX > kViewW - kMargin) ||
                (dir < 0 && leftPx - kStepX < kMargin)) {
                fy += kDrop; dir = -dir;
            } else {
                fx += dir * kStepX;
            }
            if (maxR >= 0 && alienY(maxR) + kCell >= cannonY) loseLife();  // invaders reached you
        }

        // 6d. Invader bombs: the lowest invader in a random column drops one now and then.
        if (--bombTimer <= 0 && static_cast<int>(bombs.size()) < kMaxBombs && aliveCount() > 0) {
            const int c = static_cast<int>(nextRand(rng) % kColsF);
            for (int r = kRowsF - 1; r >= 0; --r)
                if (alien[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)]) {
                    bombs.push_back(Bomb{alienX(c) + kCell / 2.0f, alienY(r) + kCell, true, nextSpawnId++});
                    break;
                }
            bombTimer = 40 + static_cast<int>(nextRand(rng) % 50);
        }
        for (Bomb& b : bombs) {
            if (!b.alive) continue;
            b.y += kBombSpeed;
            const int bcx = static_cast<int>(b.x) / kCell, bcy = static_cast<int>(b.y) / kCell;
            if (b.y > kViewH) b.alive = false;
            else if (Block* blk = blockAt(bcx, bcy)) { --blk->hp; b.alive = false; }
            else if (invuln == 0 && b.y >= cannonY && b.y < cannonY + kCell &&
                     std::abs(b.x - (cannonX + kCell / 2.0f)) < kCell) { b.alive = false; loseLife(); }
        }

        // 6e. UFO: crosses the top now and then; bonus when shot.
        if (!ufoAlive && --ufoTimer <= 0) {
            ufoAlive = true; ufoDir = (nextRand(rng) & 1) ? 1 : -1;
            ufoX = ufoDir > 0 ? -static_cast<float>(kCell) : static_cast<float>(kViewW);
            ufoTimer = 600;
        }
        if (ufoAlive) { ufoX += ufoDir * kUfoSpeed; if (ufoX < -kCell || ufoX > kViewW) ufoAlive = false; }

        // 6f. Age explosions; reap dead bombs.
        for (Explo& e : explos) ++e.age;
        explos.erase(std::remove_if(explos.begin(), explos.end(),
                                    [](const Explo& e) { return e.age >= 12; }), explos.end());
        bombs.erase(std::remove_if(bombs.begin(), bombs.end(),
                                   [](const Bomb& b) { return !b.alive; }), bombs.end());
        bunkers.erase(std::remove_if(bunkers.begin(), bunkers.end(),
                                     [](const Block& b) { return b.hp <= 0; }), bunkers.end());

        // 6g. Wave cleared → next (faster, lower) wave.
        if (aliveCount() == 0) startWave();
    });

    std::vector<TileCell> bgCells(static_cast<std::size_t>(kCols) * kRows);
    std::vector<Sprite>   sprites;
    // Every sprite gets a STABLE identity key — a formation cell, a bunker cell, a per-spawn id, or a
    // singleton name — NEVER its emission index: an index-derived key shifts as the sprite population
    // changes, so the interpolator would match a sprite to a DIFFERENT object's previous-tick state and
    // ease it from that stale position (a one-tick flash at the wrong place). ObjectKey owns its bytes, so
    // a key assembled per frame moves straight into the sprite.
    // Each sprite names its sheet + palette directly; `pal` indexes the uploaded palette handles in palSet.
    auto put = [&](std::string key, float x, float y, Spr s, int pal) {
        sprites.push_back(Sprite{
            .key = std::move(key),
            .x = static_cast<int>(x), .y = static_cast<int>(y), .size = AssetDimensions{kCell, kCell},
            .atlas = spriteAtlas, .tile = tileOf(s), .palette = palSet[static_cast<std::size_t>(pal)]});
    };

    // ── 7. Render ────────────────────────────────────────────────────────────────────────────────────
    loop.renderLoop([&]() {
        // Each HUD cell names the HUD sheet + its palette directly (void backdrop vs lit glyph).
        for (auto& c : bgCells) { c.tile = kTileSolid; c.atlas = bgAtlas; c.palette = voidPal; }
        auto putNum = [&](int v, int endCol) { int x = v, col = endCol;
            do { bgCells[static_cast<std::size_t>(col)].tile = static_cast<std::uint16_t>(kTileDigit0 + x % 10);
                 bgCells[static_cast<std::size_t>(col)].palette = hudPal; x /= 10; --col;
            } while (x > 0 && col >= 0); };
        putNum(score, 6);
        putNum(lives, kCols - 2);

        sprites.clear();
        // Every sprite below carries a STABLE identity key (see the stable-key note above): a formation
        // cell for an invader, a grid cell for a bunker, a per-spawn id for a bomb / explosion, and a
        // fixed name for each singleton. Emission ORDER no longer determines identity, so a changing
        // population never shifts a sprite onto another object's history.
        // invaders — tile by row type + the current march frame; palette by ROW (five colors).
        for (int r = 0; r < kRowsF; ++r)
            for (int c = 0; c < kColsF; ++c)
                if (alien[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)])
                    put("inv_" + std::to_string(r) + "_" + std::to_string(c),
                        alienX(c), alienY(r), static_cast<Spr>(rowType(r) + animFrame),
                        palRow[static_cast<std::size_t>(r)]);
        // bunkers — full block, cracked at hp 1; keyed by their fixed grid cell.
        for (const Block& b : bunkers)
            put("bunk_" + std::to_string(b.cx) + "_" + std::to_string(b.cy),
                static_cast<float>(b.cx * kCell), static_cast<float>(b.cy * kCell),
                b.hp >= 2 ? BUNKER : BUNKER_X, palBunker);
        if (ufoAlive) put("ufo", ufoX, static_cast<float>(kCell), UFO, palUfo);
        for (const Bomb& b : bombs) put("bomb_" + std::to_string(b.id), b.x - kCell / 2.0f, b.y - kCell, BOMB, palBomb);
        if (bulletAlive) put("bullet", bulletX - kCell / 2.0f, bulletY, BULLET, palBullet);
        for (const Explo& e : explos) put("explo_" + std::to_string(e.id), e.x, e.y, EXPLO, palExplo);
        // The cannon is always "cannon" — a fixed identity, regardless of how many other sprites exist.
        if (invuln == 0 || (invuln / 8) % 2 == 0) put("cannon", cannonX, cannonY, CANNON, palCannon);  // blink in grace

        FrameDrawState frame;
        DrawLayer bg{.key = "hud"}; bg.z = 0; bg.size = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles = kCols, .heightInTiles = kRows,
                                 .cells = std::span<const TileCell>(bgCells)};
        frame.layers.push_back(bg);
        DrawLayer sp{.key = "sprites"}; sp.z = 10; sp.size = PixelSize{kViewW, kViewH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame.layers.push_back(sp);
        renderer.renderFrame(frame);
    });

    std::printf("Space Invaders (SNES, 60 Hz) — arrows / A·D / d-pad move the cannon, X (pad south) "
                "fires (one bolt at a time). Clear the formation; mind the bombs and the UFO. "
                "Close to quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
