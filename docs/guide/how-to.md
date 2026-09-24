# How-to recipes

Task-oriented snippets for common things you'll want to do. Each assumes you have the four core
objects wired (see [getting-started.md](getting-started.md)) and an atlas + palette uploaded. The
reference for every type used here is in [draw-state.md](draw-state.md),
[tiles-and-colour.md](tiles-and-colour.md), and [input.md](input.md).

Recipes:

- [Scroll a background](#scroll-a-background)
- [Make a character walk behind scenery](#walk-behind)
- [Add a HUD that doesn't scroll](#fixed-hud)
- [Draw and animate a sprite](#animate-a-sprite)
- [Fade the screen / day-night tint](#screen-fade)
- [Recolour a scene without new art](#recolour)
- [Fill a shape with a live effect (an outline that does stuff inside)](#fill-a-shape)
- [Load a tileset from a PNG](#load-png)
- [Slice an atlas into addressable assets](#slice-atlas)
- [Play an animation (frames + palette over time)](#play-animation)
- [Tween a value over time (fades, ramps, transitions)](#tween-a-value)
- [React to an action press (menus)](#button-press)
- [Make a draggable title bar on a chromeless window](#draggable-title-bar)
- [Retained vs rebuilt frame state](#retained-vs-rebuilt-frame)
- [Play music and a sound effect](#play-audio)
- [Play a recorded audio file](#play-audio-file)
- [Make something glow](#make-something-glow)
- [Blend a layer into what's under it](#blend-a-layer)
- [Tell whether two things actually touch](#hit-test)
- [Move a sprite along a path](#move-along-a-path)
- [Save the player's progress](#save-progress)
- [Keep the player's other files](#user-files)
- [Ship an asset inside the binary](#embed-an-asset)
- [Go fullscreen, or scale the window](#fullscreen)
- [Rumble the controller](#rumble)
- [Run a routine as original hardware code](#run-a-routine)

---

## Scroll a background <a id="scroll-a-background"></a>

A layer's `scroll` offsets where the tilemap is sampled. By default the map wraps toroidally
(`TileContent::wrap == TileWrap::Repeat`), so a small map tiles across an arbitrarily large scroll;
set `wrap` to `Clamp` or `Blank` for a finite map (see [draw-state.md](draw-state.md)). Move the
camera in your tick, apply it in your render:

```cpp
// tick: advance a camera from input or game state
cam.x += speed;

// render: point the layer at the camera
frame.layers[bgIndex].scroll = LayerScroll{cam.x, cam.y};
```

Different layers with different scroll rates give you **parallax** for free — a far layer scrolls
slower than a near one.

## Make a character walk behind scenery <a id="walk-behind"></a>

**Depth is `z` alone.** To make the player pass behind a treetop, put the treetop on a layer with a
higher `z` than the player's layer:

```cpp
playerLayer.z  = 10;
treetopLayer.z = 20;   // higher z = drawn later = in front of the player
```

Give the treetop layer per-source transparency (so its empty pixels don't block the player) — see
[load a tileset from a PNG](#load-png) and [images-and-transparency.md](images-and-transparency.md)
for the transparent-index upload. Want the player *in front* sometimes and *behind* other times?
Change which layer's `z` is higher per frame — `z` is ordinary per-frame data.

## Add a HUD that doesn't scroll <a id="fixed-hud"></a>

A HUD is just another layer — give it a high `z` (so it's on top) and a fixed `scroll` of `{0, 0}`
(so it ignores the camera) while your world layers scroll:

```cpp
hud.key  = "HUD";
hud.z      = 1000;            // above everything
hud.scroll = LayerScroll{0, 0};   // stays put while the world scrolls beneath it
```

The HUD can be tiles (a status bar) or sprites (icons, a cursor). Nothing else is special about it —
"HUD" is your meaning, not a platform role.

## Draw and animate a sprite <a id="animate-a-sprite"></a>

Put sprites on a layer with `SpriteContent`. Each `Sprite` names a position (in the layer's space,
before scroll), a size, an atlas tile, and its own sheet + palette. Animate by changing the `tile` (or
position) over time:

```cpp
Sprite hero{.key = "hero"};   // key is required — the stable identity the interpolator tracks
hero.x = heroX; hero.y = heroY;
hero.size    = AssetDimensions::GameBoy8x16;
hero.atlas   = spriteAtlas;     // the sprite names its own sheet…
hero.tile    = walkFrame;       // advance walkFrame on a timer for animation
hero.palette = heroPal;         // …and its own palette

std::array<Sprite, 1> sprites{hero};
layer.content = SpriteContent{std::span<const Sprite>(sprites)};
```

Sprite transparency is opt-in, exactly like the tile path: a sheet declares which palette indices are
holes at upload (`uploadAtlas(..., TransparentIndices::GameBoy)` for the conventional index-0 OBJ hole,
or `::of({n})`), and a palette entry with alpha 0 is a hole too — either way sprite art reads through to
whatever is behind it. A sprite on a scrolling layer tracks the world; on a `{0,0}` layer it stays fixed
(a cursor). Details + flip in [draw-state.md](draw-state.md). For
timed playback (looping / once / N-loops / palette-cycling) without hand-tracking the frame counter,
use the animation layer — see [Play an animation](#play-animation).

To fade a single sprite — a respawn blink, a ghosting enemy — set its `alpha` (`hero.alpha = 0.5f`,
default `1.0` opaque). It multiplies under the layer's own `alpha`, so one sprite fades while the rest of
its layer stays solid, and it eases smoothly between ticks under the automatic interpolator.

## Fade the screen / day-night tint <a id="screen-fade"></a>

Whole-frame colour is a screen-space effect: a **`ColorFill`** region (no shape → whole viewport) plus a
blend mode, distinct from the per-pixel palette colouring. Day/night is a **Multiply** `ColorFill` (the
fill tints/darkens the whole scene); a fade or a flash is a **Normal** `ColorFill` at `alpha = strength`:

```cpp
// Fade to black: a Normal black fill, alpha 0 → 1.
frame.regions.push_back(Region{
    .key     = "screenFade",                 // key is required — the first member of every Region
    .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{0, 0, 0}}},
    .alpha   = fade});                       // 0 = clear, 1 = black

// Day/night: a Multiply fill (scene · tint). A cutscene flash is the same with .fill = white, Normal blend.
frame.regions.push_back(Region{
    .key     = "dayNight",
    .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = nightTint}},
    .blend   = BlendMode::Multiply});
```

Multiply darkens; to brighten use Add (`scene + fill`). Confine any of these to a shape for a glow / shadow
/ spotlight. The runnable showcase is [`examples/colour_effects_demo`](../../examples/colour_effects_demo/main.cpp). See
[draw-state.md](draw-state.md#whole-frame-colour).

## Recolour a scene without new art <a id="recolour"></a>

Because colour is a palette applied at render time, you recolour by changing the palette — not the
art. Either upload a new palette and rewrite the cells' `palette` handle to it, or point each cell at a
different already-uploaded palette. A water-shimmer or palette-cycle is a per-frame palette swap, no new
tiles and no shader edit. See [tiles-and-colour.md](tiles-and-colour.md#where-to-change-things).

## Fill a shape with a live effect (an outline that does stuff inside) <a id="fill-a-shape"></a>

To make a *shape* whose interior does something — a porthole that ripples, a heat-shimmer pond, a
scrying lens — put an effect in a `Region` for that shape. Think of it as a **fill**: the region's shape
is the area, its effect is *what fills it*. Three independent pieces compose with no glue:

```cpp
#include "retropp/curve.h"        // the shape (a closed curve — or use ShapePoints::circle/rectangle/…)
#include "retropp/draw_state.h"   // Region, ShapePoints, ScreenSpaceEffect

Curve outline = Curve::quadratic({80, 32}, {128, 32}, {128, 72});   // a rounded shape, smooth boundary
outline.quadraticTo({128, 112}, {80, 112})
       .quadraticTo({32, 112}, {32, 72})
       .quadraticTo({32, 32}, {80, 32});

ScreenSpaceEffect fill{ .kind = ScreenSpaceEffectKind::Ripple, .amplitude = 3.0f,
                        .frequency = 5.0f, .center = Point{80, 72}, .decay = 2.0f };
frame.regions.push_back(Region{ .key     = "lens",                           // key is required (Region's first member)
                                .shape   = ShapePoints::fromCurve(outline),  // the shape IS the fill area
                                .effects = {fill} });                        // the region owns the effect
```

What fills the shape is whichever effect you pick:

| effect | the interior becomes |
|---|---|
| `Ripple` / `RowDisplacement` / `Swirl` | a warped copy of the scene — shimmer, heat haze, water, a whirlpool |
| `ColorFill` | a flat colour (`.fill`), solid — or translucent via the `Region`'s `alpha` |
| `ColorSaturation` / `Gleam` | the interior colour-graded in place — desaturated toward grey, or a luminance-keyed sheen |
| `Bloom` | the interior's bright content glowing — a blurred halo of its own light added over the scene inside the shape |
| `Glow` | the interior radiating a colour you pick (`.fill`) — an authored aura, dark content included |
| a `Custom` shader | anything it draws — a texture, a pattern, a procedural fill (it need not sample the scene) |

A flat-colour fill is the built-in **`ColorFill`** (`.kind = ScreenSpaceEffectKind::ColorFill`;
[reference](draw-state.md#painting-a-colour-into-a-region-colorfill)) — set `.fill` to an `Rgba8` colour;
no custom shader needed. Make it a translucent tint by giving the owning `Region` an `alpha` below 1. Give
the region's shape new coordinates or a `shape.transform` each frame and the fill glides with it — a roaming
spotlight or scanner sweep. A region works on a layer too (`DrawLayer::regions`), confining the fill to one
layer.

**Drawing the outline itself.** To show a *stroke* — a coloured line *along* the shape's boundary instead
of a filled interior — give the region a `shape.strokeWidth` and fill the band with `ColorFill`: a stroked
`Region` + `ColorFill` **is** a drawn line (a ring, a border, or — with an open `fromCurve` — an arbitrary
curved path). No walking the curve into sprites by hand. See
[stroke / outline](draw-state.md#confining-an-effect-to-a-shape-region) and
[`ColorFill`](draw-state.md#painting-a-colour-into-a-region-colorfill).

**A fill *adds*; it does not make a layer see-through.** Inside the shape you see the scene **plus** the
effect — the layers underneath are untouched. To instead *reveal what is below* a layer along a shape — a
true see-through window or cutout — use the **`stencil()`** helper (a `Transparency` effect in a region;
see [draw-state.md](draw-state.md#making-a-layer-see-through-the-stencil-helper)). In short: a region
**fills** a shape, `stencil()` makes a shape **see-through**.

See [draw-state.md](draw-state.md#confining-an-effect-to-a-shape-region) for the full region surface
(shapes, `radius`, `transform`, curved boundaries) and [curve.md](curve.md) for authoring the shape.

## Load a tileset from a PNG <a id="load-png"></a>

`loadPng` decodes an indexed/grayscale PNG into an index plane you feed straight to `uploadAtlas`:

```cpp
#include "retropp/image.h"

const LoadedImage img = loadPng("assets/tileset.png");
const AtlasId atlas = renderer.uploadAtlas(img.indices.data(), img.width, img.height).atlasId;

// For a transparent colour (a hole that reveals the layer beneath), name its index on upload:
const AtlasId holed = renderer.uploadAtlas(img.indices.data(), img.width, img.height, TransparentIndices::of({0})).atlasId;
```

Author art as **indexed or grayscale** PNGs (the faithful console format); supply colour separately
via `uploadPalette`, or use the PNG's embedded palette (`img.palette`). Full routing + transparency
rules in [images-and-transparency.md](images-and-transparency.md).

## Slice an atlas into addressable assets <a id="slice-atlas"></a>

When a PNG holds a *grid* of tiles or sprite frames, `loadAtlas` uploads it once and hands back an
**`AtlasManifest`** — the atlas handle plus one **`AssetSlot`** per carved sub-asset (its top-left
atlas cell + dimensions), so you never hand-compute tile indices. Pick the asset size, a
**`ContentKind`** (`Single` / `Tileset` / `SpriteSeries`), and a **`ReadOrder`**:

```cpp
#include "retropp/renderer.h"   // AtlasManifest; ContentKind / ReadOrder come from image.h

// A 16-wide strip of 8×8 frames, read left-to-right (the default order):
const AtlasManifest walk =
    renderer.loadAtlas("assets/hero_walk.png", AssetDimensions::GameBoy8x8, ContentKind::SpriteSeries);

Sprite frame{.key = "hero"};   // key is required (see Draw and animate a sprite)
frame.size = walk[walkFrame].dimensions;   // walk[i] is the i-th carved slot
frame.tile = walk[walkFrame].tile;         // advance walkFrame on a timer
```

`Single` yields one slot covering the whole image; `Tileset` and `SpriteSeries` carve a grid
identically (the names just read your intent at the call site). The **read order** has all eight
permutations as named presets — `ReadOrder::LeftRightThenDown` (western default),
`TopBottomThenRight` (column-major), and the rest — for art laid out in non-western orders. If a sheet
has room for more cells than its art uses, pass a `count` so you get exactly the real frames:
`loadAtlas(path, size, ContentKind::SpriteSeries, ReadOrder::LeftRightThenDown, /*count=*/5)`. A
trailing partial cell is dropped (full cells only); a degenerate request yields an empty manifest.
To re-slice the same uploaded atlas in a different order/count without re-uploading, call the pure
`sliceLayout(...)` directly. Full reference in
[images-and-transparency.md](images-and-transparency.md#slicing).

## Play an animation (frames + palette over time) <a id="play-animation"></a>

The hand-rolled "advance `walkFrame` on a timer" in
[Draw and animate a sprite](#animate-a-sprite) works, but `animation.h` removes the
bookkeeping. An **`Animation`** is a list of **`AnimationFrame`**s — each `{ label, sheet, tileIndex,
palette, duration }` — and a game-owned **`AnimationPlayer`** plays it: advance it each sim tick and
thread `current()` into draw state.

```cpp
#include "retropp/animation.h"
using namespace std::chrono_literals;

// each frame names its sheet and a slot index into it
const Animation walk{{
    {.label = "step0", .sheet = sheet, .tileIndex = 0, .palette = pal, .duration = 120ms},
    {.label = "step1", .sheet = sheet, .tileIndex = 1, .palette = pal, .duration = 120ms},
}};

AnimationPlayer p{.animation = &walk};                  // inherits the platform cadence (setActive)

loop.simTick([&](const InputState&) { p.advance(); });  // loops by default
loop.renderLoop([&] {
    const AnimationFrame& f = p.current();
    sprite.atlas = f.atlas();     sprite.size = f.size();
    sprite.tile  = f.tile();      sprite.palette = f.palette;   // the frame resolves its art through its sheet
    // … submit the layer …
});
```

**How it plays is chosen when you play it** — pass `single()`, `loopNTimes(n)`, or `playForDuration(2s)`
to `advance()` (default `loopIndefinitely()`). Palette-cycling is the same type: vary `.palette` and omit
`.tileIndex` to hold the art. The full reference — the data model, the pure resolver, multi-clip sheets,
and the player's
`play`/`pause`/`stop`/`seek`/`finished` controls — is in **[animation.md](animation.md)**, worked end to
end (one button per playback mode) in
[`examples/animation_demo.cpp`](../../examples/animation_demo.cpp).

## Tween a value over time (fades, ramps, transitions) <a id="tween-a-value"></a>

Animations resolve elapsed ticks → *which frame*; **`tween.h`** resolves elapsed ticks → *a value* —
a layer's `alpha`, a `ColorFill` channel, an effect parameter, a transform angle. Same shape as
animations: the platform gives a pure resolver, you own a **`TweenPlayer<T>`** cursor and write the result
into draw state. A **`Tween<T>`** is a start anchor `from` plus a list of timed, eased moves (`of` for
the first, `then()` to chain) — so a **yoyo is just a 2-segment looped track**, no special mode:

```cpp
#include "retropp/tween.h"
using namespace std::chrono_literals;

// fade out and back, forever
const Tween<float> fade = Tween<float>::of(1.0f, 0.0f, 1s, Easing::InOutSine)
                                       .then(1.0f, 1s, Easing::InOutSine);

TweenPlayer<float> fader{.tween = &fade};

loop.simTick([&](const InputState&) { fader.advance(); });   // loops by default
loop.renderLoop([&] {
    upperLayer.alpha = fader.value();                        // write the value into any sink
    // … submit …
});
```

**An effect or shader parameter is the same sink** — write `value()` into a built-in effect's amplitude
or a custom shader's reflected param exactly like a layer's `alpha`. The full reference — the curve set
(`Easing`), the pure resolver, the playback modes, and the player's controls — is in
**[tween.md](tween.md)**, worked end to end (layer alpha + dusk ramp) in
[`examples/tween_demo.cpp`](../../examples/tween_demo.cpp).

## React to an action press (menus) <a id="button-press"></a>

The tick's `InputState` gives you held state **and edges**, keyed by your own action enum. Use
edges for menus and "on press" actions, held for movement:

```cpp
enum class Action : std::uint8_t { Confirm, Down, Right };
// at startup: bind each action to its sources, then platform.actions(map)

loop.simTick([&](const InputState& in) {
    if (in.justPressed(Action::Confirm)) confirm();       // fires once, on the press
    if (in.justPressed(Action::Down))    moveCursor(+1);
    if (in.isHeld(Action::Right))        walk(+1);        // every tick while held
});
```

Edges are sim-tick-keyed, so they're deterministic and never double-fire from a fast display. Full
surface (the action map, sources, presets, per-family rows) in [input.md](input.md).

## Make a draggable title bar on a chromeless window <a id="draggable-title-bar"></a>

Open the window without OS chrome, draw your own title bar as a `Region`, and declare that same
value a drag handle — the OS window manager then drags the window by the painted bar, pixel-exact,
with no per-frame code:

```cpp
const EngineConfig config{
    .identity = {.organization = "MyStudio", .application = "My Game"},
    .window   = {.title = "My Game", .suppressNativeWindowChrome = true}};  // borderless from frame one
EngineConfig::setActive(config);

// The title bar: an ordinary drawn Region — and the drag handle.
const Region titleBar{.key   = "titlebar",
                      .shape = ShapePoints::rectangle(Point{0.0f, 0.0f}, 160.0f, 12.0f),
                      .effects = {{.kind = ScreenSpaceEffectKind::ColorFill,
                                   .fill = Rgba8{70, 96, 150}}}};

platform.window().dragHandles({titleBar});               // press the drawn bar → the OS drags the window
platform.window().autoMove({.trigger = Action::Grab});   // optional: any input drags it too (gamepads)
```

Draw `titleBar` in the frame like any other region. The handle and the painted bar are the same
value, so they agree to the pixel — curved shapes included. `autoMove` adds the input-driven drag
for devices with no mouse press to hit-test (hold the trigger action and the pointer, sticks, and
d-pad move the window). Full surface — the noun pairs, `WindowState`, `WindowMovement::None` — in
[platform-and-windowing.md](platform-and-windowing.md#the-window-window); working chrome in
[`examples/window_drag`](../../examples/window_drag/main.cpp) and
[`examples/Numberator`](../../examples/Numberator/main.cpp) (a full classic-Mac title bar with a
close button carved out of the handle).

## Retained vs rebuilt frame state <a id="retained-vs-rebuilt-frame"></a>

Each frame the renderer draws whatever `FrameDrawState` you hand it. **How you produce that state is
your choice** — the platform holds no opinion and no persistent per-layer state of its own. Two styles,
both fully supported, both shown in the examples:

**Rebuilt (immediate-mode)** — clear the layer stack and rebuild it every frame. Simplest mental
model: there's no state to keep in sync, the frame is purely a function of current game state. Good
when layers come and go a lot, or you just prefer stateless assembly. This is what
[`examples/beach_demo.cpp`](../../examples/beach_demo.cpp) and
[`examples/layer_transparency_demo.cpp`](../../examples/layer_transparency_demo.cpp) do:

```cpp
loop.renderLoop([&]() {
    frame.layers.clear();              // rebuild from scratch
    frame.layers.push_back(makeWorldLayer(state));
    frame.layers.push_back(makeHudLayer(state));
    renderer.renderFrame(frame);
});
```

`clear()` keeps the vector's capacity, so there's no per-frame heap churn — rebuilding is cheap.

**Retained** — build the layers once and mutate only what changed each frame. Good for mostly-static
scenes: a background you only scroll, a HUD that rarely changes. You don't re-describe unchanged
layers. This is what [`examples/controller_scrolling.cpp`](../../examples/controller_scrolling.cpp) does:

```cpp
// once, before the loop:
frame.layers.push_back(DrawLayer{.key = "world"});   // key is required — no default constructor
frame.layers[0].content = makeWorldContent(state);

loop.renderLoop([&]() {
    frame.layers[0].scroll = LayerScroll{cam.x, cam.y};  // touch only what moved
    renderer.renderFrame(frame);
});
```

Both submit the same way and produce identical output; pick whichever fits how you think about a given
scene — you can even mix them (retain the static layers, rebuild a volatile one). There is no
"mode" to set: the choice lives entirely in your render code.

**It is not a performance decision.** The renderer compares what you submit against what it last
uploaded and sends only what actually changed — per tile layer, per sprite layer, and for the whole
composite — so rebuilding a static scene every frame produces the same GPU traffic as retaining it.
See [the renderer only uploads what changed](rendering.md#upload-skip). What rebuilding *does* cost is
whatever your own assembly code costs: if regenerating a large cell array each frame is expensive for
you, that expense is yours and the renderer's skip does not remove it.

> **Lifetime note.** The renderer reads a layer's content spans (`cells`, `sprites`) *during*
> `renderFrame`. Whatever those spans point at must stay alive across the call — in the retained style,
> that means the backing arrays live as long as the frame does (declare them alongside it). The platform
> never copies your content; it references it.

## Play music and a sound effect <a id="play-audio"></a>

Registration is program-wide and lives on the `AudioLibrary`; cueing happens on an `AudioSystem`.
Register once, keep the `AudioId`, and cue it whenever you like:

```cpp
auto& lib = AudioLibrary::instance();
const AudioId overworld = lib.registerAudio("audio/overworld.asm", AudioType::Music, Isa::Sm83);
const AudioId hit       = lib.registerAudio("audio/sfx/hit.asm",   AudioType::Sfx,   Isa::Sm83);

AudioSystem audio{AudioKind::Chiptune};   // a chiptune system owns its output
audio.play(overworld);                    // music plays until you stop it
audio.play(hit);                          // a one-shot SFX closes itself when it goes quiet
```

`play()` never silences what is already playing — cueing a second sound adds a voice rather than
replacing one. `AudioType::Music` and `AudioType::Vocals` are yours to `stop()`; an
`AudioType::Sfx` cue stops on its own once its sound has finished. Production runs on the system's
own threads — a machine to a thread, mixed by the system — so a slow simulation frame cannot starve
the sound and you never step audio from your loop. Full surface in [audio.md](audio.md).

## Play a recorded audio file <a id="play-audio-file"></a>

The same `registerAudio` name takes a `.wav` / `.ogg` / `.flac` / `.mp3` through its no-ISA
overload, and it plays on a PCM system:

```cpp
const AudioId chime = AudioLibrary::instance().registerAudio("audio/chime.wav", AudioType::Sfx);

AudioSystem audio{AudioKind::Pcm};   // decodes and streams; no VM, no driver
audio.play(chime);
```

The kind is inferred from the extension and frozen into the entry, so a file cannot be mis-filed as
a chiptune driver. A system produces only its own kind — `play()` throws if you cue the other one.

## Make something glow <a id="make-something-glow"></a>

`Glow` is an authored-colour aura: you choose the colour the thing radiates, which is what separates
it from `Bloom` (a glow of the source's *own* light). Name the kind and fill the fields it reads:

```cpp
ship.effects = {ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Glow,
                                  .fill      = Rgba8{255, 66, 26, 255},  // the colour you chose
                                  .radius    = 5.0f,
                                  .intensity = 255}};
```

`threshold` at 0 makes the whole silhouette emit, so dark art radiates too. On a sprite the radius is
in that sprite's own art pixels; at frame or layer scope it is in viewport pixels. The same effect
value works at every site — frame, layer, region, sprite — which is the point of the grammar. See
[draw-state.md](draw-state.md).

## Blend a layer into what's under it <a id="blend-a-layer"></a>

`blend` is a field on the container, not a separate mechanism:

```cpp
smoke.blend    = BlendMode::Add;        // additive — light on light
shadow.blend   = BlendMode::Multiply;   // darken what is beneath
overlay.blend  = BlendMode::Half;       // an even mix
```

It reads the same on a `DrawLayer`, a `Sprite`, or a `Region`. A partial-`alpha` container under a
non-`Normal` blend composites correctly — the source colour is recovered before the operator runs, so
a half-transparent additive layer is half as bright, not a quarter. The per-mode maths is in
[blend-modes.md](blend-modes.md).

## Tell whether two things actually touch <a id="hit-test"></a>

A sprite's **mask** is its image treated as coverage, so a hit test respects the art's real shape
instead of its bounding box — holes and notches included:

```cpp
if (enemy.mask(Space::Layer).contains(Point{cursor.x, cursor.y})) {
    // the cursor is over a lit pixel of the enemy, not merely inside its quad
}
```

Three forms, by what you need to outlive: `mask(space)` borrows for the frame, `freezeMask(space)`
owns a storable snapshot (a trail, a collider you keep past the sprite), and `maskShape(n, space)`
hands back the mask as geometry you can draw as a `Region` or feed to physics. `Space::Layer`
answers where the sprite is on screen; `Space::Quad` answers in the sprite's own art. Details in
[sprites.md](sprites.md).

## Move a sprite along a path <a id="move-along-a-path"></a>

A `SpritePath` composes movement, orientation and animation off one clock, and writes the result
into a sprite:

```cpp
SpritePath walker{.nodes = {{.move   = SpritePathMove::through({{140, 118}, {20, 118}}),
                             .facing = FacingPolicy::FlipX}}};

// each tick — bare advance() steps one tick and loops at the end of the sequence
walker.advance();

// each frame
Sprite s{.key = "walker"};
walker.applyTo(s);          // writes position, frame art, and flip
```

Node **sequences** chain from where the previous one ended, and an **interrupt stack** lets a
reaction take over and hand control back. By default a resumed path continues from where the sprite
actually is rather than snapping back — drift is the intent, not a bug. See
[sprite-path.md](sprite-path.md), and [path-walker.md](path-walker.md) for the lower-level cursor.

## Save the player's progress <a id="save-progress"></a>

`SaveStore` writes versioned byte documents to the platform's save location, atomically, so a crash
mid-write cannot leave a half-file:

```cpp
SaveStore store;                                   // uses EngineConfig::identity
store.write("slot1", /*schemaVersion=*/1, bytes);

if (auto doc = store.read("slot1")) {
    load(doc->payload);                            // doc->schemaVersion says which shape it is
}
```

When your save format changes, raise the current version and register how to get there from the old
one; `read` walks the chain for you:

```cpp
store.setCurrentVersion(2);
store.registerMigration(1, [](std::vector<std::byte> old) { return upgradeV1toV2(old); });
```

The store needs an application identity — set `EngineConfig::identity` before constructing one, or
it refuses. See [persistence.md](persistence.md).

## Keep the player's other files <a id="user-files"></a>

For content that is not a save document — an extracted asset tree, an exported screenshot, a log —
`UserFiles` puts bytes in the same per-user directory without the document machinery:

```cpp
UserFiles files;
files.write("assets/tiles.png", bytes);            // relative paths may carry subdirectories
if (auto data = files.read("assets/tiles.png")) { /* … */ }
```

Writes are atomic and create parent directories; what lands on disk is exactly your bytes, with no
envelope or version header. Paths are contained to the store's root — an absolute path, or one that
starts at a root, is refused rather than escaping.

## Ship an asset inside the binary <a id="embed-an-asset"></a>

The policy in the call is the whole mechanism — the build reads it and acts, and you write no build
rule either way:

```cpp
// Baked into the binary — an explicit override of loadAtlas's per-type default.
renderer.loadAtlas("art/tiles.png", AssetDimensions::GameBoy8x8, ContentKind::Tileset,
                   ReadOrder::LeftRightThenDown, /*count=*/0, TransparentIndices::None,
                   /*framesPerAnimation=*/0, AssetPolicy::Embed);

// No policy argument — takes loadAtlas's default, LoadFromPath: copied beside the binary and
// read from disk at runtime.
renderer.loadAtlas("art/dlc.png", AssetDimensions::GameBoy8x8, ContentKind::Tileset);
```

Paths must be compile-time literals so the build scan can find them; a genuinely runtime path is a
compile error, and you read those bytes yourself and use `loadAtlasFromMemory` instead. The default
is deliberately `LoadFromPath` for atlases — art is the copyright surface, so baking is always the
explicit choice. See [assets-and-embedding.md](assets-and-embedding.md).

## Go fullscreen, or scale the window <a id="fullscreen"></a>

Window state is a set of noun pairs that apply only on change:

```cpp
platform.window().fullscreen(true);                       // native fullscreen
const bool isFull = platform.window().fullscreen();       // and read it back
```

Startup size comes from `EngineConfig` — a logical `windowScale` multiplying the viewport, clamped to
what the display can actually show. The viewport itself is unchanged by any of this: the platform
renders at its internal resolution and blits, so going fullscreen changes how many screen pixels a
game pixel covers, never how much of the world is visible. See
[platform-and-windowing.md](platform-and-windowing.md).

## Rumble the controller <a id="rumble"></a>

Vibration is declared per tick like any other output — you state the motor state you want, and
restating the same value changes nothing:

```cpp
platform.gamepad(0).vibration({
    .low          = 180,   // the heavy motor
    .high         = 60,    // the light one
    .triggerLeft  = 0,
    .triggerRight = 0,
});
```

Declare all-zero to stop. Pads without motors accept the call and do nothing, so you need no
capability check. See [input.md](input.md).

## Run a routine as original hardware code <a id="run-a-routine"></a>

For the narrow set of routines a native re-implementation cannot reproduce exactly — RNG that reads
a free-running hardware register, a cycle-driven sound driver — register the routine once and call
it as an ordinary typed function:

```cpp
Vm vm{VMPlatform::GameBoyColor};

auto roll = vm.registerRoutine<std::uint8_t()>("vm/random.asm",
                                               {.output = gb::A});
std::uint8_t n = roll();     // plain C++ at the call site
```

Registers and addresses appear only in the binding, never at a call site. No game ROM is loaded or
executed — the platform assembles your `.asm` in-process and injects it. A malformed binding throws at
registration rather than failing quietly later. See [vm-and-routines.md](vm/vm-and-routines.md).
