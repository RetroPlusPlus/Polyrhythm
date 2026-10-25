// Region-stencil post-process fragment shader — curve boundary by a baked signed-distance mask.
//
// The mask peer of region_stencil_curve.frag: the same see-through (source × survival, one texture), but the
// boundary's signed distance comes from a TEXTURE sampled per fragment instead of an in-shader per-segment
// solve. The mask is Curve::signedDistance baked once into a single-channel signed-distance field (negative
// inside, positive outside, shape-local pixels); this handles cubic / Catmull-Rom / arbitrary boundaries the
// analytic linear+quadratic path cannot solve in closed form. Coverage mirrors retropp::sampleCurveMaskField
// + stencilCoverage / stencilSurvival exactly (postprocess.h): the fragment is mapped into shape-local space
// by the region transform's inverse homography, then into mask UV by the bake box, and the bilinearly-filtered
// sampled distance is inflated by radius / banded by stroke / flipped by invert / ramped by feather just like
// the analytic stencil. radius / stroke / transform compose with no re-bake.
//
// SDL_GPU HLSL conventions: fragment sampled textures + samplers in space2 (t0/s0 = source, t1/s1 = the baked
// mask, bilinear-filtered, clamp-to-edge); the uniform buffer in space3.

Texture2D<float4> SrcTexture  : register(t0, space2);
SamplerState      SrcSampler  : register(s0, space2);
Texture2D<float>  MaskTexture : register(t1, space2);
SamplerState      MaskSampler : register(s1, space2);

// The mask carries the field; the cbuffer carries only the placement + the bake box + the stencil scalars.
cbuffer CurveMaskStencilUniforms : register(b0, space3) {
    float4 uInvRow0;  // region transform inverse homography, row 0 (xyz; w = invert flag)        — register 0
    float4 uInvRow1;  //                          row 1 (xyz; w = stroke band width, px)          — register 1
    float4 uInvRow2;  //                          row 2 (xyz)                                     — register 2
    float4 uMisc;     // x = 1/viewportW, y = 1/viewportH, z = radius, w = mode (0 inside, 1 out) — register 3
    float4 uBake;     // xy = bake box min (shape-local px), zw = 1 / bake box extent             — register 4
    float4 uStencil;  // x = feather (px), y = snap (1 = viewport grid, crisp); zw unused         — register 5
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 src = SrcTexture.Sample(SrcSampler, uv);

    float radius  = uMisc.z;
    uint  mode    = (uint)(uMisc.w + 0.5);
    float feather = uStencil.x;
    float stroke  = uInvRow1.w;  // > 0 → make a band along the boundary see-through (a ring), not the fill

    // Fragment UV → viewport pixels → shape-local via the inverse homography (perspective divide).
    float2 fragPx = float2(uv.x / uMisc.x, uv.y / uMisc.y);
    // Crisp evaluation (uStencil.y): snap the fragment to its viewport-cell center before the mask sample,
    // so the stencil resolves per viewport pixel (pixel-identical upscale). A no-op at compose scale 1.
    if (uStencil.y != 0.0) fragPx = floor(fragPx) + 0.5;
    float  wgt    = uInvRow2.x * fragPx.x + uInvRow2.y * fragPx.y + uInvRow2.z;
    float2 local  = float2(uInvRow0.x * fragPx.x + uInvRow0.y * fragPx.y + uInvRow0.z,
                           uInvRow1.x * fragPx.x + uInvRow1.y * fragPx.y + uInvRow1.z) / wgt;

    // Shape-local → mask UV; the bilinear sampler reconstructs the signed distance between texels.
    float2 maskUv     = (local - uBake.xy) * uBake.zw;
    float  signedDist = MaskTexture.Sample(MaskSampler, maskUv) - radius;
    if (stroke > 0.0) signedDist = abs(signedDist) - stroke * 0.5;  // boundary → band (mirror of bandSignedDistance)
    if (uInvRow0.w > 0.5) signedDist = -signedDist;  // region invert: make the opposite side see-through

    // Coverage = how far inside the boundary, ramped over `feather` (mirror of stencilCoverage).
    float coverage = feather > 0.0 ? clamp(0.5 - signedDist / feather, 0.0, 1.0)
                                   : (signedDist <= 0.0 ? 1.0 : 0.0);

    // Survival = mode-selected (mirror of stencilSurvival). Scale all four premultiplied channels.
    float survival = (mode == 0u) ? (1.0 - coverage) : coverage;
    return src * survival;
}
