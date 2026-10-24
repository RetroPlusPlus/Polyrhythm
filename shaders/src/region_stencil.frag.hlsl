// Region-stencil post-process fragment shader.
//
// The see-through sibling of region_select.frag: the same per-fragment region SDF (polygon / circle /
// capsule), but instead of selecting between two textures it makes the layer's own pixels in/around
// the shape SEE-THROUGH. It reads ONE source (the layer's rendered pixels), computes the region signed
// distance, turns it into a feathered survival factor, and scales the (premultiplied) color by it.
// TransparentInside punches a hole (the layers below show through); TransparentOutside keeps only the
// inside (a porthole). The region_select gate is a separate pass and is untouched.
//
// Containment is the exact CPU mirror of retropp::sdPolygon (postprocess.h); the survival math
// mirrors retropp::stencilCoverage / stencilSurvival. The polygon's `count` vertices are viewport
// pixels, mapped to shape-local space by the region transform's inverse homography before the SDF; the
// SDF degenerates to point (count 1 → circle) and segment (count 2 → capsule) distance, each compared
// against `radius`.
//
// SDL_GPU HLSL conventions: one fragment sampled texture + sampler in space2 (t0/s0 = the source); the
// uniform buffer in space3.

Texture2D<float4> SrcTexture : register(t0, space2);
SamplerState      SrcSampler : register(s0, space2);

// The polygon vertices ride in the cbuffer two-per-register (≤64 vertices → uPoints[32]); the inverse
// homography + misc tail mirror region_select.frag exactly; uStencil adds the two stencil scalars.
cbuffer StencilUniforms : register(b0, space3) {
    float4 uPoints[32]; // ≤64 vertices, xy packed 2-per-register (registers 0..31), viewport px
    float4 uInvRow0;    // region transform inverse homography, row 0 (xyz; w = invert flag) — register 32
    float4 uInvRow1;    //                              row 1 (xyz; w = stroke band width, px) — register 33
    float4 uInvRow2;    //                                       row 2               — register 34
    float4 uMisc;       // x = 1/viewportW, y = 1/viewportH, z = count (as float), w = radius — register 35
    float4 uStencil;    // x = mode (0 TransparentInside, 1 TransparentOutside), y = feather (px),
                        //   z = snap (1 = viewport grid, crisp); w pad                             — register 36
};

#include "polygon_sdf.hlsli"  // regionPoint, sdPolygon — reads uPoints above

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 src = SrcTexture.Sample(SrcSampler, uv);

    uint  count   = (uint)(uMisc.z + 0.5);
    float radius  = uMisc.w;
    uint  mode    = (uint)(uStencil.x + 0.5);
    float feather = uStencil.y;
    float stroke  = uInvRow1.w;  // > 0 → make a band along the boundary see-through (a ring), not the fill

    // Coverage = how far inside the shape, ramped over `feather` (mirror of stencilCoverage). count 0 →
    // whole viewport inside (coverage 1) — the no-region degenerate.
    float coverage;
    if (count == 0u) {
        coverage = 1.0;
    } else {
        // Fragment UV → viewport pixels → shape-local via the inverse homography (perspective divide).
        float2 fragPx = float2(uv.x / uMisc.x, uv.y / uMisc.y);
        // Crisp evaluation (uStencil.z): snap the fragment to its viewport-cell center before the SDF, so
        // the stencil resolves per viewport pixel (pixel-identical upscale). A no-op at compose scale 1.
        if (uStencil.z != 0.0) fragPx = floor(fragPx) + 0.5;
        float  wgt = uInvRow2.x * fragPx.x + uInvRow2.y * fragPx.y + uInvRow2.z;
        float2 local = float2(uInvRow0.x * fragPx.x + uInvRow0.y * fragPx.y + uInvRow0.z,
                              uInvRow1.x * fragPx.x + uInvRow1.y * fragPx.y + uInvRow1.z) / wgt;
        float signedDist = sdPolygon(local, count) - radius;
        if (stroke > 0.0) signedDist = abs(signedDist) - stroke * 0.5;  // boundary → band (mirror of bandSignedDistance)
        if (uInvRow0.w > 0.5) signedDist = -signedDist;  // region invert: make the opposite side see-through
        coverage = feather > 0.0 ? clamp(0.5 - signedDist / feather, 0.0, 1.0)
                                 : (signedDist <= 0.0 ? 1.0 : 0.0);
    }

    // Survival = mode-selected (mirror of stencilSurvival): TransparentInside makes the covered area
    // see-through, TransparentOutside keeps it. Scale all four premultiplied channels together.
    float survival = (mode == 0u) ? (1.0 - coverage) : coverage;
    return src * survival;
}
