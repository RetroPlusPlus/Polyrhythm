# Run loop & timing

`RunLoop` runs your game logic at a fixed rate and your rendering at the display's rate, decoupled. You
give it two callbacks — a **tick** (one logical step) and a **render** — and it calls them at the right
cadence.

```cpp
#include "retropp/run_loop.h"       // RunLoop, kMaxFrameTime
#include "retropp/clock.h"          // Clock, SteadyClock
#include "retropp/timing.h"         // TimingProfile, TickPeriodNs, CpuTiming
#include "retropp/pacing.h"         // FrameDeadline, nextFrameDeadline  (used by WindowedHost)
```

## Contents

- [Model](#model)
- [`RunLoop`](#runloop)
  - [Callbacks](#callbacks)
  - [Tick advances state; render only draws it](#tick-advances-state-render-only-draws-it)
  - [Input each tick](#input-each-tick)
  - [Frame-time clamp](#frame-time-clamp)
- [Exiting the application](#exiting-the-application)
- [The clock — `Clock` / `SteadyClock`](#the-clock--clock--steadyclock)
- [Interpolation](#interpolation)
  - [Worlds that advance every few ticks](#worlds-that-advance-every-few-ticks)
- [Frame pacing](#frame-pacing)
- [Timing profile](#timing-profile)
- [Where to change things](#where-to-change-things)

## Model

The simulation advances in fixed **ticks**; rendering happens once per displayed frame. Each call to
`advance()`:

1. reads the clock,
2. runs as many whole ticks as the elapsed time covers (each at the fixed period),
3. renders once, at a blend factor `alpha ∈ [0, 1)` — where to draw along the interval between the
   states the renderer holds.

The **first** `advance()` only takes a timing baseline: it runs zero ticks (there is no previous tick to
advance from yet) and renders once at `alpha = 0`, so the opening frame composites the initial state
verbatim. Every call after it runs the due ticks.

Because ticks are fixed-rate, game logic is deterministic and independent of display refresh. The
renderer eases each object between the states it holds by `alpha` **automatically** (see
[Interpolation](#interpolation)), so motion is smooth even when the display outruns the tick rate — with
no game-side work.

One iteration can commit more than one tick when a frame runs long, which leaves the renderer's states
that many fixed steps apart. `alpha` spans whatever interval they are apart, so an object covers ground
in proportion to elapsed time however the ticks bunched. In the steady state — one tick per iteration —
`alpha` is exactly the sub-tick fraction `accumulator / tickPeriod`.

In a window you don't call `run()` — [`WindowedHost`](platform-and-windowing.md) owns the loop and calls
`advance()` for you. `run()` is the headless driver for tests and tools.

## `RunLoop`

```cpp
class RunLoop {
public:
    using TickCallback   = std::function<void(const InputState&)>;
    using RenderCallback = std::function<void()>;

    static inline TimingProfile defaultTiming = TimingProfile::GameBoyColor;  // unless EngineConfig sets it

    explicit RunLoop(Clock& clock, TimingProfile timing = defaultTiming) noexcept;

    void simTick(TickCallback cb);                       // one logical step, given the tick's input
    void renderLoop(RenderCallback cb);                   // draw the latest state

    void setRawInput(const InputSample& raw) noexcept;   // the platform's sample (push each host frame)

    void advance();                                      // run due ticks, render once
    void run();                                          // call advance() until stop() / an exit resolves
    void stop() noexcept;

    enum class ExitVerdict { Proceed, NotYet, Veto };    // a guard's per-boundary answer to a pending exit
    using ExitGuard = std::function<ExitVerdict()>;

    void exitAction(ExitGuard fn);                       // register the close-out guard
    void exitRequest() noexcept;                         // submit exit intent (from a tick)
    bool exitPending()  const noexcept;                  // raised, not yet resolved/vetoed
    bool exitResolved() const noexcept;                  // a guard Proceeded — terminal

    std::uint64_t            tickCount()  const noexcept;
    const TimingProfile&     timing()     const noexcept;
    std::chrono::nanoseconds tickPeriod() const noexcept;
};
```

### Callbacks

- **`simTick`** takes `void(const InputState&)` — your logical step. The argument is the per-tick
  input view (held state + press/release edges + pointer/analog; see [input.md](input.md)).
- **`renderLoop`** takes `void()` — your draw. The platform owns interpolation, so you submit the latest
  state each frame and the renderer eases it; see [Interpolation](#interpolation).

A bare `RunLoop{clock}` uses `defaultTiming` (`TimingProfile::GameBoyColor`, or whatever
`EngineConfig::setActive` set — see [platform-and-windowing.md](platform-and-windowing.md)). Pass a
profile to override it: `RunLoop{clock, TimingProfile{TickPeriodNs::Hz60}}`.

### Tick advances state; render only draws it

Put everything that **changes** the world in the tick callback, and nothing that changes it in the render
callback. The tick runs at the fixed cadence; the render runs once per displayed frame — as often as the
monitor refreshes. Anything you advance in `renderLoop` therefore moves at the display's refresh rate, so
the same program runs faster on a 144 Hz screen than on a 60 Hz one, and differently on two machines.
Advance it in `simTick` and it runs at the same speed everywhere, because ticks are wall-clock-paced.

This applies to *all* evolving state, not just gameplay: a scroll offset, an animation counter, an
effect's phase, a colour cycle — if it changes over time, it advances in the tick.

```cpp
int frame = 0;

// Correct: state advances on the fixed tick; the render reads it.
loop.simTick([&](const InputState&) {
    ++frame;                        // ~59.7 steps per second on every machine
});
loop.renderLoop([&] {
    drawScrolledBy(frame / 4);      // reads state; never changes it
});
```

```cpp
// Wrong: the counter advances once per displayed frame, so scroll speed tracks the refresh rate —
// invisible at 60 Hz, too fast on anything higher, and inconsistent between displays.
loop.renderLoop([&] {
    drawScrolledBy(frame / 4);
    ++frame;                        // display-rate-dependent
});
```

The render callback should only read state and draw it. If you find yourself writing `++`, `+=`,
or any assignment to game or animation state inside `renderLoop`, it belongs in `simTick`. (`RunLoop`
also exposes `tickCount()` if the render wants the current tick number without keeping its own counter.)

### Input each tick

Push the platform's `InputSample` every host frame (`setRawInput(platform.input())`); the loop
samples it at the start of each tick, per player slot:

- the digital action level becomes the tick's held state, and a `justPressed` is reported for any
  action that went down since the previous tick, **even one already released by tick time** (a tap
  shorter than a tick, or input from a host frame that ran no tick, still registers one press);
- relative analog quantities (raw mouse delta, wheel) **sum** across host frames between ticks;
  absolute quantities (cursor, sticks, per-action values, the active device) take the latest.

When one `advance()` runs several catch-up ticks, they share one input sample, so each press edge fires
once. See [input.md](input.md) for the full input surface.

### Frame-time clamp

```cpp
inline constexpr std::chrono::nanoseconds kMaxFrameTime{250'000'000};  // 250 ms
```

`advance()` caps the elapsed time it feeds the accumulator at `kMaxFrameTime`. If a host frame takes
longer (a breakpoint, a long stall), the sim runs at most that many ticks instead of an unbounded
catch-up burst — it slows rather than freezes. This is independent of the timing profile.

## Exiting the application

A game ends its own run — a title-screen "Esc quits", a pause-menu "Quit" — by submitting exit intent;
the platform then drives a registered **close-out guard** to completion before the program tears down, so
a resume snapshot, a save, or a fade-out runs first. Every exit source routes through the one guard:
the programmatic `exitRequest()`, the OS window-close button, and a headless `run()`.

```cpp
enum class ExitVerdict { Proceed, NotYet, Veto };   // the guard's per-boundary answer
using ExitGuard = std::function<ExitVerdict()>;

void exitAction(ExitGuard fn);   // register the close-out guard (like simTick / renderLoop)
void exitRequest() noexcept;     // submit exit intent — from a tick

bool exitPending()  const noexcept;   // raised, not yet resolved or vetoed
bool exitResolved() const noexcept;   // a guard Proceeded — the loop is stopping
```

- **`exitRequest()`** raises the pending state and returns. Call it from a tick (a pause-menu
  selection). The sim keeps advancing frame-by-frame while the exit is pending, so a multi-tick close-out
  (a fade) keeps animating. Repeated calls while pending are ignored; after an exit resolves it does
  nothing.
- **`exitAction(fn)`** registers the guard. While an exit is pending, the platform calls it **once per
  frame boundary** (once per `advance()`) and acts on its `ExitVerdict`. The guard runs after the tick
  batch, so sim state is settled when it reads it for a snapshot.
- **`ExitVerdict`** is the answer: `Proceed` stops the loop and tears the program down, `NotYet` keeps
  running and asks again next boundary (answer `NotYet` while a save or fade-out is still in progress,
  then `Proceed`), `Veto` abandons the exit and resumes normal running (a "Quit? → No").
- **With no guard registered, a pending exit `Proceed`s immediately.**

A guard typically drives a save and proceeds once it finishes:

```cpp
enum class Action { PauseQuit /* … */ };

Save save;                 // your save-in-progress handle
loop.exitAction([&]() -> ExitVerdict {
    if (!save.started()) save.begin(worldSnapshot());   // start the close-out on the first boundary
    return save.done() ? ExitVerdict::Proceed : ExitVerdict::NotYet;
});

loop.simTick([&](const InputState& in) {
    if (in.justPressed(Action::PauseQuit)) loop.exitRequest();   // submit intent; the guard resolves it
});
```

In a window, `WindowedHost` unions the OS close button into the same pending state, so the X button runs
the guard too, and a `Veto` cancels the OS close and keeps the app running. See
[platform-and-windowing.md](platform-and-windowing.md). For the save itself see
[persistence.md](persistence.md). The worked round-trip — a callback guard and a multi-tick guard, each
saving a snapshot restored on relaunch — is
[`examples/exit_snapshot/`](../../examples/exit_snapshot/main.cpp).

## The clock — `Clock` / `SteadyClock`

```cpp
class Clock      { virtual std::chrono::nanoseconds now() const noexcept = 0; };
class SteadyClock final : public Clock;   // monotonic (std::chrono::steady_clock)
```

`RunLoop` reads time through a `Clock`. Use `SteadyClock` in a real program; pass your own `Clock` in
tests to drive the loop tick-by-tick with no real waiting.

## Interpolation

When the display refreshes faster than the sim ticks, positions would step once per tick. The platform
smooths this **automatically and by default**: the renderer matches each layer and sprite to its
previous tick by its `key` and eases the two states by `alpha`, so motion is smooth with **no game-side
code** — you submit your latest state each render and the platform blends.
The `bongusoid` example works this way. See
[rendering.md](rendering.md#per-frame-submission-renderframe) for the renderer side and
[draw-state.md](draw-state.md) for the `key` that drives the matching.

Give each object a stable `key` and the matching engages; `EngineConfig::interpolation = false` (or
`Renderer::automaticInterpolation(false)`) composites each submission verbatim for tick-quantized output.

### Worlds that advance every few ticks

Some games move on a divider: the overworld steps two pixels every second tick, not one pixel every
tick. The renderer eases a move across the time it takes, and by default that is one tick — so a
two-tick move gets crammed into the one frame the change was submitted on, and how it splits depends on
where the sub-tick fraction happens to fall. Declare the divider and the move is spread across the ticks
it really occupies:

```cpp
FrameDrawState frame;
frame.advancesEvery = 2;                  // the whole frame's world steps every second tick

frame.layers.push_back(DrawLayer{
    .key = "hud", .z = 10,
    .advancesEvery = 1,                   // …except the HUD, which runs every tick
});
```

`FrameDrawState::advancesEvery` is the frame-wide default; `DrawLayer::advancesEvery` overrides it for
one layer, and a sprite advances with the layer it lives in. Both are read fresh from every submission,
so changing one takes effect on the next tick. A declared `0` reads as `1`.

**What it costs.** A declared cadence of N draws the world N ticks behind instead of one — the renderer
needs somewhere left to travel to at the moment it first sees a new value. Declaring 1, or declaring
nothing, costs nothing at all: the factor is then the plain sub-tick fraction across the commit's span.

**Where a span begins is observed, not scheduled.** A change in the submitted value starts the span, so
a world that moves sooner than it declared simply starts a new one, and nothing drifts out of phase.
Declaring a longer cadence than the world actually uses still draws even motion — it only draws it
further behind.

**The compose skip waits for a layer to finish moving.** A world that stops settles once its cadence has
elapsed, and the frame becomes skippable then.

## Frame pacing

`RunLoop` decides when ticks are due; pacing the host iteration to the display is `WindowedHost`'s job.
It paces each iteration to a monotonic deadline spaced by the display's current refresh period, so the
loop runs at the display's cadence:

```cpp
struct FrameDeadline {
    std::chrono::nanoseconds nextDeadline;   // carry to the next iteration
    std::chrono::nanoseconds sleepFor;       // time to sleep now (never negative)
};

constexpr FrameDeadline nextFrameDeadline(std::chrono::nanoseconds prevDeadline,
                                          std::chrono::nanoseconds period,
                                          std::chrono::nanoseconds now) noexcept;
```

`nextFrameDeadline` is pure (unit-testable, no clock, no sleep). After each present, `WindowedHost`
sleeps `sleepFor` and carries `nextDeadline` forward. An iteration that finishes on time or early
advances the deadline by one period from the deadline itself, so the cadence is drift-free; an
iteration that finishes late re-anchors the deadline to `now`, so lateness is forgiven rather than
carried. A frame is asked to sleep nothing only when the frame before it consumed at least a full
period — a slow stretch never repays itself as a burst of unpaced frames.
The OS time / refresh / sleep primitives are the `Platform` pacing seam (`nowMonotonic`,
`displayRefreshPeriod`, `sleepPrecise`); `RunLoop` has no SDL dependency. See
[platform-and-windowing.md](platform-and-windowing.md).

## Timing profile

`TimingProfile` sets the cadence. It carries a tick **period** (always) and an optional CPU-timing block
(a console's cycle arithmetic, for a game that spends cycles by hand — see
[vm-and-routines.md](vm/vm-and-routines.md)).

```cpp
enum class TickPeriodNs : std::int64_t {
    GameBoy      = 16'742'706,   // 59.7275 Hz — one Game Boy frame (70'224 cycles @ 4'194'304 Hz)
    GameBoyColor = 16'742'706,   // same period
    Snes         = 16'639'263,   // 60.0988 Hz — one SNES frame (357'366 cycles @ 236'250'000/11 Hz)
    SnesPal      = 19'997'208,   // 50.0070 Hz — one PAL SNES frame (425'568 cycles @ 21'281'370 Hz)
    Hz60         = 16'666'667,   // a round 60 Hz
};

struct CpuTiming {                // optional; a console's cycle arithmetic
    std::uint32_t cpuClockHz;
    std::uint32_t cyclesPerFrame;
    std::uint32_t doubleSpeedCyclesPerFrame;
    std::uint32_t cpuClockHzDivisor = 1;   // the clock is cpuClockHz / cpuClockHzDivisor Hz
    constexpr bool operator==(const CpuTiming&) const noexcept = default;
};

struct TimingProfile {
    TickPeriodNs             tickPeriodNs = TickPeriodNs::GameBoyColor;
    std::optional<CpuTiming> cpu{};

    constexpr std::chrono::nanoseconds tickPeriod()       const noexcept;  // what RunLoop schedules on
    constexpr std::uint32_t            cpuCyclesPerTick() const noexcept;  // cpu cycles per tick (0 if no cpu block)
    constexpr std::uint64_t            ticksForDuration(std::chrono::nanoseconds) const noexcept;
    constexpr bool                     operator==(const TimingProfile&) const noexcept = default;

    static const TimingProfile GameBoy;
    static const TimingProfile GameBoyColor;
    static const TimingProfile Snes;
    static const TimingProfile SnesPal;
};
```

- The enum value is a **period in nanoseconds**, not a rate (59.7275 Hz isn't an exact integer; its
  period is). Pass a preset or a raw period: `static_cast<TickPeriodNs>(16'700'000)`.
- The Game Boy tick is one hardware frame ⇒ **59.7275 Hz, not 60**.
- The SNES presets are one frame of each region: `Snes` is **60.0988 Hz** and `SnesPal` is
  **50.0070 Hz**. The 60 Hz console's master clock is 236'250'000 / 11 Hz, so its CPU block carries
  `cpuClockHzDivisor = 11`; every other preset leaves the divisor at 1.
- GBC double speed is a CPU cycle budget (`doubleSpeedCyclesPerFrame`); the display rate is unchanged.
- `cpuCyclesPerTick()` is the amount to advance the VM's divider per tick (see
  [vm-and-routines.md](vm/vm-and-routines.md)). `ticksForDuration(std::chrono::seconds{2})` converts a
  wall-clock interval to a tick count.
- The presets are static members, usable in `constexpr` contexts (including the `RunLoop` default).

## Where to change things

- **Cadence:** pass a `TimingProfile` to `RunLoop`, set `EngineConfig::timing`, or assign
  `RunLoop::defaultTiming`.
- **Smooth motion:** automatic by default (the platform eases by `alpha`); set
  `EngineConfig::interpolation = false` for tick-quantized rendering.
- **Run in a window:** `WindowedHost{loop, platform}.run()`
  ([platform-and-windowing.md](platform-and-windowing.md)).
- **End the run:** `exitRequest()` from a tick, with an `exitAction` guard for any close-out (save,
  fade); see [Exiting the application](#exiting-the-application).
- **Deterministic tests:** inject your own `Clock` (and a `MockPlatform` for pacing).
