# Engine shaders

The engine's GPU shaders. Authored once in HLSL; **compiled by every build, on every
platform, to that platform's native format** as an ordinary build step. No bytecode is
committed and nothing is cross-built — a developer cloning the repo builds their own
shaders with their platform's standard tools.

## Layout

```
shaders/
├── src/                       ← HLSL source (the human-authored truth)
│   ├── blit.vert.hlsl         ← fullscreen-triangle blit (viewport → swapchain)
│   ├── blit.frag.hlsl         ← samples the viewport texture
│   ├── tile.vert.hlsl         ← fullscreen-triangle over the viewport (tile layer)
│   ├── tile.frag.hlsl         ← tilemap index → atlas sample (the compositor's tile path)
│   ├── sprite.vert.hlsl       ← instanced per-sprite quad from a sprite storage buffer
│   ├── sprite.frag.hlsl       ← indexed atlas → transparency discard → palette (the sprite path)
│   ├── postprocess.vert.hlsl  ← shared fullscreen-triangle vertex for every post-process stage
│   ├── displace.frag.hlsl     ← row-displacement built-in effect (wavy water / heat haze)
│   ├── ripple.frag.hlsl       ← radial-ripple built-in effect (a water droplet)
│   └── region_select.frag.hlsl← region gate: confine any effect to a shape (inside ? eff : src)
├── include/                   ← preambles PREPENDED to a game's custom fragment; never on the -I path
│   ├── retropp_effect.hlsli   ← frame/layer/region stage (source texture + sampler)
│   ├── retropp_sprite_effect.hlsli       ← sprite-chain stage
│   └── retropp_sprite_below_effect.hlsli ← sprite Below-lens stage
├── common/                    ← the engine's own shared kernels, reached by #include from src/
│   ├── blend_ops.hlsli        ← blendOp: the separable BlendMode operator
│   ├── curve_sdf.hlsli        ← quadratic-Bezier boundary distance (reads uSegs)
│   ├── polygon_sdf.hlsli      ← polygon boundary distance (reads uPoints)
│   ├── emission_mask.hlsli    ← glowMask: the emission keying function
│   ├── emission_field.hlsli   ← sampleEmissionField (reads uEmissionRects)
│   ├── rounding.hlsli         ← roundHalfUp: the CPU mirror's tie rule
│   ├── sprite_color.hlsli     ← applyGleam, applySaturation
│   ├── sprite_blend.hlsli     ← blendChannel, applyBlendMode
│   ├── sprite_stencil.hlsli   ← stencil coverage + the inline-polygon distance
│   ├── sprite_displace.hlsli  ← art-space RowDisplacement / Ripple / Swirl re-reads
│   └── sprite_art_sample.hlsli← the sprite context + retroppSpriteArtSample
├── gen_shader.cmake           ← build-time generator: HLSL → this platform's bytecode → header
├── gen_effect_fields.cmake    ← reflects each custom shader's cbuffer → ScreenSpaceEffect params + packers
└── README.md                  ← this file
```

The generator is wrapped by a CMake function `retropp_generate_shader(STEM … SRC … OUT … [HEADERS_VAR …])`
(root `CMakeLists.txt`), which the engine calls for its own stems above. Each generated header exposes one
ready-to-use `retropp::shaders::<stem>` `ShaderVariants` constant (the renderer binds it directly).

A consuming game does **not** call the generator directly. It registers a **custom shader stage by path** —
`renderer.registerPostProcessStage("game/shaders/foo.frag.hlsl")` — and the CMake function
`retropp_autocompile_shaders(<target>)` (applied once per consumer target) scans the target's sources for
those `.hlsl` paths, compiles each through the generator, and registers it under the path. The game's
shader declares its **own** parameter cbuffer; a second generator (`gen_effect_fields.cmake`) reflects it
and surfaces those fields on `ScreenSpaceEffect` by name (plus a packer that fills the cbuffer), so the
game sets its shader's params **inline** — no uniform struct. See
[`docs/guide/rendering.md`](../docs/guide/rendering.md) "Custom shader stages"; the engine's
`custom_shader_demo` registers three custom shaders this way.

Generated headers land in the **build tree** (`<build>/generated-shaders/shaders/generated/`),
never in the repo. The renderer includes them by the `shaders/generated/<stem>.h` path.

### Two directories of `.hlsli`

`include/` and `common/` hold shared shader text and work in opposite directions:

- **`include/`** — preambles the generator **prepends** to a game's custom fragment, which is what hands
  that shader `sampleSource()` and the engine cbuffer. A game never `#include`s them; they are already at
  the top of its translation unit.
- **`common/`** — the engine's own shared kernels, reached by a real `#include` from `src/`. Only this
  directory is on the `-I` path of the HLSL frontends (`glslang`, `dxc`).

Keeping `include/` off the search path is what stops a game shader from `#include`-ing a declaration it has
already been handed, which would be a redefinition.

Every header in `common/` is a build dependency of every generated shader, so editing one recompiles them
all. A header that reads shader-declared state — a cbuffer or a texture — names that requirement at its top
and is included **after** the shader's declarations rather than at the top of the file.

## Per-platform generation

CMake compiles each shader to the one format this platform's `SDL_GPU` backend runs,
at build time, with the platform's native tools:

| Platform | Format | Backend | Tools (build-time only) |
|---|---|---|---|
| macOS | metallib | Metal | `glslang` + `spirv-cross` + Metal toolchain — `brew install glslang spirv-cross`; metallib via `xcrun metal`/`metallib` |
| Linux | SPIR-V | Vulkan | `glslang` — `apt install glslang-tools` |
| Windows | DXIL | Direct3D 12 | `dxc` — ships with the Windows SDK |

The generator is a pure CMake script (`cmake -P`) — no Python or other scripting
runtime; CMake is the one tool every build already requires. A missing shader tool
fails the CMake configure with the install hint. The tools are build-time dependencies
only — the shipped binary embeds the bytecode and needs nothing.

Each generated header exposes the same six symbols regardless of platform — `kSpirv` /
`kDxil` / `kMetallib` byte arrays plus a per-format entrypoint constant. The formats not
built on this platform are `nullptr` constants; `shader_format.h`'s `selectShader`
treats null data as absent, and a device never reports a format its backend doesn't
run, so the renderer code is identical on every platform.

Editing a `src/*.hlsl` (or `gen_shader.cmake`) regenerates the affected headers on the
next build automatically — they are ordinary build dependencies.

### MSL binding fidelity

`gen_shader.cmake` passes `--msl-decoration-binding` to spirv-cross so Metal
`[[texture(n)]]` / `[[buffer(n)]]` indices come from the HLSL register numbers —
the same slot order the renderer binds resources in. Without it, spirv-cross
allocates indices in variable-ID order, which can silently swap same-set resources
(observed: `tile.frag`'s atlas and tilemap swapping slots).

## Resource bindings (SDL_GPU convention)

Authored per the `SDL_CreateGPUShader` docs so the per-platform compilers map them
correctly across backends:

- Vertex inputs use `TEXCOORD0`, `TEXCOORD1`, … semantics; system values use `SV_*`.
- A fragment shader's sampled texture + sampler live in register `space2`
  (`register(t0, space2)` / `register(s0, space2)`).
- A fragment shader's read-only **storage** textures continue the `t` register file in
  `space2` *after* the sampled textures (sampled first, then storage).
- A fragment shader's **uniform** buffers live in `space3` (`register(b0, space3)`).

## The tile shaders

`tile.vert.hlsl` emits the same fullscreen triangle as the blit (it covers the offscreen viewport
target); `tile.frag.hlsl` turns each output pixel into a layer-local pixel, applies the layer scroll and
the optional per-layer transform, wraps into the tilemap per the layer's wrap mode, `Load`s + unpacks the
**two-word** cell, resolves the cell's atlas handle to its region in the flat store via the global
atlas-region table, `Load`s the palette **index** from the indexed atlas, applies transparency, and
`Load`s the final color from the palette store. The tile path is **all integer `Load` — no sampler**.
Its bindings:

| Resource | Register | Kind | Notes |
|---|---|---|---|
| flat atlas store | `t0, space2` | read-only storage texture | `R32_UINT`, integer `Load`; every uploaded sheet stacked in one store |
| tilemap cells | `t1, space2` | read-only storage texture | `R32G32_UINT`, integer `Load`; the `packTileCell` two-word layout |
| palette store | `t2, space2` | read-only storage texture | a UNORM color texture `Load`ed as `float4`; FLAT colors wrapped W wide → texel `(flat%W, flat/W)` |
| atlas-region table | `t3, space2` | read-only storage texture | `R32G32B32A32_UINT`, one texel per `AtlasId` → `(storeY, cols, transpMaskLo, transpMaskHi)` |
| `TileUniforms` | `b0, space3` | uniform buffer | scroll, sizes, alpha, palette-store width, plus the inverse transform rows + a control word |

`num_samplers = 0`, `num_storage_textures = 4` for the tile fragment shader; they bind in one
`SDL_BindGPUFragmentStorageTextures` call.

The two-word cell the shader unpacks — word 0 `[tile:16][flipX:1][flipY:1]`, word 1
`[atlas:16][palette:16]` — mirrors `retropp::packTileCell` / `unpackTileCell` exactly (the unit-tested
reference). The atlas handle indexes the global atlas-region table; the palette handle **is** the flat
offset into the palette store, read directly with no per-layer set. There is no priority bit; depth is
layer `z` alone. The wrap math (`floorModF`) mirrors `retropp::sampleTilemap`; the shader wraps in float
with `floor()` because HLSL integer `%` is undefined for negative operands across backends.

**Transparency is two independent layers, both per-pixel in the shader.** *Structural*: the sheet's
transparent-index set is a 64-bit bitmask carried in the atlas-region texel's `.z` (indices 0–31) and `.w`
(32–63); a pixel whose palette index is a member is `discard`ed — a hole the lower layer shows through.
*Material*: a palette entry whose alpha is 0 is also `discard`ed. The empty index set (the default) and
opaque entries discard nothing, so a faithful opaque background draws every pixel. An index ≥ 64 is
expressible only via alpha. The per-pixel `ScreenSpaceEffect` compositing happens in the post-process
stages, not here.

## The sprite shaders

`sprite.vert.hlsl` is an **instanced per-sprite quad** — 6 `SV_VertexID` values trace a unit quad's two
triangles, one instance (`SV_InstanceID`) per sprite. There is **no vertex buffer**: each sprite's record
is read from a read-only **storage buffer**. The record already holds the quad in **clip space** — the
whole transform chain (scroll subtraction, the per-sprite and per-layer transforms, viewport scale,
top-left-origin V-flip) is baked CPU-side in `retropp::makeGpuSprite` — so the vertex stage carries **no
uniform buffer**, just the one storage buffer. `sprite.frag.hlsl` turns the interpolated within-sprite UV
into a pixel, flips it per the sprite's flags, resolves the sprite's own atlas handle to its store region
via the global atlas-region table, addresses the indexed atlas at the sprite's top-left cell origin + that
pixel (a `w×h` sprite reads a contiguous `w×h` atlas rectangle — a 16×16 sprite spans a 2×2 cell block),
`Load`s the palette **index**, applies the **same two-layer transparency as the tile path** (the sheet's
transparent-index set, then a fully-transparent palette entry), and `Load`s the color from the palette
store. All integer `Load`, no sampler.

| Resource | Register | Kind | Notes |
|---|---|---|---|
| sprite records | `t0, space0` | read-only storage buffer (vertex) | `StructuredBuffer<GpuSprite>`; one 64-byte record per sprite, indexed by `SV_InstanceID` |
| flat atlas store | `t0, space2` | read-only storage texture (fragment) | `R32_UINT`, integer `Load`; one palette index per pixel |
| palette store | `t1, space2` | read-only storage texture (fragment) | a UNORM color texture `Load`ed as `float4`; FLAT colors wrapped W wide → texel `(flat%W, flat/W)` |
| atlas-region table | `t2, space2` | read-only storage texture (fragment) | `R32G32B32A32_UINT`, one texel per `AtlasId` → `(storeY, cols, transpMaskLo, transpMaskHi)` |
| `SpriteFragUniforms` | `b0, space3` | uniform buffer (fragment) | tile px + layer alpha + palette-store width; 16 bytes |

Vertex stage: `num_storage_buffers = 1`, `num_uniform_buffers = 0`. Fragment stage: `num_samplers = 0`,
`num_storage_textures = 3`, `num_uniform_buffers = 1`. A sprite names its own sheet and palette directly
in its record — `atlasPalette` packs the atlas handle (low 16 bits, indexing the atlas-region table) and
the palette flat offset (high 16 bits) — so one sprite layer mixes sheets and palettes with no per-layer
set. The record mirrors `retropp::GpuSprite` / `makeGpuSprite` / `packSpriteFlags` exactly (the
unit-tested reference). `flags` is `flipX | flipY`; there is no priority bit; depth is layer `z` alone.

> **Why the vertex stage has no uniform buffer.** SDL_GPU's Metal backend places a stage's storage
> buffers at `[[buffer]]` indices *offset past* its uniform buffers (`SDL_gpu_metal.m`), but the
> single-pass HLSL→SPIR-V→MSL generator (`--msl-decoration-binding`) maps both a `t0`-space0 storage
> buffer and a `b0`-space1 uniform buffer to `[[buffer(0)]]` — Metal rejects the duplicate slot and
> shader creation fails. A vertex stage with both a storage buffer and a uniform buffer can't be
> expressed for Metal and Vulkan simultaneously through this toolchain. Baking the screen→clip
> transform into each sprite record leaves the vertex stage with a single buffer, sidestepping the
> collision. A genuinely multi-buffer stage would need a richer MSL remap (or `SDL_shadercross`)
> than the current generator provides.

The tile and sprite pipelines share the offscreen viewport target and the same alpha blend, so
TILES and SPRITES layers interleave correctly by `z` in the single viewport pass — the compositor
binds the tile or sprite pipeline per layer by content kind.
