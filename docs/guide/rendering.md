# Rondo — the renderer

**Rondo** is the platform's renderer. The `Renderer` object: the internal viewport it draws into, how that viewport is scaled and
letterboxed onto the window, the once-per-frame submission entry point, the amortized atlas/palette
uploads, and how shaders reach the GPU. The *content* model a frame carries (layers, tiles, sprites,
colour) is [draw-state.md](draw-state.md) + [tiles-and-colour.md](tiles-and-colour.md); this page is
the object and the output path.

```cpp
#include "retropp/renderer.h"       // Renderer
#include "retropp/viewport.h"       // ViewportResolution
#include "retropp/geometry.h"       // PixelSize, IntRect, integerScaleToFitRect, fitWindowScale
#include "retropp/output.h"         // SamplingMode
```

## Contents

- [The model](#the-model)
- [The internal viewport: `ViewportResolution`](#the-internal-viewport-viewportresolution)
- [Filling the window: `integerScaleToFitRect`](#filling-the-window-integerscaletofitrect)
- [Sampling: `SamplingMode`](#sampling-samplingmode)
- [Evaluation grid: `EvaluationGrid`](#evaluation-grid-evaluationgrid)
- [Per-frame submission: `renderFrame`](#per-frame-submission-renderframe)
- [Offscreen capture: `captureViewport`](#offscreen-capture-captureviewport)
- [Post-process effects: `postEffects`](#post-process-effects-posteffects)
  - [Built-in effect library](#built-in-effect-library)
- [Custom shader stages: register a shader by path](#custom-shader-stages-register-a-shader-by-path)
  - [Emission: a custom stage's own glow](#emission-a-custom-stages-own-glow)
- [Amortized resources: `uploadAtlas` / `uploadPalette`](#amortized-resources-uploadatlas--uploadpalette)
- [Render statistics: `renderStats`](#render-statistics-renderstats)
- [Layer-key collision policy](#layer-key-collision-policy)
- [How shaders reach the GPU](#how-shaders-reach-the-gpu)
- [Where to change things](#where-to-change-things)

## The model

The renderer draws every frame into an **offscreen internal viewport** at a fixed retro resolution
(160×144 by default), then **blits that viewport onto the window** at the largest integer scale that
fits, centred, with letterbox/pillarbox bars filling any leftover. Game content is authored once at
the small native resolution; the renderer always fills whatever window it's given, crisply. This
two-stage path (render small → fill window) is what keeps pixels square at any window size. The
presentation *size* — how big that window is — is the window-scale concern owned by the platform
(`EngineConfig::enhancements.windowScale` + `window().size()`, see
[platform-and-windowing.md](platform-and-windowing.md)); the renderer's only output knob is
**sampling** (nearest/bilinear, below). Post-process display filters (CRT and friends) are a later
stage (planned — see the Coverage table in the [guide index](README.md)).

The renderer is constructed from a live device + window (handed out by `SdlPlatform`) — drawing is
the renderer's job, the platform owns the window/device/input.

```cpp
class Renderer {
public:
    // Settable defaults — EngineConfig::setActive() seeds each from the host config so a bare
    // Renderer{device, window} inherits them (see platform-and-windowing.md).
    static inline ViewportResolution defaultViewport      = ViewportResolution::GameBoyColor;  // 160×144
    static inline SamplingMode       defaultSamplingMode  = SamplingMode::Nearest;             // crisp
    static inline bool               defaultInterpolation = true;                              // smooth motion
    static inline EvaluationGrid     defaultEvaluationGrid = EvaluationGrid::Viewport;          // crisp analytic paths

    Renderer(SDL_GPUDevice* device, SDL_Window* window,
             ViewportResolution viewport = defaultViewport);   // window == nullptr → compose-only

    // Amortized indexed-atlas + palette uploads (see "Amortized resources" below).
    AtlasManifest uploadAtlas(const std::uint8_t* indices, int width, int height,
                              TransparentIndices transparent = TransparentIndices::None);
                              // also uint16_t / uint32_t overloads, and assetSize/kind carve
                              // overloads declaring the sheet's grid (tiles-and-colour.md)
    AtlasManifest uploadAtlas(const LoadedImage&);             // always throws — load PNGs via loadAtlas()
    PaletteId uploadPalette(std::span<const Rgba8>  colors);   // 8-bit source, widened ×257 into the store
    PaletteId uploadPalette(std::span<const Rgba16> colors);   // 16-bit colour source, appended direct
    // PNG loading + slicing (loadAtlas / loadAtlasFromMemory / loadPaletteImage) lives in
    // images-and-transparency.md and tiles-and-colour.md.

    // Bake a cubic / arbitrary curved boundary into an SDF mask a Region samples (see draw-state.md).
    CurveMaskId bakeCurveMask(const Curve& boundary, float padding = 8.0f, int maxResolution = 256);
    ShapePoints bakeCurveRegion(const Curve& boundary, float radius = 0.0f, Transform t = {},
                                float padding = 8.0f, int maxResolution = 256);

    PostProcessStageId registerPostProcessStage(LiteralPath shaderPath);         // custom shader, by .hlsl path (string literal)
    PostProcessStageId registerPostProcessStage(const ShaderVariants& fragment); // lower-level seam the path form resolves to

    void renderFrame(const FrameDrawState& frame);   // composite + present; auto-interpolates (default)

    std::vector<Rgba8> captureViewport(const FrameDrawState& frame);                   // compose offscreen, download pixels (scale 1)
    std::vector<Rgba8> captureViewport(const FrameDrawState& frame, int composeScale); // same, at an explicit compose scale

    void setLayerCollisionPolicy(LayerKeyCollisionPolicy) noexcept;
    LayerKeyCollisionPolicy layerCollisionPolicy() const noexcept;

    void automaticInterpolation(bool enabled) noexcept;          // automatic per-object easing; default on
    bool automaticInterpolation() const noexcept;

    void           evaluationGrid(EvaluationGrid) noexcept;   // analytic-path grid: Viewport / Output
    EvaluationGrid evaluationGrid() const noexcept;

    void         samplingMode(SamplingMode) noexcept;   // blit sampler: Nearest / Bilinear
    SamplingMode samplingMode() const noexcept;
};
```

## The internal viewport: `ViewportResolution`

```cpp
struct ViewportResolution {
    int width = 160, height = 144;

    PixelSize size()   const;   // {width, height} — always integer pixels
    Vec2      center() const;   // {width/2, height/2} — float; a half-pixel on odd dimensions

    static const ViewportResolution GameBoy, GameBoyColor, GameBoyAdvance,
                                    Nes, Snes, SnesHiRes, SnesInterlaced, SnesHiResInterlaced,
                                    Genesis, MasterSystem;
};
```

The internal render resolution defaults to the original Game Boy 160×144 (the faithful default) and
is configurable so a game can request a larger internal viewport — e.g. a wider visible world for a
zoom-out feature — without the platform assuming a fixed size. Common-platform resolutions are named
presets (the self-type-constant idiom shared with `PaletteSize` / `TimingProfile`); a resolution **is**
a `{width, height}` tuple, so a preset and a raw value are interchangeable:

```cpp
Renderer{dev, win, ViewportResolution::Nes};   // a preset (256×240)
Renderer{dev, win, {256, 224}};                // or any raw {width, height}
```

`size()` returns the dimensions as a `PixelSize` — always integer, since a render target is a whole count
of pixels. `center()` returns the geometric middle as a `Vec2`: a float, because an arbitrary odd-width
viewport centers on a half-pixel (a 257-wide viewport at 128.5). Feed it to a sprite's placement to sit
content dead-center on the viewport; even dimensions (every preset) give a whole number.

It is not an exhaustive registry — add platforms as needed. The platform generalizes beyond the Game
Boy, so a fixed resolution baked into the type would be the hardcoded-dimensions mistake the project
avoids elsewhere.

A console whose picture comes in more than one size names each: `Snes` is 256×224, `SnesHiRes` 512×224 —
the picture drawn in half-pixels — `SnesInterlaced` 256×448 — two fields woven — and `SnesHiResInterlaced`
512×448. A viewport that holds the largest shows every size such a cartridge draws exactly, through a
raster layer's `fit` ([draw-state.md](draw-state.md#rastercontent--a-raster-of-pixels)).

## Filling the window: `integerScaleToFitRect`

```cpp
IntRect integerScaleToFitRect(PixelSize drawable, PixelSize viewport) noexcept;  // geometry.h
```

Each frame the renderer reads the window's `drawableSize()` and fills it at the **largest integer
multiple of the viewport that fits**, centred, with the leftover split into letterbox/pillarbox bars
(a pure, GPU-independent, unit-tested helper). It tracks resizes, so the content follows the window
live. This is *fill*, not *size* — the renderer crisply fills whatever window it's handed. Choosing
how big that window is (the presentation scale) is `windowScale` on the platform side; see
[platform-and-windowing.md](platform-and-windowing.md). The two compose: the window is sized to an
integer multiple of the viewport, and the renderer then fills it exactly with no bars.

## Sampling: `SamplingMode`

```cpp
enum class SamplingMode { Nearest, Bilinear };                     // output.h
void         Renderer::samplingMode(SamplingMode) noexcept;     // runtime-dynamic
SamplingMode Renderer::samplingMode() const noexcept;
```

`Nearest` (the default) is point sampling — crisp, square pixels, the faithful look. `Bilinear`
smooths the upscale. This is a blit **sampler** swap, not a shader change: the renderer builds both
samplers up front and binds the one the current mode selects. The tile/atlas path always samples
nearest; only the final viewport→window blit honours the mode. The default (`Nearest`) reproduces the
faithful crisp-pixel output value-for-value; a consumer reads `EngineConfig::enhancements.sampling` and calls
the setter, then toggles it live from a settings menu.

**Native fullscreen** is a `Platform`-seam concern, not a renderer one — see
[platform-and-windowing.md](platform-and-windowing.md). The fill blit absorbs the fullscreen target
size with no renderer change. **High-DPI** is automatic: the window opts into
`SDL_WINDOW_HIGH_PIXEL_DENSITY`, `drawableSize()` reports physical pixels, and the fill picks the
larger integer scale so the art renders crisp at native resolution.

## Evaluation grid: `EvaluationGrid`

```cpp
enum class EvaluationGrid { Viewport, Output };                       // output.h
void           Renderer::evaluationGrid(EvaluationGrid) noexcept;  // runtime-dynamic
EvaluationGrid Renderer::evaluationGrid() const noexcept;
```

`Viewport` (the default) evaluates every analytic render path on the viewport grid — transformed tile
layers, the effect regions (select / stencil, in polygon / analytic-curve / baked-mask form), the
sampling effects (`RowDisplacement` / `Ripple` / `Swirl`), **geometrically-transformed sprites** (`Sprite::transform`
/ `DrawLayer::transform`), **a `Bloom` or `Glow` halo read from a field** — a region `Bloom`'s, and a Below
lens's (the field is held per viewport pixel and read back through the grid, so the glow quantizes to whole
viewport pixels along with the content it grades), and
**custom shader stages** (automatically — see
[Custom shader stages](#custom-shader-stages-register-a-shader-by-path)). The image is then
pixel-identical to the viewport-resolution rasterization,
nearest-upscaled: crisp, square pixels — even when placement composites onto a finer grid for steady
motion. `Output` evaluates each path per output pixel instead, so edges and displacement resolve smoothly
at the higher resolution (softer under upscale). The setting is a mathematical no-op when the compositor
runs at viewport resolution — it only bites once compositing runs above it.

A transformed sprite resolves its coverage per viewport cell: the rotated / scaled / foreshortened
silhouette AND the internal texel stairs land on the viewport grid, so a spun sprite reads as chunky
blocks that upscale cleanly rather than resampled edges. An untransformed sprite is unaffected either way
— its texels are already solid blocks and its sub-pixel placement is the steady-motion mechanism.

Evaluation granularity is independent of **placement** granularity: object placement stays sub-pixel on
either setting (the steady-motion mechanism), so `EvaluationGrid` chooses only where the geometry is
*sampled*, not where whole objects *land*. Seed it from `EngineConfig::evaluationGrid` (which `setActive()`
fans into `Renderer::defaultEvaluationGrid`), then flip it live from a settings menu.

## Per-frame submission: `renderFrame`

```cpp
void renderFrame(const FrameDrawState& frame);
```

Call this once per render callback. The game hands a whole `FrameDrawState` (the Z-sorted layer
stack — see [draw-state.md](draw-state.md)); the renderer composites the layers back-to-front into
the offscreen viewport — applying any **per-layer screen-space effects** (`DrawLayer::effects`,
`Layer` / `Below` scope) as it goes — then runs the **frame-level post-process chain** (`postEffects`,
below) and the frame's shape-confined `regions`, and blits the viewport integer-scaled + letterboxed
onto the swapchain and presents. There is **no mid-frame state-change API** — a frame is computed whole
and submitted whole, every frame. A frame with no per-layer effects composites in a single pass — the
per-layer path is paid for only where used.

**Ordering.** Layers stack by `DrawLayer::z` (unique within the frame, ascending, back-to-front).
*Within* one sprite layer, sprites stack by `Sprite::z` — non-unique, ascending, equal values keeping
submission order (a stable sort) — so one layer holds an articulated creature or a Y-sorted crowd
without a layer per rank. See
[draw-state.md](draw-state.md#spritecontent--sprite--placed-sprites) for the field and
[anchors-and-articulation.md](anchors-and-articulation.md) for the articulated use.

**Automatic interpolation.** With interpolation on (the default),
the renderer eases each layer and sprite between its previous and current simulation-tick state by the
run loop's sub-tick factor, matching each object to its prior tick by its `key` (see
[draw-state.md](draw-state.md)); motion stays smooth when the display refreshes faster than the
simulation ticks. The game submits its latest state each frame and the platform blends.
`automaticInterpolation(false)` (or `EngineConfig::interpolation = false`) turns it off, and each submission
composites verbatim — the game then owns any blending itself. The full model, including the
developer-owned path, is in [run-loop-and-timing.md](run-loop-and-timing.md#interpolation).

## Offscreen capture: `captureViewport`

```cpp
std::vector<Rgba8> captureViewport(const FrameDrawState& frame);
```

`captureViewport` composites `frame` exactly as `renderFrame` does — the copy pass, the layer
composite, the post-process chain — but **downloads** the finished viewport as packed `Rgba8` instead
of blitting it to the window. The result is `viewport.width × viewport.height` pixels, row-major, top
row first; the bytes are what `renderFrame` would have shown. Use it for a thumbnail, a screenshot, a
server-side render, or an image-diff test.

It works on any renderer, **including a windowless one**. A `Renderer` built with `nullptr` for the
window is *compose-only*: it builds the device-side compose resources but no blit pipeline and never
acquires a swapchain, so it composites and captures but cannot present.

```cpp
Renderer offscreen{device, /*window=*/nullptr, ViewportResolution::GameBoyColor};
std::vector<Rgba8> pixels = offscreen.captureViewport(frame);   // composes on the GPU, no window
```

A compose-only renderer needs a GPU **device** but no display, so it runs on a headless machine. The
call **blocks** until the download lands (a fence wait); it is a capture, not part of the per-frame
loop, so keep it off the hot path.

## Post-process effects: `postEffects`

After compositing, the renderer runs `FrameDrawState::postEffects` — a list of screen-space effects
applied to the **whole composited viewport** before the blit. Each effect is one full-viewport pass;
the renderer ping-pongs two internal scratch targets so any number of effects chain in submission
order. An **empty list is the faithful path** — the blit samples the composited viewport
directly. To **confine** an effect to a shape, put it in a `Region` (which owns the shape and the
effect) on `FrameDrawState::regions` or `DrawLayer::regions` — the renderer gates it with an extra
select pass that leaves the rest of the image untouched; see
[draw-state.md](draw-state.md#confining-an-effect-to-a-shape-region).

By default these effects and their region gates evaluate on the **viewport grid** (crisp — the result is
pixel-identical to the viewport-resolution rasterization, nearest-upscaled); switch to per-output-pixel
(smooth) evaluation with `EvaluationGrid::Output` — see [Evaluation grid](#evaluation-grid-evaluationgrid).

### Built-in effect library

The platform ships a growing set of **built-in effects** behind `ScreenSpaceEffectKind` — you name the
kind and set parameters; the platform owns the shader (no registration, no shader authoring). Shipped:

- **`RowDisplacement`** — wavy water / heat haze / per-line scroll (axis-aligned), with a developer-
  selectable frame-edge (`Blank` default / `Stretch`).
- **`Ripple`** — a radial concentric ripple (a water droplet): rings expand outward from a `center`,
  faded with radius by `decay`. Aspect-corrected so the rings stay circular.
- **`Swirl`** — an angular twist about a `center` (a whirlpool): content within `radius` rotates, the
  full turn at the centre easing smoothly to nothing at the rim, content outside the disc untouched.
  Ripple's angular sibling — Ripple pushes the sample along the radius, Swirl carries it around. The turn
  is `amplitude` + `phase` in **degrees** (advance `phase` to spin it); positive turns the content
  clockwise, matching `Transform::rotation`. A zero turn or a zero `radius` is an exact identity.
- **`ColorFill`** — paint a colour (`Rgba8 fill`, scaled by `float fillIntensity`) into the effect's
  region: `out.rgb = fill · fillIntensity`. A filled region is a solid shape, a stroked region a drawn
  colored line/path; a `Region`'s `alpha` makes its fill translucent. (Writes over existing pixels — see
  [draw-state.md](draw-state.md#painting-a-colour-into-a-region-colorfill) for the opaque-source scopes.)
- **`Transparency`** — make the effect's region **see-through** (reveal what's behind it) instead of
  painting a colour — the subtractive sibling of `ColorFill`. `stencil` (`TransparentInside` default /
  `TransparentOutside`) chooses which side of the shape clears; `feather` softens the edge. The `stencil()`
  free helper is its shorthand (see [draw-state.md](draw-state.md)).
- **`Gleam`** — a luminance-keyed diagonal **sheen sweep** (a marquee "shine"): `sweep` slides the band
  across, `width` sets its extent, `slant` its diagonal angle, and `gain` its strength (`gain = 0` is an
  exact identity). Bright content catches the light; dark stays dark.
- **`ColorSaturation`** — a cross-channel **colour drain**: pull each pixel toward its own luminance. The
  `saturation` field is a `uint8` — `255` is full colour (the exact identity), `0` is greyscale, values
  between desaturate partway. A mood/pause grade a `ColorFill` cannot express (it works per-channel toward a
  solid colour; this pulls every channel toward the pixel's own brightness).
- **`Bloom`** — a threshold-blur-add **glow**: pixels brighter than `threshold` (a `uint8` luminance
  floor; `0` blooms everything) blur outward by `radius` (a Gaussian blur, in the site's own pixels)
  and add back over the source scaled by `intensity` (a `uint8`; `0` is the exact identity default,
  `255` full strength) — bright content radiates its OWN light as a soft halo. On a sprite the radius is
  in the sprite's own art pixels, scaled by the placement's transform.
- **`Glow`** — an **authored-colour aura**, Bloom's sibling: the content radiates a colour YOU pick
  (`fill` × `fillIntensity`, per channel; `fillIntensity > 1` is an HDR-hot aura), not its own light —
  the halo's chroma is the tint, never the source hue, so dark art radiates gold or ember at full
  strength. `radius` / `intensity` behave as Bloom's (`intensity` or `radius` at `0` is the exact
  identity); `threshold` is the emission floor — `0` makes the WHOLE silhouette emit (dark pixels
  included), higher keys the emission on brightness like Bloom's floor. On a sprite it is chain-only
  (whole-silhouette or a Below lens; a sprite-region `Glow` is skipped with a warning — layer and frame
  regions confine it fully).

**`radius` means something different once a halo is confined.** Unconfined — a frame or layer `postEffects`
entry, or a sprite's own chain — the halo spills past the content that emits it, and `radius` is how far it
reaches: widen it and the glow grows. Confined — inside a `Region`, or through a sprite's silhouette at
`Below` scope — everything outside the shape is clipped away and never drawn, so the light cannot leave. The
blur still spreads a fixed amount of light over a larger area, and the part that lands outside is discarded
rather than shown, so **widening `radius` there softens and dims the halo instead of growing it.**

Author it accordingly: inside a shape, `radius` is a softness control and `intensity` is the strength one.
Reach for `intensity` when a confined halo should read stronger, and expect a wide `radius` to give a faint,
diffuse interior light. This is what a bloom becomes when it cannot spill — the spill is the part `radius`
normally buys, and a confined halo does not have it.

**What a halo costs.** `Bloom` and `Glow` are the only built-ins that read a neighbourhood, and both
realize as a fixed chain of cheap passes rather than one expensive gather: the emission is extracted (or,
on sprites, rasterized) into a scratch buffer, reduced when the radius is wide, blurred separately on each
axis, and added back. The consequence is that **radius is nearly free** — a reach of 20 costs about what a
reach of 2 does. Author the radius the scene wants.

On sprites the blur work tracks the **distinct reaches** a layer authors rather than its sprite count: every
halo at one radius blurs together in a single pass, however many sprites or lenses carry it and however far
apart they sit. Two radii cost two passes.

Each halo on a sprite occupies a field sized to that sprite's own drawn footprint, packed with the layer's
others into a shared atlas, so a halo costs the area it covers rather than a screenful. Every site runs the
chain — frame, layer, region, sprite chain, sprite `Below` lens, and both `Layer`- and `Below`-scope sprite
regions; none gathers.

A game's own `Custom` stage reaches this same chain by declaration: mark it an emission consumer and it
gets an extracted, blurred field of its own, read back through `sampleEmission()`, at the same cost — no
per-pixel gather. See [Emission: a custom stage's own glow](#emission-a-custom-stages-own-glow) below.

You build one with plain designated-init — set `.kind` and the fields that kind consults; every field is
settable inline, nothing is hidden:

```cpp
frame.postEffects.push_back(ScreenSpaceEffect{
    .kind = ScreenSpaceEffectKind::Ripple, .amplitude = 6.0f, .frequency = 6.0f,
    .phase = t * 0.012f, .center = {80, 72}, .decay = 2.5f});
frame.postEffects.push_back(ScreenSpaceEffect{
    .kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 3.0f, .frequency = 3.0f,
    .phase = t * 0.01f, .axis = Axis::Horizontal});
```

`center` is in **viewport pixels** (the platform normalizes to UV); advance `phase` slowly off your frame
counter to animate (slow expansion — no strobing). The effect type, its scopes (frame-level here vs.
per-layer), the edge choice, and which fields each kind consults are documented in
[draw-state.md](draw-state.md#screen-space-effects). These are *content* effects declared on the
draw state — distinct from output-side display filters (CRT/scanlines), a separate planned stage (below).

## Custom shader stages: register a shader by path

When the built-in effect vocabulary stops, a game writes its **own fragment shader** and uses it as a
first-class screen-space effect. Registration is just the shader's **path**:

```cpp
auto stage = renderer.registerPostProcessStage("game/shaders/my_effect.frag.hlsl");
```

That is the whole thing — **no `ShaderVariants`, no uniform type, no generated-header include, no CMake
rule.** A build-time source scan sees that `.hlsl` path referenced in your code, compiles it to this
platform's GPU bytecode, embeds it in the executable, and registers it under the path string; the call
resolves the path against the embedded registry at load time and builds the pipeline pair.

> **The path must be a string literal.** Because the scan reads it out of your source verbatim, a path
> passed as a variable, a `std::string`, or a computed/concatenated value is invisible to it. The
> parameter type enforces this: a non-literal is a **compile error**, not a runtime surprise. Write the
> literal directly at the call — `registerPostProcessStage("game/shaders/my_effect.frag.hlsl")`.

Your shader declares its **own parameters** in a cbuffer — its own names — and writes only `main()`; the
platform supplies the source texture and sampler:

```hlsl
// game/shaders/my_effect.frag.hlsl — you write only the cbuffer + main()
cbuffer Params : register(b1, space3) {
    float2 center;
    float  strength;
};
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return sampleSource(uv + (center - uv) * strength);  // sampleSource() honours the effect's edge policy
}
```

The build **reflects that cbuffer** and surfaces its fields on `ScreenSpaceEffect` by name, so you set them
inline — exactly like a built-in's named params, with **no uniform struct, no `as_bytes`, no size**:

```cpp
frame.postEffects.push_back(ScreenSpaceEffect{
    .kind = ScreenSpaceEffectKind::Custom, .customShader = stage,
    .center = {0.5f, 0.5f}, .strength = 0.2f});   // your shader's OWN params, mutated live per frame
```

It composes with the built-ins in submission order, at either attachment point (`postEffects` or
`DrawLayer::effects`), and is region-gateable — wherever a built-in works, a custom shader does too.

**The fragment contract.** The game supplies a *fragment only* — the platform's shared fullscreen-triangle
`postprocess.vert` is the vertex stage. The platform supplies **`sampleSource(uv)`** (the
composited image, or the prior chain pass) plus a platform cbuffer (`b0`, `space3`). Your shader adds its own
parameter cbuffer at **`b1`, `space3`**, which the build reflects into the effect's inline fields and fills
per frame from them. **Always sample through `sampleSource()`** — it obeys the effect's **edge policy**
(`ScreenSpaceEffect::edge`): `Blank` (the default) returns transparent outside `[0,1]` so an effect that
displaces past the frame edge reveals the backdrop / layers below; `Stretch` clamps (smears) the border.
The edge behaviour is the **layer/effect's** choice, not the shader's — sampling `SourceTexture` directly
opts out and always clamps. **No runtime shader compiler** — the bytecode is built and embedded; nothing is
loaded from disk at run time. Handles live until the renderer is destroyed.

**Crisp automatically.** A custom shader written to this contract — spatial math on the `uv` your
`main()` receives, sampling through `sampleSource()` — is crisp on the default
[`EvaluationGrid::Viewport`](#evaluation-grid-evaluationgrid) with **no shader-side work**. The platform's
generated entry point hands your `main()` the centre of the viewport cell its fragment falls in, so your
math evaluates once per viewport pixel; `sampleSource()` quantizes the displacement you request to whole
viewport pixels; `paramRowAtUv()` lands on the row the viewport-resolution rasterization reads. The
result upscales as solid square pixels even when the compositor runs at output resolution for steady
motion. `EvaluationGrid::Output` hands `main()` the raw output-resolution uv and samples exactly what you
request — smooth evaluation, wholesale.

**Build wiring.** The platform's own examples get the source scan automatically. A standalone game applies it **once
per target** — `retropp_autocompile_shaders(<target>)` after defining the target — and then never touches
CMake again, however many shaders it adds; re-run CMake after adding a *new* path reference (the scan is a
configure-time read of your sources). The custom path is for the long tail the built-in library doesn't
cover — effects that *do* have a use case are built-ins that need none of this. The `custom_stage_test`
exercises the reflection + packing device-free.

### Emission: a custom stage's own glow

`Bloom` and `Glow` are cheap because the platform extracts the emission into a scratch field, blurs it once,
and hands it back — rather than gathering a neighbourhood per pixel ([What a halo costs](#built-in-effect-library)).
A custom stage joins that same chain with **one declaration line** near the top of its `.hlsl`:

```hlsl
// @retropp:emission
```

That marks the stage an **emission consumer**: the platform extracts a field for it, blurs the field by the
effect's `.radius`, and hands it back to `main()` through `sampleEmission(uv)`. The reach is *declared* — it
rides the submission — never computed in the shader. Demand rides the same `.radius` / `.threshold` a built-in
`Bloom` / `Glow` reads, so there are **no new effect fields and no API call**:

```cpp
frame.postEffects.push_back(ScreenSpaceEffect{
    .kind = ScreenSpaceEffectKind::Custom, .customShader = stage,
    .radius = 15.0f, .threshold = 200});   // the reach, and the stock brightpass floor
```

There are two ways to fill the field:

- **Stock brightpass — declaration only.** With no `emission()` body, the platform fills the field with the
  built-in brightpass at `.threshold` (`threshold` 0 = the whole content emits). That is a `Bloom` of the
  source's own bright light, obtained in one line:

  ```hlsl
  // @retropp:emission
  float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
      return sampleSource(uv) + sampleEmission(uv);   // the source plus its own bloomed light
  }
  ```

- **Authored — an `emission()` body.** Define `float4 emission(float2 uv)` beside `main()` and it authors the
  field itself, so it can emit a signal the luminance brightpass cannot single out. The platform runs the body
  in the extract pass, reading the scene through the same `sampleSource()` the stock extract has:

  ```hlsl
  // @retropp:emission
  float4 emission(float2 uv) {
      float3 c = sampleSource(uv).rgb;
      float  e = saturate(c.b - max(c.r, c.g));   // a blue-dominant mask a luminance key would miss
      return float4(e, e, e, e);
  }
  float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
      return sampleSource(uv) + sampleEmission(uv);   // the source plus its authored glow
  }
  ```

`sampleEmission(uv)` takes the same `uv` `sampleSource(uv)` does, and the stage works at **every site a
built-in `Glow` / `Bloom` does** — a frame `postEffect`, a `DrawLayer::effects` (either scope), a region, a
sprite effect chain, and a `Below`-scope sprite lens. On the **Layer-scope sprite chain** there is no
composited scene to hand an `emission()` body, so the field content there is the sprite's own art brightpass
at `.threshold`; a body is used at the scene-facing sites — the frame chain and the below lens.
`examples/custom_emission/` shows both fill modes, at a frame stage and on a below lens.

Two things follow from the chain being the platform's, not the shader's. An emission stage has **no gather
variant**, so declaring both `// @retropp:emission` and `// @retropp:additive` on one shader is a build error
(an additive delta and a retained-scratch consumer are different contracts). And the blur is **obtained by
declaration, never computed** — the platform cannot verify a hand-rolled blur any more than it can verify the
additive promise, so declaring the stage an emission consumer is the whole surface.

**Fast path for many additive regions — one declaration line.** When a custom shader is used across *many*
region-confined effects on one frame (e.g. a glow per pickup, a light per particle), the naive cost is two
full-frame passes per region — a serialized cliff as the count grows. If your shader's output is its source
**plus a term that does not depend on the source's contents** — `out = sampleSource(uv) + D(uv)` (a bloom, a
light, an additive tint) — declare it additive with a single comment line near the top of the `.hlsl`:

```hlsl
// @retropp:additive
```

That is the **entire** opt-in — no API call, no `Region` field, no build rule. The platform compiles a second
variant and collapses all eligible same-shader regions into **one** instanced additive pass (pass count
becomes independent of region count). It engages automatically for a region on `Normal` blend at full
`alpha` whose shape is a circle or capsule; any other region silently keeps the per-region path with
identical output, so you never structure your scene around it. The declaration is a *promise*: a shader that
multiplies the source, samples its neighbours, or otherwise depends on the source contents will render
incorrectly on the fast path — the platform cannot verify additivity, which is exactly why the safe (per-
region) path is the default and the fast path is the thing you vouch for. Built-in additive effects need no
declaration (the platform already knows their math). An additive glow that tints and adds without reading
what is beneath it is the worked case.

**Fast path for many source-dependent regions — automatic.** The additive declaration above only covers
effects whose output does *not* read the source's contents. A shader that *does* read the source — a
displacement, a refraction, a hue rotation of the sampled image (`out = sampleSource(uv + duv)`) — has no
source-independent term to extract, so it cannot use the additive path. For these, the platform collapses many
same-shader region-confined effects into **one union-shape pass** that reads the previous image once and, per
pixel, applies the effect with the winning region's own params (each region's inline params ride the pass, so
they may differ — a warp whose amplitude tapers per segment gathers fine). This engages **automatically** — a
custom stage can only see `sampleSource` and its own params, so the gathered result is always well-defined;
you write **nothing**. It applies to a region on `Normal` blend at full `alpha` whose shape is a circle or
capsule; any other region keeps the per-region path. The worked case is Ferryman's mutant reality-warp
wake — many trailing warp circles on one layer, now one pass regardless of count.

Two things to know about the semantics:

- **Where same-shader regions overlap, the effect applies once with the last (topmost) region's params** —
  not once per overlapping region compounded. The union of the shapes reads as a single region; displaced
  samples always read the pristine previous image, never a neighbouring region's output. For most animated
  content this is invisible, and it is the more predictable behaviour; but if you *want* an effect to compound
  where two regions overlap (`eff(eff(src))`), opt out:

  ```hlsl
  // @retropp:no-gather
  ```

  which keeps every region on the sequential per-region path.

- **The same effect chained twice on one region also gathers to a single application.** Intentional
  re-application of one shader needs the `no-gather` line (or a second shader file).

**Fast path for many `ColorFill` regions — automatic, and wider.** Drawing UI as `ColorFill` regions —
panels, swatches, meters, whole-frame grades — is the built-in case of the same cliff, and it collapses
the same way: a contiguous run of ColorFill-confined regions renders as **one** pass regardless of count.
Nothing to declare, and unlike the custom fast paths there are no shape or blend carve-outs to design
around: any `Region::alpha`, any blend mode, inverted regions, stroked outlines, transformed shapes, and
polygons all take the fast path (only a curve-boundary shape keeps the per-region path). The output is
pixel-identical to the sequential per-region result — overlapping grades still compound, and translucent
fills still stack, because the pass composites every covering region in submission order rather than
picking a winner. A lone ColorFill region keeps the per-region path at identical cost. The worked case is
`examples/input_probe/` — its ~30 solid-rectangle UI regions render as one pass per frame.

## Amortized resources: `uploadAtlas` / `uploadPalette`

Pixel art and colour are uploaded **once** (at load time / on change), not per frame; the draw state
then references them by handle each frame. See [tiles-and-colour.md](tiles-and-colour.md) for the
colour model and [images-and-transparency.md](images-and-transparency.md) for loading art from PNG.

- `uploadAtlas(indices, w, h, transparent = TransparentIndices::None)` → `AtlasManifest` (the sheet;
  take the handle explicitly via `sheet.atlasId`). An **indexed** atlas: one palette index per pixel (not RGBA).
  Every upload declares the sheet's carve — this form carves `Single`; the `assetSize`/`kind`
  overloads declare a grid (see [tiles-and-colour.md](tiles-and-colour.md)). The optional
  `TransparentIndices` set declares that source's structural index holes (default `None` = every
  index draws) — see [images-and-transparency.md](images-and-transparency.md).
- `uploadPalette(colors)` → `PaletteId`. One palette's colours, written to a row of the renderer-owned
  palette store.

Each upload writes only what it adds, so uploading during play costs the new colours or pixels rather
than everything already in the store; the store texture is recreated only when the content outgrows the
room it has.

Handles stay valid until the renderer is destroyed (there is no per-handle eviction yet).

## The renderer only uploads what changed <a id="upload-skip"></a>

You describe the whole frame every time. The renderer does **not** send the whole frame to the GPU
every time — it compares what you submitted against what it last uploaded and skips the transfer when
nothing moved. This happens at three levels, all automatic:

- **Tile layers.** The packed cells are hashed each frame; an unchanged map costs no DMA. A huge map
  can skip even the hash by answering the question itself with `TileContent::contentChanged` — see
  [tilemaps.md](tilemaps.md#mixing-sheets-in-one-layer).
- **Sprite layers.** The built sprite records are hashed the same way, so a layer of stationary
  sprites re-uploads nothing. Sprites that actually move upload every frame, by correctness.
- **The whole frame.** When a submission is bit-identical to the last one and every interpolated
  value has settled, the composite is not re-run at all — the retained output is re-blitted instead.
  An idle screen composes zero times per second while still presenting at the refresh rate.

Each layer is tracked by its `key`, which is why the key is required and why it must be stable across
frames: it is the identity the comparison is keyed on. A layer whose key changes every frame looks
like a new layer each time and cannot be skipped.

**The consequence worth knowing:** whether you rebuild your `FrameDrawState` from scratch every frame
or retain it and mutate what changed, the GPU traffic is the same, because the decision to upload is
made from the content rather than from how you assembled it. That choice is about how you prefer to
write your game — see [Retained vs rebuilt frame state](how-to.md#retained-vs-rebuilt-frame) — not
about render cost. What it does not cover is the CPU work *you* do assembling the frame; rebuilding a
very large cell array every frame still costs you the rebuild, whatever the renderer then decides.

`renderStats()` reports how often each skip fired, which is how you confirm it rather than assume it.

## Render statistics: `renderStats`

`renderStats()` returns a by-value `RenderStats` — cumulative counters of how much work the renderer
issued versus skipped as redundant, since construction. Read two snapshots and diff them to measure a
window (one frame, or a run of frames):

```cpp
const RenderStats before = renderer.renderStats();
// ... run a while ...
const RenderStats now = renderer.renderStats();
const auto composed = now.composePasses - before.composePasses;
const auto skipped  = now.composeSkips  - before.composeSkips;   // idle screen -> composed 0, skipped ~refresh rate
```

| Field | Meaning |
|---|---|
| `tilemapUploads` / `tilemapSkips` | per-layer tilemap uploads issued / skipped (cell content unchanged) |
| `spriteUploads` / `spriteSkips`   | per-layer sprite-record uploads issued / skipped (built records unchanged) |
| `composePasses` / `composeSkips`  | full-frame composes run / skipped (frame bit-identical, retained output re-blitted) |
| `presentPasses` / `presentSkips`  | `renderFrame` calls that blitted a swapchain texture / had none to blit |
| `lastFrame`                       | the most recent frame's phase split (below) |

Each skip counter reads against its issued counterpart — the pair is the ratio. On a still screen the
skip counters climb while the issued counters hold steady; under motion they invert. `composePasses`
counts both `renderFrame` and `captureViewport` composes; only `renderFrame` skips — `captureViewport`
always composes. A headless renderer (no window) presents nothing, so `presentSkips` counts every frame.

### Where a frame's time went — `lastFrame`

`lastFrame` is a `RenderStats::PhaseTimings`: the CPU milliseconds the most recent `renderFrame` spent,
split by phase, plus two flags describing what that frame did.

| Field | Meaning |
|---|---|
| `acquireMs` | acquiring the command buffer |
| `interpMs` | interpolation — reconciling the submission and easing it |
| `composeMs` | composing the viewport image; `0` when the frame skipped it |
| `presentMs` | swapchain acquire, blit and submit |
| `presented` | this frame reached the screen |
| `composeSkipped` | the retained output was re-blitted instead of recomposed |

Read it alongside the counters: a phase that grows while its counter holds steady is doing more work per
unit rather than more units of work.

**Count presented frames, not render calls.** On Metal the swapchain is acquired non-blocking, so when the
GPU falls behind the render callback keeps firing at full rate while presents are silently skipped. A frame
rate computed from callbacks reports 60 for a scene the eye sees as frozen. Diff `presentPasses` for the
true rate and read the callback rate beside it — callbacks far above presents means the GPU is behind,
both falling together means a CPU or callback stall.

```cpp
const RenderStats a = renderer.renderStats();
// ... one second of frames ...
const RenderStats b = renderer.renderStats();
const auto fps = b.presentPasses - a.presentPasses;         // what the viewer actually saw
const auto missed = b.presentSkips - a.presentSkips;
if (b.lastFrame.composeMs > 8.0) { /* the image is expensive to draw */ }
```

## Layer-key collision policy

A frame's layers must have unique `z` and unique `key` (see [draw-state.md](draw-state.md)). The
renderer validates this each frame and reacts per a policy you can override:

```cpp
renderer.setLayerCollisionPolicy(LayerKeyCollisionPolicy::Throw);          // fail fast (dev default)
renderer.setLayerCollisionPolicy(LayerKeyCollisionPolicy::WarnAndResolve); // keep a shipped game up
```

The default is `Throw` in debug builds (a collision surfaces the instant its frame runs) and
`WarnAndResolve` in release (a deterministic order is still produced, the game stays up).

## How shaders reach the GPU

The platform authors its shaders once in HLSL and **compiles them to the running platform's native
format at build time** — metallib on macOS (Metal), SPIR-V on Linux (Vulkan), DXIL on Windows (D3D12) —
using that platform's standard tools (`glslang` / `spirv-cross` + the Metal toolchain / `dxc`). Nothing is cross-compiled
and no bytecode is committed; a clone builds its own shaders. At runtime the renderer picks the
variant the live device accepts:

```cpp
std::optional<std::pair<ShaderBytecode, SDL_GPUShaderFormat>>
selectShader(SDL_GPUShaderFormat supported, const ShaderVariants& variants) noexcept;
```

A device never reports a format its backend can't run, and a variant not generated on this platform
is a null entry `selectShader` treats as absent — so the renderer code is identical on every
platform. This is internal plumbing; a consumer never calls it. The shader source and the build-time
generator live under `shaders/` (see `shaders/README.md`); the build-time tools are dependencies of
*building the platform*, never of the shipped binary.

### Two directories of `.hlsli`, and which one is yours

`shaders/` holds shared shader text in two places, working in opposite directions:

- **`shaders/include/`** — the **preambles the build prepends** to a custom shader you register. This is
  what hands your shader `sampleSource()` and the platform cbuffer without you declaring anything. You never
  `#include` these; they are already at the top of your translation unit. See "Custom shader stages" above.
- **`shaders/common/`** — the **platform's own shared kernels**, reached by a real `#include` from
  `shaders/src/`. Only this directory is on the HLSL include search path, which is what stops a custom
  shader from `#include`-ing a declaration it has already been handed.

A custom shader stage neither includes nor is included by anything in `shaders/common/` — the plumbing it
gets comes from `shaders/include/` as a prepended preamble, not an `#include`. That includes the emission
grammar: a `// @retropp:emission` stage receives `sampleEmission()` the same way (see
[Emission: a custom stage's own glow](#emission-a-custom-stages-own-glow)), while the multi-pass blur behind
it is the platform's, run before the stage's own fragment. The layout, and the rule for headers that read
shader-declared state, are in [`shaders/README.md`](../../shaders/README.md).

## Where to change things

- **Internal render resolution:** the `Renderer` constructor's `ViewportResolution` (or
  `EngineConfig::viewport`).
- **A new shader / shader edit:** edit the HLSL under `shaders/src/` — the next build regenerates the
  affected per-platform header automatically (`shaders/README.md`).
- **Sampling (crisp vs smoothed):** `Renderer::samplingMode` (`Nearest` / `Bilinear`), or seed it
  from `EngineConfig::enhancements.sampling`.
- **Analytic-path evaluation (crisp vs smooth edges/displacement):** `Renderer::evaluationGrid`
  (`Viewport` / `Output`), or seed it from `EngineConfig::evaluationGrid` — see "Evaluation grid" above.
- **Window size / presentation scale:** that's `windowScale` + `window().size()` /
  `window().fullscreen()`, on the platform side ([platform-and-windowing.md](platform-and-windowing.md)) —
  the renderer always fills whatever window it's given.
- **Screen-space content effects (wavy water, heat haze):** `FrameDrawState::postEffects` for the
  whole frame, or `DrawLayer::effects` (`Layer` / `Below` scope) for a single layer / everything below a
  layer — see `postEffects` above and [draw-state.md](draw-state.md#screen-space-effects).
- **Scale / rotate / skew / perspective a layer (Mode-7-style floors):** `DrawLayer::transform` (a
  `Transform`) + `DrawLayer::transformEdge` — see [draw-state.md](draw-state.md#transforms).
- **Transform a single sprite (spin/scale/foreshorten about its own pivot):** `Sprite::transform`,
  composing with the layer transform — see [draw-state.md](draw-state.md#per-sprite-transforms).
- **Capture a frame offscreen (thumbnail, screenshot, headless render):** `Renderer::captureViewport`
  on a windowless (`nullptr`-window) renderer — composes on the GPU and downloads pixels, no display
  needed; see "Offscreen capture" above.
- **Your own shader effect (the built-ins don't cover it):** `registerPostProcessStage` + a
  `Custom`-kind effect — see "Custom shader stages" above.
- **Post-process display filters (CRT, scanlines):** author them as a `Custom` stage today (a
  full-frame fragment over the composited image), or wait for a planned built-in stage on the
  same chain machinery; the fill + sampling above is the faithful default it builds on.
