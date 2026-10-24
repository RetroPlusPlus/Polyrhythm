#include "game.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "retropp/timing.h"  // TimingProfile / TickPeriodNs — the 8 s ignite → ticks

namespace polter {

using namespace retropp;

namespace {
using namespace std::chrono_literals;

const TimingProfile kProfile{TickPeriodNs::Hz60};
const std::uint64_t kIgniteTicks = kProfile.ticksForDuration(8s);

// A tiny clock-seeded LCG (variety per run; native game, not a Game Boy ROM).
std::uint32_t nextRand(std::uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }
}  // namespace

PolterGame::PolterGame()
    : rng(static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())) {}

void PolterGame::applySpeeds() {
    const auto ramp = static_cast<float>(boardNum - 1);
    ballSpeed  = std::min(kBallSpeedBase * std::pow(kBallSpeedGrow, ramp), kBallSpeedCap);
    ghostSpeed = std::min(kGhostSpeedBase * std::pow(kGhostSpeedGrow, ramp), kGhostSpeedCap);
}

void PolterGame::newGame() {
    score = 0;
    lives = kLives;
    boardNum = 1;
    applySpeeds();
    board.build(0);
    igniteLeft = 0;
    chainIdx   = 0;
    ++epoch;
    squad.reset();
    beginServe();
}

void PolterGame::beginServe() {
    serving = true;
    ++serveCount;  // the ball teleports to the paddle — a fresh key makes it mount-snap, not streak
    parkBall();
}

void PolterGame::parkBall() {
    ballX  = paddleX + kPaddleW / 2 - kBallSz / 2;
    ballY  = kPaddleY - kBallSz - 1;
    ballVx = 0;
    ballVy = 0;
}

// Aim the ball UPWARD with a desired horizontal velocity plus a tiny random jitter, renormalized so
// |v| ≈ ballSpeed. The jitter keeps the ball off any perfectly-periodic orbit — it always drifts
// enough to reach every corridor eventually.
void PolterGame::aimBallUp(float desiredVx) {
    desiredVx += (static_cast<float>(nextRand(rng) % 1000) / 1000.0f - 0.5f) * 0.6f;
    desiredVx = std::clamp(desiredVx, -kMaxEnglish, kMaxEnglish);
    if (std::abs(desiredVx) < kMinEnglish) desiredVx = (desiredVx < 0 ? -kMinEnglish : kMinEnglish);
    ballVx = desiredVx;
    ballVy = -std::sqrt(std::max(0.5f, ballSpeed * ballSpeed - ballVx * ballVx));
}

void PolterGame::serve() {
    aimBallUp((static_cast<float>(nextRand(rng) % 200) / 100.0f - 1.0f) * 1.2f);
    serving = false;
    emit(GameEventKind::Serve);
}

void PolterGame::loseLife() {
    igniteLeft = 0;  // a lost life ends any ignite (and un-frightens the survivors below, via reset)
    chainIdx   = 0;
    if (--lives <= 0) {
        std::printf("game over — score %d — back to title\n", score);
        state = GameState::Title;
        return;
    }
    ++epoch;
    squad.reset();
    beginServe();
}

void PolterGame::nextBoard() {  // every pellet eaten → fresh (alternate) maze, keep score + lives
    score += kScoreClear;
    emit(GameEventKind::LevelClear);
    std::printf("board %d cleared — score %d\n", boardNum, score);
    ++boardNum;
    applySpeeds();
    board.build((boardNum - 1) % 2);
    igniteLeft = 0;
    chainIdx   = 0;
    ++epoch;
    squad.reset();
    beginServe();
}

// Ball vs the maze's solid cells: find the overlapped solid cell (of the ≤4 the ball can touch),
// reflect off the nearer face (min-penetration axis), and EJECT the ball clear so the next tick
// can't re-trigger. A soft wall breaks — permanently — instead of just bouncing: kind flips to
// Floor, so BOTH the ball's corridors and the ghosts' routes change for the rest of the board.
// One cell per tick, the nearest-face rule — the same collision shape a brick field uses.
void PolterGame::collideMaze() {
    if (ballY >= static_cast<float>(kMazeBottom) || ballY + kBallSz <= static_cast<float>(kMazeTop)) {
        return;  // entirely in the court (or the HUD line, which the top border row prevents)
    }
    const int c0 = std::max(0, static_cast<int>(ballX) / kCell);
    const int c1 = std::min(kMazeCols - 1, static_cast<int>(ballX + kBallSz - 0.01f) / kCell);
    const int r0 = std::max(0, (static_cast<int>(ballY) - kMazeTop) / kCell);
    const int r1 = std::min(kMazeRows - 1, (static_cast<int>(ballY + kBallSz - 0.01f) - kMazeTop) / kCell);

    // Pick the solid cell the ball overlaps MOST — resolving the deepest contact first keeps a
    // corner clip from reflecting off the wrong face.
    int   bc = -1, br = -1;
    float bestArea = 0.0f;
    for (int r = r0; r <= r1; ++r) {
        for (int c = c0; c <= c1; ++c) {
            if (!board.ballSolid(c, r)) continue;
            const float ox = std::min(ballX + kBallSz, cellPxX(c) + kCell) - std::max(ballX, cellPxX(c));
            const float oy = std::min(ballY + kBallSz, cellPxY(r) + kCell) - std::max(ballY, cellPxY(r));
            if (ox > 0 && oy > 0 && ox * oy > bestArea) { bestArea = ox * oy; bc = c; br = r; }
        }
    }
    if (bc < 0) return;

    const float cx = cellPxX(bc), cy = cellPxY(br);
    const float overlapL = (ballX + kBallSz) - cx;          // penetration from the cell's left face
    const float overlapR = (cx + kCell) - ballX;            // from the right
    const float overlapT = (ballY + kBallSz) - cy;          // from the top
    const float overlapB = (cy + kCell) - ballY;            // from the bottom
    if (std::min(overlapL, overlapR) < std::min(overlapT, overlapB)) {
        ballVx = -ballVx;
        ballX += (overlapL < overlapR ? -overlapL : overlapR);
    } else {
        ballVy = -ballVy;
        ballY += (overlapT < overlapB ? -overlapT : overlapB);
    }

    CellKind& kind = board.kind[static_cast<std::size_t>(br)][static_cast<std::size_t>(bc)];
    if (kind == CellKind::Soft) {
        kind = CellKind::Floor;  // carved for the rest of the board — no pellet appears (demolition
        score += kScoreSoftWall;  // must never ADD to the win condition)
        emit(GameEventKind::SoftWallBreak, cx, cy);
    } else {
        emit(GameEventKind::WallBounce, cx, cy);
    }
}

void PolterGame::eatAtBallCell() {
    // Dots: the cell under the ball's CENTER (eating is about where the ball is, not what it grazes).
    const float bcx = ballX + kBallSz / 2, bcy = ballY + kBallSz / 2;
    const int   c   = static_cast<int>(bcx) / kCell;
    const int   r   = (static_cast<int>(bcy) - kMazeTop) / kCell;
    if (Board::inGrid(c, r) && bcy >= static_cast<float>(kMazeTop) &&
        board.pellet[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)]) {
        board.pellet[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = false;
        --board.pelletsLeft;
        score += kScorePellet;
        emit(GameEventKind::PelletEat, cellPxX(c), cellPxY(r));
    }

    // Power pellets: a real 16×16 pickup at its cell center — overlap, not center-cell, so a fast
    // graze still collects it.
    for (int i = 0; i < board.powerCount; ++i) {
        if (!board.powerAlive[static_cast<std::size_t>(i)]) continue;
        const CellRC cell = board.power[static_cast<std::size_t>(i)];
        const float  px   = cellPxX(cell.c) + kCell / 2.0f - 8.0f;
        const float  py   = cellPxY(cell.r) + kCell / 2.0f - 8.0f;
        if (ballX < px + 16 && ballX + kBallSz > px && ballY < py + 16 && ballY + kBallSz > py) {
            board.powerAlive[static_cast<std::size_t>(i)] = false;
            --board.pelletsLeft;
            score += kScorePower;
            igniteLeft = static_cast<int>(kIgniteTicks);  // a second pellet RESETS the clock
            chainIdx   = 0;                               // …and the chain
            squad.frightenAll();
            emit(GameEventKind::PowerIgnite, px, py);
        }
    }
}

void PolterGame::ghostContacts() {
    const float half = kGhostBox / 2.0f;
    for (int i = 0; i < kGhostCount; ++i) {
        const Ghost& g = squad.ghost(i);
        if (g.state != GhostState::Active) continue;  // pen dwellers + flying eyes don't touch play
        const Vec2 gc = squad.center(i);
        const bool hit = ballX < gc.x + half && ballX + kBallSz > gc.x - half &&
                         ballY < gc.y + half && ballY + kBallSz > gc.y - half;
        if (!hit) continue;
        if (g.frightened) {
            // Smashed: chain-scored, eyes fly home, the hunt continues.
            const int pts = kGhostChain[static_cast<std::size_t>(std::min(chainIdx, 2))];
            ++chainIdx;
            score += pts;
            squad.down(i);
            emit(GameEventKind::GhostDown, gc.x, gc.y, pts);
        } else {
            // Swallowed: the ghost caught the thing it was hunting. Same cost as dropping the ball.
            emit(GameEventKind::BallSwallowed, gc.x, gc.y);
            loseLife();
            return;
        }
    }
}

void PolterGame::tick(const InputState& in) {
    events_.clear();

    // Title: ENTER (Return / gamepad Start) starts a fresh game.
    if (state == GameState::Title) {
        if (in.justPressed(Action::Start)) { state = GameState::Playing; newGame(); }
        return;
    }

    // Paddle control — digital, gamepad stick, and the mouse (absolute + relative). The keyboard and
    // stick move it incrementally; then, if the mouse moved THIS tick, the most-recent intent wins:
    // an on-screen cursor positions the paddle absolutely, else the raw delta drives it as a spinner.
    if (in.isHeld(Action::MoveLeft))  paddleX -= kPaddleSpeed;
    if (in.isHeld(Action::MoveRight)) paddleX += kPaddleSpeed;
    const float stickX = in.stick(Stick::Left).x;
    if (std::abs(stickX) > 0.25f) paddleX += stickX * kPaddleSpeed;
    if (in.cursorOnScreen() && in.cursorDelta().x != 0) {
        paddleX = static_cast<float>(in.cursor().x) - kPaddleW / 2.0f;   // absolute cursor → paddle
    } else if (in.rawDeltaX() != 0.0f) {
        paddleX += in.rawDeltaX() * kSpinnerSens;                        // relative spinner feel
    }
    paddleX = std::clamp(paddleX, 0.0f, kViewW - kPaddleW);

    // The ghosts roam whether or not the ball is live — lining up a serve under gathering pressure
    // is part of the loop (their targets clamp to the gates while the ball waits in the court).
    const Vec2 ballCenter{ballX + kBallSz / 2, ballY + kBallSz / 2};
    squad.tick(board, ballCenter, Vec2{ballVx, ballVy}, ghostSpeed, ignited(), rng);

    // While serving the ball rides the paddle; Serve (keyboard X / gamepad South) or a left mouse
    // click launches it.
    if (serving) {
        parkBall();
        if (in.justPressed(Action::Serve) || in.mouseJustPressed(MouseButton::Left)) serve();
    } else {
        // Integrate the ball, then resolve the world in contact order: side walls → the maze →
        // eating → the paddle → the ghosts. Falling past the court's bottom edge loses the life.
        ballX += ballVx;
        ballY += ballVy;

        if (ballX < 0)                { ballX = 0;                 ballVx = std::abs(ballVx);  emit(GameEventKind::WallBounce); }
        if (ballX > kViewW - kBallSz) { ballX = kViewW - kBallSz;  ballVx = -std::abs(ballVx); emit(GameEventKind::WallBounce); }
        if (ballY > kViewH) {
            emit(GameEventKind::BallLost, ballX, ballY);
            loseLife();
            return;
        }

        collideMaze();
        eatAtBallCell();
        if (board.pelletsLeft == 0) { nextBoard(); return; }

        // Paddle: bounce up with english from where on the paddle the ball hit (center = straight
        // up, edges = a steep angle). Speed is renormalized so |v| stays ≈ ballSpeed.
        if (ballVy > 0 &&
            ballX < paddleX + kPaddleW && ballX + kBallSz > paddleX &&
            ballY + kBallSz >= kPaddleY && ballY + kBallSz <= kPaddleY + kPaddleH + 6) {
            const float hit = ((ballX + kBallSz / 2) - (paddleX + kPaddleW / 2)) / (kPaddleW / 2);
            aimBallUp(hit * kMaxEnglish);
            ballY = kPaddleY - kBallSz - 1;
            emit(GameEventKind::PaddleBounce);
        }

        ghostContacts();
        if (state == GameState::Title) return;  // a swallow on the last life ended the game
    }

    // The ignite clock burns down whether the ball is live or parked; at zero the tables turn back.
    if (igniteLeft > 0 && --igniteLeft == 0) squad.calmAll();
}

}  // namespace polter
