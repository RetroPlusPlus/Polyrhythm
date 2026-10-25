#include "game.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace bong {

using namespace retropp;

namespace {
// A tiny clock-seeded LCG (variety per run; native game, not a GB ROM).
std::uint32_t nextRand(std::uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }
}  // namespace

BongGame::BongGame()
    : rng(static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())) {}

// Lay out a board: rows 0..5 color (one hue each), row 6 silver (2 hits), row 7 color with three
// indestructible GOLD bricks (ends + center) — isolated, so they never wall off the board.
void BongGame::buildBoard() {
    bricksLeft = 0;
    for (int r = 0; r < kBrickRows; ++r) {
        for (int c = 0; c < kBrickCols; ++c) {
            Cell& cell = grid[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
            if (r <= 5) {
                cell = Cell{Brick::Colour, 1, r};
                ++bricksLeft;
            } else if (r == 6) {
                cell = Cell{Brick::Silver, 2, 0};
                ++bricksLeft;
            } else {  // r == 7
                if (c == 0 || c == kBrickCols / 2 || c == kBrickCols - 1) {
                    cell = Cell{Brick::Gold, 0, 0};  // indestructible, not counted
                } else {
                    cell = Cell{Brick::Colour, 1, 2};
                    ++bricksLeft;
                }
            }
        }
    }
}

void BongGame::parkBall() {
    ballX = paddleX + kPaddleW / 2 - kBallSz / 2;
    ballY = kPaddleY - kBallSz - 1;
    ballVx = 0; ballVy = 0;
}

// Aim the ball UPWARD with a desired horizontal velocity plus a tiny random jitter, renormalized so
// |v| ≈ kBallSpeed. The jitter keeps the ball off any perfectly-periodic orbit, so it can't trap itself
// bouncing forever in an already-cleared column — it always drifts to reach every brick.
void BongGame::aimBallUp(float desiredVx) {
    desiredVx += (static_cast<float>(nextRand(rng) % 1000) / 1000.0f - 0.5f) * 0.8f;
    desiredVx = std::clamp(desiredVx, -kMaxEnglish, kMaxEnglish);
    if (std::abs(desiredVx) < kMinEnglish) desiredVx = (desiredVx < 0 ? -kMinEnglish : kMinEnglish);
    ballVx = desiredVx;
    ballVy = -std::sqrt(std::max(0.5f, kBallSpeed * kBallSpeed - ballVx * ballVx));
}

void BongGame::serve() {
    aimBallUp((static_cast<float>(nextRand(rng) % 200) / 100.0f - 1.0f) * 1.4f);
    serving = false;
    emit(GameEventKind::Serve);
}

void BongGame::newGame() {
    score = 0; lives = kLives; serving = true;
    buildBoard(); parkBall();
}

void BongGame::nextBoard() {  // cleared the board → fresh bricks, keep score + lives
    buildBoard(); serving = true; parkBall();
    emit(GameEventKind::LevelClear);
    std::printf("board cleared — score %d — next board\n", score);
}

void BongGame::loseLife() {
    if (--lives <= 0) {
        std::printf("game over — score %d — back to title\n", score);
        state = GameState::Title;
        return;
    }
    serving = true; parkBall();
}

void BongGame::tick(const InputState& in) {
    events_.clear();

    // Title: ENTER (Return / gamepad Start) starts a fresh game.
    if (state == GameState::Title) {
        if (in.justPressed(Action::Start)) { state = GameState::Playing; newGame(); }
        return;
    }

    // Paddle control — digital, gamepad stick, and the mouse (absolute + relative). The keyboard and
    // stick move it incrementally; then, if the mouse moved THIS tick, the most-recent intent wins: an
    // on-screen cursor positions the paddle absolutely, else the raw delta drives it as a spinner.
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

    // While serving the ball rides the paddle; Serve (keyboard X / gamepad South) OR a left mouse
    // click launches it.
    if (serving) {
        parkBall();
        if (in.justPressed(Action::Serve) || in.mouseJustPressed(MouseButton::Left)) serve();
        return;
    }

    // Integrate the ball.
    ballX += ballVx;
    ballY += ballVy;

    // Walls: bounce off left / right / top; falling past the bottom loses a life.
    bool wallHit = false;
    if (ballX < 0)                { ballX = 0;                 ballVx = std::abs(ballVx);  wallHit = true; }
    if (ballX > kViewW - kBallSz) { ballX = kViewW - kBallSz;  ballVx = -std::abs(ballVx); wallHit = true; }
    if (ballY < kPlayTop)         { ballY = kPlayTop;          ballVy = std::abs(ballVy);  wallHit = true; }
    if (wallHit) emit(GameEventKind::WallBounce);
    if (ballY > kViewH)           { emit(GameEventKind::BallLost); loseLife(); return; }

    // Paddle: bounce up with english from where on the paddle the ball hit (center = straight up, edges
    // = a steep angle). Speed is renormalized so |v| stays ≈ kBallSpeed.
    if (ballVy > 0 &&
        ballX < paddleX + kPaddleW && ballX + kBallSz > paddleX &&
        ballY + kBallSz >= kPaddleY && ballY + kBallSz <= kPaddleY + kPaddleH + 6) {
        const float hit = ((ballX + kBallSz / 2) - (paddleX + kPaddleW / 2)) / (kPaddleW / 2);  // [-1,1]
        aimBallUp(hit * kMaxEnglish);
        ballY = kPaddleY - kBallSz - 1;
        emit(GameEventKind::PaddleBounce);
    }

    // Bricks: reflect off the nearer face (min-translation axis) and EJECT the ball out of the brick (so
    // an indestructible gold brick can't trap it). Color breaks in one hit; silver cracks then breaks on
    // the second; gold only bounces. One brick per tick.
    for (int r = 0; r < kBrickRows && (ballVx != 0 || ballVy != 0); ++r) {
        for (int c = 0; c < kBrickCols; ++c) {
            Cell& cell = grid[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
            if (cell.type == Brick::None) continue;
            const float bx = brickX(c), by = brickY(r);
            if (ballX < bx + kBrickW && ballX + kBallSz > bx &&
                ballY < by + kBrickH && ballY + kBallSz > by) {
                const float overlapL = (ballX + kBallSz) - bx;   // penetration from the left face
                const float overlapR = (bx + kBrickW) - ballX;   // from the right
                const float overlapT = (ballY + kBallSz) - by;   // from the top
                const float overlapB = (by + kBrickH) - ballY;   // from the bottom
                if (std::min(overlapL, overlapR) < std::min(overlapT, overlapB)) {
                    ballVx = -ballVx;
                    ballX += (overlapL < overlapR ? -overlapL : overlapR);
                } else {
                    ballVy = -ballVy;
                    ballY += (overlapT < overlapB ? -overlapT : overlapB);
                }
                if (cell.type == Brick::Gold) {
                    emit(GameEventKind::BrickHit, bx, by);  // indestructible — bounce only.
                } else if (cell.type == Brick::Silver && --cell.hp > 0) {
                    emit(GameEventKind::BrickHit, bx, by);  // cracked — palette change + a hit cue.
                } else {
                    const int pts = (cell.type == Brick::Silver) ? kScoreSilver : kScoreColour;
                    score += pts;
                    emit(GameEventKind::BrickBreak, bx, by, pts);
                    cell.type = Brick::None;
                    if (--bricksLeft == 0) nextBoard();
                }
                return;  // one brick per tick
            }
        }
    }
}

}  // namespace bong
