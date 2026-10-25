#pragma once

// Bongusoid — shared layout / tuning constants + the small value types every TU needs. Pure data: no
// engine types here, so the loader (assets), the simulation (game), and the draw step (render) all share
// one definition of the playfield without pulling each other in. (Extracted verbatim from the S1 monolith
// when the example became multi-file in S2.)

#include <cstdint>

namespace bong {

constexpr int kTile = 8;

// ── Layout / tuning for the 640×480 playfield (absolute px / px-per-tick at 60 Hz) ────────────────────
constexpr int   kViewW = 640, kViewH = 480;
constexpr int   kMapW  = kViewW / kTile;   // 80 backdrop/text tiles wide
constexpr int   kMapH  = kViewH / kTile;   // 60 tall

constexpr float kPaddleW = 80.0f, kPaddleH = 16.0f;
constexpr float kPaddleY = 448.0f;          // paddle top-y
constexpr float kBallSz  = 12.0f;
constexpr float kPaddleSpeed = 6.0f;        // keyboard / gamepad px per tick
constexpr float kSpinnerSens = 1.5f;        // raw mouse delta → paddle px (the spinner feel)
constexpr float kBallSpeed   = 4.4f;        // constant ball speed magnitude
constexpr float kMaxEnglish  = 3.6f;        // max horizontal speed imparted by a paddle-edge hit
constexpr float kMinEnglish  = 1.3f;        // the ball can never go perfectly vertical (anti-trap)
constexpr float kPlayTop     = 40.0f;       // ball ceiling (room for the HUD)
constexpr int   kLives       = 3;
constexpr int   kScoreColour = 10;
constexpr int   kScoreSilver = 30;

// The HUD/status strip occupies the top of the screen (the ball ceiling kPlayTop = 40px below it); its
// bottom edge gets a full-width rule on this text-grid row (row 4 → the rule sits at ~y=38, just above
// the play field).
constexpr int   kStatusBorderRow = 4;

// Brick grid: 13 cols × 8 rows; 44×24 cell (incl. gap), 40×18 drawn brick, 34px side margin.
// 13·44 + 2·34 = 640; field spans y = 80 … 272.
constexpr int   kBrickCols = 13, kBrickRows = 8;
constexpr float kBrickAreaTop = 80.0f;
constexpr float kBrickCellW = 44.0f, kBrickCellH = 24.0f;
constexpr float kBrickW = 40.0f, kBrickH = 18.0f;
constexpr float kBrickMarginX = 34.0f;

// Top-left of a brick cell (col, row) in playfield pixels — shared by the sim's collision and the
// renderer's sprite placement, so they can never drift.
[[nodiscard]] constexpr float brickX(int col) { return kBrickMarginX + static_cast<float>(col) * kBrickCellW; }
[[nodiscard]] constexpr float brickY(int row) { return kBrickAreaTop + static_cast<float>(row) * kBrickCellH; }

// Sprite-sheet slot order (matches gen_bongusoid_assets.py → the manifest's slot order).
enum Slot { S_VAUS = 0, S_BALL, S_BRICK, S_SILVER, S_GOLD };

// Sprite-layer palette-set indices. Brick rows 0..5 are palettes 0..5; the rest follow.
constexpr int kBrickRowPals = 6;
enum Pal {
    PAL_SILVER       = kBrickRowPals,  // 6  — silver, undamaged
    PAL_SILVER_CRACK,                  // 7  — silver, one hit taken
    PAL_GOLD,                          // 8
    PAL_PADDLE,                        // 9
    PAL_BALL,                          // 10
};

// Text-layer palette-set indices.
enum TextPal { TXT_WHITE = 0, TXT_GOLD, TXT_CYAN };

// One brick cell: its kind, its remaining hits, and (for a color brick) which row palette colors it.
enum class Brick : std::uint8_t { None, Colour, Silver, Gold };
struct Cell {
    Brick type   = Brick::None;
    int   hp     = 0;
    int   colour = 0;  // palette index 0..5 for a Color brick
};

// The two app states the S1 scaffold carries (S4 fleshes this into the full shell).
enum class GameState { Title, Playing };

}  // namespace bong
