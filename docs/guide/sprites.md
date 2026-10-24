# Sprites

A `Sprite` is a placed, instanced quad that names its own sheet and palette and draws through the sprite
pipeline. It is the platform's richest drawable: on top of placement and art it carries opacity, a blend mode,
a geometric transform, an articulation surface (origin / pivot / anchors), and the full effect-carrier
grammar (an effect chain, confined regions, and Below-scope scene lensing) — the same grammar a `DrawLayer`,
a `Region`, and the frame speak. Everything here is immediate-mode: you build the `Sprite` values fresh each
tick and hand them to the frame; the platform reconciles them against the previous tick by `key`.

Sprites live in a sprite layer (`SpriteContent`); one layer mixes sheets, palettes, sizes, and effects
freely because every sprite names its own. For how sprite layers sit in the frame and interleave with tile
layers by `z`, see [draw-state.md](draw-state.md); for the effect *kinds* a sprite carries, see
[draw-state.md](draw-state.md#screen-space-effects) and [blend-modes.md](blend-modes.md).

## Contents

- [The type](#the-type)
- [Identity — `key`](#identity--key)
- [Placement — `x` / `y`, `origin`, `pivot`](#placement--x--y-origin-pivot)
- [Art — `size`, `atlas`, `tile`, `palette`](#art--size-atlas-tile-palette)
  - [`tile` — the top-left cell](#tile--the-top-left-cell)
  - [`size` — the read rectangle, arbitrary 8-aligned dimensions](#size--the-read-rectangle-arbitrary-8-aligned-dimensions)
  - [Transparency](#transparency)
- [Flips & rotation](#flips--rotation)
- [Within-layer stacking — `z`](#within-layer-stacking--z)
- [Opacity — `alpha`](#opacity--alpha)
- [Blend — `blend`](#blend--blend)
- [Geometric transform — `transform`](#geometric-transform--transform)
- [Anchors & articulation — `anchors`, `anchor`, `toLayer`](#anchors--articulation--anchors-anchor-tolayer)
- [The effect carrier — `effects` & `regions`](#the-effect-carrier--effects--regions)
- [The sprite mask — `mask`, `freezeMask`, `maskShape`](#the-sprite-mask--mask-freezemask-maskshape)
  - [Below-scope — the sprite as a refraction lens](#below-scope--the-sprite-as-a-refraction-lens)
- [What interpolates, what snaps](#what-interpolates-what-snaps)
- [See also](#see-also)

## The type

```cpp
struct Sprite {
    ObjectKey       key;                        // REQUIRED reconciliation identity (no default ctor)
    int             x = 0, y = 0;               // place the ORIGIN in the layer's space (before scroll)
    std::int32_t    z = 0;                      // within-layer stacking; NON-unique, ties keep submission order
    AssetDimensions size = AssetDimensions::GameBoy8x8;
    AtlasId         atlas{};                    // which uploaded sheet this sprite draws from
    std::uint16_t   tile = 0;                   // top-left atlas cell (8px grid) within that sheet
    PaletteId       palette{};                  // which uploaded palette colors it
    float           alpha = 1.0f;               // per-sprite opacity [0,1]; multiplies UNDER the layer alpha
    BlendMode       blend = BlendMode::Normal;  // how the sprite composites over its container's image
    bool            flipX = false, flipY = false;
    Rotation        rotation = Rotation::None;  // 90° texture rotation; composes with the flips
    Transform       transform{};                // per-sprite geometric transform, sprite-local space, about pivot
    Point           pivot{};                    // transform center (QUAD-space px; {0,0} = top-left)
    Point           origin{};                   // placement handle — the point x/y place (QUAD-space px)
    std::span<const Anchor> anchors;            // published art-space points (game-owned; empty = none)
    std::vector<ScreenSpaceEffect> effects;     // whole-silhouette effect chain over the sprite's own pixels
    std::vector<Region>            regions;      // confined + containered effects; quad-space shape ∩ silhouette

    // resolvers (all const, constexpr):
    Point anchor(std::string_view | std::size_t, Space) const; // the anchor in the space you pass (Quad or Layer)
    Point toLayer(Point quadPoint) const noexcept;             // map any quad-space point into the layer's space
    Point center(Space) const noexcept;                        // sprite middle: Quad = art midpoint, Layer = drawn center
};
```

The minimum is a key, a size, an atlas, a tile, and a palette — everything else defaults to a plain,
opaque, axis-aligned sprite placed by its top-left corner:

```cpp
Sprite hero{.key = "hero", .x = 40, .y = 72, .size = AssetDimensions::Snes16x16,
            .atlas = heroSheet, .tile = 0, .palette = heroPalette};
```

## Identity — `key`

`key` is a required `ObjectKey` (the first member) — the stable, developer-supplied name the interpolator
matches this sprite to its previous-tick state by. It has **no default constructor**, so omitting it is a
compile error. It must be **unique within a frame across every sprite layer** (the renderer keeps one sprite
map for the whole frame, not one per layer), and it must be a **stable identity for the logical object** —
the same value re-emitted for the same object every frame.

This matters because the renderer reconciles by key to interpolate motion: each render frame it matches a
sprite to the object that carried the same key last tick and eases the two states together (position *and*
transform). So the key must name the *object*, not its slot in this frame's array.

**The trap: keying by emission index.** It is tempting to hand out `"s0"`, `"s1"`, … by the sprite's
position as you build the list:

```cpp
// WRONG — key is the emission index. As the population changes, indices shift.
sprite.key = keyPool[sprites.size()];
```

The moment the population changes — a bullet spawns, an enemy dies, a pickup appears — every index after the
change shifts by one. A key that named object A last frame now lands on object B, so the interpolator eases
B *from A's last position*: B flashes at A's location for one tick. With objects spawning and dying
continuously the whole scene twitches, and even a stationary object (a HUD element, the player) flickers to
a wrong spot whenever something *else* changes count. The platform does exactly what it is told; the keys are
lying about identity.

**The fix: name the object.** Derive the key from something intrinsic and stable — an `ObjectKey` owns its
string, so a key you assemble each frame drops straight in:

```cpp
enemy.key  = "enemy_" + std::to_string(enemy.id);                    // a per-spawn id assigned at creation
brick.key  = "brick_" + std::to_string(r) + "_" + std::to_string(c); // a fixed grid cell
player.key = "player";                                               // a singleton
```

- **Pooled / spawned objects** (bullets, enemies, particles): give each a monotonic `id` at spawn and key by
  it. A new object gets a new key and mounts fresh (snaps into place — no ease from whatever last used its
  slot); a dead object's key disappears and unmounts. This also handles a **teleport** (a respawn, a screen
  wrap, a reset that jumps an object across the screen): hand it a fresh key that tick and it mount-snaps
  instead of streaking across the gap.
- **Fixed-grid objects** (tiles-as-sprites, bricks, a formation): key by the cell.
- **Singletons** (the player, a boss, a cursor): a fixed string literal.

The identity is the string *value*, which the renderer copies into its own map during `renderFrame()` and
matches by value; a literal, a held `std::string`, and a name built on the spot are all equivalent. Short
keys like `"enemy_5"` sit in the string's small-buffer, off the heap. Uniqueness is enforced like layer keys
(`validateSpriteKeys` — throw in debug, warn in release); a duplicate or empty key is a bug.

## Placement — `x` / `y`, `origin`, `pivot`

`x` / `y` place the sprite's **origin** in the layer's coordinate space (before the layer's scroll is
applied, so a sprite on a world-scrolling layer tracks the background and a HUD layer at `scroll {0,0}`
stays fixed). `origin` and `pivot` are two quad-space points doing two different jobs:

- **`origin`** is the *placement handle* — the quad-space point that lands at `(x, y)`. Default `{0,0}`
  (top-left), so a sprite that never sets it places by its top-left corner.
- **`pivot`** is the *transform center* — the point `transform` spins about. Default `{0,0}`.

A quad point `p` lands at `(x, y) + (pivot − origin) + transform·(p − pivot)`. At identity this cancels to
`(x, y) + (p − origin)` — **the pivot drops out, so changing `pivot` never moves an untransformed sprite.**
Set the two to the *same* point to attach a joint: with `origin = pivot = a mount point`, that point sits at
`(x, y)` **and** the sprite spins about it — the placement handle and the hinge coincide.

`center(Space::Quad)` returns the raw art midpoint `{size.width/2, size.height/2}`, the common case for
placing and spinning:

```cpp
s.origin = s.center(Space::Quad);   // place s by its middle (x/y is now the center)
s.pivot  = s.center(Space::Quad);   // spin s about its middle
```

`center(Space::Layer)` gives that midpoint through transform + placement — the drawn center a consumer
reads. Set origin/pivot from an art feature with [`anchor`](#anchors--articulation--anchors-anchor-tolayer)
in Quad space (`claw.pivot = claw.anchor("hinge", Space::Quad)`). Origin and pivot are quad-space
transform inputs, so texture ops (`flip`, `rotation`) never move them and a `Space::Layer` value must not
feed them back.

## Art — `size`, `atlas`, `tile`, `palette`

Four fields locate the sprite's art. `atlas` (`AtlasId`) names the uploaded sheet it draws from and
`palette` (`PaletteId`) the uploaded palette it colors through — **both are per-sprite handles**, so one
sprite layer freely mixes sheets and palettes (there is no per-layer palette set; every sprite carries its
own two handles). `tile` and `size` locate and size the rectangle read out of that sheet.

### `tile` — the top-left cell

`tile` (`std::uint16_t`, default `0`) is the index of the sprite's **top-left cell** in its sheet. Cells are
laid out on a fixed **8-pixel grid**, row-major: cell `0` is the top-left 8×8 block, cell `1` the next 8px
to the right, wrapping by the sheet's width in cells. `tile` therefore addresses an 8px-aligned pixel origin
in the sheet; it is the *starting corner* of the read, not the whole sprite. The 16-bit width means a sheet
may address up to 65 536 cells.

### `size` — the read rectangle, arbitrary 8-aligned dimensions

`size` (`AssetDimensions`, default `AssetDimensions::GameBoy8x8` = `{8, 8}`) is the **pixel size of the
rectangle read from the sheet**, starting at `tile`'s pixel origin. The sprite reads a `size.width ×
size.height` block of 8×8 cells from that corner:

```cpp
struct AssetDimensions {
    int width  = 8;   // pixels — a multiple of 8
    int height = 8;   // pixels — a multiple of 8
    constexpr bool operator==(const AssetDimensions&) const = default;
    // console presets (static members) listed below
};
```

`AssetDimensions` (in `geometry.h`) is a `{width, height}` pixel tuple. **The size is arbitrary — but, like
the atlas cell grid it reads from, both `width` and `height` must be multiples of 8.** You are not limited
to the named presets: any 8-aligned `width` and `height` are legal — `AssetDimensions{40, 24}` (a 5×3 cell
block), `AssetDimensions{80, 48}`, `AssetDimensions{8, 40}` — the presets are just the common console sizes.
A sprite whose size spans several cells reads them contiguously: a 16×16 sprite reads the 2×2 cell block
whose top-left cell is `tile`, a 24×16 sprite reads a 3×2 block, and so on. The platform imposes no maximum
beyond the sheet's own extent — a read that runs off the sheet's edge reads whatever the atlas store holds
there, so keep the rectangle within the sheet you uploaded.

The named presets are **static members of the type** (the self-type-constant idiom, the same pattern as
`ViewportResolution` / `TimingProfile`), so a preset and a raw `{w, h}` are interchangeable at any call
site. They are a convenience for legibility, **not** an enum you must choose from — the platform generalizes
past the Game Boy, and an arbitrary `AssetDimensions{w, h}` covers anything not named. The full preset set:

| Preset | Size | Preset | Size |
|---|---|---|---|
| `GameBoy8x8` (default) | 8×8 | `Snes8x8` | 8×8 |
| `GameBoy8x16` | 8×16 | `Snes16x16` | 16×16 |
| `GameBoyColor8x8` | 8×8 | `Snes32x32` | 32×32 |
| `GameBoyColor8x16` | 8×16 | `Snes64x64` | 64×64 |
| `GameBoyAdvance8x8` | 8×8 | `Nes8x8` | 8×8 |
| `MasterSystem8x8` | 8×8 | `Nes8x16` | 8×16 |
| `MasterSystem8x16` | 8×16 | `Genesis32x32` | 32×32 |

The preset *names* carry their dimensions (`GameBoy8x16`, not "GameBoyTall") so the value reads clearly at
the call site. `AssetDimensions` is also the unit the atlas slicer carves an uploaded image into, so the
size you draw a sprite at and the size you sliced its sheet by are the same type — see
[images-and-transparency.md](images-and-transparency.md#slicing).

### Transparency

A sprite's transparency is opt-in **per sheet**, declared at upload, not per sprite:

- **Index holes** — pass `TransparentIndices` to `uploadAtlas`: `TransparentIndices::GameBoy` makes palette
  index 0 a hole (the conventional OBJ transparency), `TransparentIndices::of({n, …})` names specific
  indices, and the default (`TransparentIndices::None`) makes none transparent. A pixel whose palette index
  is a declared hole is discarded — nothing drawn, the layers below show through.
- **Material holes** — independently, a palette entry whose **alpha is 0** is a hole wherever it is used,
  regardless of index declarations.

Both are *structural* transparency (a discarded pixel), distinct from `alpha` opacity (which dims but still
draws). This coverage is also the sprite's effect **silhouette** — the mask a whole-silhouette effect or a
Below-scope lens is confined to. The full model — how holes interact with `alpha`, with slicing, and with
the effect silhouette — is in [images-and-transparency.md](images-and-transparency.md) and
[tiles-and-color.md](tiles-and-color.md).

## Flips & rotation

`flipX` / `flipY` and `rotation` are **texture** operations — they change which source pixel is read, not
the sprite's geometry. `rotation` (`Rotation::None` / `Rot90` / `Rot180` / `Rot270`, clockwise) rotates the
art in 90° steps and **composes with the flips**, giving all eight orientations of square art from one cell.
For arbitrary-angle rotation, warp the quad geometry with `transform` (below) instead — a sprite can carry
both (its art reoriented by rotation + flips, its quad warped by transform). Flips and rotation are discrete;
they snap to the submission and never ease.

## Within-layer stacking — `z`

Sprites on one layer draw back-to-front by ascending `z` — the within-layer sibling of `DrawLayer::z`, with
one deliberate asymmetry: **sprite `z` is not unique.** Any values are legal (negatives included), equal-`z`
sprites keep their submission order (the sort is stable), and nothing validates or throws. Write whatever
orders the scene — explicit ranks for an articulated creature's parts, or the feet `y` for a top-down
Y-sort — without burning a layer per row:

```cpp
part.z = part.footY;     // Y-sort: a lower sprite draws in front
```

`z` is discrete like the flips; it snaps to the current submission and never eases, so a mid-motion rank
change pops immediately.

## Opacity — `alpha`

`alpha` (default `1.0`, opaque) fades one sprite on its own. It composes **multiplicatively under** the
layer: a pixel's final opacity is `palette α × sprite α × layer α`, so a sprite can be more transparent than
its layer, never more opaque. Like `DrawLayer::alpha`, it **eases between ticks** under the automatic
interpolator (keyed by `key`). It is opacity, not a hole — `alpha = 0` shows nothing but punches no
structural hole (only a material-transparent palette entry does that). To fade a whole group, use the
layer's `alpha`; reach for `Sprite::alpha` when one sprite in a shared layer needs it.

## Blend — `blend`

`blend` (default `Normal`) is the sprite's half of the container pair beside `alpha`: `alpha` is *how much*
the sprite contributes, `blend` is *how* it combines over its container's image. A `Multiply` sprite is a
shadow decal that darkens the scene under it; an `Add` sprite is a flare that lifts it. The grade uses
`applyBlendMode` over the sprite's own opaque pixels; the container it grades against is the image the
sprite layer draws into — the scene beneath for an ordinary layer, or the layer's own content for a layer
composited in isolation (one carrying its own `DrawLayer::blend` or an effect chain).

```cpp
Sprite shadow{.key = "shadow", .x = 64, .y = 96, .atlas = decals, .tile = kShadow,
              .palette = grays, .alpha = 0.7f, .blend = BlendMode::Multiply};
Sprite flare {.key = "flare",  .x = 80, .y = 60, .atlas = decals, .tile = kFlare,
              .palette = warm,  .blend = BlendMode::Add};
```

`blend` is discrete — it snaps to each submission. Ease *toward* a blend by easing `alpha` with a
[`Tween`](tween.md). Same-mode sprites cost one composite pass per contiguous run in `z` order — the cost
scales with how many distinct modes a layer uses, never with the sprite count; an all-`Normal` layer takes
the plain instanced draw. The modes and the full container rule are in
[blend-modes.md](blend-modes.md#per-sprite-blend).

## Geometric transform — `transform`

`transform` is the sprite's own geometric transform, applied in sprite-local pixel space (the
`[0, size.width] × [0, size.height]` rectangle) about `pivot`. It is the path for arbitrary-angle rotation,
scale, and skew — distinct from the 90° texture `rotation`. It composes with the layer's own
`DrawLayer::transform` (sprite transform first, then the layer's), exactly as a tile layer's content does.
The identity default is a no-op. See the transforms section of [draw-state.md](draw-state.md#transforms) for
the `Transform` constructors (`rotation(θ)`, `scale`, `skew`, …).

```cpp
s.pivot     = s.center(Space::Quad);    // spin about the middle
s.transform = Transform::rotation(degrees);   // the sprite pre-subtracts pivot, so this spins about pivot
```

`transform` **eases between ticks** under the interpolator, so resubmitting a new angle each tick gives
smooth rotation with no manual tweening.

## Anchors & articulation — `anchors`, `anchor`, `toLayer`

`anchors` is the sprite's published points — a game-owned span of `Anchor{label, x, y}` in art space (a
static constexpr table works; the span must stay valid for the queries made against it). They turn art
features into addressable points other things pin to:

- **`anchor(k, Space::Quad)`** — the anchor on the placed **quad**, with orientation applied (a flipped
  leg's socket mirrors with the leg), before transform / placement. The bridge from an art feature to
  `pivot` / `origin` (both quad-space transform inputs).
- **`anchor(k, Space::Layer)`** — that anchor in the **layer's** space, through transform + placement:
  `(x, y) + (pivot − origin) + transform·(anchor(k, Space::Quad) − pivot)`. This is the value a same-layer
  sibling consumes (`forearm.pivot`-drives on `upperArm.anchor("elbow", Space::Layer)`) and the point a
  `Curve` / `PathWalker` / tween / emitter pins to.
- **`toLayer(p)`** — map any quad-space point through this sprite's transform + placement into the layer's
  space (the general form `anchor(k, Space::Layer)` is built on).

`k` is a label (`std::string_view`) or an index; both **throw `std::out_of_range`** on a miss, so a bad
anchor address fails loudly. A resolver reads only this sprite's own fields — it never sees the layer's
scroll or transform, so cross-layer consumers compose that themselves. The full articulation model
(hierarchies, joints, the flip/rotation interaction) is in
[anchors-and-articulation.md](anchors-and-articulation.md).

## The effect carrier — `effects` & `regions`

A sprite carries the same effect grammar every other container speaks: `effects`, a whole-silhouette effect
**chain**, and `regions`, a list of shape-confined `Region`s. Both default empty; a sprite that sets neither
is byte-identical to a plain sprite. **The effect domain is the sprite, not the atlas texture behind it** —
the art sits in an infinite transparent field, so a displacing effect that pulls the art aside exposes
transparency (the layers below show through), never black and never a smeared edge, and a displacing
effect inflates the sprite's render footprint by its displacement so a wobble crest is never clipped at the
static quad.

`effects` applies **first**, in list order, to the sprite's own pixel; `regions` applies **after**, each
confining its effects to its `shape` ∩ the silhouette and grading over the pixel by the region's own `alpha`
/ `blend`. Both evaluate **inline in the sprite fragment — no added passes**, with `Bloom` and `Glow` the
one exception (below). The effect *kinds*
(`ColorFill`, `Gleam`, `ColorSaturation`, `Bloom`, `Glow`, `Transparency`, `RowDisplacement`, `Ripple`, `Swirl`, `Custom`) are documented in
[draw-state.md](draw-state.md#screen-space-effects); this section covers how a sprite carries them.

```cpp
// A hero that pulses white when hit (a whole-silhouette flash) and darkens on its lower half (a region):
hero.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Gleam, .sweep = sheen, .gain = 1.6f}};
hero.regions = {
    Region{.key = "flash", .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                                         .fill = Rgba8{255, 255, 255, 255}}}, .alpha = pulse},
    Region{.key = "shade", .shape = ShapePoints::rectangle(Point{0, 8}, 16, 8),
           .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{90, 90, 130, 255}}},
           .blend = BlendMode::Multiply}};
```

Sprite-specific semantics of the shared grammar:

- **Whole-silhouette by default.** A chain effect grades or displaces the whole silhouette; confine it by
  putting it in a `regions` entry instead.
- **Quad-space shapes.** A region's `shape` is read in the sprite's **quad space** (art-pixel units, the
  pivot / origin / anchor space), evaluated pre-transform so it rides the sprite's transform exactly as the
  art does. An empty shape covers the whole silhouette; a circle / capsule / polygon confines to part of it
  ("flash only the bridge").
- **Displacing kinds are in the sprite's own art px.** `RowDisplacement` / `Ripple` / `Swirl` re-read the art at a
  displaced within-sprite position (`amplitude` / `center` in art pixels, not viewport pixels); out-of-art
  reads are transparent (the default `Blank` edge) or clamp to the art border (`Stretch`).
- **`Bloom` and `Glow` radiate through a shared emission buffer.** A glowing sprite's halo is not gathered
  per pixel: the sprite rasterizes its *emission* — the light it contributes — into a buffer shared by
  every glowing sprite on the layer, which is blurred once and composited once **per (kind, radius)**. So
  a field of fifty relics at one radius costs what one relic costs, and a wide halo costs barely more than
  a narrow one. Three consequences worth knowing:
  - The `radius` is authored in the sprite's own art pixels and blurs in viewport pixels, so a transformed
    sprite's halo scales with it (through the placement's linear scale — one isotropic factor, even under a
    non-uniform or perspective transform).
  - Chain steps **before** the glow feed the emission (recolor a sprite and it radiates the new color);
    steps after it grade the art alone, since the halo has already left the pixel. A `Custom` step has no
    emission pass, so a glow authored after one radiates the color from before it (logged).
  - The halo composites over the whole layer's art, so a sprite drawn later in the same layer does not
    occlude an earlier sprite's halo.
- **`Glow` is chain-only on a sprite.** A sprite `Glow` runs whole-silhouette (or as a Below lens) — a
  `Glow` inside a sprite `regions` entry is skipped with a logged warning, because its tint occupies the
  record lanes a region's shape needs. Layer and frame regions confine `Glow` fully. A `Bloom` inside a
  sprite region *is* supported at both scopes: at `Below` scope it grades the shared field like any other
  lens, and at `Layer` scope it is a graded source under the region's `blend` and `alpha` rather than an
  additive halo.
- **A confined `Bloom` does not spill, so its `radius` reads as softness rather than size.** Inside a
  region — or through a silhouette at `Below` scope — the halo is clipped to the shape and the light that
  would fall outside is never drawn. The blur still spreads a fixed amount of light over a larger area, so
  widening `radius` makes the halo fainter and more diffuse instead of larger. Raise `intensity` when a
  confined halo should read stronger. An unconfined halo (a frame or layer effect, or a sprite's own chain)
  behaves the other way, because there the spill is exactly what `radius` buys.
- **One `Custom` shader per chain.** A `Custom` chain effect runs a game-registered shader inline, its
  `sampleSource()` reading the sprite's own art (whole-silhouette, float params). See
  [blend-modes.md](blend-modes.md#a-custom-shader-as-a-lens) for registration.
- **Region keys are required** like every drawable's, but a sprite-carried region is **not interpolated**
  (uniform with layer / frame regions).

### Below-scope — the sprite as a refraction lens

Set an effect's `.scope` to `Below` and the sprite becomes a **lens**: instead of transforming its own
pixels, the effect distorts or grades the **composited scene beneath the sprite's layer**, confined to the
silhouette. **A Below sprite draws no art of its own** — the art is purely the coverage mask, so its alpha
sets the lens strength (an opaque mask fully replaces the scene on the silhouette; a partial-alpha mask
blends the distortion with the original scene). For a sprite that shows art *and* lenses the scene, use two
sprites.

Every effect kind is first-class at Below scope:

- `ColorFill` / `Gleam` / `ColorSaturation` / `Bloom` / `Glow` / `RowDisplacement` / `Ripple` / `Swirl` grade or distort the scene whole-silhouette (a Below
  displacement's `amplitude` / `center` are **viewport** px — it distorts the scene — where a Layer
  displacement reads them as art px; a Below `Glow` makes the scene's content radiate the authored tint
  through the silhouette).
- `Custom` runs a game shader over the scene through the silhouette (its `sampleSource()` reads the scene).
- `Transparency` scales the lens strength — whole-silhouette it is a binary reveal, region-confined it
  feathers a soft porthole of untouched scene.
- A Below-scope **region** confines the scene grade to its shape ∩ the silhouette.

**A Below `Bloom` / `Glow` reads a prepared field, and its `radius` is in viewport pixels.** The renderer
extracts the scene's emission into a rect sized to the lens's own drawn footprint, packs every such rect into
a shared atlas, and blurs the rects that share a reach together. Three things follow:

- **The reach is authored in viewport pixels**, like a Below displacement's `amplitude` and unlike a
  Layer-scope glow's art-pixel radius — a lens works on the scene, which is a viewport-resolution image, so
  the placement's scale does not enter.
- **Cost tracks the reaches, not the lenses.** Ten lenses at one `radius` blur in a single pass together,
  however far apart they sit; two different radii cost two passes. Lens draws are unaffected either way —
  below runs still split by pipeline alone, so the field work never adds a pass per sprite.
- **One lens can carry several.** A `Bloom` and a `Glow` at different reaches on the same sprite each get
  their own field and both land in the single draw the lens already costs.

A field costs the area it covers rather than a screenful, and fields page: whatever one sheet cannot hold is
planned onto the next. So no count and no area bounds a layer — only memory, one sheet at a time. The single
case that cannot be placed is a field larger than a whole sheet on its own, and that is counted and named
rather than dropped in silence.

A lens halo is clipped to the silhouette, so — as with a region — `radius` softens it rather than growing
it, and `intensity` is the strength control. A lens reaching well past its own art spends most of its light
outside the shape, where nothing is drawn.

```cpp
ScreenSpaceEffect bloom{.kind = ScreenSpaceEffectKind::Bloom,
                        .radius = 6.0f, .threshold = 60, .intensity = 220};
bloom.scope = ScreenSpaceEffectScope::Below;
ScreenSpaceEffect glow{.kind = ScreenSpaceEffectKind::Glow, .fill = Rgba8{255, 170, 60, 255},
                       .radius = 18.0f, .threshold = 60, .intensity = 200};
glow.scope = ScreenSpaceEffectScope::Below;
lens.effects = {bloom, glow};   // the scene's own light, spread and gilded through the silhouette
```

See [`examples/sprite_effects_demo`](../../examples/sprite_effects_demo/main.cpp) — its lens cycle ends on
exactly this pair.

Below-scope lenses render one pass per below-pipeline per layer, not per sprite. Layer-scope effects on a
lens are ignored (the renderer logs it — the art doesn't draw). The full lens surface, with worked
examples, is in [blend-modes.md](blend-modes.md#below-scope-sprite-effects--the-refraction-lens).

## The sprite mask — `mask`, `freezeMask`, `maskShape`

A sprite can hand back its **image as a mask** — its visible pixels, transparency accounted for,
**decoupled from the texture that defined them**. Test points against it (pixel-exact collision, the
most common use), or take it as geometry and use it anywhere a shape is usable — including a
**textureless region in the shape of the sprite**, which nothing else in the platform can produce (a
`Sprite` always carries a texture and draws it). Three forms along two axes: **ownership** (a live borrow
vs an owned snapshot) and **fidelity** (the exact coverage vs a coarse polygon). Each takes a `Space`;
the sprite resolves its own coverage from its `atlas` against its uploaded sheet:

```cpp
#include "retropp/sprite_mask.h"   // SpriteMask, FrozenSpriteMask, ArtMask, traceSilhouette

SpriteMask       live = ship.mask(Space::Layer);          // borrow — exact, live, frame life
FrozenSpriteMask kept = ship.freezeMask(Space::Layer);    // own — an exact snapshot, storable
ShapePoints      hull = ship.maskShape(16, Space::Layer); // own — the mask as geometry (≤16 points)
```

| Call | Returns | Ownership | Fidelity | Use it for |
|---|---|---|---|---|
| `mask(space)` | `SpriteMask` | borrow (frame life) | exact | a point test on the sprite where it is *now* |
| `freezeMask(space)` | `FrozenSpriteMask` | own (storable) | exact | a mask kept past the sprite (a trail, a stored collider) |
| `maskShape(n, space)` | `ShapePoints` | own (a polygon) | coarse (≤ `n`) | the mask as **geometry** — a `Region` / `stencil()` / physics |

**The exact forms answer `contains(point)` and `bounds()`** — no polygon is built. `contains(p)` is the
difference between hitting a sprite's *rectangle* and hitting its *shape*: it maps `p` back to an art pixel
and reports that pixel's visibility, so a point in a transparent gap is not contained. It is pixel-exact,
O(1), and GPU-free. `bounds()` is the AABB for a broad-phase.

The two exact forms differ only in **lifetime**. `mask` is a **borrow** — it reads the sprite's coverage
live, so it tracks the sprite's flips, rotation, transform, and placement for free, and must not outlive
the sprite. `freezeMask` **owns** — it copies the coverage and captures the placement at the call, so it
keeps answering after the sprite has moved on. Use `freezeMask` whenever a mask must be **stored**: a
motion trail of past positions, a collider snapshotted for later.

```cpp
if (enemy.mask(Space::Layer).contains(cursor)) { /* clicked the enemy's shape, not its box */ }

FrozenSpriteMask ghost = enemy.freezeMask(Space::Layer);   // capture where it is now
// ...frames later the enemy has moved; the ghost still answers its captured coverage.
```

**`maskShape(n, space, trace)`** traces the mask to a `ShapePoints` polygon of at most `n` vertices — the
mask as **geometry**, the one form that yields a real shape. It drops into a `Region` (a textureless
region shaped like the sprite, carrying effects with no art behind them), [`stencil()`](draw-state.md), or
your own SAT — and everything `ShapePoints` composes stays available: `radius` inflates it **evenly and
perpendicularly** (a scaled sprite copy cannot — scaling a long thin hull pushes further along its length
than across its beam), `strokeWidth` bands it into a hoop, `invert` means everything *except* the sprite,
and the result answers [`contains()`](draw-state.md#confining-an-effect-to-a-shape-region) so a modified mask is
testable, not just drawable. `ShapeTrace` (default `Conservative`) keeps the mask **contained** at every
budget — simplification only adds area, degenerating toward the convex hull, then the bounding box (the
coarsest form: a budget below 4 still returns the 4-corner box). `ShapeTrace::Balanced` hugs tightest,
erring both ways. The trace is **outer-boundary-only** — an interior hole is bridged, so even the
exact-detail polygon fills a hole that the exact `contains()` reports empty; disconnected blobs merge into
one polygon. `n < 3` throws `std::invalid_argument`; a fully-transparent tile returns an empty
`ShapePoints`. A `Region` sends ≤ 64 vertices to the GPU, so keep a region-bound budget ≤ 64.

**The cost model — exactness is expensive to DRAW, not to test.** The intuition "coarse = cheap, exact =
expensive" is backward for point tests: the exact `contains()` is **one coverage read, O(1)**; a
`ShapePoints::contains()` test is **O(vertices)**. The coarse polygon is not the fast mask — it is the
differently-*usable* one. Where vertex count really costs is **drawing**: a region's gate evaluates the
polygon SDF per fragment, so a drawn polygon's vertex count is a per-pixel multiplier while it is on
screen. Two per-call costs to keep out of hot loops: `freezeMask()` copies a coverage mask on every call
(per-frame-per-object use is allocation churn plus retained memory), and `maskShape()` traces the coverage
on every call — cache the polygon and re-query on pose change rather than re-tracing per frame.

Which collision each form is for: **point vs sprite** → the exact `contains()`; **sprite vs sprite, exact**
→ grid-sample the overlap of two `bounds()` and test `contains()` on both; **sprite vs sprite, cheap** →
`maskShape(n)` polygons + your own SAT. The platform ships the primitives; the overlap loop or the SAT is
yours, which keeps the exact-vs-coarse choice with the game.

**When you just want the art again, the mask is the wrong tool.** A second `Sprite` with the same
`atlas` / `tile` / `transform` re-draws the image as one instanced quad (a whole-silhouette `ColorFill` in
its `effects` flat-colors it) — far cheaper than drawing a polygon. That covers **re-drawing the art and
nothing else**: it cannot give a textureless region, a detached storable shape, or an evenly inflated
outline. Those are the mask's jobs.

**Gotchas.** A borrow must not outlive its sprite — to keep a mask, `freezeMask()` it. Every form reads the
**current tile** — re-query after a frame change. The coverage comes from the sprite's uploaded sheet
(resolved from `atlas`), so upload the atlas before you query; a sprite whose sheet was never uploaded reads
as an empty mask. The exact forms are **not** `ShapePoints`, so they do *not* go through the `Region` SDF
gate — to confine a screen-space effect to a sprite's exact coverage, put the effect on the sprite
([`effects`](#the-effect-carrier--effects--regions) / a Below-scope lens); `maskShape` is what feeds a
*polygon* to a region. `Space` is mandatory (no default).

The runnable showcase is [`examples/sprite_mask`](../../examples/sprite_mask/main.cpp).

## What interpolates, what snaps

The automatic interpolator eases a sprite between ticks by its `key`. It eases the **continuous placement
and geometry**: `x` / `y`, `alpha`, `transform`, `pivot`, and `origin`. It **snaps** the discrete state:
`z`, `flipX` / `flipY`, `rotation`, and `blend` (each takes the current submission's value with no
in-between). **Effect parameters are per-tick data on both `effects` and `regions` — the interpolator never
eases them;** a game eases an effect (a flash's alpha, a sheen's sweep, a wobble's phase) by recomputing it
each tick, typically with a [`Tween`](tween.md), and resubmitting. This is why a sprite that flashes or
wobbles is rebuilt every tick, not mutated.

## See also

- [draw-state.md](draw-state.md) — the frame / layer model sprites live in, and the effect *kinds*.
- [blend-modes.md](blend-modes.md) — the blend operators, the container rule, and the Below-scope lens surface.
- [anchors-and-articulation.md](anchors-and-articulation.md) — hierarchies, joints, and the anchor model.
- [images-and-transparency.md](images-and-transparency.md) — atlases, slicing, and per-sheet transparency.
- [tween.md](tween.md) — easing effect parameters and blends between ticks.
