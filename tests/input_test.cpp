#include <gtest/gtest.h>

#include "retropp/analog_input.h"
#include "retropp/input.h"

namespace retropp {
namespace {

// The test game's vocabulary. Hi pins the top of the 64-action range.
enum class Act : std::uint8_t { Up, Down, Left, Right, Fire, Alt, Move, Start = 10, Hi = 63 };

ActionSet only(Act a) {
    ActionSet s;
    s.set(actionId(a), true);
    return s;
}

InputSample sampleOf(ActionSet level, int player = 0) {
    InputSample s;
    s.players[static_cast<std::size_t>(player)].held = level;
    return s;
}

// Most digital cases want the common case: an action active this tick is also the press source for
// the tick (heldUnion == level — what the run loop produces for a continuously-held action). This
// helper keeps those readable while sampleTick's real signature is (sample, pressedSinceTick).
void step(InputState& in, ActionSet level, int player = 0) {
    std::array<ActionSet, kMaxPlayers> pressed{};
    pressed[static_cast<std::size_t>(player)] = level;
    in.sampleTick(sampleOf(level, player), pressed);
}

void stepAnalog(InputState& in, const AnalogInput& a) {
    InputSample s;
    s.players[0].analog = a;
    in.sampleTick(s, {});
}

}  // namespace

TEST(ActionSet, DefaultIsAllInactive) {
    ActionSet s;
    EXPECT_EQ(s.bits(), 0u);
    EXPECT_FALSE(s.test(actionId(Act::Fire)));
}

TEST(ActionSet, SetAndTestRoundTripPerAction) {
    ActionSet s;
    s.set(actionId(Act::Start), true);
    EXPECT_TRUE(s.test(actionId(Act::Start)));
    EXPECT_FALSE(s.test(actionId(Act::Fire)));  // a neighboring bit is untouched
    s.set(actionId(Act::Start), false);
    EXPECT_FALSE(s.test(actionId(Act::Start)));
}

TEST(ActionSet, BitsArePackedOneBitPerAction) {
    ActionSet s;
    s.set(actionId(Act::Up), true);     // bit 0
    s.set(actionId(Act::Start), true);  // bit 10
    EXPECT_EQ(s.bits(), (std::uint64_t{1} << 0) | (std::uint64_t{1} << 10));
}

TEST(ActionSet, HoldsTheFullSixtyFourActionRange) {
    // The regression guard for the 64-action capacity: the top bit works like the bottom one.
    ActionSet s;
    s.set(actionId(Act::Hi), true);  // bit 63
    EXPECT_TRUE(s.test(actionId(Act::Hi)));
    EXPECT_FALSE(s.test(actionId(Act::Up)));
    EXPECT_EQ(s.bits(), std::uint64_t{1} << 63);
}

TEST(ActionSet, EqualityComparesActiveState) {
    ActionSet a;
    ActionSet b;
    EXPECT_EQ(a, b);
    a.set(actionId(Act::Alt), true);
    EXPECT_NE(a, b);
    b.set(actionId(Act::Alt), true);
    EXPECT_EQ(a, b);
}

TEST(ActionSet, UnionMergesActiveBitsOfBothSets) {
    // The press-buffering union the run loop relies on: OR keeps every action active in either set.
    ActionSet a = only(Act::Fire);
    ActionSet right = only(Act::Right);
    EXPECT_EQ((a | right).bits(), only(Act::Fire).bits() | only(Act::Right).bits());
    a |= right;
    EXPECT_TRUE(a.test(actionId(Act::Fire)));
    EXPECT_TRUE(a.test(actionId(Act::Right)));
}

TEST(InputState, IsHeldReflectsTheSampledLevel) {
    InputState in;
    step(in, only(Act::Fire));
    EXPECT_TRUE(in.isHeld(Act::Fire));
    EXPECT_FALSE(in.isHeld(Act::Alt));
}

TEST(InputState, JustPressedFiresOnlyOnTheTransitionTick) {
    InputState in;
    const ActionSet down = only(Act::Fire);

    step(in, down);  // inactive (baseline) → active: pressed edge on tick 1
    EXPECT_TRUE(in.justPressed(Act::Fire));
    EXPECT_TRUE(in.isHeld(Act::Fire));

    step(in, down);  // active → active: no edge
    EXPECT_FALSE(in.justPressed(Act::Fire));
    EXPECT_TRUE(in.isHeld(Act::Fire));
}

TEST(InputState, JustReleasedFiresOnlyOnTheReleaseTick) {
    InputState in;
    const ActionSet down = only(Act::Alt);
    const ActionSet up;  // all inactive

    step(in, down);
    step(in, up);  // active → inactive: release edge
    EXPECT_TRUE(in.justReleased(Act::Alt));
    EXPECT_FALSE(in.isHeld(Act::Alt));

    step(in, up);  // inactive → inactive: no edge
    EXPECT_FALSE(in.justReleased(Act::Alt));
}

TEST(InputState, ActionHeldAcrossManyTicksPressesOnce) {
    InputState in;
    const ActionSet down = only(Act::Left);

    step(in, down);
    EXPECT_TRUE(in.justPressed(Act::Left));
    for (int i = 0; i < 5; ++i) {
        step(in, down);
        EXPECT_FALSE(in.justPressed(Act::Left));
    }
    EXPECT_TRUE(in.isHeld(Act::Left));
}

// ── Press buffering ───────────────────────────────────────────────────────────────────────────────

TEST(InputState, SubTickTapStillRegistersAsAPress) {
    // A tap whose action was seen active in a frame poll (pressedSinceTick) but is already released
    // by tick time (level = inactive) fires justPressed exactly once — the press-buffering
    // guarantee. isHeld is honest (false), so nothing sticks.
    InputState in;
    std::array<ActionSet, kMaxPlayers> pressed{};
    pressed[0] = only(Act::Fire);
    in.sampleTick(sampleOf(ActionSet{}), pressed);
    EXPECT_TRUE(in.justPressed(Act::Fire));
    EXPECT_FALSE(in.isHeld(Act::Fire));

    // The following tick with nothing pressed must NOT re-fire it.
    in.sampleTick(sampleOf(ActionSet{}), {});
    EXPECT_FALSE(in.justPressed(Act::Fire));
}

TEST(InputState, HeldDirectionDoesNotSuppressABufferedFireTap) {
    // The canonical scenario: a direction held continuously while fire is tapped between ticks.
    // The direction stays active (no press edge after the first), and the fire tap registers.
    InputState in;
    const ActionSet right = only(Act::Right);
    step(in, right);  // start holding Right
    EXPECT_TRUE(in.justPressed(Act::Right));

    // Next tick: Right still held; Fire was tapped and released between ticks (in the union only).
    std::array<ActionSet, kMaxPlayers> pressed{};
    pressed[0] = right | only(Act::Fire);
    in.sampleTick(sampleOf(right), pressed);
    EXPECT_TRUE(in.isHeld(Act::Right));
    EXPECT_FALSE(in.justPressed(Act::Right));  // direction does not re-press
    EXPECT_TRUE(in.justPressed(Act::Fire));    // the fire tap is not dropped
    EXPECT_FALSE(in.isHeld(Act::Fire));
}

// ── Player slots ──────────────────────────────────────────────────────────────────────────────────

TEST(InputState, SlotsAreIsolated) {
    InputState in;
    step(in, only(Act::Fire), /*player=*/1);
    EXPECT_TRUE(in.player(1).isHeld(Act::Fire));
    EXPECT_FALSE(in.player(0).isHeld(Act::Fire));  // slot 1's press is invisible to slot 0
    EXPECT_FALSE(in.isHeld(Act::Fire));            // the direct methods ARE slot 0
}

TEST(InputState, DirectMethodsAreThePlayerZeroView) {
    InputState in;
    step(in, only(Act::Up));
    EXPECT_EQ(in.isHeld(Act::Up), in.player(0).isHeld(Act::Up));
    EXPECT_EQ(in.justPressed(Act::Up), in.player(0).justPressed(Act::Up));
    EXPECT_EQ(in.activeDevice(), in.player(0).activeDevice());
}

TEST(InputState, PerSlotEdgesAreIndependent) {
    InputState in;
    // Tick 1: both players press Fire.
    InputSample s = sampleOf(only(Act::Fire), 0);
    s.players[1].held = only(Act::Fire);
    std::array<ActionSet, kMaxPlayers> pressed{};
    pressed[0] = only(Act::Fire);
    pressed[1] = only(Act::Fire);
    in.sampleTick(s, pressed);
    EXPECT_TRUE(in.player(0).justPressed(Act::Fire));
    EXPECT_TRUE(in.player(1).justPressed(Act::Fire));

    // Tick 2: player 0 releases, player 1 keeps holding — each slot edges on its own history.
    InputSample s2;
    s2.players[1].held = only(Act::Fire);
    std::array<ActionSet, kMaxPlayers> pressed2{};
    pressed2[1] = only(Act::Fire);
    in.sampleTick(s2, pressed2);
    EXPECT_TRUE(in.player(0).justReleased(Act::Fire));
    EXPECT_FALSE(in.player(1).justReleased(Act::Fire));
    EXPECT_TRUE(in.player(1).isHeld(Act::Fire));
    EXPECT_FALSE(in.player(1).justPressed(Act::Fire));
}

// ── The active-device signal ──────────────────────────────────────────────────────────────────────

TEST(InputState, ActiveDeviceDefaultsToNone) {
    InputState in;
    EXPECT_EQ(in.activeDevice(), (ActiveDevice{DeviceKind::None, ControllerType::Unknown}));
}

TEST(InputState, ActiveDevicePassesThroughPerSlot) {
    InputState in;
    InputSample s;
    s.players[0].device = ActiveDevice{DeviceKind::KeyboardMouse, ControllerType::Unknown};
    s.players[1].device = ActiveDevice{DeviceKind::Gamepad, ControllerType::Nintendo};
    in.sampleTick(s, {});
    EXPECT_EQ(in.activeDevice().kind, DeviceKind::KeyboardMouse);
    EXPECT_EQ(in.player(1).activeDevice().kind, DeviceKind::Gamepad);
    EXPECT_EQ(in.player(1).activeDevice().family, ControllerType::Nintendo);
}

// ── Valued actions ────────────────────────────────────────────────────────────────────────────────

TEST(InputState, VectorAndAxisReadThePerActionValues) {
    InputState in;
    InputSample s;
    s.players[0].values[actionId(Act::Move)] = Vec2{-0.5f, 0.25f};
    s.players[0].values[actionId(Act::Alt)]  = Vec2{0.7f, 0.0f};  // a trigger pull rides x
    in.sampleTick(s, {});
    EXPECT_FLOAT_EQ(in.vector(Act::Move).x, -0.5f);
    EXPECT_FLOAT_EQ(in.vector(Act::Move).y, 0.25f);
    EXPECT_FLOAT_EQ(in.axis(Act::Alt), 0.7f);
    EXPECT_FLOAT_EQ(in.axis(Act::Move), -0.5f);  // axis IS the x component
}

TEST(InputState, ValuesAreAbsoluteLatestAtTick) {
    InputState in;
    InputSample s;
    s.players[0].values[actionId(Act::Move)] = Vec2{1.0f, 0.0f};
    in.sampleTick(s, {});
    s.players[0].values[actionId(Act::Move)] = Vec2{0.0f, -1.0f};
    in.sampleTick(s, {});
    EXPECT_FLOAT_EQ(in.vector(Act::Move).x, 0.0f);   // the latest value, not a sum
    EXPECT_FLOAT_EQ(in.vector(Act::Move).y, -1.0f);
}

// ── Analog / pointer surface ──────────────────────────────────────────────────────────────────────

TEST(InputState, StoresCursorAndOnScreenFlag) {
    InputState in;
    AnalogInput a;
    a.cursor = Vec2i{40, 72};
    a.cursorOnScreen = true;
    stepAnalog(in, a);
    EXPECT_EQ(in.cursor(), (Vec2i{40, 72}));
    EXPECT_TRUE(in.cursorOnScreen());
}

TEST(InputState, CursorDeltaIsThePerTickViewportChange) {
    InputState in;
    AnalogInput a;
    a.cursor = Vec2i{10, 10};
    stepAnalog(in, a);  // first sample: prev is default {0,0}
    EXPECT_EQ(in.cursorDelta(), (Vec2i{10, 10}));

    a.cursor = Vec2i{13, 7};
    stepAnalog(in, a);
    EXPECT_EQ(in.cursorDelta(), (Vec2i{3, -3}));
}

TEST(InputState, RawDeltaAndWheelArePassedThroughFromTheSample) {
    InputState in;
    AnalogInput a;
    a.rawDeltaX = 4.5f;
    a.rawDeltaY = -2.0f;
    a.wheel = 1.0f;
    stepAnalog(in, a);
    EXPECT_FLOAT_EQ(in.rawDeltaX(), 4.5f);
    EXPECT_FLOAT_EQ(in.rawDeltaY(), -2.0f);
    EXPECT_FLOAT_EQ(in.wheel(), 1.0f);
}

TEST(InputState, MouseButtonEdgesMirrorTheDigitalEdges) {
    InputState in;
    AnalogInput up;          // no mouse buttons
    AnalogInput leftDown;
    leftDown.mouseHeld = std::uint8_t{1} << static_cast<int>(MouseButton::Left);

    stepAnalog(in, leftDown);  // released → held
    EXPECT_TRUE(in.mouseHeld(MouseButton::Left));
    EXPECT_TRUE(in.mouseJustPressed(MouseButton::Left));
    EXPECT_FALSE(in.mouseJustReleased(MouseButton::Left));

    stepAnalog(in, leftDown);  // held → held
    EXPECT_FALSE(in.mouseJustPressed(MouseButton::Left));

    stepAnalog(in, up);        // held → released
    EXPECT_FALSE(in.mouseHeld(MouseButton::Left));
    EXPECT_TRUE(in.mouseJustReleased(MouseButton::Left));
}

TEST(AnalogInput, MouseDownTestsTheRightBit) {
    AnalogInput a;
    a.mouseHeld = std::uint8_t{1} << static_cast<int>(MouseButton::Right);
    EXPECT_TRUE(a.mouseDown(MouseButton::Right));
    EXPECT_FALSE(a.mouseDown(MouseButton::Left));
    EXPECT_FALSE(a.mouseDown(MouseButton::Middle));
}

TEST(InputState, SticksAndTriggersPassThrough) {
    InputState in;
    AnalogInput a;
    a.leftX = -0.5f;  a.leftY = 0.25f;
    a.rightX = 0.9f;  a.rightY = -0.1f;
    a.triggerL = 0.3f; a.triggerR = 0.7f;
    stepAnalog(in, a);
    EXPECT_FLOAT_EQ(in.stick(Stick::Left).x, -0.5f);
    EXPECT_FLOAT_EQ(in.stick(Stick::Left).y, 0.25f);
    EXPECT_FLOAT_EQ(in.stick(Stick::Right).x, 0.9f);
    EXPECT_FLOAT_EQ(in.stick(Stick::Right).y, -0.1f);
    EXPECT_FLOAT_EQ(in.trigger(Trigger::Left), 0.3f);
    EXPECT_FLOAT_EQ(in.trigger(Trigger::Right), 0.7f);
}

}  // namespace retropp
