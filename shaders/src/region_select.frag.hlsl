// Region-select post-process fragment shader.
//
// The engine-side gate that confines a screen-space effect to a SHAPE without touching any effect
// shader. It runs AFTER the effect has been rendered full-frame into a scratch: it reads the effect
// result (EffTexture) and the original source (SrcTexture) and returns, per fragment,
//   inside(region) ? eff : src
// so the effect applies only inside the shape and the source passes through everywhere else. Used at
// every realization site (frame-level postEffects, per-layer Below, per-layer Layer) and for every
// effect KIND (built-in RowDisplacement + game-registered Custom) — the custom-shader contract is
// untouched because the gate lives here, in the compositor, not in the effect's own fragment.
//
// Containment is a SIGNED-DISTANCE test, the exact CPU mirror of retropp::sdPolygon / regionContains
// (postprocess.h). The polygon's `count` vertices are viewport pixels; the fragment is mapped back into
// shape-local space by the region transform's INVERSE homography (perspective divide included, exactly
// like the tile path) before the SDF, so a scaled / stretched / skewed / rotated / moved region warps
// the gate. The SDF degenerates cleanly: count 1 → circle (distance-to-point), count 2 → capsule
// (distance-to-segment), count ≥ 3 → polygon (winding sign), each compared against `radius`.
//
// SDL_GPU HLSL conventions: fragment sampled textures + samplers in space2 (t0/s0 = eff, t1/s1 = src);
// the uniform buffer in space3.

Texture2D<float4> EffTexture : register(t0, space2);
SamplerState      EffSampler : register(s0, space2);
Texture2D<float4> SrcTexture : register(t1, space2);
SamplerState      SrcSampler : register(s1, space2);

// The polygon vertices ride in the uniform cbuffer (the proven path), packed two-per-register: up to
// kRegionCbufferMaxPoints (64) vertices → uPoints[32]. The renderer truncates a longer polygon and
// warns; a fragment storage buffer for truly-unbounded counts is a follow-up (needs on-device bring-up).
cbuffer RegionUniforms : register(b0, space3) {
    float4 uPoints[32]; // ≤64 vertices, xy packed 2-per-register (registers 0..31), viewport px
    float4 uInvRow0;    // region transform inverse homography, row 0 (xyz; w = invert flag) — register 32
    float4 uInvRow1;    //                              row 1 (xyz; w = stroke band width, px) — register 33
    float4 uInvRow2;    //                              row 2 (xyz; w = region alpha)          — register 34
    float4 uMisc;       // x = 1/viewportW, y = 1/viewportH, z = count (as float), w = radius — register 35
    float4 uBlend;      // x = blend mode (BlendMode as float, rounded to uint); y = snap (1 = viewport
                        //   grid, crisp); zw unused                                          — register 36
};

#include "blend_ops.hlsli"  // blendOp — the separable BlendMode operator, mirror of retropp::blendChannel

#include "polygon_sdf.hlsli"  // regionPoint, sdPolygon — reads uPoints above

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 eff = EffTexture.Sample(EffSampler, uv);
    float4 src = SrcTexture.Sample(SrcSampler, uv);

    uint  count  = (uint)(uMisc.z + 0.5);
    float radius = uMisc.w;
    float stroke = uInvRow1.w;  // > 0 → confine to a band of this width along the boundary (the outline)
    if (count == 0u) {
        return eff;  // no region → effect everywhere (the renderer never takes the gated path here)
    }

    // Fragment UV → viewport pixels → shape-local via the inverse homography (perspective divide).
    float2 fragPx = float2(uv.x / uMisc.x, uv.y / uMisc.y);
    // Crisp evaluation (uBlend.y): snap the fragment to its viewport-cell center before the SDF, so the
    // gate resolves per viewport pixel — the upscale is pixel-identical to the viewport-resolution
    // rasterization. A no-op at compose scale 1 (fragments already land on cell centers). Off = the
    // per-output-pixel evaluation (smooth boundary).
    if (uBlend.y != 0.0) fragPx = floor(fragPx) + 0.5;
    float  wgt = uInvRow2.x * fragPx.x + uInvRow2.y * fragPx.y + uInvRow2.z;
    float2 local = float2(uInvRow0.x * fragPx.x + uInvRow0.y * fragPx.y + uInvRow0.z,
                          uInvRow1.x * fragPx.x + uInvRow1.y * fragPx.y + uInvRow1.z) / wgt;

    float sd = sdPolygon(local, count) - radius;
    if (stroke > 0.0) sd = abs(sd) - stroke * 0.5;  // boundary signed distance → band (mirror of bandSignedDistance)
    bool inside = sd <= 0.0;
    if (uInvRow0.w > 0.5) inside = !inside;  // region invert: confine to the OUTSIDE of the shape

    // The region's blend mode grades how the effect result combines over the scene, before the alpha mix.
    // Normal (0) keeps the plain alpha-over the gate always ran (byte-identical). Mirror of applyBlendMode.
    uint   mode   = (uint)(uBlend.x + 0.5);
    float4 graded = eff;
    if (mode != 0u) {
        float  sa  = eff.a;
        float3 rgb = saturate((1.0 - sa) * src.rgb + sa * blendOp(mode, src.rgb, eff.rgb));
        float  a   = saturate(sa + src.a * (1.0 - sa));
        graded = float4(rgb, a);
    }
    return inside ? lerp(src, graded, uInvRow2.w) : src;  // uInvRow2.w = the region's alpha (its effects' opacity)
}
