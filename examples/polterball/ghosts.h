#pragma once

// Polterball — the GHOST AI, its own translation unit. Three ghosts patrol the maze's cell grid and
// hunt THE BALL (never the player) with the classic junction rules:
//
//   • Movement is cell-center to cell-center. A ghost picks its next direction only when it arrives
//     at a cell center, and it NEVER reverses except on a mode change — which is why the boards are
//     authored dead-end-free (a no-reverse ghost in a dead end would be stuck forever).
//   • The pick is GREEDY, not pathfinding: among the open non-reverse directions, take the one whose
//     next cell is straight-line closest to the target (tie order Up, Left, Down, Right).
//   • Personalities differ ONLY by target: the Chaser aims at the ball's cell, the Ambusher aims a
//     few cells AHEAD of the ball's velocity (it cuts corridors off), and the Wanderer picks randomly
//     at every junction (constant background pressure).
//   • A scatter/chase clock alternates hunting with a retreat to fixed home corners, so the pressure
//     breathes. The clock pauses while the ball is ignited.
//   • Frightened (the ball ate a power pellet): random at junctions, slower, reversed on entry — the
//     hunters become the hunted. A downed ghost's EYES fly home to the pen along a smooth Curve
//     (baked once into an ArcLengthTable, walked at constant speed), then it respawns and exits.
//
// The squad only MOVES ghosts; the ball↔ghost contact rules (swallow vs down) live in the sim, which
// reads each ghost's position from here.

#include <array>
#include <cstdint>
#include <optional>

#include "retropp/curve.h"     // Curve / ArcLengthTable — the eyes' homing path
#include "retropp/geometry.h"  // Vec2

#include "layout.h"

namespace polter {

enum class GhostRole : std::uint8_t { Chaser, Ambusher, Wanderer };

enum class GhostState : std::uint8_t {
    InPen,    // waiting in the ghost house (release timer running)
    Exiting,  // walking up through the gate to the exit row
    Active,   // hunting (or fleeing, when frightened)
    Eyes,     // downed — the eyes are flying home along the curve
};

struct Ghost {
    GhostRole  role  = GhostRole::Chaser;
    GhostState state = GhostState::InPen;
    bool       frightened = false;  // meaningful while Active/Exiting; cleared on a down / calm

    // The cell walk: position = center of `from` + `dir` * t (t px progressed toward the next cell).
    CellRC from{};
    int    dirC = 0, dirR = -1;
    float  t = 0.0f;

    int penTimer = 0;  // InPen: ticks until release

    // The eyes' baked homing path (present only while state == Eyes).
    std::optional<retropp::ArcLengthTable> eyesPath;
    float eyesS   = 0.0f;
    float eyesLen = 0.0f;
};

class GhostSquad {
public:
    // Place the cast for a fresh board / a fresh life: the Chaser starts hunting immediately at the
    // pen's exit row; the other two wait in the pen on staggered release timers.
    void reset();

    // Advance every ghost one tick. `ignited` pauses the scatter/chase clock (frightened time doesn't
    // burn the schedule); the ball's center + velocity feed the personalities' targets.
    void tick(const Board& board, retropp::Vec2 ballCenter, retropp::Vec2 ballVel, float ghostSpeed,
              bool ignited, std::uint32_t& rng);

    void frightenAll();   // ignite start: flag + reverse every hunter
    void calmAll();       // ignite end (or a life reset): back to normal
    void down(int i);     // ghost i was smashed: its eyes fly home, it respawns from the pen

    [[nodiscard]] const Ghost&   ghost(int i) const { return ghosts_[static_cast<std::size_t>(i)]; }
    [[nodiscard]] retropp::Vec2  center(int i) const;  // the ghost's center, in viewport px

private:
    void chooseDir(Ghost& g, const Board& board, CellRC target, bool randomPick,
                   std::uint32_t& rng) const;
    [[nodiscard]] CellRC targetFor(const Ghost& g, retropp::Vec2 ballCenter,
                                   retropp::Vec2 ballVel) const;

    std::array<Ghost, kGhostCount> ghosts_{};
    bool chaseMode_ = false;  // scatter first, like the classic opening wave
    int  modeTicks_ = 0;      // ticks left in the current scatter/chase phase
};

}  // namespace polter
