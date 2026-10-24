# Sprite path

A helper for **driving a sprite along a route while it rotates, scales, and animates** — one game-owned
cursor that composes movement, orientation, and the concurrent tracks off a single clock and writes the
result into a [`Sprite`](draw-state.md). Where a [path walker](path-walker.md) resolves elapsed ticks → a
position and a facing and nothing else, a **sprite path** takes that movement and composes it with rotation
and scale [tween](tween.md) tracks, a frame [animation](animation.md), and a facing policy, then writes the
whole thing into a sprite in one call. It is the orchestrator one level up from the path walker.

A path plays a **sequence** of nodes back-to-back — a patrol route is a chain of legs, each departing from
where the last one ended — under a sequence-level playback mode (loop, rest, N laps, play-for-a-duration).
On top of that sits the **interrupt stack**: suspend the whole current playback, run a detour that departs
from where the sprite stands, and — because movement is relative — carry the route on from where the detour
ended (or snap back to where it began, your choice).

It stays in the same family shape: the content is pure data (`SpritePathNode`), the runtime state lives in
your object (`SpritePath`), and the platform adds **no state and no new render path** — a sprite path just
fills in a `Sprite` you submit, exactly as if you had written the fields by hand each frame.

```cpp
#include "retropp/sprite_path.h"   // SpritePathMove, FacingPolicy, SpritePathNode, SpritePathSample, SpritePath
using namespace std::chrono_literals;
```

## Contents

- [The family](#the-family)
- [The model](#the-model)
- [`SpritePathMove` — where a node travels](#spritepathmove--where-a-node-travels)
- [`FacingPolicy` — how travel orients the sprite](#facingpolicy--how-travel-orients-the-sprite)
- [`SpritePathNode` — the movement plus the tracks](#spritepathnode--the-movement-plus-the-tracks)
- [`SpritePath` — the game-owned cursor](#spritepath--the-game-owned-cursor)
  - [Timing default](#timing-default)
  - [Lazy bake and re-pathing](#lazy-bake-and-re-pathing)
- [Sequences — chaining nodes into a route](#sequences--chaining-nodes-into-a-route)
  - [Node-local clocks](#node-local-clocks)
  - [The wait node and the sentinel node](#the-wait-node-and-the-sentinel-node)
  - [Sequence playback modes](#sequence-playback-modes)
- [The interrupt stack](#the-interrupt-stack)
- [Writing into a sprite — `applyTo`](#writing-into-a-sprite--applyto)
- [Reading the raw sample](#reading-the-raw-sample)
- [Where to change things](#where-to-change-things)

## The family

`SpritePath` sits atop the same players the rest of the guide describes — it resolves each of its tracks
through their pure resolvers off one clock:

| Data (pure, no clock) | Player (game-owned cursor) | Resolves elapsed ticks → |
|---|---|---|
| [`Tween<T>`](tween.md) | `TweenPlayer<T>` | a value |
| [`Animation`](animation.md) | `AnimationPlayer` | a frame |
| [`Curve`](curve.md) / `ArcLengthTable` | [`PathWalker`](path-walker.md) | a position + facing |
| `SpritePathNode` (a sequence of) | **`SpritePath`** | a whole sprite (position + orientation + scale + frame) |

A path walker moves *something*; a sprite path moves *a sprite* — and rotates, scales, and animates it at
the same time, along a whole route, from one `advance()` call.

## The model

Without this layer, a game driving a walking, spinning, breathing sprite along a route runs a walker, two
tween resolvers, an animation player, a tangent-to-angle conversion, and several field writes by hand every
tick — and re-arms all of it at each leg boundary. `SpritePath` is that whole bundle behind one cursor. Three
pieces:

- **`SpritePathNode`** — pure data describing one leg of motion: where it travels (`SpritePathMove`), how
  fast ([`PathPacing`](path-walker.md#pathpacing--the-time--distance-driver)), which way it faces
  (`FacingPolicy`), and the optional rotation / scale / animation tracks that run alongside the move.
- **`SpritePathSample`** — the composed per-tick result as raw values (position, facing, total rotation in
  degrees, scale, flipX, the current frame, resolved distance, and whether the sequence's movement finished).
- **`SpritePath`** — the game-owned cursor: it holds the elapsed-tick counter, the base node sequence, the
  interrupt stack, and the playback controls, and each tick it composes the sample and lets you write it into
  a `Sprite`.

A path plays a **sequence** of nodes; the pieces below describe one node, then how a route chains them.

## `SpritePathMove` — where a node travels

A movement spec, resolved to a [`Curve`](curve.md) when the node is entered. It has four forms, each with a
named constructor (aggregate initialization works too and is interchangeable):

```cpp
struct SpritePathMove {
    enum class Kind : std::uint8_t { Line, ThroughPoints, Hermite, Curve };
    Kind                kind = Kind::Line;      // identity, first member
    std::optional<Vec2> origin{};              // absent → the inherited origin (start / previous node's end)
    Vec2                destination{};          // Line / Hermite: the point traveled to
    std::vector<Vec2>   points;                // ThroughPoints: traveled through in order, ending at back()
    Vec2                originTangent{};         // Hermite: the departure directional vector
    Vec2                destinationTangent{};    // Hermite: the arrival directional vector
    Curve               curve{};               // Curve: travel this exact curve verbatim (no origin defaulting)
    static SpritePathMove to(Vec2 destination);                          // a straight line
    static SpritePathMove through(std::vector<Vec2> points);             // a Catmull-Rom through the points
    static SpritePathMove hermite(Vec2 destination, Vec2 originTangent, Vec2 destinationTangent);
    static SpritePathMove onCurve(Curve c);                              // a pre-authored curve, verbatim
    // …each (except onCurve) also has a leading-origin overload…
};
```

- **`to(destination)`** — a straight line from the origin to `destination`.
- **`through(points)`** — a smooth [Catmull-Rom](curve.md) curve traveling through the listed points in
  order, ending at the last one.
- **`hermite(destination, originTangent, destinationTangent)`** — a cubic leaving the origin along
  `originTangent` and arriving at `destination` along `destinationTangent` (a directional vector *is* a
  tangent).
- **`onCurve(curve)`** — travel a curve you already built (Bézier handles, a loop, anything a `Curve` can be).

**Origin defaulting.** The origin is only known when the node is entered, so for `to` / `through` / `hermite`
it defaults to the **inherited origin**: the path's `start` for the first node, or the previous node's end for
every node after it (see [Sequences](#sequences--chaining-nodes-into-a-route)). Pass an explicit origin with
the leading-`Vec2` overload (`to(origin, destination)`) to author a **jump**. `onCurve` is the other
exception — it starts wherever its own geometry starts, so origin defaulting does not apply.

## `FacingPolicy` — how travel orients the sprite

```cpp
enum class FacingPolicy : std::uint8_t { None, FlipX, RotateToFacing };
```

- **`None`** (default) — travel does not touch the sprite's orientation.
- **`FlipX`** — the sprite mirrors (`flipX = true`) while it travels toward −x. It **holds** its previous
  mirror state while the horizontal component of travel is zero, so purely vertical motion never flip-flops a
  mirrored sprite — and it holds that state across node boundaries too.
- **`RotateToFacing`** — the sprite rotates to point along travel (art is assumed authored facing +x). This
  angle **sums** with the rotation track below, so a sprite can nose along its path *and* spin.

The raw `facing()` stays available whichever policy is set, so a game can always compute its own orientation.

## `SpritePathNode` — the movement plus the tracks

```cpp
struct SpritePathNode {
    std::string_view            label;                     // optional symbolic id
    SpritePathMove              move;                      // where it travels
    PathPacing                  pacing;                    // how fast (default: parked)
    FacingPolicy                facing = FacingPolicy::None;
    std::optional<Tween<float>> rotationDegrees;           // rotation track (absent = none)
    PlaybackMode                rotationMode = PlaybackMode::single();
    std::optional<Tween<Vec2>>  scale;                     // scale track (absent = none)
    PlaybackMode                scaleMode = PlaybackMode::single();
    const Animation*            animation = nullptr;       // frames track (game-owned)
    PlaybackMode                animationMode = PlaybackMode::loopIndefinitely();
    std::optional<Vec2>         pivot;                     // rotation/scale pivot (absent = sprite center)
};
```

A node is **pure data** — copyable, self-contained, with no runtime state. The rotation and scale tracks are
held **by value** ([`Tween`](tween.md) values are small and copyable); the `Animation` is held **by pointer**
(a shared asset the game owns for the cursor's lifetime), as is a [`DistanceTween`](path-walker.md#pathpacing--the-time--distance-driver)
pacing profile.

Each track resolves against the node's **node-local clock** (ticks since the node was entered), under its own
[`PlaybackMode`](animation.md#playbackmode--how-it-plays-chosen-when-you-play-it). An **absent** track (a
`std::nullopt` or a null `animation`) contributes identity — no rotation, unit scale, no frame change — which
is distinct from a present track resting at its final value. The track defaults suit the common case: the
tween tracks play once (`single()`), and the animation loops (`loopIndefinitely()`).

The optional **`label`** names the node so game logic keyed to a route leg can read it back off the cursor
(`currentNode()->label`).

## `SpritePath` — the game-owned cursor

The "just play it" cursor over a node sequence. **State lives here, in your object — not in the platform.** You
construct it, call `advance()` each sim tick, and either write it into a sprite with `applyTo` or read the
composed sample. The same control surface as the other players.

```cpp
struct SpritePath {
    static inline TimingProfile defaultTiming = TimingProfile::GameBoyColor;

    std::vector<SpritePathNode> nodes;             // the base sequence this path plays
    Vec2                        start;             // where the chain begins when nodes[0] names no origin
    TimingProfile               profile = defaultTiming;
    std::uint64_t               elapsedTicks = 0;  // the ACTIVE content's clock (base, or the top interrupt)
    bool                        playing = true;
    SpritePathSample            sample{};          // cached by advance() so the getters need no arguments

    void advance(PlaybackMode mode = PlaybackMode::loopIndefinitely(), std::uint64_t deltaTicks = 1);
    void applyTo(Sprite& s) const;

    Vec2  position() const;   Vec2  facing()  const;   float rotationDegrees() const;
    Vec2  scaleValue() const; bool  flipX()   const;   const AnimationFrame* frame() const;
    float distance() const;   bool  finished() const;

    std::size_t           currentNodeIndex() const;    // the active content's current node
    const SpritePathNode* currentNode()      const;    // nullptr when the active content is empty

    void interrupt(std::vector<SpritePathNode> nodes, PlaybackMode mode = PlaybackMode::single(),
                   ResumePolicy resume = ResumePolicy::Continue);
    void popInterrupt();                               // end the current interrupt now; resume
    bool        interrupted()    const;
    std::size_t interruptDepth() const;

    void play();     void pause();    void stop();     void restart();
    void seek(std::chrono::nanoseconds at);
};
```

`advance(mode)` accrues `elapsedTicks` **only while `playing`** and re-composes the sample. `mode` is the
**base sequence's** playback mode (default `loopIndefinitely()`, so a bare `advance()` loops the route). `stop()`
pauses and rewinds to base node 0 (clearing the interrupt stack); `restart()` rewinds and resumes (also
clearing the stack — the re-path entry point); `seek` jumps the active content's clock to a **wall-time
offset** (resolved to ticks via the profile) and leaves the stack untouched. `finished()` reports whether the
active content's **movement** finished under its mode — the tracks are subordinate and never gate completion.

### Timing default

`SpritePath::defaultTiming` is the cadence a bare-constructed path resolves pacing and track durations
against. [`EngineConfig::setActive`](platform-and-windowing.md) fans the configured cadence into it at
startup, alongside the other players' `defaultTiming`, so a bare path inherits the platform cadence with
nothing extra to type. Assign it directly at any time, or override one path via `.profile`.

### Lazy bake and re-pathing

The base sequence is baked to per-node [`ArcLengthTable`](curve.md#reusing-the-arc-length-table)s **lazily on the first
`advance()`** (and re-baked by `stop()` / `restart()`), because designated initialization cannot run a bake
and the chain start is only known then. To send a mover down a different route, assign a new `nodes` list and
`restart()`:

```cpp
mover.nodes = {SpritePathNode{.move = SpritePathMove::to({120.0f, 40.0f}), .facing = FacingPolicy::RotateToFacing}};
mover.restart();   // re-bakes the new route's geometry
```

## Sequences — chaining nodes into a route

A path's `nodes` is the route it walks. The nodes play **back-to-back**, and each node's move — when it
authors no origin — departs from **the previous node's end** (node 0 departs from `start`). A patrol is just
a list of legs:

```cpp
SpritePath guard{.nodes = {{.label = "march",  .move = SpritePathMove::to({130.0f, 112.0f}), .pacing = PathPacing::speed(30.0f)},
                           {.label = "sweep",  .move = SpritePathMove::through({{130.0f, 84.0f}, {40.0f, 84.0f}}), .pacing = PathPacing::speed(30.0f)},
                           {.label = "return", .move = SpritePathMove::hermite({30.0f, 112.0f}, {-40.0f, 60.0f}, {40.0f, 30.0f}), .pacing = PathPacing::speed(30.0f)}},
                 .start = {30.0f, 112.0f}};
```

`march` runs `start` → (130,112); `sweep` inherits (130,112) and runs through the two points; `return`
inherits the sweep's end and arcs back to (30,112). No origin is repeated — each leg picks up where the last
left off. **"The previous node's end"** is its final resolved movement position: the curve's end for
`Speed` / `Eased` pacing, or wherever a `DistanceTween` comes to rest (a tween that stops mid-curve chains
from mid-curve — continuity over geometry). An **explicit origin** (`to(origin, destination)`) or an
`onCurve` leg is an authored **jump** that ignores the chain.

Boundaries are exact: a batched `advance(mode, 100)` crossing several legs resolves identically to 100
tick-by-tick advances, and `seek(t)` re-derives the same landing from the same tick count — the chain's leg
durations and end positions are clock-independent.

### Node-local clocks

Each node's movement and tracks resolve against ticks **since the node was entered** — the movement plays one
pass per entry, and every entry (including re-entry when the sequence loops) restarts the node's clock. A node
is a **self-contained scene that replays whole**: an animation on a looping patrol leg restarts each lap, a
`single()` rotation track on a leg runs fresh each time that leg is entered. The node's movement duration —
how long the leg holds the cursor before it rolls to the next — is the movement's own one-pass length (for
`Speed`, the ticks to cover the leg; for `Eased`, its duration; for a `DistanceTween`, the tween's length). A
track *longer* than the movement is simply cut off when the leg ends; a track *shorter* than the movement rests
(or loops) at its own mode for the remainder of the leg.

When a finite sequence (`single()` / `loopNTimes`) comes to rest at the last leg's end, the movement holds the
endpoint but the **node-local clock keeps running**, so the last leg's tracks keep playing — a courier resting
at the route's end whose walk cycle is still animating.

### The wait node and the sentinel node

Two idioms fall out of the pacing kinds, both intentional:

- **The wait node** — a zero-length move (`to` the same point) with `Eased` pacing. The move covers no
  distance, so the sprite stands still, but the node stays entered for the eased duration while its tracks
  play. A patrol pausing at a corner is a wait node between two traveling legs.
- **The sentinel node** — `Speed` 0 (the parked default) on a move with real geometry. Constant speed 0 never
  reaches the end, so the leg **never finishes**: the sequence rests there indefinitely, tracks playing, until
  an interrupt or a re-path moves it on. A guard standing post until something happens is a sentinel node.

### Sequence playback modes

The `mode` passed to `advance()` is the **sequence-level** playback mode over the base node list, mirroring
the [`PlaybackMode`](animation.md#playbackmode--how-it-plays-chosen-when-you-play-it) the other players use —
but applied to the *route*, not a single track:

| Mode | The route does |
|---|---|
| `loopIndefinitely()` *(default)* | wrap to node 0 after the last node (re-chaining from `start`); never finishes |
| `single()` | one pass, then rest at the last node's end; `finished()` |
| `loopNTimes(n)` | `n` laps, then rest at the last node's end; `loopNTimes(0)` rests at the chain start |
| `playForDuration(d)` | wrap until `d` elapses, then hold the sample at the cutoff; `finished()` past `d` |

`finished()` tracks the *movement* finishing under the mode — a rested route's current node keeps resolving
its tracks, so a finished path is not a frozen one.

## The interrupt stack

The reason `SpritePath` exists beyond a bare walker: a guard mid-patrol gets distracted, runs a detour, and
carries the patrol on from where the detour leaves him — or snaps back to his post, your call. `interrupt()`
suspends the *entire* current playback and starts new content on top; when that content finishes it
**auto-pops** and the base resumes — drifting on from the detour's end by default (`ResumePolicy::Continue`),
or restored to its exact pre-interrupt state under `ResumePolicy::Return`.

```cpp
// The guard breaks off to a spot, then resumes the patrol where it was interrupted:
guard.interrupt({{.label = "detour", .move = SpritePathMove::to({80.0f, 44.0f}), .pacing = PathPacing::speed(52.0f)}});
```

```cpp
enum class ResumePolicy : std::uint8_t {
    Continue,  // carry on from the sprite's current position — the route drifts by the detour's displacement (default)
    Return,    // snap back to the exact position held when the interrupt was pushed
};
```

- **Departure.** The interrupting content's first node inherits its origin from the **sprite's current
  position** — the detour departs from where the sprite stands (explicit origins and `onCurve` stay authored
  jumps, as always).
- **Its own mode.** `interrupt(nodes, mode)` plays the content under its own captured `mode` (default
  `single()`); the base's `advance(mode)` argument is not consulted while an interrupt is active.
- **How it hands back — `ResumePolicy`.** Movement is relative (a node's shape is authored against its
  origin), so on pop the route by default **continues from where the detour ended**:
  - **`Continue`** (default) — the resumed content carries on from the sprite's current position; its
    geometry shifts by the detour's net displacement, so the route **drifts** on. A guard who chases something
    across the yard keeps patrolling from the new corner.
  - **`Return`** — the sprite snaps back to the exact position it held when the interrupt was pushed, and the
    route resumes there unchanged. A guard who glances at a noise and returns to his post.
- **Auto-pop.** When the interrupt's sequence finishes under its mode, it pops **within the same
  `advance()`**, and the leftover ticks of that batch flow into the resumed content (drifted under `Continue`,
  restored under `Return`). Progress and clock are preserved either way — only the position anchor differs.
- **`popInterrupt()`** ends the current interrupt immediately and resumes under the frame's `ResumePolicy`. It
  is the **only** exit from a `loopIndefinitely()` interrupt (a chase loop the game ends explicitly), which
  never auto-pops.
- **Depth > 1.** An interrupt during an interrupt suspends the same way; pops cascade naturally, each frame
  restoring the runtime beneath it. `interrupted()` and `interruptDepth()` report the stack; `currentNode()`
  and `currentNodeIndex()` reflect the **active** content (the top of the stack).
- **`stop()` / `restart()`** clear the whole stack and reset to base node 0. **`seek()`** drives the active
  content's clock and leaves the stack untouched.

## Writing into a sprite — `applyTo`

`applyTo(sprite)` writes the fields the path's content declares, and leaves everything else as the game set
it. Two of the decisions are a **union over all the content the path currently holds** (the base nodes plus
every stacked interrupt) — so a transition off a rotating or mirroring node clears the stale state instead of
freezing it:

| Sprite field | Written when | Value |
|---|---|---|
| `x` / `y` | always | `std::lround(position)` — the one quantize point (the path's math is float end-to-end) |
| `atlas` / `tile` / `size` / `palette` | the **current** node has an animation track | the current frame's art (a node without one leaves the last art showing — holding art is meaningful; writing "no art" is not) |
| `transform` | **any** node the path holds declares rotation, scale, or `RotateToFacing` | scale, then rotation, about the pivot (`node.pivot`, or the sprite's center using the size after the frame write). While the *current* node drives neither axis the sample is identity, so a tumble **stops** instead of freezing at the last rotation |
| `flipX` | **any** node the path holds uses `FacingPolicy::FlipX` | the mirror state from travel (held across nodes that don't drive it) |
| `key`, `alpha`, `flipY`, the 90° texture `rotation` | never | left untouched — the game's |

So a sprite already carrying a `key`, an `alpha`, and its own art keeps all of them; `applyTo` only moves,
orients, and (if the current node animates) re-arts it.

```cpp
SpritePath mover{.nodes = {{.move   = SpritePathMove::to({140.0f, 40.0f}),
                            .pacing = PathPacing::speed(30.0f),
                            .facing = FacingPolicy::RotateToFacing}},
                 .start = {20.0f, 40.0f}};

loop.simTick([&](const InputState&) { mover.advance(); });

loop.renderLoop([&]() {
    Sprite s{.key = "mover"};
    s.atlas = arrowAtlas;   // art the node doesn't animate is set by the game
    s.palette = arrowPalette;
    mover.applyTo(s);       // writes position + the RotateToFacing transform
    // … submit s in a sprite layer …
});
```

Interpolation (default on) composes for free: `applyTo` writes into a game-keyed sprite each tick and the
interpolator eases between ticks by [`ObjectKey`](draw-state.md).

## Reading the raw sample

When you want to place the result yourself — a shadow offset from the mover, a trail, a HUD readout — read
the composed values directly instead of calling `applyTo`:

```cpp
const Vec2 p = mover.position();       // float position
Sprite shadow{.key = "moverShadow"};
shadow.atlas = shadowAtlas;
shadow.palette = shadowPalette;
shadow.x = int(std::lround(p.x)) + 2;  // your own offset + quantize
shadow.y = int(std::lround(p.y)) + 3;
```

`applyTo` and the getters agree on every shared value — `applyTo` is the common-case write, the getters are
the composed values when you want to place them differently. The sample carries raw values (no pre-composed
`Transform`) because the rotation/scale pivot defaults to the sprite's center, and the sprite's size is only
known at the write.

Each getter returns one field of the cached `SpritePathSample`:

```cpp
struct SpritePathSample {
    Vec2                  position{};              // float — quantized at the write
    Vec2                  facing{};                // the movement facing (holds the last real heading across dead spots)
    float                 rotationDegrees = 0.0f;  // the rotation track + RotateToFacing, summed
    Vec2                  scale{1.0f, 1.0f};       // the scale track (identity when absent)
    bool                  flipX = false;           // meaningful under FacingPolicy::FlipX
    const AnimationFrame* frame = nullptr;         // the current frame (nullptr = no animation track)
    float                 distance = 0.0f;         // the movement arc-length resolved
    bool                  finished = false;        // MOVEMENT finished (the tracks never gate completion)
};
```

## Where to change things

- **Walk a sprite along a line / smooth path / arc:** a `SpritePath` whose node's `move` is
  `to` / `through` / `hermite`, `advance()` it each tick, `applyTo` a sprite.
- **Chain a whole route:** list several nodes in `nodes` — each leg inherits the previous leg's end as its
  origin. Author a jump with an explicit origin or an `onCurve` leg.
- **Pause at a corner / stand post:** a [wait node](#the-wait-node-and-the-sentinel-node) (zero-length move +
  `Eased`) or a [sentinel node](#the-wait-node-and-the-sentinel-node) (`Speed` 0 on real geometry).
- **Loop / run once / lap N times / play for a duration:** pass the [sequence
  mode](#sequence-playback-modes) to `advance()`.
- **Ride a pre-built curve:** `SpritePathMove::onCurve(myCurve)`.
- **Pace it — constant speed, eased, or a reversing profile:** the node's
  [`PathPacing`](path-walker.md#pathpacing--the-time--distance-driver), exactly as a path walker.
- **Face travel:** set `FacingPolicy::FlipX` (mirror) or `RotateToFacing` (rotate).
- **Spin or scale it while it moves:** give the node a `rotationDegrees` and/or `scale`
  [`Tween`](tween.md) track, each with its own `PlaybackMode`; pick the pivot with `node.pivot` (default is
  the sprite's center).
- **Animate its art as it moves:** point `node.animation` at an [`Animation`](animation.md).
- **Break off to a detour:** `interrupt(detourNodes)` — it departs from the current position; on finish the
  route continues from where the detour ended (`ResumePolicy::Continue`, the default — it drifts) or snaps
  back (`ResumePolicy::Return`). `popInterrupt()` ends it early (and is the only exit from a looping interrupt).
- **Send it down a new route:** assign a new `nodes` list and `restart()`.
- **Use a non-GBC cadence:** `EngineConfig::setActive` seeds it at startup, or set `SpritePath::defaultTiming`,
  or one path's `.profile`.

Two worked examples: a single-node showcase — seven movers each exercising one part of the composer — is in
[`examples/sprite_path_demo/`](../../examples/sprite_path_demo/main.cpp); the sequencing + interrupt layer —
a looping patrol with a wait node, a sentinel post, the four sequence modes, and the interrupt stack — is in
[`examples/sprite_patrol_demo/`](../../examples/sprite_patrol_demo/main.cpp).
