#pragma once

// Polterball — the SIMULATION, its own translation unit. Owns the board, the ball, the paddle, the
// ghost squad, and the score/lives/ignite state, and advances them one 60 Hz tick from the input.
// It draws nothing and makes no sound: it NAMES what happened each tick as a GameEvent stream, which
// the audio + feel layers consume. Pure game model.
//
// THE LOOP (the hybrid's emergent core):
//   • The ball is the pellet-eater — it eats every corridor pellet it ricochets across — and the
//     ghosts hunt THE BALL. You steer it only indirectly, through paddle english: every return is
//     both an aim and a lure.
//   • A ghost that touches the ball SWALLOWS it (a lost life, same stakes as dropping it past the
//     paddle). A power pellet IGNITES the ball for a few seconds: the ghosts turn and flee, and now
//     contact smashes THEM — bank-shot hunting, chain-scored.
//   • Soft walls break permanently when the ball carves them, reshaping both the bounce corridors
//     and the ghosts' routes for the rest of the board. Demolition is strategy, not decoration.
//   • Eat every pellet (power pellets included) to clear the board; the next board is faster.

#include <cstdint>
#include <span>
#include <vector>

#include "retropp/input.h"  // InputState (digital + analog) + Stick

#include "ghosts.h"
#include "layout.h"

namespace polter {

// The game's input vocabulary — the semantic actions the sim reads. main.cpp binds each action to
// its physical sources in one ActionMap; the stick and the mouse are read raw beside these.
enum class Action : std::uint8_t {
    MoveLeft,
    MoveRight,
    Serve,       // launches the parked ball
    Start,       // starts a game at the title
    Fullscreen,  // host-level toggle, read in main.cpp
};

// What happened this tick — the sim's output channel beyond the board state. Audio cues off `kind`;
// the feel layer reads `x`/`y` (where it happened) and `points` (a popup's number).
enum class GameEventKind : std::uint8_t {
    Serve, PaddleBounce, WallBounce, SoftWallBreak, PelletEat, PowerIgnite, GhostDown,
    BallSwallowed, BallLost, LevelClear,
};
struct GameEvent {
    GameEventKind kind;
    float         x      = 0.0f;
    float         y      = 0.0f;
    int           points = 0;
};

// The game model. State is public so the sim-free renderer reads it directly (a demo, not a library
// surface — clarity over encapsulation); the sim mutates it through tick() and the private helpers.
struct PolterGame {
    PolterGame();

    GameState state = GameState::Title;

    Board board;
    int   boardNum   = 1;      // 1-based; layouts alternate, speeds ramp
    float ballSpeed  = kBallSpeedBase;
    float ghostSpeed = kGhostSpeedBase;

    float paddleX = static_cast<float>(kViewW) / 2.0f - kPaddleW / 2.0f;
    float ballX = 0, ballY = 0, ballVx = 0, ballVy = 0;
    int   score = 0, lives = kLives;
    bool  serving = true;

    int igniteLeft = 0;  // ticks of ignite remaining; > 0 = the ghosts are frightened
    int chainIdx   = 0;  // ghosts smashed this ignite (indexes kGhostChain)

    // Sprite-identity epochs: bumped whenever an object TELEPORTS (a re-serve, a board/life reset),
    // so the renderer mints a fresh reconciliation key and the interpolator mount-snaps instead of
    // streaking the object across the screen.
    int epoch      = 0;  // ghosts + power pellets (bumped by every squad reset)
    int serveCount = 0;  // the ball (bumped by every re-park)

    GhostSquad squad;

    // Advance one sim tick; refills the per-tick event list (cleared at entry).
    void tick(const retropp::InputState& in);

    // The events emitted THIS tick (valid until the next tick()).
    [[nodiscard]] std::span<const GameEvent> events() const { return events_; }

    [[nodiscard]] bool ignited() const { return igniteLeft > 0; }

private:
    std::uint32_t rng;
    std::vector<GameEvent> events_;

    void emit(GameEventKind kind, float x = 0.0f, float y = 0.0f, int points = 0) {
        events_.push_back(GameEvent{kind, x, y, points});
    }

    void applySpeeds();
    void newGame();
    void beginServe();   // park the ball on the paddle under a FRESH key (it teleports there)
    void parkBall();     // position only — the serving ball rides the paddle every tick
    void aimBallUp(float desiredVx);
    void serve();
    void loseLife();
    void nextBoard();

    void collideMaze();      // ball vs wall cells: reflect, break soft walls
    void eatAtBallCell();    // pellets under the ball's center + power-pellet pickups
    void ghostContacts();    // swallow (normal) vs smash (ignited)
};

}  // namespace polter
