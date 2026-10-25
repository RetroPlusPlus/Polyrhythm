#pragma once

// Polterball — shared layout / tuning constants, the two authored maze boards, and the small value
// types every TU needs. Pure data: no engine types here, so the loader (assets), the simulation
// (game), the ghost AI (ghosts), and the draw step (render) all share one definition of the playfield
// without pulling each other in.
//
// THE PLAYFIELD, top to bottom (640×480):
//   • HUD strip   y   0.. 31 — score / board / lives text on the font tile layer.
//   • Maze        y  32..415 — a 20×12 grid of 32×32 cells: walls (hard + breakable), pellet
//                              corridors, 4 power pellets, the ghost pen, and a fully open bottom
//                              edge (ball-only — the whole width flows between maze and court).
//   • Court       y 416..479 — the open bounce strip the paddle lives in. Ghosts never enter it;
//                              the ball falls out of play past its bottom edge.

#include <array>
#include <cstdint>
#include <string_view>

namespace polter {

// ── Viewport / tile grid ───────────────────────────────────────────────────────────────────────────
constexpr int kTile  = 8;                  // the engine's 8px tile — the backdrop layer's grid
constexpr int kViewW = 640, kViewH = 480;
constexpr int kMapW  = kViewW / kTile;     // 80 backdrop tiles wide
constexpr int kMapH  = kViewH / kTile;     // 60 tall

// ── Maze grid ──────────────────────────────────────────────────────────────────────────────────────
constexpr int   kCell     = 32;                    // one maze cell = a 4×4 block of 8px tiles
constexpr int   kMazeCols = 20, kMazeRows = 12;
constexpr int   kMazeTop  = 32;                    // maze y-origin (below the HUD strip)
constexpr int   kMazeBottom = kMazeTop + kMazeRows * kCell;  // 416 — the court starts here
constexpr int   kCellTiles  = kCell / kTile;       // 4 — tiles per maze cell edge

[[nodiscard]] constexpr float cellPxX(int c) { return static_cast<float>(c * kCell); }
[[nodiscard]] constexpr float cellPxY(int r) { return static_cast<float>(kMazeTop + r * kCell); }

// ── Movers / tuning (absolute px / px-per-tick at 60 Hz) ──────────────────────────────────────────
constexpr float kPaddleW = 80.0f, kPaddleH = 16.0f;
constexpr float kPaddleY = 456.0f;          // paddle top-y (in the court strip)
constexpr float kBallSz  = 12.0f;
constexpr float kPaddleSpeed = 4.5f;        // keyboard / gamepad px per tick
constexpr float kSpinnerSens = 1.5f;        // raw mouse delta → paddle px (the spinner feel)
constexpr float kMaxEnglish  = 2.6f;        // max horizontal speed imparted by a paddle-edge hit
constexpr float kMinEnglish  = 1.1f;        // the ball can never go perfectly vertical (anti-trap)

// Base speeds, scaled per board (each cleared board multiplies, capped) — the difficulty ramp.
constexpr float kBallSpeedBase  = 3.2f, kBallSpeedGrow  = 1.04f, kBallSpeedCap  = 4.2f;
constexpr float kGhostSpeedBase = 1.6f, kGhostSpeedGrow = 1.07f, kGhostSpeedCap = 2.2f;
constexpr float kFrightSpeedMul = 0.6f;     // frightened ghosts slow down (the classic reversal feel)
constexpr float kEyesSpeed      = 5.0f;     // downed eyes fly home faster than anything alive
constexpr float kExitSpeedMul   = 0.75f;    // pen-exit crawl, relative to ghost speed

constexpr int kLives = 3;

// ── Scoring ────────────────────────────────────────────────────────────────────────────────────────
constexpr int kScorePellet    = 10;
constexpr int kScorePower     = 50;
constexpr int kScoreSoftWall  = 25;
constexpr int kScoreClear     = 500;
constexpr std::array<int, 3> kGhostChain{200, 400, 800};  // per ignite: 1st, 2nd, 3rd ghost

// ── Ghost cast ─────────────────────────────────────────────────────────────────────────────────────
constexpr int kGhostCount   = 3;
constexpr int kAmbushCells  = 3;    // the Ambusher aims this many cells AHEAD of the ball's velocity
constexpr int kGhostBox     = 20;   // the ghost's forgiving collision box (its art is 24×24)

// The pen (the ghost house): interior floor cells at rows/cols fixed by BOTH board layouts below, so
// the eyes' homing path and the respawn logic are board-independent.
constexpr int kPenFloorRow = 6;                    // the two P cells: (9,6) and (10,6)
constexpr int kPenLeftCol = 9, kPenRightCol = 10;
constexpr int kPenExitRow  = 4;                    // the corridor row a pen-exiting ghost surfaces at

// ── The HUD text grid (rows of the 80×60 backdrop layer) ──────────────────────────────────────────
constexpr int kHudTextRow   = 1;   // SCORE / BOARD / LIVES line
constexpr int kHudBorderRow = 3;   // full-width rule just above the maze

// ── Sprite-sheet slot order (matches gen_polterball_assets.py → the manifest's slot order) ────────
enum Slot { S_PADDLE = 0, S_BALL, S_GHOST_A, S_GHOST_B, S_EYES, S_POWER };

// Tile-sheet block order (each block a 4×4 group of 8px tiles; matches the generator).
enum TileBlock { T_HARD = 0, T_SOFT, T_PELLET, T_FLOOR, T_GATE };
constexpr int kTileSheetCols8 = 20;  // the tiles sheet is 160px wide → 20 8px-tile columns (stride)

// Sprite-layer palette indices (the order assets.cpp uploads them).
enum Pal {
    PAL_GHOST_0 = 0,   // Chaser   — red
    PAL_GHOST_1,       // Ambusher — pink
    PAL_GHOST_2,       // Wanderer — orange
    PAL_FRIGHT,        // any ghost, frightened — deep blue
    PAL_EYES,          // the flying eyes
    PAL_BALL,          // normal ball
    PAL_BALL_FIRE,     // ignited ball
    PAL_PADDLE,
    PAL_POW_A,         // power pellet, pulse phase A
    PAL_POW_B,         //                phase B
};

// Tile-layer palette indices.
enum TilePal { TP_WALL = 0, TP_SOFT, TP_PELLET, TP_GATE };

// Text-layer palette indices.
enum TextPal { TXT_WHITE = 0, TXT_GOLD, TXT_CYAN };

// ── The app states ────────────────────────────────────────────────────────────────────────────────
enum class GameState { Title, Playing };

// ── The boards ────────────────────────────────────────────────────────────────────────────────────
// 20×12 string-art rows, one char per maze cell:
//   '#' hard wall        '%' soft (breakable) wall       '.' corridor with a pellet
//   'o' power pellet     '-' plain corridor (no pellet)  '=' court gate (ball-only opening)
//   'G' pen gate         'P' pen floor
// Rules the layouts are authored against (checked by eye + the dev run):
//   • Every corridor cell keeps ≥2 ghost-open neighbors WITH THE SOFT WALLS INTACT — ghosts never
//     reverse, so a dead end would trap one forever. Breaking a soft wall only ADDS routes.
//   • '=' gates live only in the bottom row: open for the ball (maze ↔ court), closed for ghosts.
//   • The pen is the same 4 cells on both boards (see the kPen* constants above).
using BoardRows = std::array<std::string_view, kMazeRows>;

inline constexpr BoardRows kBoard1{
    "####################",
    "#o.......%%.......o#",
    "#.##.###.##.###.##.#",
    "#..................#",
    "#.##.#.##..##.#.##.#",
    "#....#..#GG#..#....#",
    "#.##.##.#PP#.##.##.#",
    "#.##....####....##.#",
    "#.##.##..%%..##.##.#",
    "#o................o#",
    "#...##........##...#",
    "#==================#",
};

inline constexpr BoardRows kBoard2{
    "####################",
    "#o................o#",
    "#.%#.###.##.###.#%.#",
    "#..................#",
    "#.##.#.##..##.#.##.#",
    "#....#..#GG#..#....#",
    "#.##.##.#PP#.##.##.#",
    "#.%#....####....#%.#",
    "#.##.%#..##..#%.##.#",
    "#o................o#",
    "#...##........##...#",
    "#==================#",
};

inline constexpr std::array<const BoardRows*, 2> kBoards{&kBoard1, &kBoard2};

// ── The runtime board ─────────────────────────────────────────────────────────────────────────────
// What a cell IS right now (soft walls break to Floor at runtime); pellets tracked beside it.
enum class CellKind : std::uint8_t { Hard, Soft, Floor, CourtGate, PenGate, PenFloor };

struct CellRC {
    int c = 0, r = 0;
};

struct Board {
    std::array<std::array<CellKind, kMazeCols>, kMazeRows> kind{};
    std::array<std::array<bool, kMazeCols>, kMazeRows>     pellet{};
    std::array<CellRC, 4> power{};        // the power-pellet cells, in author order
    std::array<bool, 4>   powerAlive{};
    int powerCount  = 0;
    int pelletsLeft = 0;                  // dots + alive power pellets — 0 wins the board

    void build(int layoutIndex) {
        const BoardRows& rows = *kBoards[static_cast<std::size_t>(layoutIndex)];
        powerCount = 0;
        pelletsLeft = 0;
        for (int r = 0; r < kMazeRows; ++r) {
            for (int c = 0; c < kMazeCols; ++c) {
                CellKind k   = CellKind::Hard;
                bool     dot = false;
                switch (rows[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)]) {
                    case '#': k = CellKind::Hard; break;
                    case '%': k = CellKind::Soft; break;
                    case '.': k = CellKind::Floor; dot = true; break;
                    case '-': k = CellKind::Floor; break;
                    case '=': k = CellKind::CourtGate; break;
                    case 'G': k = CellKind::PenGate; break;
                    case 'P': k = CellKind::PenFloor; break;
                    case 'o':
                        k = CellKind::Floor;
                        power[static_cast<std::size_t>(powerCount)]      = CellRC{c, r};
                        powerAlive[static_cast<std::size_t>(powerCount)] = true;
                        ++powerCount;
                        ++pelletsLeft;   // power pellets count toward the clear
                        break;
                }
                kind[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)]   = k;
                pellet[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = dot;
                if (dot) ++pelletsLeft;
            }
        }
    }

    [[nodiscard]] static bool inGrid(int c, int r) {
        return c >= 0 && c < kMazeCols && r >= 0 && r < kMazeRows;
    }
    // Solid to the BALL: walls and the whole pen shell (the ball can never enter the ghost house).
    [[nodiscard]] bool ballSolid(int c, int r) const {
        if (!inGrid(c, r)) return false;  // outside the grid = the court / HUD, handled elsewhere
        const CellKind k = kind[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
        return k == CellKind::Hard || k == CellKind::Soft || k == CellKind::PenGate ||
               k == CellKind::PenFloor;
    }
    // Open to an ACTIVE ghost: plain floor only — court gates keep ghosts in the maze, and the pen
    // is one-way (exit via the Exiting state; active ghosts never wander back in).
    [[nodiscard]] bool ghostOpen(int c, int r) const {
        return inGrid(c, r) && kind[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] ==
                                   CellKind::Floor;
    }
};

}  // namespace polter
