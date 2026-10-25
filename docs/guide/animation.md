# Animation

A small helper layer for playing **frame-based animations** — walk cycles, palette cycling, effect
flipbooks — over the platform's immediate-mode draw model. It adds three things: an `Animation` value
type, a **pure stateless resolver** that turns elapsed ticks into the current frame, and a game-owned
`AnimationPlayer` cursor that does the bookkeeping for you. It introduces **no platform state and no new
render path** — a frame still reaches the screen the same way any sprite or tile does, by being threaded
into `FrameDrawState` each tick.

The art a frame points at comes from [images-and-transparency.md](images-and-transparency.md) (an atlas
+ slots); the color it carries is a palette from [tiles-and-color.md](tiles-and-color.md); the
submission types it feeds (`Sprite`, `TileCell`) are in [draw-state.md](draw-state.md). For tweening a
*value* over time (a fade, a ramp) rather than stepping through *frames*, see [tween.md](tween.md).

```cpp
#include "retropp/animation.h"   // Animation, AnimationFrame, PlaybackMode, AnimationPlayer
using namespace std::chrono_literals;
```

## Contents

- [The model](#the-model)
- [`AnimationFrame` — one unit of an animation](#animationframe--one-unit-of-an-animation)
- [`Animation` — the ordered frame list](#animation--the-ordered-frame-list)
  - [Building the frames](#building-the-frames)
- [`PlaybackMode` — how it plays, chosen when you play it](#playbackmode--how-it-plays-chosen-when-you-play-it)
- [The pure resolver](#the-pure-resolver)
- [`AnimationPlayer` — the game-owned cursor](#animationplayer--the-game-owned-cursor)
  - [Timing default](#timing-default)
  - [Threading a frame into draw state](#threading-a-frame-into-draw-state)
- [Where to change things](#where-to-change-things)

## The model

The platform recomputes `FrameDrawState` whole every tick, so animation is *already* possible by hand:
resubmit a fresh frame each tick with a different `Sprite::tile` or `TileCell::palette`. This page's API
removes the bookkeeping — it does not add a capability. Two animation styles fall out of **one** frame
shape:

- **Frame animation** — vary the *art* across frames (a walk cycle).
- **Palette cycling** — hold the art constant and vary the *palette* (a shimmer). Palette is an
  independent per-frame field, so this is not a separate mechanism — just a different field changing.
- **Both at once** — vary art *and* palette per frame.

## `AnimationFrame` — one unit of an animation

```cpp
struct AnimationFrame {
    std::string_view           label;      // optional symbolic id (empty = unnamed); identity, first member
    AtlasId                    sheet;       // the sheet this frame's art comes from (sheet.atlasId)
    std::optional<std::size_t> tileIndex;   // slot index into sheet (sheet[*tileIndex]); omitted = palette-only
    PaletteId                  palette;     // this frame's palette → enables palette cycling
    std::chrono::nanoseconds   duration;    // how long this frame shows (real time; resolved to ticks)
    bool operator==(const AnimationFrame&) const noexcept = default;

    bool            hasArt() const;   // false when tileIndex is omitted (a palette-only frame)
    AtlasId         atlas()  const;   // resolved art (precondition: hasArt()) — feed to Sprite::atlas
    std::uint16_t   tile()   const;   //   the resolved atlas cell → Sprite::tile
    AssetDimensions size()   const;   //   the cell's size → Sprite::size
};
```

The art is named **once**: `.sheet` is the sheet's `AtlasId` and `.tileIndex` is a **slot index** into
that sheet — the `i` in `sheet[i]`, where the sheet is the [`AtlasManifest`](images-and-transparency.md)
`loadAtlas` returned. `atlas()`, `tile()`, and `size()` resolve that slot's art in the same vocabulary a
`Sprite` reads (`.atlas` / `.tile` / `.size`) — the slot's cell and size are arithmetic over the slice
geometry the renderer records at `loadAtlas`, so the frame needs no manifest at read time. Each frame's
art comes wholly from its own named sheet, and different frames may name different sheets, so multi-sheet
animations still compose across frames. Because each frame names its own palette, palette cycling is the
same type.

A **palette-only frame** omits `.tileIndex`: `hasArt()` is `false`, the frame carries no art, and a
consumer keeps whatever art the sprite already shows and updates only the palette — the shimmer idiom (the
art holds, the color changes) expressed as *a frame with no art*.

The frame is authored with the **explicit projection** `.sheet = sheet.atlasId` — dropping the
manifest's slots for the bare handle is always written, never implied. The frame keeps only the id —
nothing is retained or pointed at, so an `Animation` is a plain value with no lifetime tie to the
manifest that named it (an assets struct holding clips returns, moves, and copies freely).

Durations are written in real time (`std::chrono`) and resolved to whole sim ticks at playback against
the timing profile — tick-quantized playback is the honest granularity for a fixed-step sim, and the
the platform never stores ticks itself.

## `Animation` — the ordered frame list

```cpp
struct Animation {
    std::vector<AnimationFrame> frames;

    std::size_t           count() const;
    const AnimationFrame& operator[](std::size_t i) const;          // raw index

    std::optional<std::size_t> indexOf(std::string_view name) const; // by label; nullopt if absent
    const AnimationFrame*      find(std::string_view name) const;    // by label; nullptr if absent
};
```

Pure data plus access. **No playback state and no loop policy live here** — *how* an animation plays is
chosen at play time (next section), never baked into the asset, so the same `Animation` plays once in one
place and loops in another. Labels are programmatic ids; keep them unique within an animation (like a
layer `key` within a frame). Both ways to obtain a frame — time-driven playback and direct selection
(`operator[]` / `find`) — resolve to the same `AnimationFrame`.

### Building the frames

A frame is a plain designated-initializer literal: name the sheet, give a slot index, a palette, a
duration, and an optional label.

```cpp
const Animation walk{{
    {.label = "step0", .sheet = sheet.atlasId, .tileIndex = 0, .palette = pal, .duration = 120ms},
    {.label = "step1", .sheet = sheet.atlasId, .tileIndex = 1, .palette = pal, .duration = 120ms},
}};
```

`.tileIndex = i` selects `sheet[i]` — the slicer's `i`-th carved slot — and `atlas()` / `tile()` / `size()`
resolve from it. Multi-sheet animations name a different `.sheet` per frame:

```cpp
const Animation mixed{{
    {.label = "walk",  .sheet = walkSheet.atlasId,  .tileIndex = 0, .palette = pal, .duration = 120ms},
    {.label = "flash", .sheet = flashSheet.atlasId, .tileIndex = 0, .palette = pal, .duration =  80ms},
}};
```

A **palette-only frame** omits `.tileIndex` — it recolors the art carried over from the frame before it:

```cpp
const Animation shimmer{{
    {.label = "base", .sheet = sheet.atlasId, .tileIndex = 0, .palette = dim,    .duration = 300ms},
    {.label = "glow",                                         .palette = bright, .duration = 300ms},  // art holds
}};
```

A sheet can carve **several runs of frames** in one image (e.g. one row per facing direction): load it
with `ContentKind::AnimationSeries` and a `framesPerAnimation` count, and its slots divide into runs of
that size. `manifest.animationCount()` is how many whole runs the slots divide into, run `g` starts at
slot `g * framesPerAnimation`, and `manifest.animation(g)` is that run's slots. Build one animation's
frames by naming the manifest and that run's slot indices. See the slicer in
[images-and-transparency.md](images-and-transparency.md#slicing).

A manifest records its carve kind in `sheet.kind`, so a consumer holding a mix of sheets can pick the
animated ones before building any clips:

```cpp
for (const AtlasManifest& sheet : loadedSheets) {
    if (sheet.kind != ContentKind::AnimationSeries) continue;   // only the multi-run sheets
    for (std::size_t g = 0; g < sheet.animationCount(); ++g) {
        // sheet.animation(g) is the g-th clip's slots — build an Animation from it.
    }
}
```

## `PlaybackMode` — how it plays, chosen when you play it

```cpp
struct PlaybackMode {
    enum class Kind { Single, LoopNTimes, LoopIndefinitely, PlayForDuration };
    Kind                     kind = Kind::LoopIndefinitely;   // identity, first member
    std::uint32_t            loopCount;                       // LoopNTimes: number of full passes
    std::chrono::nanoseconds duration;                        // PlayForDuration: total wall-time
    bool operator==(const PlaybackMode&) const noexcept = default;

    static PlaybackMode single();                                    // play once, hold the final frame
    static PlaybackMode loopNTimes(std::uint32_t n);                 // loop n passes, then hold final
    static PlaybackMode loopIndefinitely();                          // loop forever (default)
    static PlaybackMode playForDuration(std::chrono::nanoseconds d); // loop for d, then stop
};
```

The playback policy is supplied at play time, not stored on the asset (a policy on the asset would be a
second source of truth). The two data-bearing modes carry their payload (`loopCount`, `duration`) via the
named constructors. `loopIndefinitely()` is the default.

## The pure resolver

```cpp
std::uint64_t  totalTicks(const Animation&, const TimingProfile&);
PlaybackState  sampleAnimation(const Animation&, std::uint64_t elapsedTicks,
                          const TimingProfile&, PlaybackMode);
const AnimationFrame& sampleAnimationFrame(const Animation&, std::uint64_t elapsedTicks,
                              const TimingProfile&, PlaybackMode);   // precondition: count() > 0

struct PlaybackState {
    std::size_t frameIndex = 0;     // the frame to show now
    bool        finished   = false; // a finite mode has reached its end
    constexpr bool operator==(const PlaybackState&) const noexcept = default;
};
```

`sampleAnimation` is **the single source of playback truth** — `AnimationPlayer` is a stateful cursor over it.
Given elapsed ticks and a mode it returns which frame to show now and whether playback has ended:

| Mode | Behavior |
|---|---|
| `LoopIndefinitely` | `elapsed` modulo one pass; never `finished`. |
| `Single` | First pass, then holds the final frame; `finished` once `elapsed ≥ totalTicks`. |
| `LoopNTimes(n)` | Wraps for `n` passes, then holds the final frame; `finished` once `elapsed ≥ n·totalTicks`. |
| `PlayForDuration(d)` | Wraps until `elapsed ≥ ticksForDuration(d)`, then holds the frame shown at the cutoff; `finished` past `d`. |

`totalTicks` is the length of one pass — the sum of each frame's duration resolved to ticks (`0` frames →
`0`). Two edge cases are handled so playback never stalls: a **zero-tick frame** (a duration that rounds
to 0 ticks) is skipped over — never the resting frame and never a stall point — and an **empty animation**
resolves to `{ frameIndex 0, finished true }`. If *every* frame is instantaneous (total length rounds to
0 ticks), a finite mode is immediately `finished` and an indefinite loop simply rests on the last
frame.

`TimingProfile` is a pass-by-value host config (see
[run-loop-and-timing.md](run-loop-and-timing.md)), so the resolver is pure — same inputs, same frame.

## `AnimationPlayer` — the game-owned cursor

The "just play it" wrapper. **State lives here, in your object — not in the platform.** You construct it,
call `advance()` each sim tick, and thread `current()` into draw state. The platform provides the type; you
own the instance, exactly like a `std::vector`. The renderer never sees it.

```cpp
struct AnimationPlayer {
    static inline TimingProfile defaultTiming = TimingProfile::GameBoyColor;

    const Animation* animation = nullptr;        // game-owned; must outlive the player
    TimingProfile    profile   = defaultTiming;  // resolves durations → ticks
    std::uint64_t    elapsedTicks = 0;
    bool             playing = true;
    PlaybackState    state{};                    // cached by advance() so current()/finished() need no args

    void advance(PlaybackMode mode = PlaybackMode::loopIndefinitely(),
                 std::uint64_t deltaTicks = 1);  // accrues ticks (only while playing) + re-resolves

    const AnimationFrame& current() const;       // (*animation)[current frame]; precondition: count() > 0
    std::size_t           currentIndex() const;
    bool                  finished() const;

    void play();                          // resume
    void pause();                         // freeze on the current frame
    void stop();                          // pause + rewind to frame 0
    void restart();                       // rewind + play
    void seek(std::size_t frameIndex);    // jump to a frame's start (no-op if out of range)
    void seek(std::string_view label);    // jump by label (no-op if absent)
};
```

`advance()` accrues `elapsedTicks` **only while `playing`** and re-resolves through `sampleAnimation` under
`mode` (default `loopIndefinitely()`, so a bare `advance()` just loops — pass `single()` / `loopNTimes(n)`
/ `playForDuration(d)` for the others). A null `animation` makes `advance()` a no-op. `stop()` pauses and
rewinds to frame 0 (not finished); `restart()` rewinds and resumes.

### Timing default

`AnimationPlayer::defaultTiming` is the cadence a bare-constructed player resolves durations against.
`EngineConfig::setActive(config)` at startup fans the configured cadence into it (alongside
`RunLoop::defaultTiming` and `Renderer::defaultViewport`), so a bare `AnimationPlayer{.animation = &a}`
inherits the platform cadence with nothing extra to type. You can also assign it directly at any time
(`AnimationPlayer::defaultTiming = loop.timing();`), or override a single player by setting its `.profile`
field. It is a single process-wide default — legitimate here because the platform is single-threaded by
design and this is a config default, not retained render state.

### Threading a frame into draw state

The player resolves *which* frame; you write that frame into your `Sprite` (or `TileCell`) each render:

```cpp
AnimationPlayer p{.animation = &walk};   // bare — inherits defaultTiming

loop.simTick([&](const InputState&) { p.advance(); });          // loops by default

loop.renderLoop([&] {
    const AnimationFrame& f = p.current();
    if (f.hasArt()) {                    // a palette-only frame leaves the current art in place
        sprite.atlas = f.atlas();        // the frame's sheet…
        sprite.tile  = f.tile();         // …its resolved cell…
        sprite.size  = f.size();         // …and size
    }
    sprite.palette = f.palette;          // always — palette cycling lives here
    // … submit the layer …
});
```

To **pin** a frame with no playback at all, index the animation directly — `walk[2]` or
`*walk.find("hurt")` — and thread that into draw state the same way. Want the pure form without the
wrapper? Call `sampleAnimationFrame(walk, elapsedTicks, profile, mode)` and own the tick counter yourself; both ship.

A worked example — one button per playback mode — is in
[`examples/animation_demo.cpp`](../../examples/animation_demo.cpp).

## Where to change things

- **Play a sprite/tile animation:** build an `Animation` of `AnimationFrame`s, hold an `AnimationPlayer`,
  `advance()` it each tick, thread `current()` into a `Sprite`/`TileCell`.
- **Palette cycling (shimmer, day/night flicker):** vary `AnimationFrame::palette` across frames and hold
  `slot` constant — same type, no new art.
- **Change how a clip plays (once vs loop vs N times vs for a duration):** pass a different `PlaybackMode`
  to `advance()` — it is not stored on the `Animation`.
- **One sheet, many clips (per-facing rows):** load with `ContentKind::AnimationSeries` +
  `framesPerAnimation` and build each clip from `manifest.animation(g)`.
- **Drive playback without the cursor object:** call the pure `sampleAnimation` / `sampleAnimationFrame` and own the
  elapsed-tick counter yourself.
- **Animate a *value* instead of frames (fade, ramp, transition):** that's a tween, not an animation —
  see [tween.md](tween.md).
