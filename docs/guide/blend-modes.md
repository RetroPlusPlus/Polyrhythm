# Blend modes

A compositing **container** — a `Sprite`, a `Region`, a `DrawLayer`, or the whole `FrameDrawState` —
carries a `BlendMode` beside its `alpha`. `alpha` is *how much* the container contributes; `blend` is *how*
its pixels combine with what they sit on. The default, `Normal`, is the alpha-over of a layer stack; the
other five are the standard separable blend operators a retro look reaches for — additive glows,
multiplicative shadows, screen bloom, a halved-average translucency.

Blend is a property of the **container that owns the pixels**, never of a screen-space effect. An effect is
a colour *source*; the region / layer / frame that holds it decides how that source merges.

## Contents

- [The modes](#the-modes)
- [Where blend lives](#where-blend-lives)
- [Scope: what a grade combines over](#scope-what-a-grade-combines-over)
- [Per-sprite blend](#per-sprite-blend)
  - [The container: what a sprite grades against](#the-container-what-a-sprite-grades-against)
- [Per-sprite effects and regions](#per-sprite-effects-and-regions)
- [Per-sprite displacement](#per-sprite-displacement)
- [Custom effects on a sprite](#custom-effects-on-a-sprite)
- [Below-scope sprite effects — the refraction lens](#below-scope-sprite-effects--the-refraction-lens)
  - [A custom shader as a lens](#a-custom-shader-as-a-lens)
  - [Transparency — dialing the lens strength](#transparency--dialing-the-lens-strength)
  - [Confining a lens to a region](#confining-a-lens-to-a-region)
- [Examples](#examples)
  - [Multiplicative exposure — `Multiply` that brightens](#multiplicative-exposure--multiply-that-brightens)
- [The CPU mirror](#the-cpu-mirror)
- [Notes](#notes)
- [Where to change](#where-to-change)
- [See also](#see-also)

## The modes

```cpp
enum class BlendMode : std::uint8_t { Normal, Add, Subtract, Multiply, Screen, Half };
```

For a source colour `src` combining over a destination `dst`, each mode applies a per-channel operator
`B(dst, src)`:

| Mode | `B(dst, src)` | Looks like |
|---|---|---|
| `Normal` | `src` | plain alpha-over (the default; unchanged output) |
| `Add` | `dst + src` | glows, fire, light — brightens |
| `Subtract` | `dst − src` | darkens hard |
| `Multiply` | `dst · src` | shadows, tints — darkens |
| `Screen` | `1 − (1 − dst)(1 − src)` | bloom — lifts toward light |
| `Half` | `(dst + src) / 2` | a halved average — translucency |

The full combine, source-alpha-weighted and clamped, is:

```
out.rgb = clamp( (1 − src.a)·dst.rgb + src.a·B(dst.rgb, src.rgb) )
out.a   = clamp( src.a + dst.a·(1 − src.a) )          // standard over alpha, mode-independent
```

`Normal` reduces to standard alpha-over, so a container left at the default is unchanged. A non-`Normal`
container combines from its own isolated render (a `Sprite`, an effected or blended `DrawLayer`); that render
arrives premultiplied, and the composite un-premultiplies it before evaluating `B`, so `src.rgb` above is the
container's straight colour and `src.a` weights it exactly once — the fill's intended strength at any `alpha`.

## Where blend lives

Each container carries the mode beside its `alpha`:

```cpp
struct Sprite          { /* … */ float alpha; BlendMode blend = BlendMode::Normal; /* … */ };
struct Region          { /* … */ float alpha; BlendMode blend = BlendMode::Normal; };
struct DrawLayer       { /* … */ float alpha; BlendMode blend = BlendMode::Normal; /* … */ };
struct FrameDrawState  { /* … */             BlendMode blend = BlendMode::Normal; /* … */ };
```

- **`Sprite::blend`** — how the sprite's own pixels combine over its container's image (see
  [Per-sprite blend](#per-sprite-blend)).
- **`Region::blend`** — how the region's effects combine over the scene inside its shape.
- **`DrawLayer::blend`** — how the whole layer composites over the layers beneath it.
- **`FrameDrawState::blend`** — how the frame's whole-frame `postEffects` / `regions` combine over the
  composited image.

A frame-level region (in `FrameDrawState::regions`) uses its own `Region::blend`, like any region.

## Scope: what a grade combines over

A whole-container colour grade is a `ColorFill` under a container blend — a `Region` carrying one `ColorFill`,
its `blend` the grade and its `alpha` the strength. A region with **no shape** covers its whole container, so
an empty-shape region on a layer or the frame is the whole-layer / whole-frame colour modifier: day/night, a
tint, a flash, a fade.

Where that grade lands is the effect's `scope` (`ScreenSpaceEffectScope`, on a per-layer effect):

- **`Layer`** — the grade combines over the layer's **own content**, in place. A hole in that layer is a
  destination of nothing, so the grade lands there against an empty pixel rather than against the scene
  beneath. Tint one plane (a sprite flashes, a single layer goes to dusk) without touching the rest of
  the scene.
- **`Below`** — the grade combines over the **accumulated image** at the layer's z: this layer's content and
  everything beneath it, coherently, including *through* the layer's holes. A wash that sits over the world (a
  whole-scene day/night, a screen flash) while layers above the grade's z ride over it untouched.

A frame-level grade (`FrameDrawState::regions` / `postEffects` under `FrameDrawState::blend`) is inherently
whole-frame — it combines over the finished composite, the reach of a `Below` grade at the top of the stack.

The grade math is `applyBlendMode` at both scopes; only the **destination it reads** changes — the layer's own
pixels (`Layer`) or the composited scene (`Below`). So the same fill and mode give, over an opaque pixel:

| Mode | Over a scene pixel `dst` (fill `f`, region alpha `a`) |
|---|---|
| `Normal` | `(1−a)·dst + a·f` — a flash/fade toward `f` |
| `Multiply` | `(1−a)·dst + a·(dst·f)` — a tint / shadow / day-night (`f`>1 via `fillIntensity` brightens) |
| `Add` | `(1−a)·dst + a·clamp(dst+f)` — a glow |
| `Screen` | `(1−a)·dst + a·(1−(1−dst)(1−f))` — bloom |
| `Subtract` | `(1−a)·dst + a·clamp(dst−f)` — a hard darken |
| `Half` | `(1−a)·dst + a·((dst+f)/2)` — a halved wash |

A transparent pixel under a `Layer` grade is left transparent (there is no own-content there to grade).

## Per-sprite blend

A `Sprite` carries `blend` beside its `alpha`, so a single sprite grades against its container's image the
same way a layer or region does — `alpha` is how much the sprite contributes, `blend` is how. A `Multiply`
sprite is a shadow decal that darkens the scene under it; an `Add` sprite is a flare or glow that lifts it;
`Screen` is a soft bloom. The grade uses `applyBlendMode` over the sprite's own opaque pixels, so it lands
on the sprite's silhouette — its transparent texels contribute nothing.

```cpp
// A soft shadow decal under a character, and a bright muzzle flare, in one sprite layer:
Sprite shadow{.key = "shadow", .x = 64, .y = 96, .atlas = decals, .tile = kShadow,
              .palette = greys, .alpha = 0.7f, .blend = BlendMode::Multiply};
Sprite flare {.key = "flare",  .x = 80, .y = 60, .atlas = decals, .tile = kFlare,
              .palette = warm,  .blend = BlendMode::Add};
```

### The container: what a sprite grades against

`blend` grades a sprite against its **compositing container** — the image the sprite layer draws into at
that moment:

- A sprite in an ordinary layer (one that composites straight into the scene) grades against the
  **accumulated scene beneath it**, plus this layer's earlier-`z` sprites already drawn. A `Multiply`
  shadow there darkens the world under the sprite.
- A sprite in a layer that is itself composited in isolation — one carrying its own `DrawLayer::blend` or a
  per-layer effect chain — grades against that **layer's own image**: the within-layer content beneath the
  sprite, and nothing else. A non-Normal sprite over the layer's transparent area has no backdrop to grade
  against, so it contributes nothing there — a shadow needs a surface. Put the sprites that a blended
  sprite should darken or light in the **same layer**, beneath it in `z`.

Discrete like the flips and `z`, `blend` snaps to each submission — it is never interpolated. Ease *toward*
a blend by easing the sprite's `alpha` with a [`Tween`](tween.md), or by resubmitting.

Blend runs of same-mode sprites cost one composite pass per run — the cost scales with how many distinct
blend modes a layer's sprites use in `z` order, never with the sprite count. A layer whose sprites are all
`Normal` (the default) takes the plain instanced draw, unchanged.

## Per-sprite effects and regions

A `Sprite` also carries an effect **chain** and a **regions** list — the same surface a `DrawLayer`, a
`Region`, and the frame carry, applied to one sprite:

```cpp
struct Sprite {
    // … placement, art, alpha, blend, anchors …
    std::vector<ScreenSpaceEffect> effects;  // whole-silhouette colour transforms, in list order
    std::vector<Region>            regions;   // confined effects: a quad-space shape ∩ the silhouette
};
```

Both default empty; a sprite that sets neither composites exactly as a plain sprite. The effect domain is
the **sprite**, not the atlas texture behind it — the art sits in an infinite transparent field, so an
effect lands on the sprite's visible pixels and its transparent texels stay clear.

`effects` transforms the sprite's own pixel in list order: `ColorFill` replaces the colour, `Gleam` adds a
luminance-keyed sheen, `ColorSaturation` drains the colour toward grey, `Bloom` and `Glow` radiate a halo
past the silhouette (through a buffer shared by the layer's glowing sprites, so a field of them costs what
one does; `radius` is in the sprite's own art pixels — `Bloom`'s halo is the art's own light, `Glow`'s the
authored `fill` tint — see [sprites.md](sprites.md#the-effect-carrier--effects--regions)),
`Transparency` makes the whole silhouette see-through, and the displacing pair
`RowDisplacement` / `Ripple` / `Swirl` re-read the art at a displaced within-sprite position (see
[Per-sprite displacement](#per-sprite-displacement)). `regions` then applies, each `Region` grading its
effects over the sprite's pixel by its own `alpha` + `blend`, confined to its `shape` intersected with the
silhouette. A region `shape` is read in the sprite's **quad space** (the pivot / origin / anchor space,
sprite-local pixels) and rides the sprite's transform like the art does; an empty shape covers the whole
silhouette. Region shapes use the polygon path — `circle` / `capsule` / `rectangle` / any polygon, with
`radius` / `strokeWidth` / `inverted()`; a curved sprite-region boundary is not evaluated inline and is
skipped with a warning.

```cpp
Sprite hero{.key = "hero", .x = 60, .y = 72, .atlas = sheet, .tile = kHero, .palette = pal};

// A damage flash over the whole sprite — a white ColorFill region eased in and out via alpha:
Region flash{.key = "flash",  // empty shape ⇒ the whole silhouette
             .effects = {{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{255, 255, 255, 255}}},
             .alpha = damage};  // damage ∈ [0,1], a Tween drives it

// A Multiply shadow tint on the lower half only (quad-space rectangle over y ∈ [8,16] of a 16px sprite):
Region shade{.key = "shade", .shape = ShapePoints::rectangle(Point{0, 8}, 16, 8),
             .effects = {{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{90, 90, 130, 255}}},
             .blend = BlendMode::Multiply};

// A wake glow keyed to the sprite's own brightness:
hero.effects = {{.kind = ScreenSpaceEffectKind::Gleam, .sweep = 0.5f, .width = 0.6f, .gain = 1.5f}};
hero.regions = {flash, shade};
```

`effects` applies before `regions`, matching a layer's effects-then-regions order. Effect parameters are
per-tick data — the interpolator never eases them; drive a flash by easing a value with a [`Tween`](tween.md)
and resubmitting. `fillIntensity > 1` on a `Multiply` region brightens (the intermediates carry the
headroom). Evaluation is inline in the sprite fragment — no added render passes, so the pass count stays
flat in the sprite count. The CPU mirror is `retropp::evalSpriteFxRecords`.

## Per-sprite displacement

`RowDisplacement`, `Ripple` and `Swirl` in a sprite's `effects` chain re-read the sprite's own art at a displaced
within-sprite position — a wavy-water sprite, a heat shimmer, a droplet ring on one sprite. They move **where**
the art is sampled, so they run before the colour effects; a colour effect and a displacing effect in the same
chain compose in one pass.

On a sprite these effects work in the sprite's **own art pixels** — `amplitude` and (for `Ripple` and
`Swirl`) `center` and `radius`
are art px, not the viewport px they mean on a layer, because the read is a re-read of the sprite's art.
(`Swirl`'s `amplitude` is the exception to the unit: it is degrees of turn at every site.) A read
that lands off the art is transparent under the default `DisplacementEdge::Blank` (the layers below show
through) or clamps to the art border under `Stretch`. The sprite's render footprint grows by the displacement
so a displaced crest is never clipped at the static quad, and a displacing sprite renders on the crisp viewport
grid.

```cpp
// A wavy-water sprite — each row shifts horizontally by a sine of its row, animated by advancing phase:
Sprite water{.key = "water", .x = 40, .y = 96, .atlas = sheet, .tile = kWater, .palette = pal};
water.effects = {{.kind = ScreenSpaceEffectKind::RowDisplacement,
                  .amplitude = 3.0f,      // up to 3 art px sideways
                  .frequency = 2.0f,      // 2 wave cycles down the sprite
                  .phase = wavePhase,     // advance per tick to animate; Blank edge ⇒ the pulled-in strip is clear
                  .axis = Axis::Horizontal}};

// A droplet ring centred on a 16px sprite, its rings fading outward:
Sprite pond{.key = "pond", .x = 80, .y = 96, .atlas = sheet, .tile = kPond, .palette = pal};
pond.effects = {{.kind = ScreenSpaceEffectKind::Ripple, .amplitude = 2.0f, .frequency = 3.0f,
                 .phase = ringPhase, .center = Point{8, 8}, .decay = 1.5f}};
```

`phase` is per-tick data like every effect parameter — advance it each tick (or ease it with a
[`Tween`](tween.md)) and resubmit. Multiple displacing effects in one chain compose, and the last one's `edge`
governs an out-of-art read. The CPU mirror is `retropp::spriteDisplacedRead`.

## Custom effects on a sprite

A `ScreenSpaceEffectKind::Custom` chain effect runs a game-registered shader inline on the sprite. Register the
shader by path — the same call a layer or frame custom effect uses — and set its handle on the effect:

```cpp
const PostProcessStageId charge = renderer.registerPostProcessStage("game/shaders/sprite_charge.frag.hlsl");

ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Custom, .customShader = charge};
e.charge = 0.7f;                     // the shader's own params, set inline by name
hero.effects = {e};
```

On a sprite, the shader's `sampleSource(uv)` reads the **sprite's own art** (the transparent field: an off-art
read is transparent under `Blank`, clamps under `Stretch`) — not a composited frame. So a custom sprite effect
transforms the sprite's own pixels, keyed off its art. The shader's params are the same inline named fields a
layer custom effect sets; the sprite draws through the shader's own pipeline, so many custom-effect sprites
still cost one pass.

A custom step reads the raw art through `sampleSource`, so place it **first** in the chain to have the built-in
colour effects ride over its output:

```cpp
hero.effects = {custom, {.kind = ScreenSpaceEffectKind::Gleam, .gain = 1.5f}};  // Gleam sheens the custom result
```

The rules of the sprite path:

- **One custom shader per sprite chain.** The sprite draws through its first Custom effect's pipeline; a second,
  different custom shader in the same chain is skipped (with a log line). The same shader used twice is fine.
- **Whole-silhouette only.** A Custom effect inside a `Sprite::regions` entry is skipped — a custom shader runs
  over the whole silhouette, not confined to a region.
- **Float params, up to 128 bytes.** A shader whose cbuffer carries an `int` / `uint` field, or is declared
  `// @retropp:no-sprite`, has no sprite variant and can't run on a sprite (its layer / Below use is unaffected);
  a sprite carrying it skips the effect.
- **The art read is nearest**, at art-pixel granularity, like the built-in kinds.

## Below-scope sprite effects — the refraction lens

Every sprite effect so far transforms the sprite's OWN pixels — that is `Layer` scope, the default. Set an
effect's `.scope` to `Below` and the sprite becomes a **lens**: the effect distorts or grades the **composited
scene beneath the sprite's layer**, confined to the sprite's silhouette (its art alpha coverage). A Below sprite
draws no art of its own — the art is purely the coverage mask, so its alpha sets the lens strength. It is the
sprite counterpart of a `Below`-scope layer effect.

```cpp
// A refraction lens: the disc's Ripple distorts the SCENE it covers, not the disc's own art.
ScreenSpaceEffect refract{.kind = ScreenSpaceEffectKind::Ripple, .amplitude = 6.0f,
                          .frequency = 14.0f, .center = Point{88, 64}, .decay = 1.0f};  // centre in viewport px
refract.scope = ScreenSpaceEffectScope::Below;   // distort the scene, not the disc's own art

Sprite lens{.key = "lens", .x = 80, .y = 56, .size = AssetDimensions::Snes16x16,
            .atlas = discAtlas, .tile = 0, .palette = maskPalette};   // opaque mask ⇒ full-strength lens
lens.effects = {refract};
```

The rules of the below-scope path:

- **The silhouette is the confinement; the art is the mask.** The art's alpha coverage shapes the lens and
  sets its strength — an opaque mask fully replaces the scene on the silhouette, a partial-alpha mask blends the
  distortion with the original scene. Where the art is transparent, the scene is untouched. The mask's colour is
  unused (the art is not drawn).
- **Displacement amplitude / centre are VIEWPORT px** here (the effect distorts the scene), where a `Layer`-scope
  displacement reads them as the sprite's own **art** px (it re-reads the art). Set a Ripple's `center` to the
  lens's on-screen position.
- **One pass per below-pipeline per layer, not per sprite.** A layer's built-in lenses draw through the
  scene-reading pipeline in one instanced pass; each distinct custom-shader lens is one more pass. All are
  composited over the accumulator before the layer's art draws — N-flat (pass count tracks the pipeline mix,
  never the sprite count).
- **A lens draws no art.** For a sprite that shows art AND lenses the scene, use two sprites. `Layer`-scope
  effects on a lens are skipped (its art does not draw).
- **Every effect kind is first-class at `Below` scope.** `ColorFill`, `Gleam`, `ColorSaturation`, `Bloom`, `Glow`, `RowDisplacement`, `Ripple`, `Swirl`,
  and `Custom` grade or distort the scene whole-silhouette; `Transparency` scales the lens strength (below);
  a `Bloom` / `Glow` lens radiates the SCENE's light through the silhouette, its `radius` in **viewport**
  pixels (like a Below displacement's `amplitude`, unlike a `Layer` glow's art pixels) — see
  [sprites.md](sprites.md#below-scope--the-sprite-as-a-refraction-lens);
  and the colour kinds can be confined to a `Sprite::regions` entry (below). What **cannot** be confined — a
  displacing kind (`RowDisplacement` / `Ripple` / `Swirl`), a `Custom` kind, a `Glow` (its tint occupies the record
  lanes a region's shape needs), or a curve-boundary region — is **skipped
  with a log line** when placed inside a region (it does *not* fall back to running whole-silhouette; only the
  whole-silhouette `effects` chain runs it).
- **A Below-scope displaced scene read clamps at the frame edge** (`sprite_below.frag.hlsl`), smearing the
  border — there is no `Blank` option here, unlike the `DisplacementEdge::Blank` / `Stretch` choice a
  `Layer`-scope sprite displacement has (it re-reads the sprite's own art, so `Blank` can reveal through it).

### A custom shader as a lens

A `Custom` effect at `Below` scope runs a registered game shader over the **scene** through the silhouette:
its `sampleSource(uv)` reads the composited scene beneath the layer (not the sprite's art), so a shader written
as a frame post-process works unchanged as a silhouette-confined lens. This is the same shader and the same
registration as a `Layer`-scope custom effect — only `.scope` differs (the platform builds a scene-reading
variant automatically; a `// @retropp:no-sprite` or int/uint-param shader has none, and the effect is skipped
with a log line).

```cpp
// The SAME shader you'd register for a Layer-scope custom effect — its sampleSource() now reads the scene.
const PostProcessStageId ripple = renderer.registerPostProcessStage("game/shaders/heat_haze.frag.hlsl");

ScreenSpaceEffect haze{.kind = ScreenSpaceEffectKind::Custom, .customShader = ripple};
haze.strength = 0.8f;                        // the shader's own inline param
haze.scope    = ScreenSpaceEffectScope::Below;   // grade the SCENE through the silhouette, not the art

Sprite lens{.key = "haze", .x = 80, .y = 56, .size = AssetDimensions::Snes16x16,
            .atlas = discAtlas, .tile = 0, .palette = maskPalette};
lens.effects = {haze};
```

On the below path `sampleSource(uv)` samples the scene at the fragment's screen position (a displacement the
shader asks for is in viewport px, quantized crisp like every scene read); the shader's output replaces the
scene inside the silhouette and leaves the surround untouched.

### Transparency — dialing the lens strength

A `Transparency` effect at `Below` scope scales the lens's **output alpha** — how strongly the graded scene
replaces the untouched scene. It is the "how much" dial for the rest of the below chain, and it behaves
differently whole-silhouette versus inside a region:

- **Whole-silhouette** it is a binary switch. `TransparentInside` drops the lens strength to zero — the
  silhouette reveals the untouched scene, dialing out a co-resident `ColorFill` / `Gleam` / `ColorSaturation` / `Bloom` / `Glow` grade;
  `TransparentOutside` leaves it at full. A whole-silhouette `Transparency` *alone* is a visual no-op: the
  lens colour IS the scene, so revealing it changes nothing — it earns its keep dialing the OTHER below kinds
  in the chain.
- **Inside a region** it feathers. `stencilCoverage` ramps over the region's `feather` (shape-local px), so a
  circle punches a soft porthole of untouched scene through an otherwise-graded lens.

```cpp
// A cyan haze over the scene, with a soft porthole at the centre that reveals the untouched scene through it.
ScreenSpaceEffect haze{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{70, 200, 230, 255}};
haze.scope = ScreenSpaceEffectScope::Below;

ScreenSpaceEffect reveal{.kind = ScreenSpaceEffectKind::Transparency,
                         .stencil = StencilMode::TransparentInside, .feather = 14.0f};
reveal.scope = ScreenSpaceEffectScope::Below;

lens.effects = {haze};
lens.regions = {Region{.key = "porthole", .shape = ShapePoints::circle({40, 40}, 22), .effects = {reveal}}};
```

### Confining a lens to a region

A `Below`-scope effect inside a `Sprite::regions` entry grades the scene only where the region's shape,
intersected with the silhouette, covers — the below counterpart of a layer region. The shape is read in the
sprite's **quad space** (art-pixel units, like a `Layer`-scope sprite region), so it rides the sprite's
transform with the art. `regions` applies after the whole-silhouette `effects`, in list order. The colour
kinds (`ColorFill`, `Gleam`, `ColorSaturation`, `Bloom`, `Transparency`) confine; a displacing kind (`RowDisplacement` / `Ripple` / `Swirl`), a
`Custom`, or a `Glow` cannot — placed inside a region it is **skipped with a log line** (such an effect only
runs whole-silhouette, through the `effects` chain, never confined to a region shape; a `Glow`'s tint
occupies the record lanes the region's shape needs).

```cpp
// Recolour the scene only inside a centred circle of the lens — the outer ring shows the untouched scene.
ScreenSpaceEffect grade{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{70, 200, 230, 255}};
grade.scope = ScreenSpaceEffectScope::Below;
lens.regions = {Region{.key = "core", .shape = ShapePoints::circle({40, 40}, 22), .effects = {grade}}};
```

## Examples

A translucent layer — averaged with the scene, no per-pixel alpha:

```cpp
DrawLayer glassPane;
glassPane.key = "glassPane";
glassPane.blend = BlendMode::Half;          // (scene + pane) / 2
glassPane.content = /* … */;
frame.layers.push_back(glassPane);
```

An additive glow and a multiply shadow, each a `Region` with a `ColorFill` source:

```cpp
const ScreenSpaceEffect warm{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{90, 60, 20}};
const ScreenSpaceEffect grey{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{120, 120, 130}};

frame.regions.push_back(Region{.shape   = ShapePoints::circle(Point{120, 48}, 26),
                               .effects = {warm},
                               .blend   = BlendMode::Add});       // a glow that brightens the scene

frame.regions.push_back(Region{.shape   = ShapePoints::rectangle(Point{16, 84}, 56, 40),
                               .effects = {grey},
                               .blend   = BlendMode::Multiply});  // a soft shadow that darkens it
```

A whole-frame overlay — a `ColorFill` `postEffect` combined over the composited image with the frame mode:

```cpp
frame.postEffects.push_back(
    ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{34, 44, 78}});
frame.blend = BlendMode::Screen;            // lift the whole scene gently toward light
```

A `ColorFill` is a pure source colour; the owning container's blend decides whether it replaces (`Normal`),
multiplies into a shadow, adds into a glow, or screens into a bloom — one fill colour, every grade. To
**darken** use `Multiply` (`scene · fill`); to **brighten** use `Add` (`scene + fill`), `Screen`, or a
`Multiply` whose fill exceeds 1 (see `fillIntensity` below). This is how whole-frame colour looks are built —
day/night is a Multiply `ColorFill`, a cutscene flash a `Normal` white `ColorFill` at `alpha = strength`, a
fade a `Normal` black one; see [draw-state.md](draw-state.md#whole-frame-colour).

### Multiplicative exposure — `Multiply` that brightens

`Multiply` is `scene · fill`. With a fill in [0,1] it can only darken. `ColorFill::fillIntensity` scales the
fill past 1, so a `Multiply` becomes a multiplicative *exposure* that lifts the scene while preserving its
contrast — what `Add` and `Screen` cannot do (they change the operator). The offscreen pipeline is float16, so
a fill above 1 survives the effect→blend round-trip; the final blit to the screen clamps back into range.

```cpp
// Brighten the whole frame 1.5× — every pixel scaled up, highlights preserved (a daytime/overexposed look):
frame.postEffects.push_back(ScreenSpaceEffect{.kind          = ScreenSpaceEffectKind::ColorFill,
                                              .fill          = Rgba8{255, 255, 255},
                                              .fillIntensity = 1.5f});
frame.blend = BlendMode::Multiply;
```

`fillIntensity` defaults to 1 (the plain fill) and only has visible effect through a headroom-carrying blend
(`Multiply`, `Screen`, `Add`); below 1 it dims the fill, 0 paints black.

## The CPU mirror

`retropp::applyBlendMode(Vec4 dst, Vec4 src, BlendMode)` (in `postprocess.h`) is the `constexpr` authority
the compositor shaders reproduce. It is `static_assert`-anchored and unit-tested, so the blend math is
verifiable without a GPU:

```cpp
// Multiply grades a 50%-grey fill over a scene to a half-strength shadow:
Vec4 shadow = applyBlendMode(scene, Vec4{0.5f, 0.5f, 0.5f, 1.0f}, BlendMode::Multiply);
```

## Notes

- A container left at `BlendMode::Normal` (the default everywhere) renders exactly as it did without the
  mode — the plain alpha-over path.
- Operators clamp to `[0, 1]`: `Add` saturates at white, `Subtract` floors at black.
- Alpha composites with the standard over rule regardless of mode — the blend grades colour, not coverage.
  A partially transparent source contributes proportionally (a hole in a blended layer reveals what is
  beneath).
- Blend never appears on `ScreenSpaceEffect`. To grade an effect, set the blend on the `Region` that owns
  it.

## Where to change

- The modes and their math: `BlendMode` in `draw_state.h`; the operator and combine in
  `retropp::applyBlendMode` / `retropp::blendChannel` (`postprocess.h`). The shaders
  (`blend.frag.hlsl`, `region_select.frag.hlsl`, `region_select_curve.frag.hlsl`) mirror that math —
  change the CPU helper and the shaders together.
- The **per-sprite** blend + effect/region run lives in `sprite.frag.hlsl` (the sprite's own blend and its
  inline `effects`/`regions` evaluation); the **Below-scope** lens (scene-reading, coverage-masked) lives in
  `sprite_below.frag.hlsl`. The CPU mirror of the sprite path is `retropp::evalSpriteFxRecords` /
  `retropp::spriteDisplacedRead`.

## See also

- [draw-state.md](draw-state.md) — the `Region` / `DrawLayer` / `FrameDrawState` containers, `alpha`, and
  the `ColorFill` source colour.
- [rendering.md](rendering.md) — the compositor and the post-process chain blend lands in.
