// Below-scope sprite fragment shader (scene-facing sprite effects).
//
// The Layer-scope sprite path (sprite.frag) transforms the sprite's OWN art. A Below-scope sprite effect
// instead distorts / grades the COMPOSITED SCENE beneath the sprite's layer, confined to the sprite's
// silhouette (its art alpha coverage). This shader realizes that: the sprite draws as instanced quads (the
// SAME vertex stage as sprite.frag), and per fragment it
//   1. reads the sprite's OWN art at the within-sprite coordinate for the COVERAGE mask (alpha only) —
//      a fragment where the art is transparent is off-silhouette and discards;
//   2. samples the accumulator (the scene, bound as SourceTexture) at the fragment's screen position;
//   3. applies the sprite's Below effect run to that scene sample (built-in kind dispatch, mirroring the
//      retropp:: apply* authorities); a displacing kind re-reads the SCENE at a displaced screen position
//      (amplitude in VIEWPORT px — it distorts the scene, unlike the Layer path's own-art px re-read);
//   4. outputs the graded scene with alpha = the art coverage.
// The renderer renders the layer's Below sprites into a transparent-cleared scratch through this pipeline
// (premultiplied, the stock sprite blend state) and composites that scratch premultiplied-over the
// accumulator — outside the coverage the scratch is transparent, so the scene is byte-identical there. The
// sprite's own art then composites on top via the Layer path (sprite.frag), so a sprite carries both: art
// on top, distorted scene showing through the silhouette. N-flat: every built-in Below sprite in a layer
// draws in ONE instanced pass.
//
// Bindings differ from sprite.frag by ONE extra sampled texture: the scene (SourceTexture) takes t0/s0 and
// the emission atlas t1/s1 space2, so the five storage textures shift to t2..t6 (SDL_GPU numbers sampled
// textures first, then storage — the same convention the layer effect path uses).
//   - t0 space2 : the accumulator (scene), SAMPLED (nearest, CLAMP) — the pixels beneath this layer
//   - s0 space2 : its sampler
//   - t1 space2 : the EMISSION ATLAS — one footprint-sized rect per realized below Bloom / Glow step,
//                 SAMPLED, on paged sheets (an array layer per sheet)
//   - s1 space2 : its sampler (linear, CLAMP) — the halo is stored per viewport pixel and filtered back up
//   - t2 space2 : flat ATLAS STORE (R32_UINT; integer Load) — for the coverage read
//   - t3 space2 : palette store (RGBA8) — for the coverage read
//   - t4 space2 : global atlas-region table (R32G32B32A32_UINT)
//   - t5 space2 : per-frame sprite-effect records (R32G32B32A32_FLOAT; the sprite's Below run)
//   - t6 space2 : the rect side-table (R32G32B32A32_FLOAT; two texels per field, one field per row)
//   - b0 space3 : per-layer fragment uniforms (tile px + layer alpha + palette-store width + compose scale +
//                 the evaluation grid)
//
// The emission bindings — the atlas AND its rect table — are declared UNCONDITIONALLY, present in every
// generated below variant whether or not that shader glows, so the layout is the same everywhere and a lens
// that authors no halo still binds a valid (tiny, idle) pair.
//
// The built-in below path realizes the scene-composing kinds — ColorFill, Gleam, ColorSaturation, Bloom,
// Glow, RowDisplacement, Ripple —
// plus Transparency (it scales the lens's output alpha, blending the grade back toward the untouched scene)
// and region-confined effects (a region grades the scene over its quad-space shape ∩ the silhouette). A
// Below-scope Custom effect routes through a generated scene-read variant instead (the #ifdef below).

Texture2D<float4>      SourceTexture   : register(t0, space2);   // the scene beneath (nearest, CLAMP)
SamplerState           SourceSampler   : register(s0, space2);
Texture2DArray<float4> uEmissionAtlas  : register(t1, space2);   // the layer's halos, one rect each
SamplerState           uEmissionSampler : register(s1, space2);  // linear, CLAMP
Texture2D<uint>        uAtlas          : register(t2, space2);
Texture2D<float4>      uPaletteStore   : register(t3, space2);
Texture2D<uint4>       uAtlasRegions   : register(t4, space2);
Texture2D<float4>      uFxStore        : register(t5, space2);   // the sprite's Below effect records
// Where each field sits: two texels per field, one field per ROW (mirrors retropp::EmissionRectEntry).
//   texel 0 : (offsetX, offsetY, innerX, innerY) — viewport position + offset = atlas position
//   texel 1 : (innerW, innerH, sheet, 0)         — the content box a read is held inside, and which sheet
Texture2D<float4>      uEmissionRects  : register(t6, space2);

cbuffer SpriteFragUniforms : register(b0, space3) {
    float uTilePx;          // register 0: tile edge length, pixels (8)
    float uAlpha;           // layer alpha, [0,1]
    float uPaletteStoreW;   // palette-store row width (colors)
    float uComposeScale;    // compose grid ÷ viewport (1 = faithful); output pixel → viewport
    float uEvalSnap;        // register 1: 1 = Viewport grid (snap the field read to the cell center), 0 = Output
};

// Steps per texel the emission read quantizes its sample point to — mirrors retropp::kEmissionSampleSteps.
static const float kEmissionSampleSteps = 256.0f;

// ── Effect math (mirrors the retropp:: CPU authorities in postprocess.h) ──────────────────

#include "sprite_color.hlsli"  // applyGleam, applySaturation — the per-pixel color operators

#include "sprite_blend.hlsli"  // blendChannel, applyBlendMode — the scalar BlendMode operators

#include "sprite_stencil.hlsli"  // stencilCoverage, stencilSurvival, spriteRegionSignedDistance

// Round-half-up (HLSL round() is round-to-even and breaks scale-1 parity at .5 ties).
#include "rounding.hlsli"  // roundHalfUp — the CPU mirror's tie rule

// The center of the VIEWPORT cell a screen uv falls in — the crisp evaluation point (the scene is a
// viewport-resolution image scaled to compose res; per-cell math matches the viewport rasterization).
float2 snapViewport(float2 uv, float2 dims) {
    float2 c = uv;
    if (dims.x > 0.0f) c.x = (floor(uv.x * dims.x) + 0.5f) / dims.x;
    if (dims.y > 0.0f) c.y = (floor(uv.y * dims.y) + 0.5f) / dims.y;
    return c;
}

// RowDisplacement over the SCENE: the modulated axis offsets by amplitude·sin(2π(freq·otherAxis + phase)),
// quantized to whole VIEWPORT px (the scene's native pixels). params = (amplitude, frequency, phase, axis:
// 0 Horizontal / 1 Vertical). amplitude 0 ⇒ identity. `dims` is the viewport size.
float2 sceneDisplace(float2 uv, float4 params, float2 dims) {
    if (params.x == 0.0f) return uv;
    const float kTwoPi = 6.283185307179586f;
    float2 e = snapViewport(uv, dims);
    if ((uint)params.w == 0u) {  // Horizontal: offset in u, wave over v
        float s   = sin(kTwoPi * (params.y * e.y + params.z));
        float off = roundHalfUp(params.x * s) / dims.x;
        return float2(uv.x + off, uv.y);
    }
    float s   = sin(kTwoPi * (params.y * e.x + params.z));  // Vertical: offset in v, wave over u
    float off = roundHalfUp(params.x * s) / dims.y;
    return float2(uv.x, uv.y + off);
}

// Ripple over the SCENE: a radial re-read pushed along the radius from `center` (VIEWPORT px, in gate.yz)
// by amplitude·sin(2π(freq·dist − phase))·exp(−decay·dist), quantized to whole viewport px. params =
// (amplitude, frequency, phase, _); gate.w = decay. amplitude 0 or the center pixel ⇒ identity.
float2 sceneRipple(float2 uv, float4 params, float4 gate, float2 dims) {
    if (params.x == 0.0f) return uv;
    const float kTwoPi = 6.283185307179586f;
    float invW = 1.0f / dims.x, invH = 1.0f / dims.y;
    float cu = gate.y * invW, cv = gate.z * invH;   // center viewport px → screen uv
    float2 e   = snapViewport(uv, dims);
    float  dx  = e.x - cu, dy = e.y - cv;
    float  cx  = dx * (invH / invW);                // aspect-correct so the rings stay circular
    float  dist = sqrt(cx * cx + dy * dy);
    if (dist <= 1e-5f) return uv;                   // the center has no radial direction
    float  wave   = sin(kTwoPi * (params.y * dist - params.z));
    float  env    = exp(-gate.w * dist);
    float  offset = params.x * wave * env;          // viewport px
    return float2(uv.x + roundHalfUp(dx / dist * offset) * invW,
                  uv.y + roundHalfUp(dy / dist * offset) * invH);
}

// Swirl over the SCENE: an angular re-read rotating the sample about `center` (VIEWPORT px, in gate.yz) by
// twist·(1 − t²)², t = dist/radius — the full turn at the center easing to none at the rim. params =
// (twist, radius, _, _); `twist` arrives already resolved to RADIANS and signed (swirlParams does the
// degrees conversion), `radius` is in viewport px. Working space is viewport px (square units, so the disc
// stays circular on any aspect). twist 0, radius ≤ 0, or a fragment at or beyond the rim ⇒ identity.
float2 sceneSwirl(float2 uv, float4 params, float4 gate, float2 dims) {
    if (params.x == 0.0f || params.y <= 0.0f) return uv;
    float2 e = snapViewport(uv, dims) * dims;        // evaluate from the viewport cell center, in px
    float2 c = float2(gate.y, gate.z);
    float2 d = e - c;
    float  r = length(d);
    if (r >= params.y) return uv;                    // outside the disc: its own coordinate
    float  t     = r / params.y;
    float  f     = 1.0f - t * t;
    float  theta = params.x * f * f;
    float  s, cs;
    sincos(theta, s, cs);
    float2 rd = float2(cs * d.x - s * d.y, s * d.x + cs * d.y);
    return (c + rd) / dims;
}

// ── Sprite art coverage read (the silhouette mask) ─────────────────────────────────────────
//
// Read the sprite's OWN art alpha at a within-sprite quad coordinate to decide coverage — the same
// transparent-field material read sprite.frag does, but only the alpha matters here (the mask). A read
// outside the art, a structural hole, or a fully-transparent palette entry is coverage 0 (off-silhouette).
// (Duplicated from sprite.frag rather than shared, to leave the shipped, golden sprite path untouched; a
// future cleanup can factor the read into a shared include.)
float4 spriteArtSample(float2 uv, uint tile, uint atlasPalette, uint flags, uint packedSize) {
    int2 sz = int2((int)(packedSize >> 16), (int)(packedSize & 0xFFFFu));
    if (uv.x < 0.0f || uv.x >= 1.0f || uv.y < 0.0f || uv.y >= 1.0f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);                 // off-art → no coverage
    int2 px = clamp(int2(floor(uv * float2(sz))), int2(0, 0), sz - int2(1, 1));
    bool flipX    = (flags & 1u) != 0u;
    bool flipY    = (flags & 2u) != 0u;
    uint rotation = (flags >> 2u) & 3u;
    if (flipX) px.x = sz.x - 1 - px.x;
    if (flipY) px.y = sz.y - 1 - px.y;
    if (rotation == 1u)      { int rt = px.x; px.x = px.y;            px.y = sz.x - 1 - rt; }
    else if (rotation == 2u) { px.x = sz.x - 1 - px.x; px.y = sz.y - 1 - px.y; }
    else if (rotation == 3u) { int rt = px.x; px.x = sz.y - 1 - px.y;  px.y = rt; }

    uint atlasId       = atlasPalette & 0xFFFFu;
    uint paletteOffset = atlasPalette >> 16;
    uint4 region       = uAtlasRegions.Load(int3((int)atlasId, 0, 0));
    int   storeX       = (int)(region.x & 0xFFFFu);
    int   storeY       = (int)(region.x >> 16);
    int   atlasCols    = (int)region.y;
    if (atlasCols == 0) return float4(0.0f, 0.0f, 0.0f, 0.0f);

    int  tilePx = (int)uTilePx;
    int  col    = (int)tile % atlasCols;
    int  row    = (int)tile / atlasCols;
    int2 texel  = int2(storeX + col * tilePx + px.x, storeY + row * tilePx + px.y);
    uint idx    = uAtlas.Load(int3(texel, 0));
    bool hole = (idx < 32u) ? (((region.z >> idx)         & 1u) != 0u)
              : (idx < 64u) ? (((region.w >> (idx - 32u)) & 1u) != 0u)
                            : false;
    if (hole) return float4(0.0f, 0.0f, 0.0f, 0.0f);           // structural transparency
    uint   flat = paletteOffset + idx;
    int    W    = (int)uPaletteStoreW;
    return uPaletteStore.Load(int3((int)(flat % (uint)W), (int)(flat / (uint)W), 0));  // a==0 = material hole
}

#include "emission_field.hlsli"  // sampleEmissionField — mirror of retropp::emissionFieldSamplePoint

// ── Custom (Below-scope) hook ───────────────────────────────────────────────────────────────
//
// A Below-scope Custom effect runs a game-registered shader inline to produce the whole graded scene (it
// reads the scene through sampleSource() and its own params from the record lanes). The base pipeline carries
// no game shader, so this is a sentinel — on the base pipeline the built-in kind path runs instead (the
// #ifdef in main() compiles the built-in path, not this call). A generated <ns>_sprite_below variant
// (gen_shader.cmake SPRITE_BELOW mode) replaces the marker below with the game body + its scene-reading
// sampleSource (retropp_sprite_below_effect.hlsli) + a record-lane param loader, and #defines
// RETROPP_SPRITE_BELOW_CUSTOM so the built-in path is compiled out and this call produces the lens color.
// `sceneUv` is the fragment's screen uv, `viewportDim` the viewport size (the displacement quantization unit;
// an emission-consumer variant maps a uv to a field read by `uv * viewportDim * uComposeScale`), `ri` the
// sprite's single Below-custom record row in the sprite-effect store.
// @retropp:sprite-below-custom-hook
#ifndef RETROPP_SPRITE_BELOW_CUSTOM
float4 retroppSpriteBelowCustom(float2 sceneUv, float2 viewportDim, int ri) { return float4(0.0f, 0.0f, 0.0f, 0.0f); }
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
    // The within-sprite QUAD coordinate — the coverage-read space, pre-orientation. A transformed sprite on
    // the analytic flag (bit 4) resolves coverage per VIEWPORT cell (SV_Position → viewport → cell center →
    // screen→unit inverse); a plain sprite takes the rasterized spriteUV. A Below sprite's footprint is NOT
    // inflated (Below displacement re-reads the scene, not the art — spriteDisplacedRead skips Below), so a
    // fragment outside the true quad on the analytic path is off-silhouette and discards.
    bool analytic = (flags & 16u) != 0u;
    float2 fxUv;
    if (analytic) {
        float2 c  = floor(pos.xy / uComposeScale) + 0.5f;
        float  cw = inv2.x * c.x + inv2.y * c.y + inv2.z;
        if (cw <= 0.0f) discard;
        fxUv = float2((inv0.x * c.x + inv0.y * c.y + inv0.z) / cw,
                      (inv1.x * c.x + inv1.y * c.y + inv1.z) / cw);
        if (fxUv.x < 0.0f || fxUv.x >= 1.0f || fxUv.y < 0.0f || fxUv.y >= 1.0f) discard;
    } else {
        fxUv = spriteUV;
    }

    // Coverage: the sprite's own art alpha. Off-silhouette (transparent art) ⇒ nothing to distort here.
    float coverage = spriteArtSample(fxUv, tile, atlasPalette, flags, packedSize).a;
    if (coverage == 0.0f) discard;
    // The lens's output alpha (its strength). A Transparency step (whole-silhouette or region-confined) scales
    // it down, blending the grade back toward the untouched scene.
    float outCoverage = coverage;

    // The scene sample coordinate: this fragment's screen position over the compose target (the accumulator
    // is the compose-resolution scene). Its viewport size drives the displacement math (viewport px).
    float2 composeDim;
    SourceTexture.GetDimensions(composeDim.x, composeDim.y);
    float2 sceneUv     = pos.xy / composeDim;
    float2 viewportDim = composeDim / uComposeScale;

#ifdef RETROPP_SPRITE_BELOW_CUSTOM
    // Custom-Below: a game shader produces the graded scene wholesale. It reads the scene through sampleSource
    // and its own params (the record's idle lanes at fxOffset); the built-in kind path below is compiled out.
    float3 c = retroppSpriteBelowCustom(sceneUv, viewportDim, int(fxOffset)).rgb;
#else
    // Displacement pre-pass — compose the Below run's displacing effects (RowDisplacement / Ripple / Swirl)
    // into the scene READ coordinate before the scene is sampled. Color kinds then apply to the read color.
    float2 readUv = sceneUv;
    [loop]
    for (uint di = 0u; di < fxCount; di++) {
        int    dri = int(fxOffset + di);
        float4 dh  = uFxStore.Load(int3(0, dri, 0));   // head: kind, flags
        uint   dk  = (uint)dh.x;
        if (dk == 1u) {                                 // RowDisplacement
            float4 dp = uFxStore.Load(int3(2, dri, 0)); // params (amplitude, frequency, phase, axis)
            readUv = sceneDisplace(readUv, dp, viewportDim);
        } else if (dk == 2u) {                          // Ripple
            float4 dp = uFxStore.Load(int3(2, dri, 0));
            float4 dg = uFxStore.Load(int3(1, dri, 0)); // gate: center (yz) + decay (w)
            readUv = sceneRipple(readUv, dp, dg, viewportDim);
        } else if (dk == 10u) {                         // Swirl
            float4 dp = uFxStore.Load(int3(2, dri, 0)); // params (twist radians, radius, _, _)
            float4 dg = uFxStore.Load(int3(1, dri, 0)); // gate: center (yz)
            readUv = sceneSwirl(readUv, dp, dg, viewportDim);
        }
    }

    // Sample the scene at the (displaced) coordinate — nearest, CLAMP at the frame edge (a displaced read
    // that runs off-frame smears the border rather than punching a transparent gap into the scene).
    float2 clampedUv = clamp(readUv, float2(0.0f, 0.0f), float2(1.0f, 1.0f));
    float3 c = SourceTexture.Sample(SourceSampler, clampedUv).rgb;
    // The same read position in COMPOSE pixels — what a halo read is addressed by, so a displaced chain
    // moves the halo exactly as it moved the scene beneath it.
    float2 readPx = readUv * composeDim;

    // Effect run — the whole-silhouette chain steps then the region steps, in record order (mirrors
    // evalSpriteFxRecords for the scene-facing kinds). A chain step grades the whole scene sample; a region
    // step gates on its quad-space shape (∩ silhouette implicit) and grades over the scene where it covers.
    // Displacing kinds already moved the read in the pre-pass above.
    int2 sz = int2((int)(packedSize >> 16), (int)(packedSize & 0xFFFFu));
    [loop]
    for (uint i = 0u; i < fxCount; i++) {
        int    ri     = int(fxOffset + i);
        float4 head   = uFxStore.Load(int3(0, ri, 0));   // kind, flags, blend, pointCount
        float4 gate   = uFxStore.Load(int3(1, ri, 0));   // alpha, radius, strokeWidth, _
        float4 params = uFxStore.Load(int3(2, ri, 0));
        uint   kind   = (uint)head.x;
        uint   fl     = (uint)head.y;
        bool   isRegion = (fl & 1u) != 0u;
        if (!isRegion) {                                                     // whole-silhouette chain step
            if (kind == 5u)      c = params.xyz;                            // ColorFill — paint over the scene
            else if (kind == 6u) c = applyGleam(c, sceneUv.x, sceneUv.y, params);  // Gleam — keyed sheen
            else if (kind == 7u) c = applySaturation(c, params.x);         // ColorSaturation — desaturate the scene
            // Bloom / Glow — the halo the renderer already made, scaled by this lens's own strength. A step
            // with no strength (an identity, or one the field store could not seat) samples nothing at all.
            else if (kind == 8u) {                                              // Bloom — the scene's own light
                if (params.z != 0.0f)
                    c = c + params.z * sampleEmissionField(readPx, params.w).rgb;
            } else if (kind == 9u) {                                            // Glow — authored-color aura;
                if (params.z != 0.0f)                                           //   tint rides the gate lanes
                    c = c + params.z * sampleEmissionField(readPx, params.w).a
                          * float3(gate.y, gate.z, gate.w);
            }
            else if (kind == 4u) outCoverage *= stencilSurvival((uint)params.x, 1.0f);  // Transparency — lens strength
            continue;                                                       // displacing kinds moved the read above
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
        if (kind == 4u) {                                                   // Transparency — survival from the SDF
            float cov = stencilCoverage(sd, params.y);
            outCoverage *= stencilSurvival((uint)params.x, cov);
            continue;
        }
        bool invert = (fl & 2u) != 0u;
        bool inside = (sd <= 0.0f) != invert;
        if (!inside) continue;
        float4 src = float4(params.xyz, gate.x);                            // ColorFill: the fill; gate.x = region alpha
        if (kind == 6u) src = float4(applyGleam(c, sceneUv.x, sceneUv.y, params), gate.x);
        else if (kind == 7u) src = float4(applySaturation(c, params.x), gate.x);
        else if (kind == 8u)   // Bloom — this step's own halo graded in as the region's source color
            src = float4(params.z != 0.0f ? c + params.z * sampleEmissionField(readPx, params.w).rgb : c,
                         gate.x);
        c = applyBlendMode(float4(c, 1.0f), src, (uint)head.z).rgb;         // head.z = region blend; grade the scene
    }
#endif
    if (outCoverage == 0.0f) discard;

    // Output the graded scene, opacity = the lens strength × layer α × per-sprite α. The renderer composites
    // this (premultiplied by the stock sprite blend state) premultiplied-over the accumulator, so the
    // distortion lands only on the silhouette; the transparent surround leaves the scene byte-identical.
    return float4(c, outCoverage * uAlpha * spriteAlpha);
}
