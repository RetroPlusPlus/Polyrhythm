#include <gtest/gtest.h>

#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/run_loop.h"
#include "retropp/timing.h"
#include "retropp/windowed_host.h"

#include "manual_clock.h"
#include "mock_platform.h"

namespace retropp {
namespace {

using test::ManualClock;
using test::MockPlatform;

// The default-profile tick period (GBC) these cases advance the clock by, read from the profile.
constexpr auto kTickPeriod = TimingProfile::GameBoyColor.tickPeriod();

enum class Act : std::uint8_t { Fire, Right, Pause };

// A platform that quits after N pumps drives exactly N host iterations, and the render
// callback fires once per iteration.
TEST(WindowedHost, QuitAfterNPumpsRunsExactlyNIterations) {
    ManualClock  clock;
    RunLoop      loop{clock};
    MockPlatform platform{5};

    int renders = 0;
    loop.renderLoop([&] { ++renders; });

    WindowedHost host{loop, platform};
    host.run();

    EXPECT_EQ(platform.pumpCount(), 5);
    EXPECT_EQ(renders, 5);  // render fired once per iteration
}

// A platform already quit-requested at startup routes that close through the exit guard like any other
// window-close (the guard is the ONE path every exit takes). With no guard registered it Proceeds on
// the first frame boundary, so the host runs exactly one iteration — one pump, one present, no ticks
// (the clock never advances) — then stops.
TEST(WindowedHost, ImmediateQuitRoutesThroughGuardAndStopsAfterOneIteration) {
    ManualClock  clock;
    RunLoop      loop{clock};
    MockPlatform platform{0};

    int renders = 0;
    loop.renderLoop([&] { ++renders; });

    WindowedHost host{loop, platform};
    host.run();

    EXPECT_EQ(platform.pumpCount(), 1);
    EXPECT_EQ(renders, 1);
    EXPECT_EQ(loop.tickCount(), 0u);
    EXPECT_TRUE(loop.exitResolved());
}

// Each iteration pushes the platform's current InputSample into the loop BEFORE
// advancing, so a held action supplied by the platform reaches the tick callback.
// The per-pump hook advances the injected clock by one tick period, so a tick fires
// each iteration and samples the pushed input.
TEST(WindowedHost, PushedInputReachesTickCallback) {
    ManualClock  clock;
    RunLoop      loop{clock};
    MockPlatform platform{6};
    platform.setOnPump([&] { clock.advanceBy(kTickPeriod); });

    ActionSet held;
    held.set(actionId(Act::Fire), true);
    held.set(actionId(Act::Right), true);
    platform.setHeld(held);

    bool sawFire = false, sawRight = false, sawPause = false;
    loop.simTick([&](const InputState& in) {
        if (in.isHeld(Act::Fire))  sawFire = true;
        if (in.isHeld(Act::Right)) sawRight = true;
        if (in.isHeld(Act::Pause)) sawPause = true;  // never held → must stay false
    });

    WindowedHost host{loop, platform};
    host.run();

    EXPECT_GT(loop.tickCount(), 0u);  // ticks actually fired
    EXPECT_TRUE(sawFire);
    EXPECT_TRUE(sawRight);
    EXPECT_FALSE(sawPause);
}

// Ordering guard: each iteration pushes input BEFORE advancing, so a tick firing on
// that iteration observes the input pushed on the same iteration. The loop's baseline
// is pre-consumed with one manual advance() so the host's very first iteration already
// produces a tick — which therefore sees the pushed input only if setRawInput precedes
// advance. Swapping the host's pump/setRawInput/advance order turns this red.
TEST(WindowedHost, PushedInputIsObservedOnTheSameIteration) {
    ManualClock  clock;
    RunLoop      loop{clock};

    int  ticks = 0;
    bool pressedFire = false;
    loop.simTick([&](const InputState& in) {
        ++ticks;
        if (in.justPressed(Act::Fire)) pressedFire = true;
    });

    // Consume the lazy baseline (started_ = true, last_ = now) with no tick, so the
    // host's first iteration's advance() crosses a full tick period.
    loop.advance();

    MockPlatform platform{1};
    platform.setOnPump([&] { clock.advanceBy(kTickPeriod); });
    ActionSet held;
    held.set(actionId(Act::Fire), true);
    platform.setHeld(held);

    WindowedHost host{loop, platform};
    host.run();

    EXPECT_EQ(ticks, 1);
    EXPECT_TRUE(pressedFire);
}

// Pacing: a never-blocking platform — one whose present does NOT block, so
// platform-monotonic time advances ONLY via the host's own pacing sleep — sleeps out the full refresh
// period every iteration after the first. This is the macOS-idle bug case; the deadline is what now
// throttles it. (onPump advances the RunLoop's sim clock, NOT the platform clock — the two are distinct.)
TEST(WindowedHost, FreeSpinIsPacedToTheRefreshPeriod) {
    constexpr auto kRefresh = std::chrono::nanoseconds{10'000'000};  // 10 ms / 100 Hz, clean arithmetic
    ManualClock  clock;
    RunLoop      loop{clock};
    MockPlatform platform{4};
    platform.setRefreshPeriod(kRefresh);
    platform.setOnPump([&] { clock.advanceBy(kTickPeriod); });  // moves sim time only; platform clock stays put

    WindowedHost{loop, platform}.run();

    EXPECT_EQ(platform.sleepCount(), platform.pumpCount());  // paced exactly once per iteration
    EXPECT_EQ(platform.lastSleep(), kRefresh);               // each spin sleeps the full period
    EXPECT_EQ(platform.totalSlept(), kRefresh * 3);          // iteration 1 anchors (sleeps 0); 2–4 sleep one period
}

// Pacing: a platform whose present blocks for a full refresh period each iteration (the healthy vsync case —
// modeled by the pump advancing the platform clock a whole period) is ALREADY at its deadline, so the host's
// pacing sleep is ~0. The deadline composes with vsync rather than fighting it (same period → the longer block
// wins; here the present already paced, so the host adds nothing).
TEST(WindowedHost, VsyncBlockedFrameSleepsNearZero) {
    constexpr auto kRefresh = std::chrono::nanoseconds{10'000'000};
    ManualClock  clock;
    RunLoop      loop{clock};
    MockPlatform platform{4};
    platform.setRefreshPeriod(kRefresh);
    platform.setOnPump([&] { platform.advanceNow(kRefresh); });  // the "present" consumes a full period

    WindowedHost{loop, platform}.run();

    EXPECT_EQ(platform.sleepCount(), platform.pumpCount());  // still paced once per iteration
    EXPECT_EQ(platform.totalSlept(), std::chrono::nanoseconds::zero());  // present already at the deadline → no extra sleep
}

// The Platform fullscreen seam: a fresh platform reports windowed; fullscreen
// flips the tracked state both ways. Verified headlessly through the abstract Platform interface,
// so the windowed host / consumer can drive fullscreen with no live window.
TEST(WindowedHost, FullscreenSeamTogglesTrackedState) {
    MockPlatform platform{1};
    Platform& seam = platform;  // exercise it through the abstract interface

    EXPECT_FALSE(seam.fullscreen());  // windowed by default
    seam.fullscreen(true);
    EXPECT_TRUE(seam.fullscreen());
    seam.fullscreen(false);
    EXPECT_FALSE(seam.fullscreen());
}

// The window-sizing seam: resolve a target scale against the usable display via
// fitWindowScale, then size the window to viewport × that scale — the resize a consumer's settings
// code (or the demo's scale toggle) performs. Verified headlessly: the mock reports the resize back
// through drawableSize().
TEST(WindowedHost, WindowSizingSeamResizesToClampedScale) {
    constexpr PixelSize kViewport{160, 144};
    MockPlatform platform{1};
    platform.setUsableDisplaySize(PixelSize{2560, 1440});  // a desktop 4× fits inside
    Platform& seam = platform;

    // Target 4× fits → window becomes exactly 4× the viewport.
    int scale = fitWindowScale(kViewport, seam.usableDisplaySize(), 4);
    EXPECT_EQ(scale, 4);
    seam.windowSize(PixelSize{kViewport.width * scale, kViewport.height * scale});
    EXPECT_EQ(seam.drawableSize(), (PixelSize{640, 576}));

    // On a shallow display the same target clamps down, and the window follows the clamp.
    platform.setUsableDisplaySize(PixelSize{2560, 500});
    scale = fitWindowScale(kViewport, seam.usableDisplaySize(), 4);
    EXPECT_EQ(scale, 3);
    seam.windowSize(PixelSize{kViewport.width * scale, kViewport.height * scale});
    EXPECT_EQ(seam.drawableSize(), (PixelSize{480, 432}));
}

}  // namespace
}  // namespace retropp
