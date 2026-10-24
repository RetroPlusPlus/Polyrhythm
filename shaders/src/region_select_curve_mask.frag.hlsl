// Region-select post-process fragment shader — curve boundary by a baked signed-distance mask.
//
// The mask peer of region_select_curve.frag: the same gate (inside(region) ? eff : src, run after the
// effect is rendered full-frame, touching no effect shader), but the boundary's signed distance comes from
// a TEXTURE sampled per fragment instead of an in-shader per-segment solve. The mask is Curve::signedDistance
// baked once into a single-channel signed-distance field (negative inside, positive outside, shape-local
// pixels) over the boundary's bounding box; this handles cubic / Catmull-Rom / arbitrary boundaries the
// analytic linear+quadratic path cannot solve in closed form. Containment mirrors
// retropp::sampleCurveMaskField + curveMaskRegionContains exactly (postprocess.h): the fragment is mapped
// into shape-local space by the region transform's inverse homography, then into mask UV by the bake box, and
// the bilinearly-filtered sampled distance is inflated by radius / banded by stroke / flipped by invert just
// like the analytic gate. radius / stroke / transform compose with no re-bake.
//
// SDL_GPU HLSL conventions: fragment sampled textures + samplers in space2 (t0/s0 = eff, t1/s1 = src,
// t2/s2 = the baked mask, bilinear-filtered, clamp-to-edge); the uniform buffer in space3.

Texture2D<float4> EffTexture  : register(t0, space2);
SamplerState      EffSampler  : register(s0, space2);
Texture2D<float4> SrcTexture  : register(t1, space2);
SamplerState      SrcSampler  : register(s1, space2);
Texture2D<float>  MaskTexture : register(t2, space2);
SamplerState      MaskSampler : register(s2, space2);

// The mask carries the field; the cbuffer carries only the placement + the bake box (mirrors
// retropp::curveRegionParams + the renderer's bake metadata). No per-segment data rides here.
cbuffer CurveMaskUniforms : register(b0, space3) {
    float4 uInvRow0;  // region transform inverse homography, row 0 (xyz; w = invert flag)        — register 0
    float4 uInvRow1;  //                          row 1 (xyz; w = stroke band width, px)          — register 1
    float4 uInvRow2;  //                          row 2 (xyz; w = region alpha)                   — register 2
    float4 uMisc;     // x = 1/viewportW, y = 1/viewportH, z = radius, w = blend mode (as float)  — register 3
    float4 uBake;     // xy = bake box min (shape-local px), zw = 1 / bake box extent             — register 4
    float4 uSnap;     // x = snap (1 = viewport grid, crisp); yzw pad                             — register 5
};

#include "blend_ops.hlsli"  // blendOp — the separable BlendMode operator, mirror of retropp::blendChannel

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 eff = EffTexture.Sample(EffSampler, uv);
    float4 src = SrcTexture.Sample(SrcSampler, uv);

    float radius = uMisc.z;
    float stroke = uInvRow1.w;  // > 0 → confine to a band of this width along the boundary (the outline)

    // Fragment UV → viewport pixels → shape-local via the inverse homography (perspective divide).
    float2 fragPx = float2(uv.x / uMisc.x, uv.y / uMisc.y);
    // Crisp evaluation (uSnap.x): snap the fragment to its viewport-cell center before the mask sample, so
    // the gate resolves per viewport pixel (pixel-identical upscale). A no-op at compose scale 1.
    if (uSnap.x != 0.0) fragPx = floor(fragPx) + 0.5;
    float  wgt    = uInvRow2.x * fragPx.x + uInvRow2.y * fragPx.y + uInvRow2.z;
    float2 local  = float2(uInvRow0.x * fragPx.x + uInvRow0.y * fragPx.y + uInvRow0.z,
                           uInvRow1.x * fragPx.x + uInvRow1.y * fragPx.y + uInvRow1.z) / wgt;

    // Shape-local → mask UV; the bilinear sampler reconstructs the signed distance between texels.
    float2 maskUv = (local - uBake.xy) * uBake.zw;
    float  sd     = MaskTexture.Sample(MaskSampler, maskUv) - radius;
    if (stroke > 0.0) sd = abs(sd) - stroke * 0.5;  // boundary signed distance → band (mirror of bandSignedDistance)
    bool inside = sd <= 0.0;
    if (uInvRow0.w > 0.5) inside = !inside;  // region invert: confine to the OUTSIDE of the shape

    // The region's blend mode grades how the effect result combines over the scene, before the alpha mix.
    // Normal (0) keeps the plain alpha-over the gate always ran. Mirror of applyBlendMode.
    uint   mode   = (uint)(uMisc.w + 0.5);
    float4 graded = eff;
    if (mode != 0u) {
        float  sa  = eff.a;
        float3 rgb = saturate((1.0 - sa) * src.rgb + sa * blendOp(mode, src.rgb, eff.rgb));
        float  a   = saturate(sa + src.a * (1.0 - sa));
        graded = float4(rgb, a);
    }
    return inside ? lerp(src, graded, uInvRow2.w) : src;  // uInvRow2.w = the region's alpha (its effects' opacity)
}
