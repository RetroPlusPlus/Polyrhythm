#pragma once

// Ferryman — shared layout / tuning constants and the small enums every TU needs. Pure data: no
// engine types here, so the loader (assets), the simulation (game), the rival (abductor), and the
// draw step (render) all share one definition of the playfield without pulling each other in.
//
// THE SCREEN (640×480; the FIELD below the HUD is OPEN SEA — free movement, no grid, no wrap):
//   • HUD band      y   0.. 63 — two rich-font text rows + the bevelled rule, its own top-z layer.
//   • The SANCTUARY y  64..127 — the island band across the top: sail in with souls aboard and
//                                the whole deck banks (slot i pays i × 50).
//   • The sea       y 128..479 — open water over the two parallax planes, scattered with fixed
//                                ISLETS where colonists spawn and wait; enemy craft cross it
//                                firing straight, readable bullets (the toned-down bullet hell),
//                                and the abductor hunts whoever waits.
//   The ferry sails freely in 8 directions; the field edges clamp. THE WEIGHT RULE: every
//   colonist aboard slows the ferry — and each one aboard occasionally FIRES BACK (the cargo is
//   the difficulty AND the seed of the arsenal).

#include <array>

namespace ferryman {

// ── Viewport / tile grid ──────────────────────────────────────────────────────────────────────────
constexpr int kTile  = 8;                  // the engine's 8px tile — the tile layers' grid
constexpr int kViewW = 1280, kViewH = 720; // 16:9
constexpr int kMapW  = kViewW / kTile;     // 160 tile columns
constexpr int kMapH  = kViewH / kTile;     // 90 tile rows

// The HUD band (8px rows 0–7) is chrome, not play space: its OWN high-z tile layer.
constexpr int kHudBandRows = 8;            // 8px rows 0–7
constexpr int kFieldTop    = kHudBandRows * kTile;  // 64 — the field's top edge

// Terrain is authored as 32×32 macro-tiles (4×4 groups of engine cells).
constexpr int kBlock       = 32;
constexpr int kBlockCols   = kViewW / kBlock;                 // 40
constexpr int kBlockRows   = (kViewH - kFieldTop) / kBlock;   // 20 (720 isn't a 32-multiple; the
                                                              // 16 px remainder at the bottom is water)

// The sanctuary island band: the top two macro-tile rows of the field (y 64..127). Sailing into
// it with cargo banks the deck.
constexpr int   kSanctuaryBlocks = 2;
constexpr float kSanctuaryBottom = static_cast<float>(kFieldTop + kSanctuaryBlocks * kBlock);

// ── The ferry (free movement; center-based px) ───────────────────────────────────────────────────
constexpr float kFerryW = 32.0f, kFerryH = 22.0f;        // the side view (sailing E/W)
constexpr float kFerryNSW = 18.0f, kFerryNSH = 28.0f;    // the NARROW bow/stern view (sailing N/S) —
                                                         // the wide side view can't thread a
                                                         // one-block (32 px) channel; the hull turns
constexpr float kFerryBoxW = 24.0f, kFerryBoxH = 15.0f;  // forgiving COMBAT box (bullets/enemies)
constexpr float kFerrySpeedBase   = 2.6f;   // px/tick, empty — a nimble skiff
constexpr float kFerrySpeedPerPax = 0.35f;  // each soul aboard slows it (4 = a wallowing barge)
constexpr int   kDeckCap          = 4;
constexpr int   kLives            = 3;
constexpr float kPickupDist       = 20.0f;  // ferry-center to colonist-center boarding range

// ── Colonists / islets ────────────────────────────────────────────────────────────────────────────
constexpr int kMaxWaiting    = 6;   // the field trickle stops here
constexpr int kColonistLooks = 3;   // hood / pack / cap — chosen by id at spawn

// Terrain-sheet tile order (matches the generator). THE SEA IS ONE BIG SEAMLESS FIELD: the
// generator authors a rich 128×128 water design (a 4×4 block of tiles) that wraps at its own
// edges, and slices it into 16 tiles the renderer places BY GRID POSITION — every adjacent tile
// is a literal continuation of its neighbor, so full detail carries no seams. The field has
// THREE animation frames (crests twinkle, dashes breathe, sparkles drift over a static base),
// and each foam overlay has two. Water slot = T_WATER_0 + cell·kWaterPhases + phase, where
// cell = (blockY % 4)·4 + (blockX % 4).
constexpr int kWaterPhases   = 3;
constexpr int kWaterField    = 4;   // the field is 4×4 tiles — 16 position-mapped slices
constexpr int kSparklePhases = 2;
enum TerrainTile {
    T_BLANK = 0,                            // all index 0 — a hole (the sea shows through)
    T_WATER_0 = 1,                          // 48 slots: field cell c, phase p → 1 + c·3 + p
    T_SPARKLE_A = 49,                       // 4 slots: kind k, phase p → T_SPARKLE_A + k·2 + p
    T_SHORE_A = 53, T_SHORE_B, T_SHORE_C,   // islet coastlines: single / left cap / right cap
    T_BUOY = 56, T_MOORING,                 // islet props (authored over the right-cap base)
    T_LANE_FRESH = 58, T_LANE_WORN, T_LANE_DASH, T_OIL,   // deck-plate tiles
    T_MEDIAN_A = 62, T_MEDIAN_B, T_LAMP,    // stone slabs + lamp
    T_SANCTUARY = 65, T_BEACON, T_TRIM,     // the meadow, the beacon, the shoreline
};

// The islets are RANDOMIZED PER RUN — every game rolls a fresh archipelago (the random-tilemap
// demo's ergonomic: randomize within the valid vocabulary, clamped by constraints). IsletSpec is
// the vocabulary; the live set is `FerrymanGame::islets`, rolled at newGame() within these
// bounds. Each islet is `tilesW` blocks wide and one deep; colonists spawn onto them.
struct IsletSpec {
    int blockX;   // leftmost macro-tile column
    int blockY;   // topmost macro-tile row within the field
    int tilesW;   // width in macro-tiles (1 or 2)
    int tilesH;   // height in macro-tiles (1 or 2) — islets come vertical as well as horizontal
    int prop;     // a TerrainTile stamped on the islet's bottom-right block (0 = none)
};
// Placement stays OFF the map edges — inset one block from the left, right, and bottom borders
// (the sanctuary band already caps the top). An islet's WHOLE footprint fits inside these bounds.
constexpr int kIsletColMin = 1;               // leftmost column an islet may occupy (0 = edge)
constexpr int kIsletColMax = kBlockCols - 2;  // rightmost column (kBlockCols-1 = the right edge)
constexpr int kIsletRowMin = 4;               // first islet row — leave TWO clear water rows (2–3)
                                              // along the sanctuary shore, so islets never abut it
constexpr int kIsletRowMax = kBlockRows - 2;  // last row (kBlockRows-1 = the bottom edge)
constexpr int kIsletSpacing = 2;  // min block gap (Chebyshev) between islets — sea lanes stay open
// The islet count SCALES with the playable field so the sea reads at the same density on any
// viewport — about one islet per 18 block-cells (6–8 at 640×480, ~23–31 at 1280×720). The
// rejection sampler self-limits if a roll gets crowded, so this is a target, not a guarantee.
constexpr int kIsletCountMax =
    ((kIsletColMax - kIsletColMin + 1) * (kIsletRowMax - kIsletRowMin + 1)) / 18;
constexpr int kIsletCountMin = kIsletCountMax * 3 / 4;
// An islet's center in viewport px.
constexpr float isletCenterX(const IsletSpec& s) {
    return (static_cast<float>(s.blockX) + static_cast<float>(s.tilesW) / 2.0f) * kBlock;
}
constexpr float isletCenterY(const IsletSpec& s) {
    return static_cast<float>(kFieldTop) +
           (static_cast<float>(s.blockY) + static_cast<float>(s.tilesH) / 2.0f) * kBlock;
}
// An islet's solid rectangle in viewport px (blockX..blockX+tilesW wide, one block deep). The
// islands are SOLID: the hull collides with these, and docking against a coast is where souls
// board (no button — the coast is the jetty).
constexpr float isletLeftX(const IsletSpec& s) { return static_cast<float>(s.blockX * kBlock); }
constexpr float isletRightX(const IsletSpec& s) {
    return static_cast<float>((s.blockX + s.tilesW) * kBlock);
}
constexpr float isletTopY(const IsletSpec& s) {
    return static_cast<float>(kFieldTop + s.blockY * kBlock);
}
constexpr float isletBotY(const IsletSpec& s) {
    return static_cast<float>(kFieldTop + (s.blockY + s.tilesH) * kBlock);
}
constexpr float kCoastTouch = 5.0f;  // how far the inflated hull reaches to count as docked

// ── The enemies (the toned-down bullet hell: every bullet flies a straight, readable line) ──────
enum EnemyKind { EK_CORSAIR = 0, EK_WARDEN, EK_DREAD, EK_MUTANT };
constexpr std::array<float, 4> kEnemyW{28.0f, 36.0f, 48.0f, 18.0f};
constexpr std::array<float, 4> kEnemyH{14.0f, 18.0f, 18.0f, 18.0f};
constexpr std::array<int, 4>   kEnemyHp{1, 2, 3, 1};
constexpr std::array<int, 4>   kEnemyScore{100, 150, 250, 150};
constexpr float kCorsairSpeed = 2.0f;   // straight crossing runs with a light sway
constexpr float kWardenOrbit  = 0.011f; // radians/tick around its patrol anchor
constexpr float kWardenRadius = 70.0f;
constexpr float kDreadSpeed   = 0.8f;   // ponderous horizontal crossings
constexpr float kMutantSpeed  = 1.6f;   // the contact hunter (a lost colonist, come back wrong)
constexpr float kMutantFleeSpeed = 3.4f; // when the ferry dies, the hunt is done — it flees off the
                                         // NEAREST side at a random Y ("my job here is done"), then
                                         // despawns once it's fully off-screen
// Fire cadence (ticks) and bolt speeds (px/tick) — the readable floor of the hell.
constexpr int   kCorsairFireEvery = 150;  // one AIMED bolt
constexpr int   kWardenFireEvery  = 240;  // a 6-bolt RING
constexpr int   kDreadFireEvery   = 200;  // a 3-bolt FAN at the ferry
constexpr float kEnemyBoltSpeed   = 2.4f;
constexpr float kFanSpreadDeg     = 16.0f;
constexpr float kFireRateGrow     = 0.95f;  // per-wave cadence multiplier…
constexpr float kFireRateFloor    = 0.6f;   // …clamped: denser, never random
constexpr int   kEnemyRespawnTicks = 360;   // a downed craft re-enters after ~6 s (fresh id)
constexpr int   kDreadFirstWave    = 3;

// ── Bolts ─────────────────────────────────────────────────────────────────────────────────────────
constexpr float kBoltSize = 12.0f;
constexpr float kBoltBox  = 8.0f;

// ── The cargo's return fire (the SEED of the roguelike's bullet-heaven end) ─────────────────────
// One volley timer whose period divides by the souls aboard: n = 1 fires every ~3.5 s, n = 4
// every ~0.9 s. Weight costs speed and buys firepower at once. There is NO player fire button —
// the crew fights, the ferryman sails.
constexpr int   kCargoFirePeriod = 210;   // ticks per volley PER PASSENGER (divided by n aboard)
constexpr int   kFirstVolleyTicks = 30;   // a fresh gunner takes the rail FAST — the first soul
                                          // aboard fires within half a second of boarding
constexpr float kCargoBoltSpeed  = 3.4f;

// ── Banking / waves ───────────────────────────────────────────────────────────────────────────────
constexpr int kBankPerSlot = 50;   // delivery slot i (1-based) pays i × this — weight compounds
constexpr int kQuotaBase   = 5;    // wave quota = min(kQuotaBase + wave, kQuotaCap)
constexpr int kQuotaCap    = 10;
constexpr int kFoilScore   = 100;  // body-blocking (or shooting) an abduction

// ── The abductor ──────────────────────────────────────────────────────────────────────────────────
constexpr float kAbductorW = 32.0f, kAbductorH = 22.0f;
constexpr float kAbductorCruise  = 1.8f;   // px/tick while positioning above a target
constexpr float kAbductorEntry   = 2.6f;   // along the baked entry curve
constexpr float kAbductorDescend = 1.2f;   // px/tick down the beam
constexpr float kAbductorRise    = 1.8f;   // px/tick lifting a colonist away
constexpr float kAbductorFlee    = 3.4f;   // px/tick after a foil
constexpr float kGrabDist        = 14.0f;  // saucer-to-colonist grab range
constexpr float kFoilDist        = 20.0f;  // ferry-to-saucer body-block range
constexpr float kHoverHeight     = 44.0f;  // hover this far above the target before descending
constexpr int   kSecondAbductorWave = 4;
constexpr int   kMaxMutants         = 2;   // further lost colonists queue

// ── The HUD text grid (16px rich-glyph columns / 8px rows within the HUD layer) ─────────────────
constexpr int kHudCols    = kViewW / 16;   // 40 rich-glyph columns
constexpr int kHudRow1    = 1;             // SCORE / WAVE / LIVES line (8px rows 1–2)
constexpr int kHudRow2    = 3;             // CREW / PAYS / SAVED + alert line (8px rows 3–4)
constexpr int kHudRuleRow = 6;             // the bevelled rule (8px rows 6–7)

// ── Sprite-sheet slot order (matches gen_ferryman_assets.py → the manifest's slot order) ─────────
enum Slot {
    S_FERRY_A = 0, S_FERRY_B,
    S_COL_HOOD_A, S_COL_HOOD_B, S_COL_PACK_A, S_COL_PACK_B, S_COL_CAP_A, S_COL_CAP_B,
    S_DART, S_SWEEPER, S_HAULER,
    S_ABDUCTOR_A, S_ABDUCTOR_B, S_MUTANT_A, S_MUTANT_B,
    S_BOOM_0, S_BOOM_1, S_BOOM_2,
    S_BOLT,
    S_FERRY_NORTH,   // the narrow stern/back view (sailing north — the boat heads away)
    S_FERRY_SOUTH,   // the narrow bow/front view (sailing south — the boat heads toward you)
    S_WAKE_A, S_WAKE_B,  // the boat's trailing foam (its own effect sprite; two shimmer frames)
};

// Sprite-layer palette indices (the order assets.cpp uploads them).
enum Pal {
    PAL_FERRY = 0,   // brass/copper hull, warm cabin glass
    PAL_COLONIST_A,  // jade
    PAL_COLONIST_B,  // sea-green
    PAL_COLONIST_C,  // moss-gold
    PAL_DART_A,      // crimson corsair, running lights phase A
    PAL_DART_B,      //                 running lights phase B
    PAL_SWEEPER_A,   // steel warden, lights A
    PAL_SWEEPER_B,   //               lights B
    PAL_HAULER_A,    // amber dreadnought, lights A
    PAL_HAULER_B,    //                    lights B
    PAL_ABDUCTOR,    // bone-white saucer, violet lights
    PAL_MUTANT,      // sickly chartreuse
    PAL_BOOM,        // fire ramp
    PAL_BOLT_ENEMY,  // hot magenta — every hostile bullet
    PAL_BOLT_CARGO,  // gold — the crew's return fire (same art, the livery IS the allegiance)
    PAL_SHADOW,      // flat dark — a flying craft's cast shadow (its own art, this palette)
    PAL_WAKE,        // foam blue→white — the boat's trailing wake
};

// Terrain-layer palette indices.
enum TerrainPal {
    TP_WATER_A = 0,  // deep teal, shimmer phase A
    TP_WATER_B,      //            shimmer phase B
    TP_SHORE,        // sun-sand
    TP_LANE,         // violet-slate deck plate
    TP_MEDIAN,       // cool stone
    TP_SANCTUARY,    // verdant pad
    TP_BEACON_A,     // gold beacon, glow phase A
    TP_BEACON_B,     //              glow phase B
};

// Rich-font palette indices.
enum TextPal { TXT_WHITE = 0, TXT_GOLD, TXT_CYAN };

// ── The app states ────────────────────────────────────────────────────────────────────────────────
enum class GameState { Title, Playing };

// The ferry's heading — chosen by the dominant movement axis. E/W draw the wide side view (flipped
// for west); N/S draw the narrow bow/stern views and use a narrow collision hull so the boat can
// thread a one-block channel between vertical islets.
enum class Facing { East, West, North, South };

}  // namespace ferryman
