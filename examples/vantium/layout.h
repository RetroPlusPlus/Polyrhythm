#pragma once

// Vantium — shared layout / tuning constants and the small value types every TU needs. Pure data:
// no engine types, so the deck builder, the simulation, the wave system, and the draw step all
// share one definition of the world without pulling each other in.
//
// THE WORLD (640×480 viewport over a 2560px-wide scrolling world):
//   • HUD strip   y   0.. 43 — score / ship / waves / lives in the rich 16×16 font, over a bar.
//   • Space       everywhere — the parallax starfield layer (half the camera's scroll rate).
//   • The deck    y  96..383 — the dreadnought: an 18-row × 160-col band of 16×16 cells assembled
//                              from authored 20-col section templates (bow | mids ×5 | strip |
//                              stern). Raised superstructure is LETHAL; corridors between blocks
//                              are one cell tall — a rolled Manta threads them, a level one dies.
//   • The Manta flies anywhere; the camera follows with a lookahead toward its facing.

#include <cstdint>

namespace vant {

// ── Viewport / world ───────────────────────────────────────────────────────────────────────────
constexpr int kTile   = 8;                   // the engine's 8px tile
constexpr int kViewW  = 640, kViewH = 480;
constexpr int kMapW   = kViewW / kTile;      // 80 — the HUD layer's tile grid
constexpr int kMapH   = kViewH / kTile;      // 60
constexpr int kWorldW = 2560;                // 4 screens of dreadnought

// ── Deck geometry (16px cells; every art cell stamps as a 2×2 group of engine tiles) ──────────
constexpr int kCell       = 16;
constexpr int kDeckCols   = kWorldW / kCell;         // 160
constexpr int kDeckRows   = 18;
constexpr int kDeckTop    = 96;                       // deck band y-origin, world px
constexpr int kDeckBottom = kDeckTop + kDeckRows * kCell;  // 384
constexpr int kSectionCols = 20;                      // one authored section = 20 cols × 18 rows
constexpr int kSections    = kDeckCols / kSectionCols;  // 8: bow | mid ×5 | strip | stern
constexpr int kMidSlots    = 5;

[[nodiscard]] constexpr float deckPxX(int c) { return static_cast<float>(c * kCell); }
[[nodiscard]] constexpr float deckPxY(int r) { return static_cast<float>(kDeckTop + r * kCell); }

// ── The Manta ──────────────────────────────────────────────────────────────────────────────────
constexpr float kMantaW = 48.0f, kMantaH = 24.0f;
constexpr float kAccel = 0.22f, kDrag = 0.95f;
constexpr float kVMaxX = 6.0f, kVMaxY = 3.5f;
constexpr float kShipMinY = 16.0f, kShipMaxY = 440.0f;
constexpr int   kTurnTicks = 12;              // side-on frame while reversing facing
constexpr float kHitInsetX = 2.0f;            // hitbox 44×20 inside the 48×24 art…
constexpr float kHitLevelY = 2.0f, kHitLevelH = 20.0f;
constexpr float kHitRollY  = 8.0f, kHitRollH  = 8.0f;   // …and 44×8 while rolled
constexpr int   kCamLookahead = 96;           // px ahead of the facing
constexpr float kCamEase      = 0.08f;        // camera approach per tick
constexpr int   kInvulnTicks  = 120;          // 2 s after a respawn
constexpr int   kLives        = 3;

// ── Weapons / enemies ─────────────────────────────────────────────────────────────────────────
constexpr int   kMaxShots   = 4;
constexpr float kShotSpeed  = 12.0f;
constexpr int   kWaveSize     = 5;
constexpr float kWaveSpacing  = 26.0f;        // arc-length gap between squadron members
constexpr float kWaveSpeedBase = 4.2f, kWaveSpeedGrow = 0.15f;  // +grow per ship, capped
constexpr int   kWaveSpeedCapShip = 6;
constexpr float kEShotSpeed = 3.0f;
constexpr int   kMaxMines   = 2;
constexpr float kMineSpeed  = 1.1f;
constexpr int   kQuotaBase  = 5;              // waves to clear on ship 1 (+1 per ship, cap 9)
constexpr int   kQuotaCap   = 9;
constexpr int   kWaveLull   = 90;             // ticks between waves

// ── Landing / progression ─────────────────────────────────────────────────────────────────────
constexpr float kLandMaxVx = 1.2f, kLandMaxVy = 1.0f;
constexpr int   kLandingTicks  = 60;
constexpr int   kDestructTicks = 150;

// ── Scoring ────────────────────────────────────────────────────────────────────────────────────
constexpr int kScoreFighter = 100;
constexpr int kScoreMine    = 150;
constexpr int kScorePod     = 50;
constexpr int kScoreWave    = 500;
constexpr int kScoreLanding = 1000;

// ── Deck map-PNG vocabulary ────────────────────────────────────────────────────────────────────
// The deck sections are authored as 20×18 16-BIT GRAYSCALE MAP PNGS (one pixel per 16px cell),
// loaded through the engine's map-import pipeline (loadMapPng → IndexGrid). Ids are spread so a
// HUMAN can read the maps in an image editor: Void is pure black (0), and every other class
// climbs from 25% gray in even bands to pure white — id = 16383 + (class − 1) × 3072, topping
// out at exactly 65535. The generator (gen_vantium_assets.py) shares the class ORDER.
enum class MapGlyph : std::uint16_t {
    Void = 0,     // hull hole — the starfield shows through
    PlateA, PlateB, PlateC,
    HazardH,      // deck-edge trim (flipY on the bottom half at stamp time)
    HazardV,      // the same art, Rot90
    Corner,       // trim, auto-oriented by quadrant
    Struct,       // TALL superstructure — lethal
    PipeH, PipeV,
    PodTop, PodBot,
    MineSpawn,
    Chevron,      // landing strip
    RotorAnchor, RotorFill,   // the 64×64 set-piece (4×4 cells, lethal)
    VentAnchor, VentFill,     // the 32×32 set-piece (2×2 cells, lethal)
};
constexpr std::uint16_t kMapFirstId    = 16383;  // class 1 — 25% gray, clearly off-black
constexpr std::uint16_t kMapIdStep     = 3072;   // 16383 + 16 × 3072 = 65535 exactly
constexpr int           kMapGlyphCount = 18;

// ── Tile-strip art cells (matches gen_vantium_assets.py; each = a 2×2 engine-tile stamp) ──────
enum TileArt : std::uint8_t {
    T_PLATE_A = 0, T_PLATE_B, T_PLATE_C, T_HAZARD, T_CORNER, T_STRUCT, T_PIPE,
    T_POD_TOP, T_POD_BOT, T_POD_TOP_X, T_POD_BOT_X, T_CHEVRON, T_STAR_A, T_STAR_B, T_EMPTY,
    T_ROTOR0 = 15,   // …15 more rotor cells follow, row-major 4×4
    T_VENT0  = 31,   // …3 more vent cells, 2×2
};
constexpr int kTileStride8 = 70;   // the tiles sheet is 560px wide → 70 8px-tile columns

// ── Sprite-sheet slots (uniform 48×24 SpriteSeries cells) ─────────────────────────────────────
enum Slot : std::uint8_t {
    S_MANTA_LEVEL = 0, S_MANTA_BANK1, S_MANTA_BANK2, S_MANTA_SIDE,
    S_FIGHTER, S_MINE, S_BOLT, S_ESHOT, S_BOOM0, S_BOOM1, S_BOOM2, S_BOOM3,
};

// Sprite-layer palette indices (the order assets.cpp uploads them). Fighter waves rotate their
// livery through the four fighter palettes — one shape, four liveries.
enum Pal : std::uint8_t {
    PAL_MANTA = 0,
    PAL_FIGHTER_0, PAL_FIGHTER_1, PAL_FIGHTER_2, PAL_FIGHTER_3,
    PAL_MINE, PAL_BOLT, PAL_ESHOT, PAL_BOOM,
};
constexpr int kFighterLiveries = 4;

// Tile-layer palette indices. DECK_SHADOW is the free richness trick: the same plate art under a
// darker ramp, stamped wherever a plate sits directly beneath raised superstructure.
enum TilePal : std::uint8_t {
    TP_DECK = 0, TP_DECK_SHADOW, TP_HAZARD, TP_STRUCT, TP_ROTOR, TP_VENT,
    TP_POD_A, TP_POD_B,   // the fuel pod's two glow phases (palette-cycled)
    TP_STRIP,
    TP_STAR_A, TP_STAR_B, // the starfield's two twinkle phases (palette-cycled)
};

// Rich-font palettes (0 = the HUD bar, 1 = outline, 2 = shadow, 4..7 = the gradient).
enum TextPal : std::uint8_t { TXT_WHITE = 0, TXT_GOLD, TXT_CYAN };
constexpr int kFontStride8 = 76;   // the font sheet is 608px wide → 76 8px-tile columns

// ── HUD text grid (16px glyph columns/rows over the 80×60 tile layer) ─────────────────────────
constexpr int kHudTextRow8  = 1;   // glyphs stamp at tile rows 1–2 (y 8..40)
constexpr int kHudRuleRow8  = 4;   // the border rule at tile rows 4–5

// ── App / flight states ───────────────────────────────────────────────────────────────────────
enum class GameState : std::uint8_t { Title, Playing };
enum class FlightPhase : std::uint8_t { Flying, Landing, Destruct };

// ── The event stream (the sim names what happened; audio + feel consume) ─────────────────────
enum class GameEventKind : std::uint8_t {
    Fire, EnemyDown, MineDown, PodHit, WaveBonus, PlayerDeath, LandNow, Touchdown, ShipDestroyed,
};
struct GameEvent {
    GameEventKind kind;
    float         x      = 0.0f;   // world px, where it happened
    float         y      = 0.0f;
    int           points = 0;
};

struct VCell {
    int c = 0, r = 0;
};

}  // namespace vant
