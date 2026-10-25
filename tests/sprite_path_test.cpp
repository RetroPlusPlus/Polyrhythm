#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "retropp/animation.h"
#include "retropp/atlas_manifest.h"  // AtlasManifest — the sheet a frame's tileIndex resolves against
#include "retropp/curve.h"
#include "retropp/draw_state.h"   // Sprite, Rotation, ObjectKey
#include "retropp/path_walker.h"  // sampleWalk — the one-node parity pin
#include "retropp/renderer.h"     // Renderer — the device-backed art-field applyTo tests
#include "retropp/sprite_path.h"
#include "retropp/tween.h"

namespace retropp {
namespace {

using namespace std::chrono_literals;

// SpritePath is the movement orchestrator: it plays a SEQUENCE of nodes — each a movement spec paced along a
// curve, composed with rotation / scale tween tracks, an animation, and a facing policy, off one clock —
// chained back-to-back, under a sequence-level PlaybackMode, with an interrupt stack on top. Every check here
// is a headless value comparison; no GPU.

// One second of the default Game Boy Color cadence rounds to this many ticks — the tween/animation math is
// tick-quantized, so a half-second is 30 ticks and a full pass is 60.
constexpr std::uint64_t kTicksPerSecond = 60;

// The exact wall-time of N default ticks (so seek() lands on tick N precisely).
std::chrono::nanoseconds ticks(std::uint64_t n) {
    return std::chrono::nanoseconds{static_cast<std::int64_t>(n) * 16'742'706};
}

void expectNear(Vec2 got, Vec2 want, float eps = 0.5f) {
    EXPECT_NEAR(got.x, want.x, eps);
    EXPECT_NEAR(got.y, want.y, eps);
}

// Advance `n` ticks under one mode (each tick re-resolves, exactly as a game's sim loop drives it).
void run(SpritePath& p, int n, PlaybackMode mode = PlaybackMode::loopIndefinitely()) {
    for (int i = 0; i < n; ++i) p.advance(mode);
}

// A single straight leg at a chosen speed — the chaining-test building block.
SpritePathNode leg(Vec2 destination, float pxPerSecond) {
    return SpritePathNode{.move = SpritePathMove::to(destination), .pacing = PathPacing::speed(pxPerSecond)};
}

// ── Move-spec resolution ─────────────────────────────────────────────────────────────────────────────

TEST(SpritePathMove, LineInheritsTheStartAsItsOrigin) {
    SpritePath p{.nodes = {{.move = SpritePathMove::to({60.0f, 0.0f})}}, .start = {0.0f, 0.0f}};
    p.advance();                               // bakes; parked at the origin (default pacing = Speed 0)
    expectNear(p.position(), {0.0f, 0.0f});    // the origin is the start
}

TEST(SpritePathMove, LineAuthoredOriginOverridesTheStart) {
    SpritePath p{.nodes = {{.move = SpritePathMove::to({40.0f, 40.0f}, {80.0f, 40.0f})}},
                 .start = {0.0f, 0.0f}};
    p.advance();
    expectNear(p.position(), {40.0f, 40.0f});  // the authored origin, not the start
}

TEST(SpritePathMove, ThroughPointsPrependsTheOriginAndPassesThroughInOrder) {
    SpritePath p{.nodes = {{.move   = SpritePathMove::through({{40.0f, 0.0f}, {80.0f, 0.0f}}),
                            .pacing = PathPacing::speed(400.0f)}},
                 .start = {0.0f, 0.0f}};
    p.restart();                                    // resolve at elapsed 0 — no tick accrued yet
    expectNear(p.position(), {0.0f, 0.0f}, 1.0f);   // begins at the prepended origin (the start)
    run(p, 40, PlaybackMode::single());
    expectNear(p.position(), {80.0f, 0.0f}, 1.5f);  // ends at the last listed point
}

TEST(SpritePathMove, HermiteTravelsFromOriginToDestination) {
    SpritePath p{.nodes = {{.move   = SpritePathMove::hermite({0.0f, 0.0f}, {60.0f, 0.0f},
                                                              {40.0f, 40.0f}, {40.0f, -40.0f}),
                            .pacing = PathPacing::speed(400.0f)}},
                 .start = {0.0f, 0.0f}};
    p.restart();                                    // resolve at elapsed 0 — no tick accrued yet
    expectNear(p.position(), {0.0f, 0.0f}, 1.0f);
    run(p, 60, PlaybackMode::single());
    expectNear(p.position(), {60.0f, 0.0f}, 1.5f);
}

TEST(SpritePathMove, OnCurveUsesTheCurveGeometryAndIgnoresTheStart) {
    const Curve c = Curve::line({20.0f, 20.0f}, {30.0f, 20.0f});
    SpritePath  p{.nodes = {{.move = SpritePathMove::onCurve(c)}}, .start = {100.0f, 100.0f}};
    p.advance();
    expectNear(p.position(), {20.0f, 20.0f});  // where the curve starts, not the start anchor
}

TEST(SpritePathMove, NamedConstructorsEqualTheAggregateForm) {
    EXPECT_EQ(SpritePathMove::to({50.0f, 10.0f}),
              (SpritePathMove{.kind = SpritePathMove::Kind::Line, .destination = {50.0f, 10.0f}}));
    EXPECT_EQ(SpritePathMove::to({5.0f, 5.0f}, {50.0f, 10.0f}),
              (SpritePathMove{.kind        = SpritePathMove::Kind::Line,
                              .origin      = Vec2{5.0f, 5.0f},
                              .destination = {50.0f, 10.0f}}));
    EXPECT_EQ(SpritePathMove::through({{1.0f, 2.0f}, {3.0f, 4.0f}}),
              (SpritePathMove{.kind = SpritePathMove::Kind::ThroughPoints,
                              .points = {{1.0f, 2.0f}, {3.0f, 4.0f}}}));
    EXPECT_EQ(SpritePathMove::hermite({9.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}),
              (SpritePathMove{.kind               = SpritePathMove::Kind::Hermite,
                              .destination        = {9.0f, 0.0f},
                              .originTangent      = {1.0f, 0.0f},
                              .destinationTangent = {0.0f, 1.0f}}));
}

// ── Composition of the concurrent tracks (parked path = one sentinel node, clock == elapsed) ────────────

// A parked path (Speed 0) so movement stays put and the track under test is the only thing changing.
SpritePath parkedPath() {
    return SpritePath{.nodes = {{.move = SpritePathMove::to({10.0f, 0.0f})}}, .start = {0.0f, 0.0f}};
}

TEST(SpritePathCompose, RotationTrackResolvesMidPass) {
    SpritePath p = parkedPath();
    p.nodes[0].rotationDegrees = Tween<float>::of(0.0f, 90.0f, 1s, Easing::Linear);
    run(p, static_cast<int>(kTicksPerSecond / 2));  // halfway through a linear 0→90 pass
    EXPECT_NEAR(p.rotationDegrees(), 45.0f, 0.5f);
}

TEST(SpritePathCompose, ScaleTrackResolvesMidPass) {
    SpritePath p = parkedPath();
    p.nodes[0].scale = Tween<Vec2>::of({1.0f, 1.0f}, {2.0f, 2.0f}, 1s, Easing::Linear);
    run(p, static_cast<int>(kTicksPerSecond / 2));
    expectNear(p.scaleValue(), {1.5f, 1.5f}, 0.02f);
}

TEST(SpritePathCompose, RotationAndScaleRunConcurrentlyOffOneClock) {
    SpritePath p = parkedPath();
    p.nodes[0].rotationDegrees = Tween<float>::of(0.0f, 90.0f, 1s, Easing::Linear);
    p.nodes[0].scale           = Tween<Vec2>::of({1.0f, 1.0f}, {3.0f, 3.0f}, 1s, Easing::Linear);
    run(p, static_cast<int>(kTicksPerSecond / 2));
    EXPECT_NEAR(p.rotationDegrees(), 45.0f, 0.5f);
    expectNear(p.scaleValue(), {2.0f, 2.0f}, 0.02f);
}

TEST(SpritePathCompose, RotationTrackSingleModeRestsAtItsFinalValue) {
    SpritePath p = parkedPath();
    p.nodes[0].rotationDegrees = Tween<float>::of(0.0f, 90.0f, 1s, Easing::Linear);
    p.nodes[0].rotationMode    = PlaybackMode::single();
    run(p, static_cast<int>(kTicksPerSecond * 2));  // well past the single pass
    EXPECT_NEAR(p.rotationDegrees(), 90.0f, 0.01f);
}

TEST(SpritePathCompose, RotationTrackLoopModeWrapsBackToTheStart) {
    SpritePath p = parkedPath();
    p.nodes[0].rotationDegrees = Tween<float>::of(0.0f, 90.0f, 1s, Easing::Linear);
    p.nodes[0].rotationMode    = PlaybackMode::loopIndefinitely();
    run(p, static_cast<int>(kTicksPerSecond));  // exactly one pass → wraps to posInPass 0
    EXPECT_NEAR(p.rotationDegrees(), 0.0f, 0.01f);
}

TEST(SpritePathCompose, AbsentTracksContributeIdentity) {
    SpritePath p = parkedPath();  // no rotation track, no scale track, FacingPolicy::None
    run(p, 10);
    EXPECT_NEAR(p.rotationDegrees(), 0.0f, 0.01f);
    expectNear(p.scaleValue(), {1.0f, 1.0f}, 0.001f);
}

// ── Facing policies ──────────────────────────────────────────────────────────────────────────────────

// A parked path facing along a straight line in a chosen direction (the tangent is that direction).
SpritePath facingLine(Vec2 direction, FacingPolicy policy) {
    return SpritePath{.nodes = {{.move = SpritePathMove::to(direction), .facing = policy}},
                      .start = {0.0f, 0.0f}};
}

TEST(SpritePathFacing, RotateToFacingEastIsZeroDegrees) {
    SpritePath p = facingLine({10.0f, 0.0f}, FacingPolicy::RotateToFacing);
    p.advance();
    EXPECT_NEAR(p.rotationDegrees(), 0.0f, 0.01f);
}

TEST(SpritePathFacing, RotateToFacingSouthIsNinetyDegrees) {
    SpritePath p = facingLine({0.0f, 10.0f}, FacingPolicy::RotateToFacing);
    p.advance();
    EXPECT_NEAR(p.rotationDegrees(), 90.0f, 0.01f);  // +y is 90° in the top-left-origin clockwise space
}

TEST(SpritePathFacing, RotateToFacingWestIsOneEightyDegrees) {
    SpritePath p = facingLine({-10.0f, 0.0f}, FacingPolicy::RotateToFacing);
    p.advance();
    EXPECT_NEAR(std::abs(p.rotationDegrees()), 180.0f, 0.01f);
}

TEST(SpritePathFacing, RotateToFacingNorthIsMinusNinetyDegrees) {
    SpritePath p = facingLine({0.0f, -10.0f}, FacingPolicy::RotateToFacing);
    p.advance();
    EXPECT_NEAR(p.rotationDegrees(), -90.0f, 0.01f);
}

TEST(SpritePathFacing, RotateToFacingSumsWithTheRotationTrack) {
    SpritePath p = facingLine({10.0f, 0.0f}, FacingPolicy::RotateToFacing);  // facing contributes 0°
    p.nodes[0].rotationDegrees = Tween<float>::of(30.0f, 30.0f, 1s, Easing::Linear);  // constant +30°
    p.advance();
    EXPECT_NEAR(p.rotationDegrees(), 30.0f, 0.01f);  // 0 (facing east) + 30 (track)
}

TEST(SpritePathFacing, FlipXIsTrueTravellingTowardNegativeX) {
    SpritePath p = facingLine({-10.0f, 0.0f}, FacingPolicy::FlipX);
    p.advance();
    EXPECT_TRUE(p.flipX());
}

TEST(SpritePathFacing, FlipXIsFalseTravellingTowardPositiveX) {
    SpritePath p = facingLine({10.0f, 0.0f}, FacingPolicy::FlipX);
    p.advance();
    EXPECT_FALSE(p.flipX());
}

TEST(SpritePathFacing, FlipXHoldsItsPreviousValueWhileTheHorizontalComponentIsZero) {
    SpritePath p = facingLine({0.0f, 10.0f}, FacingPolicy::FlipX);  // purely vertical tangent (x == 0)
    p.sample.flipX = true;   // an already-mirrored sprite
    p.advance();
    EXPECT_TRUE(p.flipX());  // vertical travel does not flip it back

    SpritePath q = facingLine({0.0f, 10.0f}, FacingPolicy::FlipX);
    q.sample.flipX = false;
    q.advance();
    EXPECT_FALSE(q.flipX());
}

TEST(SpritePathFacing, NoneLeavesRotationAndFlipUntouched) {
    SpritePath p = facingLine({-10.0f, 0.0f}, FacingPolicy::None);
    p.advance();
    EXPECT_NEAR(p.rotationDegrees(), 0.0f, 0.01f);
    EXPECT_FALSE(p.flipX());
}

// ── applyTo — the write table ────────────────────────────────────────────────────────────────────────

TEST(SpritePathApplyTo, QuantizesPositionIncludingNegativeCoordinates) {
    SpritePath p = parkedPath();
    p.advance();
    p.sample.position = {10.6f, -3.4f};  // white-box: pin an exact float position
    Sprite s{.key = "mover"};
    p.applyTo(s);
    EXPECT_EQ(s.x, 11);  // std::lround(10.6)
    EXPECT_EQ(s.y, -3);  // std::lround(-3.4)
}

// (The art-field write — atlas/tile/size resolved through the sheet's slice geometry — is device-backed
// at the end of this file; a palette-only frame exercises the same write table device-free here.)
TEST(SpritePathApplyTo, WritesThePaletteWhenAnAnimationTrackIsPresent) {
    const Animation anim{.frames = {AnimationFrame{.palette = PaletteId{3}, .duration = 1s}}};
    SpritePath p = parkedPath();
    p.nodes[0].animation = &anim;
    p.advance();
    Sprite s{.key = "mover"};
    s.tile = 5;                     // pre-set art — a palette-only frame must leave it be
    p.applyTo(s);
    EXPECT_EQ(s.palette, PaletteId{3});
    EXPECT_EQ(s.tile, 5);           // no art on the frame → the art fields hold
}

TEST(SpritePathApplyTo, ComposesScaleThenRotationAboutTheDefaultCentrePivot) {
    SpritePath p = parkedPath();
    p.nodes[0].rotationDegrees = Tween<float>::of(90.0f, 90.0f, 1s, Easing::Linear);  // constant 90°
    p.nodes[0].scale           = Tween<Vec2>::of({2.0f, 2.0f}, {2.0f, 2.0f}, 1s, Easing::Linear);  // constant 2×
    p.advance();
    Sprite s{.key = "mover"};  // default size 8×8 → center pivot (4, 4)
    p.applyTo(s);
    const Transform expected =
        Transform::scale(2.0f, 2.0f, 4.0f, 4.0f).then(Transform::rotation(90.0f, 4.0f, 4.0f));
    EXPECT_EQ(s.transform, expected);
}

TEST(SpritePathApplyTo, HonoursAnExplicitPivotOverride) {
    SpritePath p = parkedPath();
    p.nodes[0].rotationDegrees = Tween<float>::of(45.0f, 45.0f, 1s, Easing::Linear);
    p.nodes[0].pivot           = Vec2{0.0f, 0.0f};
    p.advance();
    Sprite s{.key = "mover"};
    p.applyTo(s);
    const Transform expected =
        Transform::scale(1.0f, 1.0f, 0.0f, 0.0f).then(Transform::rotation(45.0f, 0.0f, 0.0f));
    EXPECT_EQ(s.transform, expected);
}

// (DefaultPivotUsesTheFrameSizeAfterTheFrameWrite is device-backed at the end of this file — the frame's
// size() resolves through the sheet's slice geometry, so it needs a loadAtlas'd sheet.)

TEST(SpritePathApplyTo, LeavesUndeclaredFieldsUntouched) {
    SpritePath p = parkedPath();  // no animation, no tracks, FacingPolicy::None
    p.advance();
    Sprite s{.key = "keepme"};
    s.alpha            = 0.5f;
    s.flipY            = true;
    s.rotation         = Rotation::Rot90;
    const Transform t0 = Transform::translation(3.0f, 4.0f);
    s.transform        = t0;
    const ObjectKey saved = s.key;

    p.applyTo(s);

    EXPECT_EQ(s.key, saved);              // reconciliation identity is never rewritten
    EXPECT_FLOAT_EQ(s.alpha, 0.5f);
    EXPECT_TRUE(s.flipY);
    EXPECT_EQ(s.rotation, Rotation::Rot90);
    EXPECT_EQ(s.transform, t0);           // no track declared anywhere → transform left as the game set it
}

// ── sample() ↔ applyTo parity ────────────────────────────────────────────────────────────────────────

TEST(SpritePathParity, ApplyToMatchesTheSampleOnTheSharedValues) {
    const Animation anim{.frames = {AnimationFrame{.palette = PaletteId{4}, .duration = 1s}}};
    SpritePath p{.nodes = {{.move      = SpritePathMove::to({-40.0f, 0.0f}),
                            .pacing    = PathPacing::speed(200.0f),
                            .facing    = FacingPolicy::FlipX,
                            .animation = &anim}},
                 .start = {0.0f, 0.0f}};
    run(p, 6);
    Sprite s{.key = "mover"};
    p.applyTo(s);
    EXPECT_EQ(s.x, static_cast<int>(std::lround(p.position().x)));
    EXPECT_EQ(s.y, static_cast<int>(std::lround(p.position().y)));
    EXPECT_EQ(s.flipX, p.flipX());
    EXPECT_EQ(s.palette, p.frame()->palette);  // art-field parity is pinned in the device section below
}

// ── The cursor ────────────────────────────────────────────────────────────────────────────────────────

TEST(SpritePathCursor, AdvanceAccruesOnlyWhilePlaying) {
    SpritePath p{.nodes = {leg({600.0f, 0.0f}, 120.0f)}, .start = {0.0f, 0.0f}};
    run(p, 5);
    const std::uint64_t at5   = p.elapsedTicks;
    const Vec2          pos5  = p.position();
    p.pause();
    run(p, 5);
    EXPECT_EQ(p.elapsedTicks, at5);      // paused → no accrual
    expectNear(p.position(), pos5, 0.001f);
    p.play();
    run(p, 5);
    EXPECT_EQ(p.elapsedTicks, at5 + 5);  // resumed
}

TEST(SpritePathCursor, StopRewindsToTheStartAndPauses) {
    SpritePath p{.nodes = {leg({60.0f, 0.0f}, 120.0f)}, .start = {10.0f, 10.0f}};
    run(p, 10);
    p.stop();
    EXPECT_EQ(p.elapsedTicks, 0u);
    EXPECT_FALSE(p.playing);
    expectNear(p.position(), {10.0f, 10.0f});  // back at the origin
}

TEST(SpritePathCursor, RestartRewindsAndResumes) {
    SpritePath p{.nodes = {leg({60.0f, 0.0f}, 120.0f)}, .start = {10.0f, 10.0f}};
    run(p, 10);
    p.restart();
    EXPECT_EQ(p.elapsedTicks, 0u);
    EXPECT_TRUE(p.playing);
    expectNear(p.position(), {10.0f, 10.0f});
}

TEST(SpritePathCursor, SeekJumpsToAWallTimeOffset) {
    SpritePath p{.nodes = {leg({60.0f, 0.0f}, 60.0f)}, .start = {0.0f, 0.0f}};
    p.advance();
    p.seek(500ms);  // half a second at 60 px/s ≈ 30 px along the line
    expectNear(p.position(), {30.0f, 0.0f}, 1.5f);
}

TEST(SpritePathCursor, BakesLazilyOnTheFirstAdvance) {
    SpritePath p{.nodes = {{.move = SpritePathMove::to({10.0f, 0.0f})}}, .start = {0.0f, 0.0f}};
    EXPECT_FALSE(p.baked);
    p.advance();
    EXPECT_TRUE(p.baked);
}

TEST(SpritePathCursor, RestartRebakesForARepath) {
    SpritePath p{.nodes = {{.move = SpritePathMove::to({100.0f, 0.0f})}}, .start = {0.0f, 0.0f}};
    run(p, 5);
    // Re-path: assign a node list whose move starts somewhere the old geometry never did, then restart.
    p.nodes = {SpritePathNode{.move = SpritePathMove::to({20.0f, 20.0f}, {20.0f, 80.0f})}};
    p.restart();
    expectNear(p.position(), {20.0f, 20.0f});  // the new origin, so the arc re-baked
}

TEST(SpritePathCursor, FinishedUnderSingleButNotUnderLoop) {
    SpritePath single{.nodes = {leg({30.0f, 0.0f}, 300.0f)}, .start = {0.0f, 0.0f}};
    run(single, 30, PlaybackMode::single());
    EXPECT_TRUE(single.finished());

    SpritePath loop{.nodes = {leg({30.0f, 0.0f}, 300.0f)}, .start = {0.0f, 0.0f}};
    run(loop, 30, PlaybackMode::loopIndefinitely());
    EXPECT_FALSE(loop.finished());
}

TEST(SpritePathCursor, ParkedPacingStaysAtTheOrigin) {
    SpritePath p{.nodes = {{.move = SpritePathMove::to({60.0f, 0.0f})}},  // default pacing = Speed 0 (sentinel)
                 .start = {12.0f, 34.0f}};
    run(p, 50);
    expectNear(p.position(), {12.0f, 34.0f});
    EXPECT_FLOAT_EQ(p.distance(), 0.0f);
}

TEST(SpritePathCursor, DegenerateEmptyMoveDoesNotCrashAndFinishesUnderSingle) {
    SpritePath p{.nodes = {{.move = SpritePathMove::through({})}},  // origin prepended → one point → empty curve
                 .start = {0.0f, 0.0f}};
    run(p, 5, PlaybackMode::single());
    EXPECT_TRUE(p.finished());       // a zero-length path is immediately finished under a finite mode
    EXPECT_FLOAT_EQ(p.distance(), 0.0f);
}

// ── Sequence chaining (§2.1) ───────────────────────────────────────────────────────────────────────────

TEST(SpritePathSequence, SecondNodeChainsFromTheFirstNodesEnd) {
    SpritePath p{.nodes = {leg({60.0f, 0.0f}, 600.0f), leg({60.0f, 60.0f}, 600.0f)}, .start = {0.0f, 0.0f}};
    p.restart();
    expectNear(p.position(), {0.0f, 0.0f}, 1.0f);           // begins at the start
    run(p, 6, PlaybackMode::single());
    expectNear(p.position(), {60.0f, 0.0f}, 2.0f);          // node 0's end == node 1's inherited origin
    run(p, 6, PlaybackMode::single());
    expectNear(p.position(), {60.0f, 60.0f}, 2.0f);         // node 1 traveled from (60,0) to (60,60)
}

TEST(SpritePathSequence, ThreeNodesChainInOrder) {
    SpritePath p{.nodes = {leg({60.0f, 0.0f}, 600.0f), leg({60.0f, 60.0f}, 600.0f), leg({0.0f, 60.0f}, 600.0f)},
                 .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 100, PlaybackMode::single());  // well past the whole chain → rest at the last node's end
    expectNear(p.position(), {0.0f, 60.0f}, 2.0f);
    EXPECT_TRUE(p.finished());
}

TEST(SpritePathSequence, ExplicitOriginMidSequenceIsAJump) {
    SpritePath p{.nodes = {leg({40.0f, 0.0f}, 600.0f),
                           {.move   = SpritePathMove::to({100.0f, 100.0f}, {180.0f, 100.0f}),
                            .pacing = PathPacing::speed(600.0f)}},
                 .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 8, PlaybackMode::single());   // node 0 done (~4 ticks), now inside node 1
    EXPECT_GE(p.position().x, 100.0f);   // departed from the EXPLICIT origin (100,100), not the chained (40,0)
    EXPECT_NEAR(p.position().y, 100.0f, 2.0f);
}

TEST(SpritePathSequence, OnCurveMidSequenceIgnoresTheChain) {
    const Curve c = Curve::line({200.0f, 200.0f}, {260.0f, 200.0f});
    SpritePath  p{.nodes = {leg({60.0f, 0.0f}, 600.0f),
                            {.move = SpritePathMove::onCurve(c), .pacing = PathPacing::speed(600.0f)}},
                  .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 6, PlaybackMode::single());   // just entered node 1 (the raw curve)
    EXPECT_GE(p.position().x, 199.0f);   // the curve's own start (200,200), not the chained (60,0)
    EXPECT_NEAR(p.position().y, 200.0f, 2.0f);
}

TEST(SpritePathSequence, ChainsFromADistanceTweenRestingMidCurve) {
    // node 0 travels a 100px line but its distance tween rests at 40px — mid-curve. node 1 chains from there.
    static const Tween<float> dist = Tween<float>::of(0.0f, 40.0f, 1s, Easing::Linear);
    SpritePath p{.nodes = {{.move = SpritePathMove::to({100.0f, 0.0f}), .pacing = PathPacing::distanceTween(dist)},
                           leg({0.0f, 60.0f}, 600.0f)},
                 .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 60, PlaybackMode::single());   // node 0's tween finishes at 60 ticks, resting at 40px → (40,0)
    expectNear(p.position(), {40.0f, 0.0f}, 2.0f);
    run(p, 60, PlaybackMode::single());   // node 1 chained from (40,0), traveling to (0,60)
    expectNear(p.position(), {0.0f, 60.0f}, 2.0f);
}

// ── Node-local clocks + rollover (§2.1) ────────────────────────────────────────────────────────────────

TEST(SpritePathSequence, BatchedAdvanceEqualsTickByTickAcrossBoundaries) {
    const auto mk = [] {
        return SpritePath{.nodes = {leg({60.0f, 0.0f}, 600.0f), leg({60.0f, 60.0f}, 600.0f),
                                    leg({0.0f, 60.0f}, 600.0f)},
                          .start = {0.0f, 0.0f}};
    };
    SpritePath a = mk();
    SpritePath b = mk();
    a.restart();
    b.restart();
    for (int i = 0; i < 15; ++i) a.advance(PlaybackMode::single());  // tick-by-tick, crosses ≥ 2 boundaries
    b.advance(PlaybackMode::single(), 15);                            // one batched step
    expectNear(a.position(), b.position(), 0.01f);
    EXPECT_EQ(a.currentNodeIndex(), b.currentNodeIndex());
}

TEST(SpritePathSequence, TracksResolveAgainstTheNodeLocalClock) {
    SpritePath p{.nodes = {leg({60.0f, 0.0f}, 600.0f),  // node 0 (fast, ~6 ticks)
                           {.move           = SpritePathMove::to({60.0f, 60.0f}),
                            .pacing         = PathPacing::speed(60.0f),  // slow: movement lasts ~60 ticks
                            .rotationDegrees = Tween<float>::of(0.0f, 90.0f, 1s, Easing::Linear),
                            .rotationMode   = PlaybackMode::single()}},
                 .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 6, PlaybackMode::single());   // just entered node 1 → its rotation clock is fresh at 0
    EXPECT_NEAR(p.rotationDegrees(), 0.0f, 3.0f);
    run(p, 30, PlaybackMode::single());  // node-local tick ~30 = halfway
    EXPECT_NEAR(p.rotationDegrees(), 45.0f, 5.0f);
}

TEST(SpritePathSequence, LoopReEntryRestartsTheNodeLocalClock) {
    SpritePath p{.nodes = {{.move           = SpritePathMove::to({60.0f, 0.0f}),
                            .pacing         = PathPacing::speed(60.0f),  // one node, ~60-tick pass
                            .rotationDegrees = Tween<float>::of(0.0f, 90.0f, 1s, Easing::Linear),
                            .rotationMode   = PlaybackMode::single()}},
                 .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 60, PlaybackMode::loopIndefinitely());  // one full pass → node 0 re-enters at local tick 0
    EXPECT_NEAR(p.rotationDegrees(), 0.0f, 3.0f);  // the track RESTARTED, not held at 90
    run(p, 30, PlaybackMode::loopIndefinitely());
    EXPECT_NEAR(p.rotationDegrees(), 45.0f, 5.0f);
}

TEST(SpritePathSequence, AnimationRestartsPerNodeEntry) {
    const AtlasManifest sheet{.atlasId = AtlasId{1},
                              .slots   = {AssetSlot{.tile = 0, .dimensions = AssetDimensions::GameBoy8x8},
                                          AssetSlot{.tile = 1, .dimensions = AssetDimensions::GameBoy8x8}},
                              .kind    = ContentKind::SingleAnimation};
    const Animation anim{.frames = {
        AnimationFrame{.sheet = sheet.atlasId, .tileIndex = 0, .palette = PaletteId{0}, .duration = 500ms},
        AnimationFrame{.sheet = sheet.atlasId, .tileIndex = 1, .palette = PaletteId{0}, .duration = 500ms}}};
    SpritePath p{.nodes = {{.move          = SpritePathMove::to({60.0f, 0.0f}),
                            .pacing        = PathPacing::speed(60.0f),  // ~60-tick pass
                            .animation     = &anim,
                            .animationMode = PlaybackMode::single()}},
                 .start = {0.0f, 0.0f}};
    p.restart();
    p.advance(PlaybackMode::loopIndefinitely());     // local tick ~1 → frame 0
    EXPECT_EQ(*p.frame()->tileIndex, 0u);
    run(p, 40, PlaybackMode::loopIndefinitely());    // local tick ~41 → frame 1 (30..60)
    EXPECT_EQ(*p.frame()->tileIndex, 1u);
    run(p, 30, PlaybackMode::loopIndefinitely());    // crossed the pass → re-entered → frame 0 again
    EXPECT_EQ(*p.frame()->tileIndex, 0u);
}

TEST(SpritePathSequence, OneNodeSingleReproducesTheWalker) {
    const Curve          c   = Curve::line({0.0f, 0.0f}, {100.0f, 0.0f});
    const ArcLengthTable arc = c.arcTable();
    const PathPacing     pac = PathPacing::speed(300.0f);
    SpritePath p{.nodes = {{.move = SpritePathMove::onCurve(c), .pacing = pac}}, .start = {0.0f, 0.0f}};
    p.restart();
    for (int t = 1; t <= 20; ++t) {
        p.advance(PlaybackMode::single());
        const WalkSample w = sampleWalk(arc, pac, static_cast<std::uint64_t>(t), TimingProfile::GameBoyColor,
                                    PlaybackMode::single());
        expectNear(p.position(), w.position, 0.01f);
        EXPECT_EQ(p.finished(), w.finished);
    }
}

// ── Sequence-level PlaybackMode (§2.2) ─────────────────────────────────────────────────────────────────

TEST(SpritePathModes, SingleRestsAtEndFinishedWithTracksStillResolving) {
    SpritePath p{.nodes = {{.move      = SpritePathMove::to({60.0f, 0.0f}),
                            .pacing    = PathPacing::speed(60.0f),  // movement ~60 ticks
                            .scale     = Tween<Vec2>::of({1.0f, 1.0f}, {2.0f, 2.0f}, 1s, Easing::Linear),
                            .scaleMode = PlaybackMode::loopIndefinitely()}},
                 .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 90, PlaybackMode::single());  // 30 ticks past the movement finish (at 60)
    EXPECT_TRUE(p.finished());
    expectNear(p.position(), {60.0f, 0.0f}, 2.0f);          // movement rests at the endpoint
    expectNear(p.scaleValue(), {1.5f, 1.5f}, 0.1f);         // scale track kept playing (local tick 90 → half)
}

TEST(SpritePathModes, LoopNTimesPlaysNPassesThenRests) {
    SpritePath p{.nodes = {leg({30.0f, 0.0f}, 300.0f)}, .start = {0.0f, 0.0f}};  // ~6-tick pass
    p.restart();
    run(p, 200, PlaybackMode::loopNTimes(2));  // 2 laps then hold
    EXPECT_TRUE(p.finished());
    expectNear(p.position(), {30.0f, 0.0f}, 2.0f);   // rests at the last node's end
}

TEST(SpritePathModes, LoopNTimesZeroRestsAtTheChainStart) {
    SpritePath p{.nodes = {leg({30.0f, 0.0f}, 300.0f)}, .start = {5.0f, 5.0f}};
    p.restart();
    run(p, 20, PlaybackMode::loopNTimes(0));
    EXPECT_TRUE(p.finished());
    expectNear(p.position(), {5.0f, 5.0f}, 0.5f);    // never played → sits at the chain start
}

TEST(SpritePathModes, LoopIndefinitelyWrapsReChainingNodeZeroFromStart) {
    SpritePath p{.nodes = {leg({30.0f, 0.0f}, 300.0f)}, .start = {0.0f, 0.0f}};  // ~6-tick pass
    p.restart();
    run(p, 6, PlaybackMode::loopIndefinitely());     // exactly one pass → wraps to node 0 at local tick 0
    expectNear(p.position(), {0.0f, 0.0f}, 1.0f);    // re-chained from the start
    EXPECT_FALSE(p.finished());
}

TEST(SpritePathModes, PlayForDurationHoldsAtTheCutoffFinished) {
    SpritePath p{.nodes = {leg({600.0f, 0.0f}, 600.0f)}, .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 40, PlaybackMode::playForDuration(500ms));  // cutoff at 30 ticks; run past it
    EXPECT_TRUE(p.finished());
    const Vec2 held = p.position();
    run(p, 20, PlaybackMode::playForDuration(500ms));  // further advance holds the cutoff sample
    expectNear(p.position(), held, 0.5f);
}

// ── Idioms + degenerates (§2.1 / §2.2) ─────────────────────────────────────────────────────────────────

TEST(SpritePathIdioms, WaitNodeStandsStillForItsEasedDuration) {
    SpritePath p{.nodes = {leg({60.0f, 0.0f}, 600.0f),
                           {.move = SpritePathMove::to({60.0f, 0.0f}), .pacing = PathPacing::eased(1s)},  // WAIT
                           leg({60.0f, 60.0f}, 600.0f)},
                 .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 6, PlaybackMode::single());    // finished node 0, entering the wait
    expectNear(p.position(), {60.0f, 0.0f}, 2.0f);
    run(p, 30, PlaybackMode::single());   // mid-wait (local tick ~30 of the 60-tick wait)
    expectNear(p.position(), {60.0f, 0.0f}, 2.0f);   // still standing at the corner
    run(p, 40, PlaybackMode::single());   // wait done, into node 2
    EXPECT_GT(p.position().y, 5.0f);       // moved on from the corner
}

TEST(SpritePathIdioms, SentinelNodeRestsForeverWithTracksPlaying) {
    SpritePath p{.nodes = {leg({60.0f, 0.0f}, 600.0f),
                           {.move      = SpritePathMove::to({60.0f, 60.0f}),  // default Speed 0 → SENTINEL
                            .scale     = Tween<Vec2>::of({1.0f, 1.0f}, {2.0f, 2.0f}, 1s, Easing::Linear),
                            .scaleMode = PlaybackMode::loopIndefinitely()}},
                 .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 6, PlaybackMode::single());     // into the sentinel node
    EXPECT_EQ(p.currentNodeIndex(), 1u);
    expectNear(p.position(), {60.0f, 0.0f}, 2.0f);   // parked at the sentinel's origin
    run(p, 45, PlaybackMode::single());    // local tick ~45 in the sentinel
    EXPECT_FALSE(p.finished());            // a sentinel sequence never finishes
    EXPECT_EQ(p.currentNodeIndex(), 1u);
    expectNear(p.scaleValue(), {1.75f, 1.75f}, 0.1f);  // scale track playing while the sequence rests
}

TEST(SpritePathDegenerate, EmptyNodesParkAtStart) {
    SpritePath p{.nodes = {}, .start = {33.0f, 44.0f}};
    run(p, 5, PlaybackMode::single());
    expectNear(p.position(), {33.0f, 44.0f}, 0.5f);
    EXPECT_TRUE(p.finished());

    SpritePath q{.nodes = {}, .start = {33.0f, 44.0f}};
    run(q, 5, PlaybackMode::loopIndefinitely());
    EXPECT_FALSE(q.finished());   // not finished under an indefinite loop
}

TEST(SpritePathDegenerate, ZeroDurationWrapDoesNotSpinAndParksAtStart) {
    SpritePath p{.nodes = {{.move = SpritePathMove::to({10.0f, 10.0f}, {10.0f, 10.0f}),  // zero-length → dur 0
                            .pacing = PathPacing::speed(300.0f)},
                           {.move = SpritePathMove::to({10.0f, 10.0f}, {10.0f, 10.0f}),
                            .pacing = PathPacing::speed(300.0f)}},
                 .start = {10.0f, 10.0f}};
    run(p, 5, PlaybackMode::loopIndefinitely());   // must not hang
    expectNear(p.position(), {10.0f, 10.0f}, 0.5f);
    EXPECT_FALSE(p.finished());
}

// ── Seek (§2.3) ────────────────────────────────────────────────────────────────────────────────────────

TEST(SpritePathSeek, SeekEqualsAdvanceFromZeroAcrossBoundaries) {
    const auto mk = [] {
        return SpritePath{.nodes = {leg({60.0f, 0.0f}, 300.0f), leg({60.0f, 60.0f}, 300.0f),
                                    leg({0.0f, 60.0f}, 300.0f)},
                          .start = {0.0f, 0.0f}};
    };
    SpritePath a = mk();
    SpritePath b = mk();
    a.restart();
    b.restart();
    run(a, 25, PlaybackMode::loopIndefinitely());  // advance-from-zero, crossing boundaries
    b.seek(ticks(25));                              // seek to the same tick count
    expectNear(a.position(), b.position(), 0.5f);
    EXPECT_EQ(a.currentNodeIndex(), b.currentNodeIndex());
}

TEST(SpritePathSeek, SeekDuringInterruptDrivesTheInterruptAndLeavesTheStack) {
    SpritePath p{.nodes = {leg({600.0f, 0.0f}, 600.0f)}, .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 10, PlaybackMode::single());
    p.interrupt({leg({0.0f, 120.0f}, 120.0f)}, PlaybackMode::loopIndefinitely());
    const Vec2 departure = p.position();
    p.seek(1s);  // 60 ticks into the detour
    EXPECT_TRUE(p.interrupted());
    EXPECT_EQ(p.interruptDepth(), 1u);
    const float moved = std::hypot(p.position().x - departure.x, p.position().y - departure.y);
    EXPECT_GT(moved, 50.0f);   // the seek drove the interrupt's own clock
}

// ── The interrupt stack (§2.3) ─────────────────────────────────────────────────────────────────────────

TEST(SpritePathInterrupt, DepartsFromTheCurrentPosition) {
    SpritePath p{.nodes = {leg({100.0f, 0.0f}, 60.0f)}, .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 10, PlaybackMode::single());
    const Vec2 here = p.position();
    p.interrupt({leg({0.0f, 60.0f}, 600.0f)});
    expectNear(p.position(), here, 1.0f);   // the detour's first node departs from where the sprite stands
    EXPECT_TRUE(p.interrupted());
    EXPECT_EQ(p.interruptDepth(), 1u);
}

TEST(SpritePathInterrupt, ExplicitPopResumesTheExactSuspendedSample) {
    SpritePath p{.nodes = {leg({120.0f, 0.0f}, 120.0f)}, .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 10, PlaybackMode::single());
    const SpritePathSample before        = p.sample;
    const std::uint64_t    elapsedBefore = p.elapsedTicks;
    p.interrupt({leg({0.0f, 40.0f}, 600.0f)}, PlaybackMode::loopIndefinitely(), ResumePolicy::Return);
    run(p, 20, PlaybackMode::single());
    EXPECT_TRUE(p.interrupted());
    p.popInterrupt();
    EXPECT_FALSE(p.interrupted());
    EXPECT_EQ(p.elapsedTicks, elapsedBefore);
    EXPECT_EQ(p.sample, before);   // Return: exact sample round-trip (snap back)
}

TEST(SpritePathInterrupt, ContinueResumesFromWhereTheDetourEndedDrifting) {
    const auto base = [] { return SpritePath{.nodes = {leg({600.0f, 0.0f}, 600.0f)}, .start = {0.0f, 0.0f}}; };
    SpritePath cont = base();  // Continue (default) — drifts on resume
    SpritePath ret  = base();  // Return — snaps back onto the base line
    cont.restart();
    ret.restart();
    run(cont, 10, PlaybackMode::single());  // both on the y == 0 base line, moving +x
    run(ret, 10, PlaybackMode::single());
    // The same detour departs from the current position and ends well off the base line (down at y = 80).
    const auto detour = [] { return std::vector<SpritePathNode>{leg({120.0f, 80.0f}, 600.0f)}; };
    cont.interrupt(detour(), PlaybackMode::single());                        // default Continue
    ret.interrupt(detour(), PlaybackMode::single(), ResumePolicy::Return);
    run(cont, 40, PlaybackMode::single());  // both detours finish, both resume the base, then advance equally
    run(ret, 40, PlaybackMode::single());
    EXPECT_FALSE(cont.interrupted());
    EXPECT_FALSE(ret.interrupted());
    EXPECT_NEAR(ret.position().y, 0.0f, 1.5f);   // Return: snapped back onto the base line
    EXPECT_GT(cont.position().y, 40.0f);          // Continue: the base drifted down by the detour's displacement
}

TEST(SpritePathInterrupt, AutoPopFlowsLeftoverTicksIntoTheResumedContent) {
    const auto base = [] { return SpritePath{.nodes = {leg({600.0f, 0.0f}, 600.0f)}, .start = {0.0f, 0.0f}}; };
    SpritePath a = base();  // interrupted
    SpritePath b = base();  // reference
    a.restart();
    b.restart();
    run(a, 10, PlaybackMode::single());
    run(b, 10, PlaybackMode::single());
    // A detour of a KNOWN 60px length (explicit origin) → ~6 ticks at 600 px/s under single().
    a.interrupt({{.move = SpritePathMove::to({0.0f, 0.0f}, {60.0f, 0.0f}), .pacing = PathPacing::speed(600.0f)}},
                PlaybackMode::single(), ResumePolicy::Return);
    a.advance(PlaybackMode::single(), 10);  // 6 consumed by the detour, 4 leftover flow into the base
    b.advance(PlaybackMode::single(), 4);   // the reference base advances by the leftover
    EXPECT_FALSE(a.interrupted());
    expectNear(a.position(), b.position(), 1.5f);
    EXPECT_EQ(a.elapsedTicks, b.elapsedTicks);  // both base clocks at 14
}

TEST(SpritePathInterrupt, DepthTwoCascadesOnResume) {
    SpritePath p{.nodes = {leg({600.0f, 0.0f}, 600.0f)}, .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 10, PlaybackMode::single());
    p.interrupt({leg({0.0f, 60.0f}, 600.0f)}, PlaybackMode::single());   // detour
    EXPECT_EQ(p.interruptDepth(), 1u);
    p.interrupt({leg({60.0f, 60.0f}, 600.0f)}, PlaybackMode::single());  // dodge on top
    EXPECT_EQ(p.interruptDepth(), 2u);
    p.advance(PlaybackMode::single(), 100);  // both finish → cascade back to the base
    EXPECT_FALSE(p.interrupted());
    EXPECT_EQ(p.interruptDepth(), 0u);
}

TEST(SpritePathInterrupt, LoopIndefinitelyInterruptNeverAutoPops) {
    SpritePath p{.nodes = {leg({600.0f, 0.0f}, 600.0f)}, .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 10, PlaybackMode::single());
    p.interrupt({leg({0.0f, 60.0f}, 600.0f)}, PlaybackMode::loopIndefinitely());
    run(p, 500, PlaybackMode::single());  // way past any finish
    EXPECT_TRUE(p.interrupted());          // a loop interrupt never auto-pops
    p.popInterrupt();
    EXPECT_FALSE(p.interrupted());          // popInterrupt is its only exit
}

TEST(SpritePathInterrupt, SurfacesReflectTheActiveContent) {
    SpritePath p{.nodes = {{.label = "leg0", .move = SpritePathMove::to({60.0f, 0.0f}),
                            .pacing = PathPacing::speed(600.0f)},
                           {.label = "leg1", .move = SpritePathMove::to({60.0f, 60.0f}),
                            .pacing = PathPacing::speed(600.0f)}},
                 .start = {0.0f, 0.0f}};
    p.restart();
    p.advance(PlaybackMode::single());
    EXPECT_EQ(p.currentNodeIndex(), 0u);
    EXPECT_EQ(p.currentNode()->label, "leg0");
    run(p, 8, PlaybackMode::single());   // into leg 1
    EXPECT_EQ(p.currentNodeIndex(), 1u);
    EXPECT_EQ(p.currentNode()->label, "leg1");
    p.interrupt({{.label = "detour", .move = SpritePathMove::to({0.0f, 60.0f}),
                  .pacing = PathPacing::speed(600.0f)}},
                PlaybackMode::loopIndefinitely());
    EXPECT_EQ(p.interruptDepth(), 1u);
    EXPECT_EQ(p.currentNode()->label, "detour");   // currentNode reflects the interrupt content
    p.popInterrupt();
    EXPECT_EQ(p.currentNode()->label, "leg1");      // back to the base leg
}

TEST(SpritePathInterrupt, StopAndRestartClearTheStack) {
    SpritePath p{.nodes = {leg({600.0f, 0.0f}, 600.0f)}, .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 10, PlaybackMode::single());
    p.interrupt({leg({0.0f, 60.0f}, 600.0f)}, PlaybackMode::loopIndefinitely());
    EXPECT_TRUE(p.interrupted());
    p.stop();
    EXPECT_FALSE(p.interrupted());
    EXPECT_EQ(p.interruptDepth(), 0u);
    p.interrupt({leg({0.0f, 60.0f}, 600.0f)}, PlaybackMode::loopIndefinitely());
    p.restart();
    EXPECT_FALSE(p.interrupted());
}

TEST(SpritePathInterrupt, BaseModeHonouredAfterResume) {
    SpritePath p{.nodes = {leg({60.0f, 0.0f}, 600.0f)}, .start = {0.0f, 0.0f}};  // ~6-tick pass
    p.restart();
    run(p, 20, PlaybackMode::single());   // base finished, rested at the end
    EXPECT_TRUE(p.finished());
    p.interrupt({leg({0.0f, 60.0f}, 600.0f)}, PlaybackMode::single(), ResumePolicy::Return);  // detour
    run(p, 20, PlaybackMode::single());   // detour finishes; base resumes under single() → still rested
    EXPECT_FALSE(p.interrupted());
    EXPECT_TRUE(p.finished());
    expectNear(p.position(), {60.0f, 0.0f}, 2.0f);
}

// ── The write envelope (§2.4) ──────────────────────────────────────────────────────────────────────────

TEST(SpritePathEnvelope, TransitionOffARotatingNodeWritesIdentityNotAFreeze) {
    SpritePath p{.nodes = {{.move           = SpritePathMove::to({60.0f, 0.0f}),
                            .pacing         = PathPacing::speed(600.0f),
                            .rotationDegrees = Tween<float>::of(0.0f, 90.0f, 1s, Easing::Linear),
                            .rotationMode   = PlaybackMode::single()},   // rotating node
                           leg({60.0f, 60.0f}, 600.0f)},                  // plain node, no rotation
                 .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 20, PlaybackMode::single());  // well into the plain node (rested at the end)
    Sprite s{.key = "m"};
    p.applyTo(s);
    // The plain node drives no rotation → the sample is identity; the union rule still WRITES the transform
    // (a rotating node exists), so the tumble STOPS at identity instead of freezing at the last rotation.
    const Transform identity = Transform::scale(1.0f, 1.0f, 4.0f, 4.0f).then(Transform::rotation(0.0f, 4.0f, 4.0f));
    EXPECT_EQ(s.transform, identity);
}

TEST(SpritePathEnvelope, FlipXWrittenWhenAnyNodeUsesItAndHeldAcrossNodes) {
    SpritePath p{.nodes = {{.move = SpritePathMove::to({-60.0f, 0.0f}), .pacing = PathPacing::speed(600.0f),
                            .facing = FacingPolicy::FlipX},              // travels -x → flipX true
                           leg({-60.0f, -60.0f}, 600.0f)},               // vertical, no FlipX policy
                 .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 4, PlaybackMode::single());   // node 0: traveling -x
    Sprite s0{.key = "m"};
    p.applyTo(s0);
    EXPECT_TRUE(s0.flipX);
    run(p, 10, PlaybackMode::single());  // into node 1 (no FlipX policy) — flipX HELD
    Sprite s1{.key = "m"};
    p.applyTo(s1);
    EXPECT_TRUE(s1.flipX);   // union write + held value carries across the node that doesn't drive it
}

TEST(SpritePathEnvelope, FrameFieldsHoldWhenCurrentNodeHasNoAnimation) {
    const Animation anim{.frames = {AnimationFrame{.palette = PaletteId{2}, .duration = 1s}}};
    SpritePath p{.nodes = {{.move = SpritePathMove::to({60.0f, 0.0f}), .pacing = PathPacing::speed(600.0f),
                            .animation = &anim},                        // animated node
                           leg({60.0f, 60.0f}, 600.0f)},                // no animation
                 .start = {0.0f, 0.0f}};
    p.restart();
    run(p, 4, PlaybackMode::single());
    Sprite s{.key = "m"};
    p.applyTo(s);                        // node 0 has an animation → writes the frame's palette
    EXPECT_EQ(s.palette, PaletteId{2});
    run(p, 10, PlaybackMode::single());  // into node 1 (no animation)
    Sprite s2{.key = "m"};
    s2.atlas   = AtlasId{5};
    s2.tile    = 9;                       // simulate the held art + palette from before
    s2.palette = PaletteId{7};
    p.applyTo(s2);                        // node 1 has no animation → frame fields left as-is, not cleared
    EXPECT_EQ(s2.tile, 9);
    EXPECT_EQ(s2.palette, PaletteId{7});
}

// ── Device-backed: applyTo's art-field write resolves through the sheet's slice geometry ─────────────

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in) << "could not open fixture: " << path;
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::string fixture(const char* name) { return std::string{RETROPP_FIXTURES_DIR} + "/" + name; }

// Windows on ARM is a courtesy runner with no production-representative GPU backend in CI; its production
// path (D3D12 + DXIL) is covered by the Windows x64 job, so a missing device there is an out-of-scope skip.
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

class SpritePathArtDevice : public ::testing::Test {
protected:
    static inline SDL_GPUDevice* device_ = nullptr;
    static inline std::string    initError_;

    static void SetUpTestSuite() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            initError_ = std::string("SDL_Init(SDL_INIT_VIDEO) failed: ") + SDL_GetError();
            return;
        }
        device_ = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
            /*debug_mode=*/false, /*name=*/nullptr);
        if (!device_) initError_ = std::string("SDL_CreateGPUDevice failed: ") + SDL_GetError();
    }

    static void TearDownTestSuite() {
        if (device_) {
            SDL_DestroyGPUDevice(device_);
            device_ = nullptr;
        }
        SDL_Quit();
    }

    void SetUp() override {
        if (!device_) {
            if (kDeviceOptional) {
                GTEST_SKIP() << "Windows on ARM is a courtesy runner with no production-representative GPU "
                                "backend in CI; the write table and envelope are covered device-free above. ("
                             << initError_ << ")";
            }
            FAIL() << "no GPU device reachable — " << initError_
                   << ". applyTo's art-field write resolves the frame through the engine renderer's recorded "
                      "slice geometry and so needs a GPU device on every production-representative platform "
                      "(a software rasterizer suffices; on a headless runner set SDL_VIDEODRIVER=offscreen).";
        }
    }

    // sheet16x32.png (gen_fixtures.py): a 2×2 grid of 8×16 assets — cells {0, 1, 4, 5}, so slot 2
    // resolves to cell 4 (index ≠ cell: the write must go through the sheet, never echo the index).
    static AtlasManifest loadSheet(Renderer& r) {
        return r.loadAtlasFromMemory(readFile(fixture("sheet16x32.png")), AssetDimensions{8, 16},
                                     ContentKind::SpriteSeries);
    }
};

TEST_F(SpritePathArtDevice, ApplyToWritesTheResolvedFrameArtFields) {
    Renderer r{device_, nullptr};  // compose-only (no window) — the one engine renderer
    const AtlasManifest sheet = loadSheet(r);
    const Animation anim{.frames = {AnimationFrame{.sheet = sheet.atlasId, .tileIndex = 2,
                                                   .palette = PaletteId{3}, .duration = 1s}}};
    SpritePath p = parkedPath();
    p.nodes[0].animation = &anim;
    p.advance();
    Sprite s{.key = "mover"};
    p.applyTo(s);
    EXPECT_EQ(s.atlas, sheet.atlasId);
    EXPECT_EQ(s.tile, 4);                            // slot 2 → cell 4, resolved through the sheet
    EXPECT_EQ(s.size, (AssetDimensions{8, 16}));
    EXPECT_EQ(s.palette, PaletteId{3});
    EXPECT_EQ(s.tile, p.frame()->tile());            // applyTo ↔ frame-read parity on the art fields
    EXPECT_EQ(s.size, p.frame()->size());
}

TEST_F(SpritePathArtDevice, DefaultPivotUsesTheFrameSizeAfterTheFrameWrite) {
    Renderer r{device_, nullptr};
    const AtlasManifest sheet = loadSheet(r);
    const Animation anim{.frames = {AnimationFrame{.sheet = sheet.atlasId, .tileIndex = 0,
                                                   .palette = PaletteId{0}, .duration = 1s}}};
    SpritePath p = parkedPath();
    p.nodes[0].animation       = &anim;
    p.nodes[0].rotationDegrees = Tween<float>::of(90.0f, 90.0f, 1s, Easing::Linear);
    p.advance();
    Sprite s{.key = "mover"};
    p.applyTo(s);  // size becomes 8×16 → center pivot (4, 8)
    const Transform expected =
        Transform::scale(1.0f, 1.0f, 4.0f, 8.0f).then(Transform::rotation(90.0f, 4.0f, 8.0f));
    EXPECT_EQ(s.transform, expected);
}

}  // namespace
}  // namespace retropp
