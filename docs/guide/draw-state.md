# Draw state

The submission envelope: the C++ shape a game hands the renderer each frame. This is the heart of
the "how do I draw something" path — an arbitrary stack of Z-sorted layers, each carrying tiles or
sprites, plus whole-frame colour and screen-space effects. The colour *model* (indexed atlases + palettes) is
[tiles-and-colour.md](tiles-and-colour.md); how a frame is composited and presented is
[rendering.md](rendering.md).

```cpp
#include "retropp/draw_state.h"   // FrameDrawState, DrawLayer, TileContent, SpriteContent, GuestFrameContent, …
```

## Contents

- [The model: a whole frame, computed fresh](#the-model-a-whole-frame-computed-fresh)
- [`FrameDrawState` + `DrawLayer`](#framedrawstate--drawlayer)
  - [Layer identity vs depth](#layer-identity-vs-depth)
  - [Layer-key uniqueness is a contract](#layer-key-uniqueness-is-a-contract)
- [Layer content: tiles, sprites, or a hosted machine's picture](#layer-content-tiles-sprites-or-a-hosted-machines-picture)
  - [`TileContent` — a scrolling tile map](#tilecontent--a-scrolling-tile-map)
  - [`SpriteContent` + `Sprite` — placed sprites](#spritecontent--sprite--placed-sprites)
  - [`GuestFrameContent` — a hosted machine's picture](#guestframecontent--a-hosted-machines-picture)
- [Whole-frame colour](#whole-frame-colour)
- [Screen-space effects](#screen-space-effects)
  - [Confining an effect to a shape (`Region`)](#confining-an-effect-to-a-shape-region)
  - [Painting a colour into a region (`ColorFill`)](#painting-a-colour-into-a-region-colorfill)
  - [Making a layer see-through (the `stencil()` helper)](#making-a-layer-see-through-the-stencil-helper)
  - [Frame edge: `DisplacementEdge`](#frame-edge-displacementedge)
  - [Custom shader stages — your own effect (`kind = Custom`)](#custom-shader-stages--your-own-effect-kind--custom)
  - [Per-row data table — an array input for an effect (`paramTable`)](#per-row-data-table--an-array-input-for-an-effect-paramtable)
- [Transforms](#transforms)
  - [Per-sprite transforms](#per-sprite-transforms)
- [Where to change things](#where-to-change-things)

## The model: a whole frame, computed fresh

The frame's draw state is computed **whole every frame** from the game's logical inputs. There is no
mid-frame state-change API, no reconstructed scanline interrupt, no register poke. A game recomputes
and resubmits a fresh `FrameDrawState` each frame — layer existence, z, scroll, size, alpha, and
effect parameters are all fresh every frame (the one amortized exception is the atlas/palette texels,
uploaded when they change). Effects a Game Boy expressed through hardware tricks are expressed here
as **layers, per-tile/per-region colour attributes, and screen-space-effect declarations** (whole-frame
colour included) — never as a hardware-register idiom.

**Identity is a typed, first-class field throughout** — the reconciliation `ObjectKey`, `AtlasId`, the
`*Kind` enums — never an array position, never a packed byte behind a comment.

## `FrameDrawState` + `DrawLayer`

```cpp
struct FrameDrawState {
    std::vector<DrawLayer>         layers;          // arbitrary N; compositor stable-sorts by z
    BlendMode                      blend = BlendMode::Normal;  // how postEffects/regions combine over the frame
    std::vector<ScreenSpaceEffect> postEffects;     // frame-level whole-frame effects on the composited image
    std::vector<Region>            regions;         // frame-level shape-confined effects (additive; see below)
    std::uint32_t                  advancesEvery = 1;  // ticks per advance, for layers that declare none
};

struct DrawLayer {
    ObjectKey         key;         // REQUIRED reconciliation identity ("hud"); unique per frame; NO role in depth
    std::int32_t      z = 0;       // back-to-front sort key; unique within a frame
    PixelSize         size{};      // independent per-layer dimensions
    LayerScroll       scroll{};    // independent scroll offset {x, y}
    float             alpha = 1.0f;// [0,1], default opaque
    BlendMode         blend = BlendMode::Normal;  // how this layer composites over the accumulator; Normal = alpha-over
    LayerContent      content{ TileContent{} };  // tiles, sprites, or a hosted machine's picture
    std::vector<ScreenSpaceEffect> effects;  // per-layer whole-layer effect chain; empty by default (each: scope Layer / Below)
    std::vector<Region> regions;   // per-layer shape-confined effects (additive; see below)
    Transform         transform{}; // per-layer geometric transform; identity by default (see Transforms)
    DisplacementEdge  transformEdge = DisplacementEdge::Blank;  // what fills the transformed footprint's exposed area
    std::optional<std::uint32_t> advancesEvery;  // ticks per advance of this layer's world; unset takes the frame's
};
```

`LayerScroll` is a plain `{int x, int y}` value pair (with `==` defined); `PixelSize` is the layer's
`{width, height}`. Both default to zero-initialised.

The compositor draws the layers **back-to-front by `z`**. There is **no semantic layer model** — the
platform imposes no "background" / "sprite" / "window" roles. A layer is just tiles-or-sprites at a
depth; "the player walks behind that tree" is simply a higher-`z` layer.

**How you produce the `layers` each frame is your choice.** Either rebuild it — `clear()` the vector
(it keeps capacity, so arbitrary N with no steady-state heap churn) and push the layers you want — or
build it once and mutate only what changed. Both are fully supported and produce identical output; the
platform holds no persistent per-layer state of its own. See
[the retained-vs-rebuilt recipe](how-to.md#retained-vs-rebuilt-frame)
([`beach_demo`](../../examples/beach_demo.cpp) rebuilds;
[`controller_scrolling`](../../examples/controller_scrolling.cpp) retains).

### Layer identity vs depth

A layer's identity is its **`key`** (`.key = "ParallaxClouds"`), an `ObjectKey`. It is **required** — a
`DrawLayer` that omits `.key` is a compile error, because the type has no default constructor. It is
identity only and **fully independent of `z`**: `z` alone controls depth, the key plays no part in
ordering. The renderer matches a layer to its previous-tick state by this key to interpolate motion, so
the game supplies the SAME key for the same layer every frame (a key that changes each frame never matches
its own prior frame and never eases). The key owns its string, so any value works — a literal, a
`std::string` the game holds, or a name built on the spot. `Sprite` and `Region` carry the same required `key`.

### Layer-key uniqueness is a contract

Within one frame, no two layers may share a `z` (their order would be undefined) **or** a `key` (the
reconciliation identity must be unambiguous), and no key may be empty. The platform enforces this two ways:

```cpp
// Compile-time: turn a fixed layer stack's collision into a BUILD error.
static_assert(layerKeysAreUnique(kMyFixedLayers), "z/key collision in layer stack");

// Runtime: layerDrawOrder() validates and reacts per the renderer's collision policy
// (Throw in debug, WarnAndResolve in release — see rendering.md).
```

`findLayerKeyCollision(layers)` returns the first collision (or `nullopt`); `layerKeysAreUnique` is
the boolean form for `static_assert`. For runtime-built layer stacks the renderer calls
`layerDrawOrder` for you each frame.

## Layer content: tiles, sprites, or a hosted machine's picture

```cpp
enum class LayerContentKind : std::uint8_t { Tiles, Sprites, GuestFrame };
using LayerContent = std::variant<TileContent, SpriteContent, GuestFrameContent>;
constexpr LayerContentKind contentKind(const LayerContent&) noexcept;
```

A layer carries exactly one content alternative — a tile map, a set of sprites, or the picture a
hosted machine drew. Layers **interleave freely by `z`** in one compositing pass whatever they carry,
so a sprite layer can sit between two tile layers, or a native layer over a machine's screen, entirely
the consumer's choice.

### `TileContent` — a scrolling tile map

```cpp
struct TileContent {
    int                       widthInTiles  = 0;
    int                       heightInTiles = 0;
    std::span<const TileCell> cells;         // row-major, widthInTiles * heightInTiles
    TileWrap                  wrap = TileWrap::Repeat;  // how the map samples beyond its bounds
};

enum class TileWrap : std::uint8_t { Repeat, Clamp, Blank };

struct TileCell {
    AtlasId       atlas{};         // which uploaded sheet this cell draws from
    std::uint16_t tile    = 0;     // cell index within its OWN sheet (`atlas`), on the 8px grid
    PaletteId     palette{};       // which uploaded palette colours it
    bool          flipX   = false;
    bool          flipY   = false;
    Rotation      rotation = Rotation::None;  // 90° texture rotation; composes with the flips
};
```

A tile layer is a row-major grid of cells sampled per-pixel in the shader against the layer's scroll,
so arbitrary layer sizes and wrapping are handled on the GPU. Each cell names its own sheet (`atlas`)
and palette and a tile within that sheet, plus flips — so one layer mixes any number of sheets and
palettes, and `TileContent` carries no atlas or palette of its own (see
[tiles-and-colour.md](tiles-and-colour.md) for the colour mechanism). `cells` is game-owned and must
outlive the `renderFrame` call. Depth is `z` alone; "the player walks behind the treetops" is just a
higher-`z` layer.

`wrap` chooses how the tilemap is sampled outside its `widthInTiles × heightInTiles` bounds:

- **`Repeat`** (default) — toroidal: the map tiles infinitely on both axes.
- **`Clamp`** — clamp the world coordinate to the map's edge row/column, smearing the border tile.
- **`Blank`** — a **finite** map: a coordinate outside the map on either axis is a transparent hole,
  so the map renders exactly once and can never show a wrap seam. This is the mode a finite overworld
  map wants (no infinite repeat as the camera scrolls past the edge).

One `wrap` governs both axes. It is independent of, and composes with, the per-layer `transform`: the
transform maps a destination pixel into the layer footprint, then `wrap` governs sampling the tilemap.

### `SpriteContent` + `Sprite` — placed sprites

```cpp
struct SpriteContent {
    std::span<const Sprite> sprites;       // each sprite names its own sheet + palette
};

struct Sprite {
    ObjectKey       key;               // REQUIRED reconciliation identity; unique per frame across ALL
                                       //   sprite layers (see sprites.md) — NO default ctor
    int             x = 0, y = 0;      // places the ORIGIN in the LAYER's space (before scroll);
                                       //   default origin {0,0} = the top-left corner
    std::int32_t    z = 0;             // within-layer stacking key; NON-unique, ties keep submission order
    AssetDimensions size = AssetDimensions::GameBoy8x8;
    AtlasId         atlas{};           // which uploaded sheet this sprite draws from
    std::uint16_t   tile = 0;          // top-left atlas cell (8px grid) within its own sheet
    PaletteId       palette{};         // which uploaded palette colours it
    float           alpha = 1.0f;      // per-sprite opacity [0,1]; multiplies UNDER the layer alpha
    BlendMode       blend = BlendMode::Normal;  // how the sprite grades over its container (Multiply shadow, Add flare)
    bool          flipX = false, flipY = false;
    Rotation      rotation = Rotation::None;  // 90° texture rotation; composes with the flips
    std::vector<ScreenSpaceEffect> effects;   // whole-silhouette effect chain over the sprite's own pixels
    std::vector<Region>            regions;    // confined effects: a quad-space shape ∩ the silhouette
    // (plus `transform` — see Transforms below — and `origin` + `pivot` + `anchors`, the articulation
    //  surface: see anchors-and-articulation.md)
};
```

Sprites are instanced per-quad and, like cells, each names its own sheet (`atlas`) and palette directly, so
one sprite layer mixes sheets, palettes, and sizes freely. `x`/`y` place the sprite's `origin` in the
layer's coordinate space (before scroll), so a sprite on a world-scrolling layer tracks the background while
a HUD layer at `scroll {0,0}` stays fixed. Within a layer, sprites stack by `Sprite::z`; across the frame,
sprite layers interleave with tile layers by `DrawLayer::z`.

**`Sprite` is the platform's richest drawable, and its full surface has its own page:
[sprites.md](sprites.md).** That page documents every field exhaustively — the `key` reconciliation identity
and the keying rules, placement (`origin` vs `pivot`, `center(Space)`), the arbitrary-dimension `size` read and
its console presets, texture `flipX`/`flipY`/`rotation`, per-sprite `alpha` and `blend`, the geometric
`transform`, the `anchors` articulation resolvers, the `effects` + `regions` effect carrier (whole-silhouette
chains, quad-space regions, and Below-scope scene lensing), and what interpolates vs snaps. The summary here
covers only how a sprite sits in the frame; reach for `sprites.md` for the fields.

The blend grammar and the Below-scope lens are in [blend-modes.md](blend-modes.md); articulation
(hierarchies, joints, anchors) is in [anchors-and-articulation.md](anchors-and-articulation.md); the effect
*kinds* a sprite's `effects` / `regions` carry are the same ones under
[Screen-space effects](#screen-space-effects) below.

### `GuestFrameContent` — a hosted machine's picture

```cpp
struct GuestFrameContent {
    std::span<const std::uint8_t> pixels;  // row-major, width * height * bytesPerPixel(format)
    int              width  = 0;
    int              height = 0;
    GuestPixelFormat format = GuestPixelFormat::Rgba8888;
    std::uint64_t    generation = 0;       // how many frames this machine has finished
};

enum class GuestPixelFormat : std::uint8_t { Rgba8888 };
constexpr std::size_t bytesPerPixel(GuestPixelFormat) noexcept;
```

A layer whose content is the last complete frame a hosted machine drew. Ask the machine for it and
hand it straight to a layer:

```cpp
machine.video(true);                    // the machine draws — off by default
machine.run(Vm::Advance::OnTick);

DrawLayer screen{.key = "screen"};
screen.size    = PixelSize{160, 144};
screen.content = machine.video();       // valid for this renderFrame call
```

The dimensions and the layout are the machine's own — a machine that draws 256×224 says so, and one
whose pixels are laid out differently names a different `GuestPixelFormat`. Nothing here says how often
a machine draws: the platform holds the frame it finished and shows that one, so a machine slower than
the display keeps its picture on screen rather than flickering.

`generation` counts the frames the machine has finished — 0 before the first one. It is the platform's
count, not something the game declares, and the renderer sends the raster to the GPU exactly when a
submission carries a generation the resident texture does not already hold. The pixels are never hashed
to decide — a full-colour raster differs every time a machine draws, so hashing one would cost more than
the upload it could save.

It is a value, so reading it leaves it where it is: two `video()` calls in one frame report the same
generation, and a submission the renderer skips still carries the right answer on the next one. It says
which frame this is rather than when it arrived, which is what lets a machine on the game's tick and a
machine on a clock of its own be read the same way.

#### A raster your program draws itself

A `GuestFrameContent` is a raster of pixels, and a layer shows whatever raster arrives in this form,
whoever drew it. A program that paints its own pixels on the CPU — a waveform, a plot, a
software-rendered effect, a decoded video frame — hands them to a layer exactly as a machine's picture
is handed over:

```cpp
std::vector<std::uint8_t> pixels(512 * 48 * 4);   // RGBA, row-major, yours to paint
std::uint64_t             drawn = 0;              // your own count of finished rasters

paint(pixels);                                    // redraw it on the CPU
++drawn;                                          // a new picture carries a new generation

DrawLayer scope{.key = "scope"};
scope.size    = PixelSize{512, 272};
scope.scroll  = LayerScroll{0, -224};             // 224 px down the viewport
scope.content = GuestFrameContent{.pixels     = pixels,
                                  .width      = 512,
                                  .height     = 48,
                                  .format     = GuestPixelFormat::Rgba8888,
                                  .generation = drawn};
```

Everything above holds for it, with `generation` yours to count:

- **The renderer uploads the raster when its generation is one the layer's texture does not hold.** Bump
  it when the pixels change and keep it when they do not: a raster resubmitted under the same non-zero
  generation is uploaded once and stays resident. Generation 0 is "nothing finished", so a raster
  submitted under 0 is uploaded on every frame.
- **The texture belongs to the layer's `key`**, sized to the raster's `width` × `height` and rebuilt when
  either changes. A layer with an empty key, or a key another guest-frame layer in the same frame already
  used, gets a texture for that frame only.
- **It draws texel for texel:** each pixel covers one viewport pixel, placed by the layer's `scroll`,
  faded by its `alpha` and warped by its `transform`. Outside its own width and height it draws nothing,
  so a small raster on a viewport-sized layer leaves the layers beneath it showing everywhere else. A
  raster whose `pixels` holds fewer than `width × height × 4` bytes is not drawn.
- **`pixels` is read during `renderFrame`**, so it lives at least that long. A raster that changes every
  frame costs one upload of its size every frame.

The SNES player (`examples/snes/player/`) draws its sound scopes this way.

The full picture surface — turning it on, what a machine owes, and how it composes with native layers —
is in [co-execution.md](co-execution.md).

## Whole-frame colour

Whole-frame colour — day/night, a cutscene flash, a fade, a tint — is a screen-space effect, not a
bespoke frame member. Each is a `ColorFill` paired with a blend mode and an `alpha` / `fillIntensity`,
pushed onto `frame.regions` (a region with no shape covers the whole viewport). `ColorFill` is a pure
source colour; its container's blend decides how it grades the scene:

```cpp
// Day/night: a Multiply ColorFill — scene · tint (darkens / cools the whole frame).
frame.regions.push_back(Region{
    .key     = "daynight",
    .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{150, 160, 200}}},
    .blend   = BlendMode::Multiply});

// Cutscene flash: a Normal white ColorFill at alpha = strength — lerp(scene, white, strength).
frame.regions.push_back(Region{
    .key     = "flash",
    .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{255, 255, 255}}},
    .alpha   = strength});

// Fade to black: a Normal black ColorFill, alpha 0 → 1.
frame.regions.push_back(Region{
    .key     = "fade",
    .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{0, 0, 0}}},
    .alpha   = fade});
```

A Multiply `ColorFill` **darkens** (a shadow / tint); to **brighten**, use `Add` (`scene + fill`, a glow /
lift), `Screen`, or a `Multiply` with `fillIntensity > 1` — a multiplicative exposure that scales the scene up
while preserving contrast (the float16 offscreen pipeline carries the > 1 fill; the blit clamps to the screen).
The same effect works at any scope: confine it with a shape for a glow / shadow / sunlit
patch (`frame.regions` / `layer.regions`), or run it per-layer. There is no colouring of source art here —
that is index + palette; this grades the already-composited frame. The runnable showcase is
[`examples/colour_effects_demo`](../../examples/colour_effects_demo/main.cpp); the blend math is [blend-modes.md](blend-modes.md).

## Screen-space effects

A screen-space effect is a function `f(row, phase)` the GPU evaluates per-pixel (wavy water, heat
haze, per-line scroll) — no reconstructed scanline counter, no HBlank interrupt. The game advances
`phase` per frame to animate.

```cpp
struct ScreenSpaceEffect {             // frame-level (postEffects) and per-layer (DrawLayer::effects)
    ScreenSpaceEffectKind kind = ScreenSpaceEffectKind::None;  // None | RowDisplacement | Ripple | Swirl | ColorFill | Gleam | ColorSaturation | Bloom | Glow | Custom | Transparency
    PostProcessStageId customShader{};  // kind == Custom: your registered shader (below)
    float amplitude = 0;   // displacement magnitude, in viewport pixels (RowDisplacement, Ripple);
                           // the turn in DEGREES (Swirl)
    float frequency = 0;   // RowDisplacement: cycles across the axis; Ripple: rings across the field
    float phase     = 0;   // animation phase (advance it per frame) (RowDisplacement, Ripple);
                           // Swirl ADDS it to `amplitude` (degrees) so advancing it spins the vortex
    Axis  axis = Axis::Horizontal;            // Horizontal = displace columns by row; Vertical = rows by column (RowDisplacement)
    DisplacementEdge edge = DisplacementEdge::Blank;  // frame-edge behaviour, below (RowDisplacement)
    ScreenSpaceEffectScope scope = ScreenSpaceEffectScope::Layer;  // per-layer reach (DrawLayer::effects only)
    Point center{};        // ripple centre, in viewport pixels (Ripple); the swirl centre (Swirl)
    float decay = 0;       // ripple radial falloff rate; 0 = no falloff (Ripple)
    StencilMode stencil = StencilMode::TransparentInside;  // Transparency: which side of its region goes see-through (TransparentInside | TransparentOutside)
    float       feather = 0;   // Transparency: soft-edge width in shape-local px; 0 = hard edge
    Rgba8 fill{};   // ColorFill: the colour painted into the region; Glow: the aura's tint (out = fill · fillIntensity)
    float fillIntensity = 1.0f;  // ColorFill/Glow: scales `fill` — a Multiply/Add/Screen container (or a Glow aura) can exceed 1; 1 = plain fill
    float sweep = 0, width = 0.1f, gain = 0, slant = 0.35f;  // Gleam: a diagonal sheen band — sweep/width/slant place it (UV); gain 0 = identity
    std::uint8_t saturation = 255;  // ColorSaturation: pull each pixel toward its luminance; 255 = full colour (identity), 0 = greyscale
    float        radius    = 0;     // Bloom/Glow: halo reach in the site's own pixels (viewport px; a sprite's own art px)
                                    // Swirl: the twisted disc's extent, same units
    std::uint8_t threshold = 0;     // Bloom: luminance floor — 0 blooms everything, higher keeps only the bright.
                                    // Glow: emission floor — 0 = the WHOLE silhouette emits (dark art included),
                                    // higher keys the emission on brightness
    std::uint8_t intensity = 0;     // Bloom/Glow: strength — 0 = no halo (the identity default), 255 = full
    // kind == Custom: your shader's OWN params, reflected from its cbuffer and surfaced here BY NAME
    // (e.g. `.pivot`, `.strength`) — set them inline like a built-in's. Generated from the custom shaders
    // your build references (empty if none). See "Custom shader stages" below.
};
```

An effect carries **no shape of its own** — `Transparency` included. To confine an effect to a shape you
put it in a **`Region`** (next section), which owns the shape and the effects applied inside it — *the
region owns the effect, not the reverse*.

`RowDisplacement` (axis-aligned wave), `Ripple` (radial droplet), `Swirl` (an angular twist about a
centre — the whirlpool; Ripple pushes the sample along the radius, Swirl carries it around), `ColorFill` (paint a colour into the
region — see "Painting a colour into a region" below), `Gleam` (a luminance-keyed diagonal sheen sweep —
the marquee "shine"), `ColorSaturation` (pull each pixel toward its own luminance — a colour drain),
`Bloom` (a threshold-blur-add glow — bright content radiates its OWN light as a soft halo), and `Glow`
(an authored-colour aura — a colour YOU pick radiates from the content, dark art included; the halo's
chroma is the tint, never the source hue) are the platform's **built-in
effects** — name the kind and set parameters, the platform owns the shader; `Custom` runs **your own
shader** (see "Custom shader stages" below). Build one with plain **designated-init** — set `.kind` and
the fields that kind consults; every field is settable inline, so you keep full control (`.scope`,
`.edge`, all of it). Which fields each kind reads: **RowDisplacement** → amplitude, frequency,
phase, axis, edge; **Ripple** → amplitude, frequency, phase, center, decay; **Swirl** → center, radius,
amplitude, phase (the turn in DEGREES — `amplitude` + `phase` summed; positive turns the content clockwise,
matching `Transform::rotation`; a zero turn or a zero radius = identity); **ColorFill** →
fill, fillIntensity (`out.rgb = fill · fillIntensity`); **Gleam** → sweep, width, gain, slant (`gain` 0 = identity); **ColorSaturation** → saturation (a `uint8`; `255` = full colour identity, `0` = greyscale); **Bloom** → radius, threshold, intensity (`intensity` 0 or `radius` 0 = identity; a glow past the art's edges); **Glow** → radius, threshold, intensity as Bloom's, plus fill × fillIntensity as the aura's tint (`threshold` 0 = the whole silhouette emits; `fillIntensity` > 1 = an HDR-hot aura); **Custom** →
`.customShader` (which registered shader) + **your shader's own reflected params** (set by name, inline);
**Transparency** → stencil, feather (it makes its region **see-through** rather than colouring it — see
"Making a layer see-through" below). `scope` applies to every kind, and confinement comes from the
`Region` that owns the effect (below). All built-ins flow through the same two
attachment points — the same type drives the effect at two places:

- **Frame-level — `FrameDrawState::postEffects`.** Each effect is a full-viewport pass on
  the **already-composited image**, run after every layer composites and before the window blit. The
  whole frame wobbles together. Push a `RowDisplacement` to wave the screen; an empty list applies no
  effect (the composited frame blits unchanged). Stack several and they run in submission order.
- **Per-layer — `DrawLayer::effects`.** A composable, Photoshop-style layer effect chain (set one entry
  for the common case; several run in submission order). `scope` chooses each entry's reach:
  - **`Layer` (default — isolated).** Displaces **only this layer's own content**, before it
    composites. A wavy water layer distorts while the layers and sprites composited above it stay
    still — the faithful per-line-scroll water. An exposed `Blank` edge here is *transparent*, so it
    reveals the layers below.
  - **`Below` (adjustment layer).** Displaces the **whole accumulated image at this layer's `z`** —
    this layer's own content *and* everything beneath it, coherently — then the layers above this `z`
    composite on top, undisplaced. A **content-less** `Below` layer placed just under your HUD wobbles
    the world while the HUD rides steady; a **content-bearing** `Below` layer wobbles itself together
    with the scene beneath. (Frame-level `postEffects` is the same idea applied above the whole stack.)

  A `None`-kind effect (the default) is no effect — that layer composites on the unchanged faithful
  path. A "blank" layer is just a layer with empty content plus an effect. An empty `effects` chain is
  no effect at all.

  **Mixing scopes.** A layer's `effects` chain (and its `regions`) may mix `Layer` and `Below` entries.
  The renderer runs all the `Layer`-scope ones first (on the layer's own isolated content) and then all
  the `Below`-scope ones (on the whole accumulated image) — the only coherent order, since a `Below`
  step reads the already-composited result. Within each scope, submission order is preserved.

### Confining an effect to a shape (`Region`)

By default an effect covers its whole reach (the viewport for `postEffects`, the layer for
`DrawLayer::effects`). To confine it to a **shape**, put it in a **`Region`** — a shape bound to the
effects applied inside it:

```cpp
struct Region {
    ObjectKey                      key;      // REQUIRED reconciliation identity — first member; no default ctor.
                                             //   Regions aren't interpolated, so a key need only be PRESENT (any literal), not unique.
    ShapePoints                    shape;    // where the effects apply (viewport pixels)
    std::vector<ScreenSpaceEffect> effects;  // applied inside `shape`, in order
    float                          alpha = 1.0f;  // opacity of this region's effects over the scene; 1 = full
    BlendMode                      blend = BlendMode::Normal;  // how its effects grade over the scene; Normal = alpha-over
};
```

A layer owns a list of them (`DrawLayer::regions`) and so does the frame (`FrameDrawState::regions`).
Regions are **additive** — they sit alongside the whole-reach `DrawLayer::effects` / `FrameDrawState::postEffects`,
never replacing them; a frame using neither composites through those paths alone. Inside the shape the effects apply;
outside, the source passes through untouched. Every other property — `kind`, `scope`, custom shader,
`edge`, the animation — still applies, *inside* the shape, identically for `RowDisplacement`, `Ripple`, `Swirl`,
and a `Custom` shader.

```cpp
#include "retropp/draw_state.h"   // Region, ScreenSpaceEffect, ShapePoints

// A ripple confined to a circle, owned by the frame:
frame.regions.push_back(Region{
    .key     = "ripple",
    .shape   = ShapePoints::circle({80, 72}, 30),
    .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Ripple, .amplitude = 4, .frequency = 6,
                                  .center = {80, 72}, .decay = 2}}});

// A wave confined to the bottom half of one layer:
bg.regions.push_back(Region{
    .key     = "wave",
    .shape   = ShapePoints::rectangle({0, 72}, 160, 72),
    .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 4,
                                  .frequency = 2.5f, .phase = t}}});
```

One region can carry **several** effects (applied in order, each seeing the last one's output), and one
effect drops into **many** regions with no duplication — the payoff of region-owns-effect.

A region's `shape` is a `ShapePoints` — a polygon of **ordered viewport-pixel vertices**, plus a `radius`
and a `Transform`. The points *are* the position. Containment is a signed-distance test, so one type
covers every shape, and `radius` rounds it:

| points | radius | shape |
|---|---|---|
| empty | — | no shape — the whole reach (the effects cover the region's whole reach) |
| 1 | r | a **circle** of radius r |
| 2 | r | a **capsule** (a thick line segment) |
| ≥ 3 | 0 | a **sharp polygon** — including arbitrary **concave** outlines |
| ≥ 3 | > 0 | a **rounded polygon** |

Named-constructor presets build them — `ShapePoints::circle / capsule / rectangle / roundedRectangle /
triangle / regularPolygon` — or build any polygon by hand: `shape.points = {{x0,y0}, {x1,y1}, …};`,
concave included. (Unbounded in the API; the GPU currently carries up to **64 vertices** and truncates a
longer polygon with a logged warning.)

**Point-testing a shape (`contains(p)`).** `ShapePoints::contains(Point)` answers whether a point is
inside the shape — the same containment the region gate resolves on screen, routed through the gate's CPU
mirrors, so a drawn region and the test agree by construction: what you see is what you hit. Every field
applies exactly as it draws: `radius`, `strokeWidth` (the test confines to the boundary band), `transform`,
`invert`, and a curve boundary. One deliberate difference from the table above: an **empty** shape
`contains()` **nothing** — as a region, an empty shape means "the whole reach", but as a tested mask it
must hit nothing. The point is in the shape's **own coordinates** — `ShapePoints` carries no notion of its
space, so matching spaces is the caller's job: a shape authored in viewport pixels tests viewport points,
and a mask from [`Sprite::maskShape(n, space)`](sprites.md#the-sprite-mask--mask-freezemask-maskshape)
tests points in that `space`. The test is O(vertices) — see the sprite-mask cost model for when the exact
O(1) forms are the better tool.

**Inside or outside (`inverted()`).** By default the region is the **inside** of the shape. To confine the
effects to the **outside** instead, give the region the inverted shape — `shape.inverted()` returns a copy
with its inside and outside swapped, so the effects run everywhere *except* the shape:

```cpp
frame.regions.push_back(Region{
    .key     = "outside",
    .shape   = ShapePoints::circle({80, 72}, 30).inverted(),  // the effects run OUTSIDE the circle
    .effects = {someEffect}});
```

(`inverted()` toggles the underlying `ShapePoints::invert` flag; an empty shape ignores it. The same flag
flips a curve boundary, and a `Transparency`'s `TransparentInside`/`TransparentOutside` is the equivalent choice of
which side of its region goes see-through.)

**Curved boundaries (`curve`).** A polygon's edges are straight. For a **smooth curved** boundary — exact
between control points, no facets at any zoom — give the shape a closed [`Curve`](curve.md) instead of
points, via `ShapePoints::fromCurve`:

```cpp
#include "retropp/curve.h"   // Curve

// A rounded boundary of four quadratic Béziers — a smooth "window" with no straight edges.
Curve outline = Curve::quadratic({80, 32}, {128, 32}, {128, 72});  // N → corner → E
outline.quadraticTo({128, 112}, {80, 112})                          // E → corner → S
       .quadraticTo({32, 112}, {32, 72})                            // S → corner → W
       .quadraticTo({32, 32}, {80, 32});                            // W → corner → N (back to the start)
region.shape = ShapePoints::fromCurve(outline, /*radius=*/0.0f);    // the boundary IS the curve
```

The curve is treated as a **closed loop** (the last segment's end joins the first's start); `radius` and
`transform` compose exactly as they do for a polygon. You can also set `shape.curve = {…segments…}`
directly; `points` is ignored whenever `curve` is non-empty.

| boundary segments | result |
|---|---|
| **Linear / Quadratic** | evaluated exactly in-shader — a true curved edge, no facets |
| **Cubic / Catmull-Rom** | exact with a baked mask (below); without one, sampled to a faceted polygon |

For the analytic (linear / quadratic) path the GPU carries up to **32 curve segments** (a longer boundary
truncates with a logged warning). The `curve_region_demo` example confines a ripple to a quadratic boundary
beside a sampled-polygon approximation of the same outline, so the no-facets difference reads directly.

**Cubic / Catmull-Rom boundaries (`bakeCurveMask`).** A cubic or Catmull-Rom curve has no closed-form
distance the shader can solve per pixel, so to get an exact (no-facet) edge for one you bake its distance
field into a mask once and let the region reference it. `bakeCurveRegion` does both — it bakes the mask and
returns a ready shape:

```cpp
#include "retropp/curve.h"   // Curve

Curve blob   = Curve::throughPoints(waypoints, /*closed=*/true);  // a wavy Catmull-Rom loop (cubic)
region.shape = renderer.bakeCurveRegion(blob);                    // boundary IS the curve, exact via a mask
```

Bake once at setup — it samples the curve's distance field, which is not a per-frame cost — then the region
reuses the mask every frame, and moving / rotating / scaling the region reuses the *same* mask with no
re-bake (the region `transform` warps the lookup). The mask has no segment cap (the boundary lives in a
texture, not the cbuffer). The lower-level call `renderer.bakeCurveMask(blob)` returns a `CurveMaskId` you
assign to `shape.curveMask` yourself — use it to share one baked mask across several shapes. A cubic
boundary with no mask attached still renders, sampled to a faceted polygon. The same mask drives the
see-through `stencil()` path, not just fills. The `curve_region_mask_demo` example fills the same wavy cubic
blob both ways side by side — exact mask vs faceted polygon — with the true curve traced over both.

**Stroke / outline (`strokeWidth`).** By default a region is the **filled interior** of its shape. Set
`shape.strokeWidth` to a positive width to instead confine the effects to a **band along the boundary** —
the shape's outline — so the effects trace a hoop, a border, or an arbitrary curved **path** instead of
filling the interior:

```cpp
ShapePoints ring = ShapePoints::circle({80, 72}, 30);
ring.strokeWidth = 8.0f;  // a ring 8 px wide along the edge — the interior is untouched
frame.regions.push_back(Region{.key = "ring", .shape = ring, .effects = {ripple}});
```

The band is centered on the boundary the fill uses (the `radius`-inflated edge), `strokeWidth` wide, in
viewport pixels. It works on every shape — circle, capsule, polygon (concave included), and a `fromCurve`
curve — and composes with `inverted()` (stroke then invert = everywhere *except* the band) and with a
`Transparency` (a see-through **ring** instead of a see-through fill). Because a stroke is symmetric about
the boundary it is sign-independent, so an **open** `fromCurve(...)` strokes into an open band — an effect
follows an arbitrary curved path, not just a closed loop. `strokeWidth = 0` (default) is the filled region.
`curve_region_demo` toggles fill ↔ stroke with **A**.

### Painting a colour into a region (`ColorFill`)

`ColorFill` is the built-in that **paints a colour** onto the pixels its region covers instead of warping
them. It carries one field — an `Rgba8 fill` — and the pixels it covers become that colour (`out.rgb =
fill`). The shape you give the region decides what the colour draws:

- **A solid shape** — a **filled** region: the interior becomes the colour.
- **A drawn line / path** — a **stroked** region (`shape.strokeWidth > 0`): the band along the boundary
  becomes the colour. A stroked circle is a coloured ring; a stroked **open** `fromCurve` is a coloured
  curved line. This is how you draw real vector lines — no hand-walking a curve into sprites.

```cpp
#include "retropp/draw_state.h"   // Region, ShapePoints, ScreenSpaceEffect
#include "retropp/palette.h"      // Rgba8

ScreenSpaceEffect cyan{ .kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{31, 219, 255} };

ShapePoints line = ShapePoints::capsule({20, 20}, {140, 60}, 2.0f);  // a 4 px-thick segment
frame.regions.push_back(Region{ .key = "line", .shape = line, .effects = {cyan} });  // a drawn cyan line over the scene
```

**Opacity is the `Region`'s, not the colour's.** Give the owning `Region` an `alpha` below 1 to blend its
fill (and any other effect it holds) over the scene — a translucent tint:

```cpp
frame.regions.push_back(Region{ .key     = "wash",
                                .shape   = ShapePoints::rectangle({96, 64}, 52, 30),
                                .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                                              .fill = Rgba8{200, 90, 0}}},
                                .alpha   = 0.5f });  // a 50% warm wash over the backdrop
```

**It emits the colour into the region's shape**, so it fills that shape wherever you attach it. The
`Region`'s `alpha` and `blend` are what compose the colour over the scene, and the attachment point
picks which scene that is:

- **Frame-level (`FrameDrawState::regions`) or a `Below`-scope layer effect** composes over the
  **composited scene** — drawing lines and shapes over the rendered frame.
- **`Layer` scope (`DrawLayer::regions`)** composes over **that layer's own content** — flat-shading a
  sprite, re-tinting a band of tiles.

Under `Normal` at full alpha the result is the colour itself at either attachment point. A mode that
reads its destination — `Multiply`, `Add`, `Screen`, `Subtract`, `Half` — differs between them, because
the destination differs.

`color_fill_demo` draws a solid fill, a stroked ring, a curved drawn line, and a translucent tint — all one
built-in, each confined by a `Region`.

**Transform + motion.** `shape.transform` is a `Transform` — the same scale / stretch / skew / rotate /
perspective / translate type layers and sprites carry — composed on top of the shape, about any pivot.
And because the frame is recomputed every frame, you **move** a shaped effect just by giving its region a
new shape each frame:

```cpp
// a wavy "porthole" gliding left↔right; nothing else animates
const float cx = 80.0f + 56.0f * std::sin(t * 0.01f);
bg.regions.push_back(Region{.key = "porthole", .shape = ShapePoints::circle({cx, 72.0f}, 30.0f), .effects = {wave}});

// or hold the shape and warp it instead:
ShapePoints box = ShapePoints::rectangle({40, 42}, 80, 60);
box.transform = Transform::scale(1.5f, 1.0f, 80, 72);  // stretch about the centre
bg.regions.push_back(Region{.key = "box", .shape = box, .effects = {wave}});
```

The `region_shapes_demo`, `region_transform_demo`, `region_motion_demo`, `region_vertical_wave_demo`,
`region_ripple_demo`, and `region_showcase_demo` examples each demonstrate one facet; the
showcase combines them (top-half parallax, a vertical wave confined to the bottom half, a roaming
built-in ripple).

### Making a layer see-through (the `stencil()` helper)

`Transparency` makes part of a layer **see-through** inside a region — nothing is erased or destroyed; the
pixels there go **transparent** so what's behind shows through. Like every effect it carries no shape of
its own; its region comes from the `Region` that owns it. For the common "make a shape see-through,
optionally run effects on each side" case there is a one-call helper — **`stencil()`** — that builds the
equivalent `Region`(s) for you:

```cpp
#include "retropp/draw_state.h"   // stencil, StencilMode, ShapePoints

// A round see-through window in a wall layer — the layers below show through it:
wall.regions = stencil(ShapePoints::circle({80, 72}, 30));   // TransparentInside, hard edge, Layer scope (the defaults)

// Keep only a soft-edged patch of this layer solid; the rest goes see-through:
wall.regions = stencil(ShapePoints::circle({80, 72}, 30), StencilMode::TransparentOutside, /*feather=*/6);
```

`stencil()` returns a `std::vector<Region>` — assign it to a layer's `regions` (or the frame's), or append
it to an existing list. Its arguments mirror the see-through exactly:

```cpp
std::vector<Region> stencil(ShapePoints shape,
                            StencilMode mode = StencilMode::TransparentInside,
                            float feather = 0,
                            ScreenSpaceEffectScope scope = ScreenSpaceEffectScope::Layer,
                            std::vector<ScreenSpaceEffect> insideRegion = {},
                            std::vector<ScreenSpaceEffect> outsideRegion = {});
```

`mode` picks which side goes see-through; `feather` softens the boundary:

| `mode` | see-through side | result |
|---|---|---|
| `TransparentInside` (default) | the pixels **inside** the shape | a **window** — what's behind shows through it |
| `TransparentOutside` | the pixels **outside** the shape | keeps **only the inside** solid; the rest is see-through |

| `feather` | edge |
|---|---|
| `0` (default) | hard — a crisp boundary |
| `> 0` | soft — a coverage ramp centered on the boundary, in shape-local pixels (the same units as `shape.radius`) |

**What shows through follows `scope`** — the same field that governs every per-layer effect:

- **`Layer` (default).** Only **this layer** goes see-through, so the layers composited **below** it show
  through. An "x-ray window onto the layer behind."
- **`Below`.** This layer **and everything beneath it** at this `z` go see-through, so the **backdrop**
  shows through — a true cut-out down to nothing.
- At **frame level** (`stencil(...)` assigned to `frame.regions`) the whole composited frame goes
  see-through inside the shape → the **backdrop** shows through.

The shape is an ordinary `ShapePoints`, so **every shape and every curve works** — circle, capsule,
polygon (concave included), and a closed [`Curve`](curve.md) via `ShapePoints::fromCurve`; `radius` and
`transform` compose exactly as for a region, so a see-through shape rotates, stretches, and drifts the
same way.

**Effects on each side (`insideRegion` / `outsideRegion`).** Pass effect lists for either side — empty by
default (no extra work):

```cpp
wall.regions = stencil(ShapePoints::circle({80, 72}, 30), StencilMode::TransparentInside, /*feather=*/0,
                       ScreenSpaceEffectScope::Layer,
                       /*insideRegion=*/  {rippleEffect},   // ripple what shows THROUGH the see-through inside
                       /*outsideRegion=*/ {waveEffect});    // wave the still-solid outside
```

A side effect runs on the **composited scene** at that point — so an effect placed on a see-through side
reaches the layers showing *through* it (the revealed content itself ripples), not merely the layer's own
transparent pixels. `insideRegion` is confined to `shape`, `outsideRegion` to `shape.inverted()`; both run
at `Below` scope so they resolve on the revealed scene. (Under the hood `stencil()` builds one `Region`
with a `Transparency` effect plus one `Region` per side effect — you can hand-build the same regions if
you want finer control.)

The `stencil_demo` example makes a drifting shape in a brick wall see-through over a vivid rear scene,
with a ripple inside and a wave outside: `A` toggles which side is see-through, `B` cycles the shape
(including a scalloped quadratic curve), `Up` toggles the feathered edge, `Down` switches `Layer` (reveal
the rear scene) versus `Below` (reveal the backdrop), and `Start` toggles the side effects on/off.

### Frame edge: `DisplacementEdge`

When a displacement pulls a row (or column) inward, it exposes a strip at the frame edge with no
source pixel behind it. You choose what fills it, per effect:

```cpp
enum class DisplacementEdge { Blank, Stretch };
effect.edge = DisplacementEdge::Blank;    // default — the exposed strip is backdrop-blank
effect.edge = DisplacementEdge::Stretch;  // duplicate the edge pixel outward (smears the border)
```

`Blank` is the default. What "blank" means depends on the scope: at frame-level and for a `Below`
effect it is the backdrop colour (those displace an opaque image, so the strip shows the backdrop
rather than a stretched edge); for a `Layer` (isolated) effect it is *transparent*, so the exposed
strip reveals the layers composited below this one. `Stretch` (edge-duplicate) is offered for the look
some effects want, at any scope. (A genuinely *seamless* wrapping water — where a pulled-in row reveals
the next wrapped tile instead of an exposed strip at all — would displace inside the tile sampler; that
is a possible future option, not built today. For most cases, sizing the wavy content to its own layer
and letting the `Blank` strip reveal what's below is the simpler answer.)

### Custom shader stages — your own effect (`kind = Custom`)

When the built-in effect vocabulary stops, register your own fragment shader and use it **exactly like
a built-in effect** — at either attachment point, composing with the built-ins. Your shader is a
first-class effect kind, not a stage bolted on at the end.

Your shader declares its **own** params in a cbuffer and writes only `main()`; the platform injects the
source texture + sampler:

```hlsl
// game/shaders/swirl.frag.hlsl — your own params, your own names
cbuffer Params : register(b1, space3) { float2 center; float angle; };
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    /* swirl uv about center by angle … then: */ return sampleSource(swirledUv);  // honours the edge policy
}
```

Two steps in C++:

```cpp
// 1) Once, at load time — register your fragment BY PATH. No uniform type, no ShaderVariants, no
//    generated-header include, no CMake rule: the build scans this source, sees the .hlsl path, compiles
//    + embeds it, reflects its cbuffer, and registers it. See rendering.md "Custom shader stages" for the
//    fragment contract + the one-time per-target build wiring.
PostProcessStageId swirl = renderer.registerPostProcessStage("game/shaders/swirl.frag.hlsl");

// 2) Per frame — attach it as an effect, setting YOUR shader's own params inline (the build surfaced
//    `.center` / `.angle` on ScreenSpaceEffect by reflecting the cbuffer). No uniform struct, no as_bytes.
frame.postEffects.push_back(ScreenSpaceEffect{
    .kind = ScreenSpaceEffectKind::Custom,
    .customShader = swirl,
    .center = {0.5f, 0.5f}, .angle = 0.3f});  // mutate live off the frame counter
```

`amplitude`/`frequency`/`phase`/`axis` are **not** read for a `Custom` effect — your shader + its own
params define its behaviour. `edge` and `scope` still apply: `edge` governs how your shader's
`sampleSource()` treats the frame border (`Blank` vs `Stretch`), and `scope` makes a per-layer `Custom`
effect `Layer`-isolated or a `Below` adjustment, exactly like a built-in — both are compositing
decisions the platform makes, not the shader. Order is purely list order: a `Custom` entry and a built-in `RowDisplacement` in the same
`postEffects` run in whatever order you push them. The build reflects each shader's cbuffer into the
effect's inline fields and fills it per frame via a generated packer — so there is nothing to keep alive
across `renderFrame()` and no size to match by hand. The `custom_stage_test` exercises the reflection +
packing device-free; the custom path is for effects the built-in library doesn't cover (the useful
ones — ripple, the wave — are built-ins).

### Per-row data table — an array input for an effect (`paramTable`)

An effect's params are scalars, so a closed-form per-row effect — `f(row, time)`: a sine wave, a gradient —
lives entirely in the shader. When the per-row values are an **arbitrary array** the game computed that
frame (a per-scanline scroll that isn't a clean curve, a digitized warp ramp, a per-region colour),
`ScreenSpaceEffect::paramTable` carries it: an inline span of `Vec4`, one entry per row — the table
counterpart to the scalar params.

```cpp
#include "retropp/draw_state.h"   // ScreenSpaceEffect, Vec4

std::vector<Vec4> scale(viewportHeight);          // one entry per scanline, refilled each frame
for (int r = 0; r < viewportHeight; ++r)
    scale[r] = Vec4{perLineScale(r), 0, 0, 0};    // whatever profile you computed this frame

frame.postEffects.push_back(ScreenSpaceEffect{
    .kind = ScreenSpaceEffectKind::Custom, .customShader = warp,
    .paramTable = scale});                        // inline span, valid for this renderFrame call
```

The shader reads its table by row through two helpers the platform injects (beside `sampleSource`):

```hlsl
// game/shaders/row_warp.frag.hlsl
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float s = paramRowAtUv(uv).x;       // the row under this fragment's scanline (maps uv.y to a row)
    float2 c = float2(0.5f, 0.5f);
    return sampleSource(c + (uv - c) * float2(s, 1.0f));   // a per-line horizontal scale
}
```

- `paramRow(i)` — row `i` of the table (`i` in `[0, rows)`).
- `paramRowAtUv(uv)` — the row under this fragment's scanline (the per-scanline case).
- Both return 0 when the effect carries no table, so an empty `paramTable` (the default) leaves the output
  unchanged.

`paramTable` is a generic effect input: any kind may carry it (a `Custom` shader reads it in v1), and it
composes everywhere an effect does — frame `postEffects`, per-layer `effects`, and region-confined via a
`Region`. It is game-owned and valid for the `renderFrame` call, like a layer's `cells` / `sprites` /
`palettes`. What the row index *means* is the shader's choice — a scanline (per-line scroll / warp /
colour) or a region id. `row_data_table_demo` drives a domed per-line horizontal scale from the table,
shown whole-frame and confined to a circle region.

## Transforms

```cpp
#include "retropp/transform.h"   // Transform
```

Every layer **and every sprite** carries a **`Transform`** — an arbitrary 2D geometric transform (scale,
rotation, skew, translation, **and perspective**), about any pivot, with no per-console hardware ceiling.
The default is the identity, so content that sets no transform renders unchanged. The
transform applies to both the **tile** path (`DrawLayer::transform`) and the **sprite** path
(`DrawLayer::transform` *and* per-sprite `Sprite::transform` — see *Per-sprite transforms* below).

`Transform` is a value type that *is* a 3×3 projective matrix — you build it with named constructors and
compose with `.then()`:

```cpp
DrawLayer floor{.key = "floor"};   // key is required — the reconciliation identity (see Layer identity above)
floor.transform = Transform::rotation(degrees, 80.0f, 72.0f)   // yaw about the viewport centre …
                      .then(Transform::perspective(0.0f, -0.0045f));  // … then a receding Mode-7 floor
```

- `Transform::identity()`, `translation(dx,dy)`, `scale(sx,sy, pivotX,pivotY)`, `rotation(deg, pivotX,pivotY)`,
  `skew(kx,ky, pivotX,pivotY)`, `perspective(gx,gy)`. Pivots are in **content-local pixels** (a 160×144 layer
  rotates about its centre with pivot `(80, 72)`).
- `a.then(b)` applies `a` first, then `b`. The affine constructors are `constexpr`; `rotation` is not (it uses
  `std::sin`/`cos`). `perspective` adds the foreshortening that makes a rotating ground recede — the spinning
  "Mode-7" floor — done per-pixel on the GPU, **not** as any per-scanline hardware trick.

**The footprint edge.** A transform places the layer's `[0, size)` rectangle as a *finite footprint* (rotate it
and it's a diamond; the viewport area outside it is exposed). `DrawLayer::transformEdge` chooses what fills that
exposed area, using the same `DisplacementEdge` vocabulary as the wavy effect:

```cpp
floor.transformEdge = DisplacementEdge::Blank;    // default — transparent; the layers below show through
floor.transformEdge = DisplacementEdge::Stretch;  // clamp-to-edge; the footprint border smears outward
```

The area *behind* a perspective horizon (where the projection has no content) is always blank in both modes — it
is the sky above the floor, never a smear. (`transformEdge` is a tile-path footprint concept; a sprite is finite
geometry, so it has no footprint edge — see below.)

> **Note.** The transform path samples nearest (crisp pixels, the faithful look). Rotating or non-integer-scaling
> low-resolution pixel art shimmers and renders cells unevenly — that is inherent to nearest-sampling a
> transformed grid, not a defect. A tile layer wraps per its `TileContent::wrap` mode: the default `Repeat`
> tiles toroidally (author tiling content to tile seamlessly at the tilemap's dimensions), while `Blank` makes
> the map finite (it ends at the edge — no seam possible) and `Clamp` smears the edge tile.

### Per-sprite transforms

A `Sprite` carries its own `Transform`, applied in **sprite-local pixel space** — the `[0, width) × [0, height)`
rectangle of the sprite's own art. It composes with the sprite's layer transform: the sprite's transform runs
**first** (in sprite-local space), then the layer's transform (in viewport space), exactly the order a tile
layer's content travels. So a single sprite can spin about its own centre while its whole layer also rotates.

```cpp
Sprite s{.key = "spinner"};   // key is required — see sprites.md
s.size   = AssetDimensions{16, 16};
s.origin = s.center(Space::Quad);               // x/y place this point — here the sprite's centre
s.pivot  = s.center(Space::Quad);               // the transform spins about this point
s.transform = Transform::rotation(degrees);     // spins about the pivot — the sprite's own centre
// …and the layer it rides can carry its own transform too — the two compose:
spriteLayer.transform = Transform::rotation(slow, 80.0f, 72.0f);  // the whole layer orbits
```

- The transform applies about **`Sprite::pivot`** (the spin centre), while `x`/`y` place **`Sprite::origin`**
  (the placement handle); set both to one point to place and spin on the same spot. Default `{0,0}` for each
  = the top-left, which is why a plain `Transform::rotation(deg)` on a sprite that sets neither turns about
  its corner. (A pivot baked into the matrix by a named constructor — `Transform::rotation(deg, 4, 4)` —
  still composes inside the transform itself. See
  [anchors-and-articulation.md](anchors-and-articulation.md) for origin/pivot as the attachment surface.)
- **Perspective works on sprites too** — a sprite carrying `Transform::perspective(...)` foreshortens into a
  trapezoid (real projective geometry, perspective-correct texture). A sprite tilted so far that a corner passes
  *behind* the projection plane is clipped by the GPU's near plane — an extreme case; gentle foreshortening and
  all affine transforms are unaffected.
- **Flips compose independently.** `Sprite::flipX`/`flipY` mirror the *texture* (a fragment-side UV op), separate
  from the quad geometry — a flipped + rotated sprite mirrors its art and rotates its quad, both at once.
- Identity (the default) is a no-op — a plain axis-aligned sprite.

> **Note.** A transformed sprite resolves its coverage on the **viewport grid** by default: the rotated /
> scaled / foreshortened silhouette and its internal texels land on whole viewport pixels, so the sprite
> reads as chunky blocks that upscale cleanly rather than resampled edges.
> `Renderer::evaluationGrid(EvaluationGrid::Output)` switches every analytic path — this one included —
> to smooth output-resolution evaluation instead (see
> [rendering.md](rendering.md#evaluation-grid-evaluationgrid)). Sub-pixel placement is unaffected either
> way; an untransformed sprite always takes the plain crisp path.

## Where to change things

- **Scale / rotate / skew / perspective a layer:** `DrawLayer::transform` (a `Transform`) + `transformEdge` for
  the exposed footprint — see Transforms above.
- **Transform one sprite (spin/scale/foreshorten it about its own pivot):** `Sprite::transform` — composes with
  the layer transform; see *Per-sprite transforms* above.
- **Stacking order / "walk behind":** set the layer's `z` — depth between layers is `z` only. Within
  one sprite layer, `Sprite::z` stacks the sprites (non-unique, stable ties) — see
  [anchors-and-articulation.md](anchors-and-articulation.md#stacking-the-parts-spritez).
- **Parallax:** give layers different `scroll` rates.
- **A see-through layer:** per-layer `alpha` (whole-layer translucency), or per-source index-hole
  transparency on the atlas (see [images-and-transparency.md](images-and-transparency.md)).
- **Fade one sprite in a shared layer:** `Sprite::alpha` — per-sprite opacity, multiplies under the layer
  alpha; see *`SpriteContent` + `Sprite`* above.
- **Day/night, fades, flashes, tints:** a `ColorFill` region + a blend mode (Multiply for day/night, Normal
  for flash/fade) on `frame.regions` — see *Whole-frame colour* above.
- **The colour of the art itself:** each cell's / sprite's `atlas` + `palette` handle
  ([tiles-and-colour.md](tiles-and-colour.md)).
