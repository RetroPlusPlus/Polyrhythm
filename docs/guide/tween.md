# Tween

A small helper layer for **animating a value over time** — a fade, a color ramp, an effect parameter
that swells and recedes — over the platform's immediate-mode draw model. Where [animation](animation.md)
resolves elapsed ticks → *which frame to show*, a tween resolves elapsed ticks → *a value* (a layer's
`alpha`, a `ColorFill` channel, a transform angle, a shader uniform). It is the same shape as the
animation system: the platform supplies a **pure stateless resolver**, you own a cursor that does the
tick bookkeeping, and you write the resolved value into draw state each frame. It introduces **no platform
state and no new render path** — the platform never writes a tween into a draw-state field itself.

> **"Tween" is short for "in-between":** you give it a start and an end, and it fills in all the values
> in between over a duration. A `Tween<float>` from `1.0` to `0.0` over one second hands you every value
> between 1 and 0 as the second elapses — that's a fade.

**Effect and shader parameters are not special** — they are one case of animating a draw-state value
over time. A tween drives a built-in effect's amplitude, or a custom shader's own reflected parameter,
exactly the way it drives a layer's `alpha`. For stepping through *frames* of art instead of
interpolating a value, see [animation.md](animation.md); for the draw-state fields a tween typically
writes into (`alpha`, effect parameters, transforms), see [draw-state.md](draw-state.md).

```cpp
#include "retropp/tween.h"   // Tween, TweenSegment, Easing, ease, lerp, sampleTween, sampleTweenValue, TweenPlayer
using namespace std::chrono_literals;
```

## Contents

- [The model](#the-model)
- [`TweenSegment` and `Tween` — the data](#tweensegment-and-tween--the-data)
- [`Easing` — the curve set](#easing--the-curve-set)
- [`lerp` — interpolation over the float vocabulary](#lerp--interpolation-over-the-float-vocabulary)
- [The pure resolver](#the-pure-resolver)
- [`TweenPlayer<T>` — the game-owned cursor](#tweenplayert--the-game-owned-cursor)
  - [Timing default](#timing-default)
  - [Threading a value into draw state](#threading-a-value-into-draw-state)
  - [An effect or shader parameter is the same sink](#an-effect-or-shader-parameter-is-the-same-sink)
- [Where to change things](#where-to-change-things)

## The model

A frame is recomputed whole every tick, so you could already animate a value by hand: compute it from a
counter each tick and write it into draw state. This API removes that bookkeeping — it does not add a
capability. Three pieces:

- **`Tween<T>`** — pure data describing the value's journey: a start anchor plus a list of timed, eased
  moves.
- **The pure resolver** (`sampleTween` / `sampleTweenValue`) — given a tween, elapsed ticks, a timing profile, and a
  playback mode, returns the value to use now.
- **`TweenPlayer<T>`** — a game-owned cursor that holds the elapsed-tick counter and the playback
  controls, so you call `advance()` each tick and read `value()`.

## `TweenSegment` and `Tween` — the data

```cpp
template <typename T>
struct TweenSegment {
    T                        to;        // value reached at the END of this segment
    std::chrono::nanoseconds duration;  // wall-time of this segment (resolved to ticks via the profile)
    Easing                   easing = Easing::InOutQuad;  // the curve over THIS segment
    bool operator==(const TweenSegment&) const noexcept = default;
};

template <typename T>
struct Tween {
    T                            from;      // value at t = 0 (the start anchor)
    std::vector<TweenSegment<T>> segments;  // each appends a timed, eased move

    std::size_t count() const;              // number of segments

    static Tween of(T from, T to, std::chrono::nanoseconds d, Easing e = Easing::InOutQuad);
    Tween&      then(T to, std::chrono::nanoseconds d, Easing e = Easing::InOutQuad);  // chainable
};
```

A tween is a start value `from` plus an ordered list of segments, each a timed eased move toward its
`to`. The value chains `from → segments[0].to → segments[1].to → …`. Pure data — it carries no playback
state and no loop policy; *how* it plays is chosen at play time (see the resolver), so the same tween
fades once in one place and yoyos forever in another.

The named constructor `of` builds the single-segment case, and `then()` chains more (the
`Transform::then()` idiom). Because a tween is a list of moves rather than a single move, **a yoyo falls
out for free** — it is a two-segment track played under a looping mode, not a special "yoyo" mode:

```cpp
// fade out, then back — played forever, this yoyos
const Tween<float> fade = Tween<float>::of(1.0f, 0.0f, 1s, Easing::InOutSine)
                                       .then(1.0f, 1s, Easing::InOutSine);

// noon → dusk → noon (a Vec3 multiplier the game writes into a Multiply ColorFill region's fill)
const Tween<Vec3> dusk = Tween<Vec3>::of({1, 1, 1}, {0.45f, 0.35f, 0.55f}, 5s)
                                     .then({1, 1, 1}, 5s);
```

Durations are written in real time (`std::chrono`) and resolved to whole sim ticks at playback against
the timing profile — tick-quantized resolution is the honest granularity for a fixed-step sim, and the
the platform never stores ticks itself. Aggregate initialization stays available if you prefer it
(`Tween<float>{.from = 1.0f, .segments = {{0.0f, 1s, Easing::InOutSine}}}`); `of` / `then` are the
ergonomic shorthand.

## `Easing` — the curve set

```cpp
enum class Easing : std::uint8_t {
    Linear,
    InQuad,  OutQuad,  InOutQuad,
    InCubic, OutCubic, InOutCubic,
    InQuart, OutQuart, InOutQuart,
    InQuint, OutQuint, InOutQuint,
    InSine,  OutSine,  InOutSine,
    InExpo,  OutExpo,  InOutExpo,
    InCirc,    OutCirc,    InOutCirc,
    InBack,    OutBack,    InOutBack,
    InElastic, OutElastic, InOutElastic,
    InBounce,  OutBounce,  InOutBounce,
};

float ease(Easing e, float t);   // shape a linear progress t ∈ [0,1] into the curve's progress
```

`Linear` plus `In` / `Out` / `InOut` of each named family — the standard easing set. `ease(e, t)` shapes
a linear progress `t` into the curve's progress; `t` is clamped to `[0, 1]` on entry. Every non-`Linear`
preset pins its endpoints exactly — `ease(e, 0) == 0` and `ease(e, 1) == 1` — so transcendental rounding
never leaks a `0.9999998` out of an endpoint, and a `Single` tween settles precisely on its target.

Three families overshoot or oscillate. `Back` **overshoots** on purpose: it returns slightly `< 0` or `> 1`
in the interior, so a tweened value passes its target and settles back. `Elastic` springs past the target
and rings in to it (a decaying sine on an exponential). `Bounce` settles onto the target in decaying hops.
All three still pin their endpoints exactly.

You rarely call `ease` directly — the resolver applies it per segment. It is public so you can shape your
own progress value when you are not using a `Tween` at all.

## `lerp` — interpolation over the float vocabulary

```cpp
constexpr float lerp(float a, float b, float t);   // a + (b - a) * t
constexpr Vec2  lerp(Vec2  a, Vec2  b, float t);
constexpr Vec3  lerp(Vec3  a, Vec3  b, float t);
constexpr Vec4  lerp(Vec4  a, Vec4  b, float t);
```

A `Tween<T>` interpolates over the platform's float vocabulary — `float`, `Vec2`, `Vec3`, `Vec4`. There is
**no integer `lerp`**: an integer draw-state sink (a `LayerScroll`, a pixel center) is animated by
tweening a `float` / `Vec2` and **quantizing at the write** into draw state — which the game already
owns, since the game writes the resolved value in. Keeping the resolver pure-float makes the easing math
exact and free of integer-accumulation artifacts. `lerp` is `constexpr` (the non-eased path is usable in
constant expressions); `ease` is not, because the curves use `std::sin` / `std::pow` / `std::sqrt`.

## The pure resolver

```cpp
std::uint64_t totalTicks(const Tween<T>&, const TimingProfile&);

TweenSample<T> sampleTween(const Tween<T>&, std::uint64_t elapsedTicks,
                       const TimingProfile&, PlaybackMode);
T              sampleTweenValue(const Tween<T>&, std::uint64_t elapsedTicks,
                       const TimingProfile&, PlaybackMode);   // == sampleTween(...).value

struct TweenSample<T> {
    T    value;             // the value to use now
    bool finished = false;  // playback ended (per mode)
    bool operator==(const TweenSample&) const noexcept = default;
};
```

`sampleTween` is the pure resolver `TweenPlayer` wraps — the player holds the elapsed-tick counter and calls
`sampleTween` each advance. Given elapsed ticks and a mode, it returns the value to use now and whether playback has ended. The
**`PlaybackMode`** vocabulary is shared verbatim with [animation](animation.md#playbackmode--how-it-plays-chosen-when-you-play-it)
(`single()` / `loopNTimes(n)` / `loopIndefinitely()` / `playForDuration(d)`):

| Mode | Behavior |
|---|---|
| `LoopIndefinitely` | `elapsed` modulo one pass; never `finished`. Snaps end → `from` at the wrap — a yoyo is a two-segment track, not this mode. |
| `Single` | First pass, then holds the final segment's `to`; `finished` once `elapsed ≥ totalTicks`. |
| `LoopNTimes(n)` | Wraps for `n` passes, then holds the final `to`; `finished` once `elapsed ≥ n·totalTicks`. |
| `PlayForDuration(d)` | Wraps until `elapsed ≥ ticksForDuration(d)`, then holds the value shown at the cutoff; `finished` past `d`. |

`totalTicks` is the length of one pass — the sum of each segment's duration resolved to ticks (`0`
segments → `0`). Two edge cases keep playback from stalling: a **zero-tick segment** (a duration that
rounds to 0 ticks) is skipped as an instantaneous snap to its `to` — never a resting value, never a loop
stall — and an **empty tween** (no segments) resolves to `{ from, finished true }`. If *every* segment is
instantaneous (the whole track rounds to 0 ticks), a finite mode is immediately `finished` and an
indefinite loop simply rests on the resting value.

`TimingProfile` is read-only host config the resolver only consults (taken by `const&`; see
[run-loop-and-timing.md](run-loop-and-timing.md)), so `sampleTween` is pure — same inputs, same value.

## `TweenPlayer<T>` — the game-owned cursor

The "just play it" wrapper. **State lives here, in your object — not in the platform.** You construct it,
call `advance()` each sim tick, and write `value()` into whatever draw-state sink you like. The platform
provides the type; you own the instance, exactly like a `std::vector`. The renderer never sees it.

```cpp
template <typename T>
struct TweenPlayer {
    static inline TimingProfile defaultTiming = TimingProfile::GameBoyColor;

    const Tween<T>* tween   = nullptr;          // game-owned; must outlive the player
    TimingProfile   profile = defaultTiming;     // resolves durations → ticks
    std::uint64_t   elapsedTicks = 0;
    bool            playing = true;
    TweenSample<T>  sample{};                    // cached by advance() so value()/finished() need no args

    void advance(PlaybackMode mode = PlaybackMode::loopIndefinitely(),
                 std::uint64_t deltaTicks = 1);  // accrues ticks (only while playing) + re-resolves

    const T& value()    const;   // the resolved value (cached by advance())
    bool     finished() const;

    void play();                          // resume
    void pause();                         // freeze at the current value
    void stop();                          // pause + rewind to `from`
    void restart();                       // rewind + play
    void seek(std::chrono::nanoseconds at);  // jump to a wall-time offset
};
```

`advance()` accrues `elapsedTicks` **only while `playing`** and re-resolves through `sampleTween` under
`mode` (default `loopIndefinitely()`, so a bare `advance()` just loops — pass `single()` /
`loopNTimes(n)` / `playForDuration(d)` for the others). A null `tween` makes `advance()` a no-op.
`stop()` pauses and rewinds to the start anchor `from` (not finished); `restart()` rewinds and resumes.
`seek` jumps to a **wall-time offset** (a tween has no frames, so a frame index would be meaningless),
resolved to ticks via the profile.

### Timing default

`TweenPlayer<T>::defaultTiming` is the cadence a bare-constructed player resolves durations against, one
per instantiated `T`. `EngineConfig::setActive(config)` at startup fans the configured cadence into it
(alongside `RunLoop::defaultTiming`, `Renderer::defaultViewport`, and `AnimationPlayer::defaultTiming`),
seeding every interpolable `T`, so a bare `TweenPlayer<float>{.tween = &t}` inherits the platform cadence
with nothing extra to type. You can also assign it directly at any time
(`TweenPlayer<float>::defaultTiming = loop.timing();`), or override a single player by setting its
`.profile` field. It defaults to `TimingProfile::GameBoyColor` and is a single process-wide default per
`T` — legitimate here because the platform is single-threaded by design and this is a config default, not
retained render state.

### Threading a value into draw state

The player resolves the value; you write it into whatever field you like each render:

```cpp
TweenPlayer<float> fader{.tween = &fade};
TweenPlayer<Vec3>  dusker{.tween = &dusk};

loop.simTick([&](const InputState&) {
    fader.advance();    // loops by default; pass single()/loopNTimes(n)/playForDuration(d) for others
    dusker.advance();
});

loop.renderLoop([&] {
    upperLayer.alpha = fader.value();                      // scalar sink
    const Vec3 m     = dusker.value();                     // a per-channel multiplier (vector sink)
    const auto u8    = [](float v) { return std::uint8_t(v * 255.0f); };
    // Whole-frame day/night is just an effect: a Multiply ColorFill region. The tween drives the fill color.
    frame.regions.clear();
    frame.regions.push_back(Region{
        .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                      .fill = Rgba8{u8(m.x), u8(m.y), u8(m.z), 255}}},
        .blend   = BlendMode::Multiply});
    // … submit …
});
```

### An effect or shader parameter is the same sink

An effect's parameters become shader uniforms; write a tween's `value()` into one exactly like any other
field. Here a built-in ripple's amplitude swells and recedes — a *custom* shader's own reflected
parameter (`ScreenSpaceEffect{ .kind = Custom, … }`) is written identically:

```cpp
TweenPlayer<float> swell{.tween = &swellTween};   // a Tween<float>, e.g. 0 → 6 → 0

loop.simTick([&](const InputState&) { swell.advance(); });
loop.renderLoop([&] {
    frame.postEffects = {{ .kind = ScreenSpaceEffectKind::Ripple,
                           .amplitude = swell.value(),     // ← the tweened shader uniform
                           .frequency = 5.0f, .center = {80, 72}, .decay = 1.5f }};
    // … submit …
});
```

Want the pure form without the cursor object? Call `sampleTweenValue(tween, elapsedTicks, profile, mode)` and own
the tick counter yourself; both ship. A worked example — a layer-alpha fade plus a dusk ramp — is in
[`examples/tween_demo.cpp`](../../examples/tween_demo.cpp).

## Where to change things

- **Fade / ramp a value over time:** build a `Tween<T>`, hold a `TweenPlayer<T>`, `advance()` it each
  tick, write `value()` into the draw-state field.
- **Yoyo (ping-pong) a value:** author a two-segment track (`of(a, b, d).then(a, d)`) and play it under
  `loopIndefinitely()` — there is no yoyo mode.
- **Change the curve feel:** pass a different `Easing` to `of` / `then` (per segment). `Back` overshoots,
  `Elastic` springs and rings in, `Bounce` hops onto the target; `Linear` is unshaped.
- **Animate an integer sink (scroll, a pixel center):** tween a `float` / `Vec2` and round at the write
  into draw state — there is no integer `lerp`.
- **Drive a value without the cursor object:** call the pure `sampleTweenValue` / `sampleTween` and own the
  elapsed-tick counter yourself.
- **Use a non-GBC cadence:** `EngineConfig::setActive` seeds the cadence at startup; to change it without
  a full config, set `TweenPlayer<T>::defaultTiming` once, or set each player's `.profile`.
- **Step through *frames* of art instead of interpolating a value:** that's an animation, not a tween —
  see [animation.md](animation.md).
