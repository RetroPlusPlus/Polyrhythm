// Device-free coverage for the window surface: the Window object behind platform.window() (noun
// setter/getter pairs for position/size/fullscreen, drawn-Region drag handles, automatic movement)
// and the aggregate platform.window(WindowState{...}) door. Driven against MockPlatform — no live
// window — so the whole surface is pinned headlessly: the noun pairs and their repeat-call no-op
// rule, region containment through the registered predicate (dragHit — the same call the production
// OS hit-test makes), the automatic mover's per-frame arithmetic (trigger gating, per-source
// contributions, the motion-set-replaces-the-default rule, remainder banking), and the aggregate
// door's engaged-fields-only application. The live SDL_SetWindowHitTest drag and real
// SDL_SetWindowPosition are exercised by running the window_drag example, not here.

#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/platform.h"
#include "retropp/window.h"

#include "mock_platform.h"

namespace retropp {
namespace {

using test::MockPlatform;

// One 60 Hz frame — the period the windowed host hands update() each frame.
constexpr std::chrono::nanoseconds kFrame{16'666'667};

// A sample whose player-0 slot holds `trigger` with the given analog surface.
template <typename A>
InputSample sampleWith(A trigger, const AnalogInput& analog) {
    InputSample sample;
    sample.players[0].held.set(actionId(trigger), true);
    sample.players[0].analog = analog;
    return sample;
}

// A drawn title bar the tests declare draggable — a Region exactly as a game builds one.
Region titleBar() {
    return Region{.key   = "titlebar",
                  .shape = ShapePoints::rectangle(Point{0.0f, 0.0f}, 160.0f, 12.0f)};
}

enum class Action : std::uint8_t { Grab, Other };

// ── Window position (seam primitive + the Window pair) ─────────────────────────

TEST(WindowPosition, DefaultsToOrigin) {
    MockPlatform platform{1};
    Platform& seam = platform;  // exercise through the abstract interface
    EXPECT_EQ(seam.windowPosition(), (Vec2i{0, 0}));
}

TEST(WindowPosition, SeamRoundTripsThePosition) {
    MockPlatform platform{1};
    Platform& seam = platform;

    seam.windowPosition(Vec2i{120, 64});
    EXPECT_EQ(seam.windowPosition(), (Vec2i{120, 64}));

    // Negative coordinates are legitimate on a multi-monitor desktop.
    seam.windowPosition(Vec2i{-40, -8});
    EXPECT_EQ(seam.windowPosition(), (Vec2i{-40, -8}));
}

TEST(WindowPosition, WindowObjectSubmitsAndReads) {
    MockPlatform platform{1};

    EXPECT_EQ(platform.window().position(), (Vec2i{0, 0}));
    platform.window().position(Vec2i{300, 200});
    EXPECT_EQ(platform.window().position(), (Vec2i{300, 200}));
    EXPECT_EQ(platform.windowPosition(), (Vec2i{300, 200}));  // the same window state, one seam
}

// ── Window size + fullscreen (the Window pairs) ────────────────────────────────

TEST(WindowSize, PairRoundTrips) {
    MockPlatform platform{1};

    platform.window().size(PixelSize{320, 288});
    EXPECT_EQ(platform.window().size(), (PixelSize{320, 288}));
    EXPECT_EQ(platform.drawableSize(), (PixelSize{320, 288}));  // mock reflects logical == physical
}

TEST(WindowFullscreen, PairRoundTrips) {
    MockPlatform platform{1};

    EXPECT_FALSE(platform.window().fullscreen());  // windowed by default
    platform.window().fullscreen(true);
    EXPECT_TRUE(platform.window().fullscreen());
    platform.window().fullscreen(false);
    EXPECT_FALSE(platform.window().fullscreen());
}

// ── Repeat-call rule: same value → no-op, only new values apply ────────────────

TEST(WindowRepeatCalls, SameValueNeverReappliesOverAnActualMove) {
    MockPlatform platform{1};

    platform.window().position(Vec2i{40, 40});
    EXPECT_EQ(platform.windowPosition(), (Vec2i{40, 40}));

    // The window moves for a reason outside this surface (a native drag). Re-stating the SAME value
    // is a no-op — the window stays where the user put it.
    platform.windowPosition(Vec2i{100, 100});
    platform.window().position(Vec2i{40, 40});
    EXPECT_EQ(platform.windowPosition(), (Vec2i{100, 100}));

    // A NEW value applies.
    platform.window().position(Vec2i{41, 40});
    EXPECT_EQ(platform.windowPosition(), (Vec2i{41, 40}));
}

TEST(WindowRepeatCalls, FullscreenSameValueIsANoOp) {
    MockPlatform platform{1};

    platform.window().fullscreen(false);  // first call: applies (mock already windowed)
    platform.fullscreen(true);            // flipped outside this surface
    platform.window().fullscreen(false);  // same value as last set → no-op
    EXPECT_TRUE(platform.fullscreen());   // the outside flip stands

    platform.window().fullscreen(true);   // a new value would apply…
    platform.window().fullscreen(false);  // …and so does leaving again
    EXPECT_FALSE(platform.fullscreen());
}

// ── The aggregate door: platform.window(WindowState{...}) ──────────────────────

TEST(WindowAggregate, AppliesEngagedFieldsOnly) {
    MockPlatform platform{1};

    platform.window(WindowState{.size = PixelSize{480, 432}, .fullscreen = true});
    EXPECT_EQ(platform.window().size(), (PixelSize{480, 432}));
    EXPECT_TRUE(platform.window().fullscreen());
    EXPECT_EQ(platform.windowPosition(), (Vec2i{0, 0}));  // position was not engaged — untouched
}

TEST(WindowAggregate, OmittedFieldsAreUntouched) {
    MockPlatform platform{1};

    platform.window().position(Vec2i{50, 60});
    platform.window(WindowState{.fullscreen = true});     // position omitted
    EXPECT_EQ(platform.windowPosition(), (Vec2i{50, 60}));
}

TEST(WindowAggregate, BothDoorsHitTheSameState) {
    MockPlatform platform{1};

    platform.window(WindowState{.position    = Vec2i{7, 8},
                                .dragHandles = std::vector<Region>{titleBar()},
                                .autoMove    = WindowMovement{.trigger = Action::Grab}});
    EXPECT_EQ(platform.window().position(), (Vec2i{7, 8}));   // read back through the object door
    EXPECT_TRUE(platform.dragHit(Vec2i{80, 6}));              // the aggregate's handles are in effect
    EXPECT_TRUE(platform.window().autoMove().has_value());    // and so is its movement
}

// ── Drag handles (drawn Regions, containment through the registered predicate) ──

TEST(DragHandles, NoDeclarationNeverHits) {
    MockPlatform platform{1};
    EXPECT_FALSE(platform.dragHit(Vec2i{80, 6}));  // no handles declared → an ordinary window
}

TEST(DragHandles, TitleBarDragsInsideOnly) {
    MockPlatform platform{1};

    // The declaration a game makes: the drawn title bar itself.
    platform.window().dragHandles({titleBar()});

    // The platform-side query — exactly what the production OS hit-test asks.
    EXPECT_TRUE(platform.dragHit(Vec2i{80, 6}));    // squarely inside the bar
    EXPECT_TRUE(platform.dragHit(Vec2i{0, 0}));     // the top-left pixel's center is inside
    EXPECT_FALSE(platform.dragHit(Vec2i{80, 12}));  // the first row below the bar
    EXPECT_FALSE(platform.dragHit(Vec2i{80, 100})); // the window body

    // Re-declaring empty clears the handles.
    platform.window().dragHandles({});
    EXPECT_FALSE(platform.dragHit(Vec2i{80, 6}));
}

TEST(DragHandles, SeveralHandlesAnyHitCounts) {
    MockPlatform platform{1};

    const Region corner{.key   = "corner",
                        .shape = ShapePoints::rectangle(Point{0.0f, 0.0f}, 20.0f, 12.0f)};
    const Region knob{.key = "knob", .shape = ShapePoints::circle(Point{150.0f, 6.0f}, 6.0f)};
    platform.window().dragHandles({corner, knob});

    EXPECT_TRUE(platform.dragHit(Vec2i{10, 6}));    // in the corner handle
    EXPECT_TRUE(platform.dragHit(Vec2i{150, 6}));   // in the round knob
    EXPECT_FALSE(platform.dragHit(Vec2i{80, 6}));   // between them
}

// ── Automatic window movement ──────────────────────────────────────────────────

TEST(AutomaticWindowMovement, NoDeclarationNeverMoves) {
    MockPlatform platform{1};

    AnalogInput analog;
    analog.rawDeltaX = 10.0f;
    analog.rawDeltaY = 5.0f;
    platform.window().update(sampleWith(Action::Grab, analog), kFrame);
    EXPECT_EQ(platform.windowPosition(), (Vec2i{0, 0}));
}

TEST(AutomaticWindowMovement, PointerDragsWhileTriggerHeld) {
    MockPlatform platform{1};
    platform.window().autoMove({.trigger = Action::Grab});

    AnalogInput analog;
    analog.rawDeltaX = 10.0f;
    analog.rawDeltaY = -4.0f;
    platform.window().update(sampleWith(Action::Grab, analog), kFrame);
    EXPECT_EQ(platform.windowPosition(), (Vec2i{10, -4}));  // pointer delta passes through 1:1

    // Trigger not held → the same motion moves nothing.
    platform.window().update(sampleWith(Action::Other, analog), kFrame);
    EXPECT_EQ(platform.windowPosition(), (Vec2i{10, -4}));
}

TEST(AutomaticWindowMovement, StickScalesPerSecond) {
    MockPlatform platform{1};
    // The drag rate is 240 pt/s at full deflection → 4 pt per 60 Hz frame.
    platform.window().autoMove({.trigger = Action::Grab, .motion = {MotionSource::LeftStick}});

    AnalogInput analog;
    analog.leftX = 1.0f;   // full right deflection
    analog.leftY = 0.0f;
    platform.window().update(sampleWith(Action::Grab, analog), kFrame);
    EXPECT_EQ(platform.windowPosition(), (Vec2i{4, 0}));
}

TEST(AutomaticWindowMovement, MotionSetReplacesTheDefault) {
    MockPlatform platform{1};
    // Dpad only — the pointer is NOT in the set, so mouse motion does not drive this drag.
    platform.window().autoMove({.trigger = Action::Grab, .motion = {MotionSource::Dpad}});

    AnalogInput analog;
    analog.rawDeltaX = 50.0f;  // mouse motion — must be ignored
    analog.dpadX     = 1.0f;   // d-pad right
    platform.window().update(sampleWith(Action::Grab, analog), kFrame);
    EXPECT_EQ(platform.windowPosition(), (Vec2i{4, 0}));  // dpad × rate only, no pointer contribution
}

TEST(AutomaticWindowMovement, NoneTurnsMovementOff) {
    MockPlatform platform{1};
    platform.window().autoMove({.trigger = Action::Grab});
    EXPECT_TRUE(platform.window().autoMove().has_value());

    platform.window().autoMove(WindowMovement::None);        // declare the movement off
    EXPECT_FALSE(platform.window().autoMove().has_value());  // none in effect — the default state

    AnalogInput analog;
    analog.rawDeltaX = 10.0f;
    platform.window().update(sampleWith(Action::Grab, analog), kFrame);
    EXPECT_EQ(platform.windowPosition(), (Vec2i{0, 0}));  // off — nothing moves
}

TEST(AutomaticWindowMovement, EmptyMotionSetMovesNothing) {
    MockPlatform platform{1};
    platform.window().autoMove({.trigger = Action::Grab, .motion = {}});

    AnalogInput analog;
    analog.rawDeltaX = 50.0f;
    analog.leftX     = 1.0f;
    analog.dpadX     = 1.0f;
    platform.window().update(sampleWith(Action::Grab, analog), kFrame);
    EXPECT_EQ(platform.windowPosition(), (Vec2i{0, 0}));  // driven by nothing
}

TEST(AutomaticWindowMovement, FractionalMotionBanksAcrossFrames) {
    MockPlatform platform{1};
    // At 240 pt/s an eighth deflection is 0.5 pt per 60 Hz frame, so the window advances one point
    // every second frame instead of stalling.
    platform.window().autoMove({.trigger = Action::Grab, .motion = {MotionSource::RightStick}});

    AnalogInput analog;
    analog.rightX = 0.125f;
    const InputSample sample = sampleWith(Action::Grab, analog);
    platform.window().update(sample, kFrame);
    EXPECT_EQ(platform.windowPosition(), (Vec2i{0, 0}));  // 0.5 banked
    platform.window().update(sample, kFrame);
    EXPECT_EQ(platform.windowPosition(), (Vec2i{1, 0}));  // 1.0 reached
}

TEST(AutomaticWindowMovement, RemainderClearsWhenTriggerReleases) {
    MockPlatform platform{1};
    platform.window().autoMove({.trigger = Action::Grab, .motion = {MotionSource::RightStick}});

    AnalogInput analog;
    analog.rightX = 0.125f;
    platform.window().update(sampleWith(Action::Grab, analog), kFrame);   // banks 0.5
    platform.window().update(sampleWith(Action::Other, analog), kFrame);  // released — the bank clears
    platform.window().update(sampleWith(Action::Grab, analog), kFrame);   // a fresh grab starts from zero
    EXPECT_EQ(platform.windowPosition(), (Vec2i{0, 0}));                  // 0.5 again, not 1.0
}

}  // namespace
}  // namespace retropp
