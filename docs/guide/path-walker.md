# Path walker

A small helper layer for **moving something along a curve over time** — a camera on a track, a projectile
arc, a particle, a menu highlight riding a rail. Where [animation](animation.md) resolves elapsed ticks →
*which frame to show* and [tween](tween.md) resolves elapsed ticks → *a value*, a path walker resolves
elapsed ticks → *a position and a facing* along a [curve](curve.md). It is the same shape as the other
two players: the platform supplies a **pure stateless resolver**, you own a cursor that does the tick
bookkeeping, and you read `position()` / `facing()` each frame and write them into whatever you like. It
introduces **no platform state and no new render path** — the platform never moves anything itself.

A [`Curve`](curve.md) answers *where is arc-length `s`, and which way does travel point there* — it has
no clock. **Time enters the geometry here, and only here,** as a *pacing driver* that turns elapsed ticks
into a distance along the path. Nothing below this layer gains a clock.

```cpp
#include "retropp/path_walker.h"   // PathPacing, WalkSample, sampleWalk, PathWalker
using namespace std::chrono_literals;
```

## Contents

- [The family](#the-family)
- [The model](#the-model)
- [`PathPacing` — the time → distance driver](#pathpacing--the-time--distance-driver)
- [The pure resolver](#the-pure-resolver)
- [`PathWalker` — the game-owned cursor](#pathwalker--the-game-owned-cursor)
  - [Timing default](#timing-default)
  - [Hold-last facing](#hold-last-facing)
  - [Writing position and facing into draw state](#writing-position-and-facing-into-draw-state)
  - [Re-pathing](#re-pathing)
- [Where to change things](#where-to-change-things)

## The family

Three players share one shape — pure data + a pure resolver + a game-owned cursor:

| Data (pure, no clock) | Player (game-owned cursor) | Resolves elapsed ticks → |
|---|---|---|
| [`Tween<T>`](tween.md) | `TweenPlayer<T>` | a value |
| [`Animation`](animation.md) | `AnimationPlayer` | a frame |
| [`Curve`](curve.md) / `ArcLengthTable` | **`PathWalker`** | a **position + facing** |

If you already use `TweenPlayer` or `AnimationPlayer`, `PathWalker` will feel identical — the only twist
is that a curve query is parameterized by *distance*, so a path walker is where a *pacing driver* converts
time into that distance.

## The model

A frame is recomputed whole every tick, so you could already move along a curve by hand: keep an
arc-length cursor, add to it each tick, and query the curve. This API removes that bookkeeping — it does
not add a capability. Three pieces:

- **`PathPacing`** — how elapsed time becomes a distance along the path (constant speed, an eased
  traversal, or a game-owned distance profile).
- **The pure resolver** (`sampleWalk`) — given a baked path, a pacing, elapsed ticks, a timing profile, and a
  playback mode, returns the position, facing, resolved distance, and whether playback has ended.
- **`PathWalker`** — a game-owned cursor that holds the elapsed-tick counter and the playback controls, so
  you call `advance()` each tick and read `position()` / `facing()`.

The path a walker holds is a baked **`ArcLengthTable`** — build it once from a curve with
`curve.arcTable()` (see [curve.md](curve.md#reusing-the-arc-length-table)); the walker owns it by value.

## `PathPacing` — the time → distance driver

```cpp
struct PathPacing {
    enum class Kind : std::uint8_t { Speed, Eased, DistanceTween };
    Kind                     kind = Kind::Speed;          // identity, first member
    float                    pxPerSecond = 0.0f;          // Speed (0 = parked at the start)
    std::chrono::nanoseconds duration{};                  // Eased: wall-time of one traversal
    Easing                   easing = Easing::InOutQuad;  // Eased: the curve over the traversal
    const Tween<float>*      distance = nullptr;          // DistanceTween: game-owned; must outlive the walker
    bool operator==(const PathPacing&) const noexcept = default;   // value type — equality-comparable

    static PathPacing speed(float pxPerSecond);
    static PathPacing eased(std::chrono::nanoseconds duration, Easing e = Easing::InOutQuad);
    static PathPacing distanceTween(const Tween<float>& distance);
};
```

Three forms of pacing, one value type:

- **`Speed`** — constant speed, in **viewport pixels per second** (wall time). The same value means the
  same on-screen speed on any cadence, because it resolves to pixels-per-tick through the walker's timing
  profile; the distance at a tick is measured in double precision, so a length that is a whole number of
  per-tick steps is reached on that step.
- **`Eased`** — one full traversal takes `duration`, shaped by an [`Easing`](tween.md#easing--the-curve-set)
  curve: `distance = length × ease(easing, t / duration)`. Slow-in / slow-out motion along the path.
- **`DistanceTween`** — a game-owned [`Tween<float>`](tween.md) *is* the distance-versus-time function. A
  multi-segment profile can accelerate, pause, and even move **backward** along the path (a non-monotone
  distance track, clamped to `[0, length]`) — motion the first two forms can't express.

Author it with the named constructors, or with aggregate initialization if you prefer — both ship and are
interchangeable:

```cpp
// named-constructor form
const PathPacing a = PathPacing::speed(40.0f);                       // 40 px/s
const PathPacing b = PathPacing::eased(3s, Easing::InOutQuad);       // one 3-second eased pass
const PathPacing c = PathPacing::distanceTween(myDistanceProfile);   // a Tween<float> drives the distance

// aggregate form — the same three values
const PathPacing a2{.kind = PathPacing::Kind::Speed, .pxPerSecond = 40.0f};
const PathPacing b2{.kind = PathPacing::Kind::Eased, .duration = 3s, .easing = Easing::InOutQuad};
const PathPacing c2{.kind = PathPacing::Kind::DistanceTween, .distance = &myDistanceProfile};
```

The default (`Speed`, 0 px/s) is a mover parked at the start — never undefined behavior. A "there and
back" is authored as a `DistanceTween` yoyo (`Tween<float>::of(0, len, d).then(0, d)`), the same way a
tween yoyo is a two-segment track — there is no ping-pong pacing mode.

## The pure resolver

```cpp
struct WalkSample {
    Vec2  position;             // the point at the resolved arc-length
    Vec2  facing;               // UNIT direction of travel there; ZERO where the curve has no direction
    float distance = 0.0f;      // the resolved arc-length (∈ [0, table.length()])
    bool  finished = false;     // playback ended (per mode)
    bool  operator==(const WalkSample&) const noexcept = default;
};

WalkSample sampleWalk(const ArcLengthTable& table, const PathPacing& pacing,
                  std::uint64_t elapsedTicks, const TimingProfile&, PlaybackMode);
```

`sampleWalk` is **the single point of movement truth** — `PathWalker` is the stateful wrapper over it.
`position` and `facing` come from the same arc-length (`table.atDistance(s)` and
`table.tangentAtDistance(s)`), so a mover's heading always matches where it is.

The **`PlaybackMode`** vocabulary is shared verbatim with [animation](animation.md#playbackmode--how-it-plays-chosen-when-you-play-it)
and [tween](tween.md#the-pure-resolver) (`single()` / `loopNTimes(n)` / `loopIndefinitely()` /
`playForDuration(d)`). Looping **wraps the distance** — continuous on a closed curve, a sawtooth snap
end → start on an open one, exactly the wrap a tween has:

| Mode | Behavior |
|---|---|
| `LoopIndefinitely` | Distance wraps each pass; never `finished`. |
| `Single` | Runs once, then holds the endpoint (`length`); `finished` once past the end. |
| `LoopNTimes(n)` | Wraps for `n` passes, then holds the endpoint. `LoopNTimes(0)` rests at the **start**, `finished` — a walker that never played sits where it started. |
| `PlayForDuration(d)` | Wraps until `elapsed ≥ ticksForDuration(d)`, then holds the distance shown at the cutoff; `finished` past `d`. |

A `DistanceTween` pacing passes the mode straight through to its tween, so the tween's own wrap / hold /
zero-segment semantics are the contract. Degenerate geometry (an empty table, or a zero-length curve)
resolves to the start with a **zero facing**, `distance` 0, and `finished` for the finite modes — the
mirror of an empty tween. `TimingProfile` is read-only host config the resolver only consults (taken by
`const&`), so `sampleWalk` is pure.

## `PathWalker` — the game-owned cursor

The "just move it" wrapper. **State lives here, in your object — not in the platform.** You construct it,
call `advance()` each sim tick, and read `position()` / `facing()`. The platform provides the type; you own
the instance, exactly like a `std::vector`. The renderer never sees it.

```cpp
struct PathWalker {
    static inline TimingProfile defaultTiming = TimingProfile::GameBoyColor;

    ArcLengthTable table;                    // the baked geometry, owned by value (= curve.arcTable())
    PathPacing     pacing;                   // the time → distance driver
    TimingProfile  profile = defaultTiming;
    std::uint64_t  elapsedTicks = 0;
    bool           playing = true;
    WalkSample     sample{};                 // cached by advance() so the getters need no arguments

    void advance(PlaybackMode mode = PlaybackMode::loopIndefinitely(),
                 std::uint64_t deltaTicks = 1);  // accrues ticks (only while playing) + re-resolves

    Vec2  position() const;   // cached by advance()
    Vec2  facing()   const;
    float distance() const;
    bool  finished() const;

    void play();                             // resume
    void pause();                            // freeze at the current position
    void stop();                             // pause + rewind to the path start
    void restart();                          // rewind + play
    void seek(std::chrono::nanoseconds at);  // jump to a wall-time offset
};
```

`advance()` accrues `elapsedTicks` **only while `playing`** and re-resolves through `sampleWalk` under `mode`
(default `loopIndefinitely()`, so a bare `advance()` just loops — pass `single()` / `loopNTimes(n)` /
`playForDuration(d)` for the others). `stop()` pauses and rewinds to the path start; `restart()` rewinds
and resumes. `seek` jumps to a **wall-time offset** (resolved to ticks via the profile) — a seek by
distance is not offered, because it would need the pacing inverse, which is ill-defined for a non-monotone
`DistanceTween`.

### Timing default

`PathWalker::defaultTiming` is the cadence a bare-constructed walker resolves pacing against.
`EngineConfig::setActive(config)` at startup fans the configured cadence into it (alongside
`AnimationPlayer::defaultTiming` and every `TweenPlayer<T>::defaultTiming`), so a bare walker inherits the
platform's cadence with nothing extra to type. You can also assign it directly at any time, or override a
single walker by setting its `.profile`. It defaults to `TimingProfile::GameBoyColor` and is a single
process-wide default — legitimate here because the platform is single-threaded and this is a config default,
not retained render state.

### Hold-last facing

Where a curve has no direction — a degenerate segment, or an empty path — the resolver honestly returns a
**zero facing**. The cursor keeps the last real heading across such a spot, so a mover parked on a
directionless point keeps pointing the way it was going rather than snapping to zero. Before any heading
has ever been seen (a bare walker, a fully degenerate curve), `facing()` is honestly zero.

### Writing position and facing into draw state

The walker resolves float geometry; you write it into whatever sink you like each render. A common case
is orienting a sprite along travel — turn the facing into a `Transform` rotation:

```cpp
PathWalker mover{.table = path.arcTable(), .pacing = PathPacing::speed(40.0f)};

loop.simTick([&](const InputState&) { mover.advance(); });   // loops by default

loop.renderLoop([&]() {
    const Vec2 p = mover.position();
    const Vec2 f = mover.facing();
    Sprite s{.key = "mover"};
    s.x = int(std::lround(p.x)) - 4;                          // quantize to the integer sink AT THE WRITE
    s.y = int(std::lround(p.y)) - 4;
    s.atlas = arrowAtlas;
    s.palette = arrowPalette;
    s.transform = Transform::rotation(std::atan2(f.y, f.x) * 57.2957795f, 4.0f, 4.0f);  // aim it along travel
    // … submit s in a sprite layer …
});
```

The walker's math is **float end-to-end**; quantization to an integer sink happens at your write (the same
discipline the [tween](tween.md#lerp--interpolation-over-the-float-vocabulary) player keeps). Interpolation
(default on) composes for free: write the per-tick position into game-keyed draw state and the interpolator
eases between ticks by `ObjectKey` — the walker needs no interpolation awareness.

### Re-pathing

The walker owns its table by value, self-contained with no coupling to the source curve. To send a mover
down a different path, assign a fresh table and restart:

```cpp
mover.table = otherCurve.arcTable();
mover.restart();
```

## Where to change things

- **Move something along a path at constant speed:** hold a `PathWalker` with
  `PathPacing::speed(pxPerSecond)`, `advance()` it each tick, read `position()`.
- **Ease along the path (slow at the ends):** `PathPacing::eased(duration, easing)`.
- **Vary the pace freely — accelerate, pause, reverse:** drive the distance from a
  [`Tween<float>`](tween.md) via `PathPacing::distanceTween(profile)` — the tween is the distance-vs-time
  function, and a non-monotone one moves the mover backward.
- **Orient a mover along its path:** read `facing()` and turn it into a `Transform` rotation
  (`std::atan2(f.y, f.x)` in degrees) — position and facing already agree.
- **Loop / play once / play for a while:** pass the [`PlaybackMode`](animation.md#playbackmode--how-it-plays-chosen-when-you-play-it)
  to `advance()`. Looping wraps the distance; a there-and-back is a `DistanceTween` yoyo, not a mode.
- **Send a mover down a new path:** assign `walker.table = newCurve.arcTable();` and `restart()`.
- **Use a non-GBC cadence:** `EngineConfig::setActive` seeds it at startup; or set
  `PathWalker::defaultTiming` once, or a single walker's `.profile`.
- **Shape the path itself:** that's a [`Curve`](curve.md) — the walker only paces travel along it.
- **Move a whole sprite — orient it, spin it, scale it, animate it as it goes:** that's a
  [sprite path](sprite-path.md), the orchestrator built on this walker.

A worked example — three movers on one curve, each a different pacing form, oriented from their headings —
is in [`examples/path_walker_demo/`](../../examples/path_walker_demo/main.cpp).

