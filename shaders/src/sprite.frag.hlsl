// Sprite layer fragment shader (indexed atlas + runtime palettes + OBJ transparency + inline effects).
//
// Per output pixel: turn the within-sprite QUAD coordinate into a within-sprite pixel, flip it per the
// sprite's flags, resolve the sprite's own atlas to its region in the flat store via the global
// atlas-region table, address the indexed atlas at the sprite's top-left cell origin + that pixel (a
// w×h sprite reads a contiguous w×h atlas rectangle — a 16×16 sprite spans a 2×2 cell block), Load the
// palette INDEX, DISCARD it if the sheet's transparent-index set marks it a hole (structural
// transparency), Load the color from the palette store at the sprite's palette offset, DISCARD a
// fully-transparent palette entry (material transparency). Then run the sprite's inline effect run
// (fxOffset/fxCount into the per-frame sprite-effect store): the chain effects transform the pixel color
// in list order, the region effects grade over it where their quad-space shape covers, and a Transparency
// step scales the pixel's alpha. Finally scale by the layer alpha × the sprite's own alpha. Every art,
// palette and record read is an integer Load; the one filtered read here is a region Bloom's halo, stored per
// viewport pixel and sampled back onto the compose grid (see sampleEmissionField).
//
// A displacing chain effect (RowDisplacement / Ripple) instead moves WHERE the art is read: a pre-pass over
// the sprite's chain records composes the displaced within-sprite coordinate (in the sprite's own art pixels)
// BEFORE the atlas read, and a read that lands off the art is transparent (the default Blank edge) or clamps
// to the border (Stretch). The sprite renders through the analytic branch and its footprint is inflated by
// the displacement bound (retropp::makeGpuSprite) so a displaced crest is never clipped at the static quad.
//
// A Bloom or Glow CHAIN effect is not evaluated here at all: the sprite rasterizes its emission through the
// sprite-emission pipeline into the shared emission buffer, which is blurred and composited over the layer
// once per (kind, reach) bucket. Such a step passes through this chain untouched, and steps after it grade
// the art alone. A Bloom inside a REGION does resolve here — it is a graded source under the region's blend
// and alpha, not an additive halo — but it READS its halo from a field of that sprite's own light the
// renderer prepares before this pass. No neighborhood is summed anywhere in this shader.
//
// SDL_GPU HLSL conventions (see SDL_CreateGPUShader docs): sampled textures are numbered before storage
// textures, so the one sampled texture takes t0/s0 in space2 and the five read-only storage textures follow
// at t1..t5; the uniform buffer is b0 in space3.
//   - t0 space2 : the emission atlas — one footprint-sized rect per realized region-Bloom step, SAMPLED
//   - s0 space2 : its sampler (linear, CLAMP) — the halo is stored per viewport pixel and filtered back up
//   - t1 space2 : flat ATLAS STORE (R32_UINT; integer Load; all sheets stacked vertically)
//   - t2 space2 : palette store (RGBA8; integer Load; FLAT colors wrapped W wide → texel (flat%W, flat/W))
//   - t3 space2 : global atlas-region table (R32G32B32A32_UINT; texel x = AtlasId → (place = column | row << 16, cols, transpMaskLo, transpMaskHi))
//   - t4 space2 : per-frame sprite-effect records (R32G32B32A32_FLOAT; ten 16-byte texels per record, one record per
//                 ROW at y = fxOffset + i; a storage TEXTURE, not a storage buffer — the buffer namespace after a
//                 uniform collides with Metal's [[buffer]] indices when the texture and uniform counts differ)
//   - t5 space2 : the rect side-table (R32G32B32A32_FLOAT; two texels per field, one field per row)
//   - b0 space3 : per-layer fragment uniforms (tile px + layer alpha + palette-store width + compose scale +
//                 the evaluation grid)
//
// Sprites front-composite by layer z — depth is layer order only. Each sprite names its OWN sheet, so
// one sprite layer mixes sheets: the sheet's region is looked up per-sprite from the global table.

// The frame's finished emission fields — SHEETS of an atlas, one array layer each, holding a rect per
// realized region-Bloom step sized to that sprite's own footprint in VIEWPORT pixels, so a field's memory does
// not grow with the window. A layer needing more fields than one sheet holds takes another sheet, so no field
// count is a ceiling. A step reads the field its record's params.w names, through the side-table below. A
// layer authoring no region Bloom binds a 1×1 stand-in and a one-row table, so both bindings are
// unconditional and every generated variant shares one layout. It is a SAMPLED texture because the field is a
// viewport-resolution image of a blurred signal, filtered back onto the compose grid.
Texture2DArray<float4> uEmissionAtlas   : register(t0, space2);
SamplerState           uEmissionSampler : register(s0, space2);   // linear, CLAMP
Texture2D<uint>   uAtlas        : register(t1, space2);
Texture2D<float4> uPaletteStore : register(t2, space2);
Texture2D<uint4>  uAtlasRegions : register(t3, space2);
// Per-frame sprite-effect records — mirrors retropp::SpriteFxRecord as ten RGBA32F texels per record row:
//   0 head (kind, flags, blend, pointCount as float-valued ints) | 1 gate (alpha, radius, strokeWidth, _) |
//   2 params | 3..5 shape transform inverse rows | 6..9 up to eight quad-space vertices (x0,y0,x1,y1,…).
// A displacing chain step (kind RowDisplacement/Ripple) reuses these lanes: params = (amplitude, frequency,
// phase, axis); a Ripple also reads its center from gate.yz (art px) and its decay from gate.w. flags bit2
// (kSpriteFxEdgeStretch) picks the out-of-art edge (set = clamp, clear = transparent). A Bloom step's
// params = (radius art px, threshold, intensity, field index) — the index names the emission field a REGION
// Bloom reads, through the rect table; a chain Bloom resolves outside this shader and reads none of these
// lanes here.
Texture2D<float4> uFxStore      : register(t4, space2);
// Where each field sits: two texels per field, one field per ROW (mirrors retropp::EmissionRectEntry).
//   texel 0 : (offsetX, offsetY, innerX, innerY) — viewport position + offset = atlas position
//   texel 1 : (innerW, innerH, sheet, 0)         — the content box a read is held inside, and which sheet
Texture2D<float4> uEmissionRects : register(t5, space2);

cbuffer SpriteFragUniforms : register(b0, space3) {
    float uTilePx;          // register 0: tile edge length, pixels (8)
    float uAlpha;           // layer alpha, [0,1]
    float uPaletteStoreW;   // palette-store row width (colors); flat offset → (f%W, f/W)
    float uComposeScale;    // compose grid ÷ viewport (1 = faithful); output pixel → viewport
    float uEvalSnap;        // register 1: 1 = Viewport grid (snap the field read to the cell center), 0 = Output
};

// Steps per texel the emission read quantizes its sample point to — mirrors retropp::kEmissionSampleSteps.
static const float kEmissionSampleSteps = 256.0f;

// ── Effect math (mirrors the retropp:: CPU authorities in postprocess.h) ──────────────────

#include "sprite_blend.hlsli"  // blendChannel, applyBlendMode — the scalar BlendMode operators

#include "sprite_color.hlsli"  // applyGleam, applySaturation — the per-pixel color operators

#include "sprite_stencil.hlsli"  // stencilCoverage, stencilSurvival, spriteRegionSignedDistance

// ── Displacement re-read ───────────────────────────────────────────────────────────────────
#include "rounding.hlsli"  // roundHalfUp — the CPU mirror's tie rule

#include "sprite_displace.hlsli"  // snapArt, spriteDisplace, spriteRipple, spriteSwirl — art-space re-reads

#include "sprite_art_sample.hlsli"  // the sprite context + retroppSpriteArtSample — reads the atlas resources above

// ── Bloom glow, region scope only (the field read) ─────────────────────────────────────────
//
// A region-confined Bloom grades the pixel with its own bloomed value under the region's blend and alpha —
// it is a source color, not an additive halo. So it cannot ride a SHARED emission field, whose content is
// the sum of a bucket's sprites: it needs this sprite's own light. It gets a field of its own instead. The
// renderer rasters that sprite's emission, blurs it separably, and leaves the halo in one layer of the field
// array; the step reads it here at this fragment's own texel. A whole-silhouette Bloom or Glow never comes
// through this branch at all — it composites over the layer after the art draws.
//
// The field carries the step's INTENSITY already (the raster applied it), so the add below takes 1. It is
// neutral in the layer and per-sprite alpha, which this fragment applies to the composed pixel at the end.

// Add the glow over a straight-rgba running pixel: convert to premultiplied, add the intensity-scaled
// glow, lift the coverage, return straight rgba (mirrors retropp::applyBloomAdd across the straight ↔
// premultiplied boundary). A pixel that gains no glow returns unchanged.
float4 spriteBloomApply(float4 c, float4 glow, float intensity) {
    float3 pmRgb = c.rgb * c.a + intensity * glow.rgb;
    float  a     = saturate(c.a + intensity * glow.a * (1.0f - c.a));
    return float4(a > 0.0f ? pmRgb / a : float3(0.0f, 0.0f, 0.0f), a);
}

#include "emission_field.hlsli"  // sampleEmissionField — mirror of retropp::emissionFieldSamplePoint

// ── Custom effect hook ─────────────────────────────────────────────────────────────────────
//
// A Custom chain step runs a game-registered shader inline. The base sprite pipeline carries no game
// shader, so the step is a no-op here (returns the running color unchanged). A generated sprite-custom
// variant (gen_shader.cmake SPRITE mode) replaces the marker below with the game body + its sampleSource
// (over retroppSpriteArtSample) + a record-lane param loader, and #defines RETROPP_SPRITE_CUSTOM so the
// real retroppSpriteCustom wins. `c` is the running pixel color, `uv` the within-sprite quad coordinate,
// `ri` the step's record row in the sprite-effect store.
// An emission-consumer sprite-custom variant reads its own blurred silhouette back through sampleEmission(),
// mapping the body's within-sprite quad uv to the field's compose position — which needs this fragment's
// placement (inv0..inv2). Such a variant defines RETROPP_SPRITE_CUSTOM_EMISSION and takes the wider signature;
// every other variant (and the base pipeline) keeps the three-argument form, so their bytecode is unchanged.
// @retropp:sprite-custom-hook
#ifndef RETROPP_SPRITE_CUSTOM
float4 retroppSpriteCustom(float4 c, float2 uv, int ri) { return c; }
#endif

float4 main(float2 spriteUV : TEXCOORD0,
            nointerpolation uint tile         : TEXCOORD1,
            nointerpolation uint atlasPalette : TEXCOORD2,
            nointerpolation uint flags        : TEXCOORD3,
            nointerpolation uint packedSize   : TEXCOORD4,
            nointerpolation float3 inv0       : TEXCOORD5,
            nointerpolation float3 inv1       : TEXCOORD6,
            nointerpolation float3 inv2       : TEXCOORD7,
            nointerpolation float  spriteAlpha : TEXCOORD8,
            nointerpolation uint fxOffset     : TEXCOORD9,
            nointerpolation uint fxCount      : TEXCOORD10,
            float4 pos : SV_Position) : SV_Target0 {
    int2 sz = int2((int)(packedSize >> 16), (int)(packedSize & 0xFFFFu));  // pixel (width, height)

    float2 fdims = float2(sz);

    // The within-sprite QUAD coordinate (fxUv) — the effect + region-gate space, pre-orientation. A sprite on
    // the analytic flag (bit 4: a transformed sprite on the Viewport grid, or ANY sprite that displaces its own
    // art) resolves coverage per VIEWPORT cell: reconstruct this fragment's viewport-space position from
    // SV_Position (pos.xy is the output-pixel center; ÷ uComposeScale → viewport space), snap to the cell
    // center, map that through the screen→unit inverse (perspective divide; behind the projection ⇒ discard).
    // fxUv may land outside the true [0,1)² quad when the footprint is inflated (a displaced crest); the
    // out-of-quad discard happens after the displacement pre-pass. The CPU mirror is retropp::sampleSpriteCell.
    // An untransformed, non-displacing sprite takes the plain source-driven spriteUV path.
    bool analytic = (flags & 16u) != 0u;
    float2 fxUv;
    if (analytic) {
        float2 c  = floor(pos.xy / uComposeScale) + 0.5f;      // viewport-cell center
        float  cw = inv2.x * c.x + inv2.y * c.y + inv2.z;
        if (cw <= 0.0f) discard;                               // behind the projection
        fxUv = float2((inv0.x * c.x + inv0.y * c.y + inv0.z) / cw,
                      (inv1.x * c.x + inv1.y * c.y + inv1.z) / cw);
    } else {
        fxUv = spriteUV;
    }

    // Displacement pre-pass — compose the chain's displacing effects (RowDisplacement / Ripple) into the
    // within-sprite READ coordinate before the art is read (mirrors retropp::spriteDisplacedRead). readUv ==
    // fxUv for a non-displacing sprite. The last displacing effect's edge governs an out-of-art read. The
    // whole scan is gated by the has-displacement flag (bit 5) so a plain or color-only sprite never pays for
    // it — its read stays at the plain coordinate.
    float2 readUv = fxUv;
    bool   hasDisp = false;
    uint   dispEdge = 0u;   // 0 = Blank (out-of-art ⇒ transparent), 1 = Stretch (clamp to the border)
    if ((flags & 32u) != 0u) {
        [loop]
        for (uint di = 0u; di < fxCount; di++) {
            int    dri  = int(fxOffset + di);
            float4 dh   = uFxStore.Load(int3(0, dri, 0));   // head: kind, flags
            uint   dk   = (uint)dh.x;
            uint   dfl  = (uint)dh.y;
            if ((dfl & 1u) != 0u) continue;                 // a region step is never a displacing re-read
            if (dk == 1u || dk == 2u || dk == 10u) {        // RowDisplacement / Ripple / Swirl
                float4 dp = uFxStore.Load(int3(2, dri, 0)); // params (amplitude, frequency, phase, axis)
                if (dk == 1u) {
                    readUv = spriteDisplace(readUv, dp, fdims);
                } else if (dk == 2u) {
                    float4 dg = uFxStore.Load(int3(1, dri, 0));  // gate carries the Ripple center + decay
                    readUv = spriteRipple(readUv, dp, dg, fdims);
                } else {
                    float4 dg = uFxStore.Load(int3(1, dri, 0));  // gate carries the Swirl center
                    readUv = spriteSwirl(readUv, dp, dg, fdims);
                }
                hasDisp  = true;
                dispEdge = ((dfl & 4u) != 0u) ? 1u : 0u;    // kSpriteFxEdgeStretch
            }
        }
    }

    // A non-displacing analytic sprite covers only its true quad — discard a fragment outside it. A
    // displacing sprite's edge is instead handled by the art read's transparent field below (off-art under
    // Blank ⇒ transparent ⇒ the material discard fires; under Stretch the read clamps to the border). The
    // plain (non-analytic) path's rasterizer already clamps spriteUV to the quad.
    if (!hasDisp && analytic) {
        if (fxUv.x < 0.0f || fxUv.x >= 1.0f || fxUv.y < 0.0f || fxUv.y >= 1.0f) discard;
    }

    // Publish the per-fragment sprite context, then read the sprite's own art at the (displaced) coordinate.
    gSpriteTile         = tile;
    gSpriteAtlasPalette = atlasPalette;
    gSpriteFlags        = flags;
    gSpritePackedSize   = packedSize;
    gSpriteTilePx       = uTilePx;
    gSpritePaletteW     = uPaletteStoreW;
    float4 colour = retroppSpriteArtSample(readUv, hasDisp && dispEdge == 1u);
    if (colour.a == 0.0f) discard;  // structural / material transparency: a hole

    // Inline effect run — the sprite's flattened effects chain then its regions (mirrors
    // retropp::evalSpriteFxRecords). Empty (fxCount 0) is the byte-identical no-effect path. Each record is
    // ten RGBA32F texels on row (fxOffset + i); the head texel's fields are float-valued ints.
    float4 c = colour;   // straight rgba (c.a = palette alpha)
    [loop]
    for (uint i = 0u; i < fxCount; i++) {
        int    ri     = int(fxOffset + i);
        float4 head   = uFxStore.Load(int3(0, ri, 0));   // kind, flags, blend, pointCount
        float4 gate   = uFxStore.Load(int3(1, ri, 0));   // alpha, radius, strokeWidth, _
        float4 params = uFxStore.Load(int3(2, ri, 0));
        uint kind     = (uint)head.x;
        uint flags    = (uint)head.y;
        bool isRegion = (flags & 1u) != 0u;      // kSpriteFxIsRegion
        if (!isRegion) {                          // whole-silhouette chain step
            if (kind == 5u) {                     // ColorFill — replace rgb with the resolved fill
                c.rgb = params.xyz;
            } else if (kind == 6u) {              // Gleam — keyed sheen over the pixel
                c.rgb = applyGleam(c.rgb, fxUv.x, fxUv.y, params);
            } else if (kind == 7u) {              // ColorSaturation — desaturate toward the pixel's luma
                c.rgb = applySaturation(c.rgb, params.x);
            } else if (kind == 4u) {              // Transparency — whole silhouette see-through
                float surv = stencilSurvival((uint)params.x, 1.0f);
                if (surv <= 0.0f) discard;
                c.a *= surv;
            } else if (kind == 3u) {              // Custom — a game shader inlined by the sprite-custom variant
#ifdef RETROPP_SPRITE_CUSTOM_EMISSION
                c = retroppSpriteCustom(c, fxUv, ri, inv0, inv1, inv2);  // emission variant reads its own field
#else
                c = retroppSpriteCustom(c, fxUv, ri);   // no-op on the base pipeline (returns c)
#endif
            }
            continue;                             // None / displacing / Bloom / Glow: no color transform here
                                                  // (displacement already moved the read in the pre-pass; a
                                                  // halo rasters through the sprite-emission pipeline)
        }
        // Region step: gate on the quad-space shape (perspective-correct inverse), ∩ silhouette is implicit.
        float4 iv0 = uFxStore.Load(int3(3, ri, 0));
        float4 iv1 = uFxStore.Load(int3(4, ri, 0));
        float4 iv2 = uFxStore.Load(int3(5, ri, 0));
        float4 p01 = uFxStore.Load(int3(6, ri, 0));
        float4 p23 = uFxStore.Load(int3(7, ri, 0));
        float4 p45 = uFxStore.Load(int3(8, ri, 0));
        float4 p67 = uFxStore.Load(int3(9, ri, 0));
        float2 v[8] = { p01.xy, p01.zw, p23.xy, p23.zw, p45.xy, p45.zw, p67.xy, p67.zw };
        float2 q   = fxUv * float2(sz);
        float  wgt = iv2.x * q.x + iv2.y * q.y + iv2.z;
        float2 loc = float2(iv0.x * q.x + iv0.y * q.y + iv0.z,
                            iv1.x * q.x + iv1.y * q.y + iv1.z) / wgt;
        float  sd  = spriteRegionSignedDistance(loc, v, (uint)head.w, gate.y, gate.z);
        if (kind == 4u) {                         // Transparency — survival everywhere from the SDF
            float cov  = stencilCoverage(sd, params.y);
            c.a *= stencilSurvival((uint)params.x, cov);
            continue;
        }
        bool invert = (flags & 2u) != 0u;         // kSpriteFxInvert
        bool inside = (sd <= 0.0f) != invert;
        if (!inside) continue;
        float4 src = float4(params.xyz, gate.x);       // ColorFill: the fill; gate.x = region alpha
        if (kind == 6u) src = float4(applyGleam(c.rgb, fxUv.x, fxUv.y, params), gate.x);
        else if (kind == 7u) src = float4(applySaturation(c.rgb, params.x), gate.x);
        else if (kind == 8u) {                         // Bloom — the bloomed pixel as the graded source
            // params.w is the step's own field row; params.z is 0 for a step that was never seated (no
            // strength, or past the store's capacity), which grades with the pixel it started from.
            float4 gl = params.z > 0.0f ? sampleEmissionField(pos.xy, params.w)
                                        : float4(0.0f, 0.0f, 0.0f, 0.0f);
            float4 bl = spriteBloomApply(c, gl, 1.0f);
            src = float4(bl.rgb, gate.x);
        }
        c = applyBlendMode(c, src, (uint)head.z);      // head.z = region blend
    }
    if (c.a == 0.0f) discard;

    return float4(c.rgb, c.a * uAlpha * spriteAlpha);  // palette α × (effects) × layer α × per-sprite α
}
