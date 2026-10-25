// Region-stencil post-process fragment shader — curve boundary (analytic linear + quadratic).
//
// The curve-boundary peer of region_stencil.frag: the same see-through (source × survival, one texture), but
// the boundary is a CLOSED CURVE of Linear and Quadratic Bezier segments instead of a straight-edged
// polygon — exact between control points, no facets, no vertex cap. Containment mirrors
// retropp::sdCurveAnalytic exactly (postprocess.h): the unsigned distance is the closed-form Bezier
// distance (a depressed-cubic solve for the quadratic), the sign is an even-odd +x ray cast (half-open
// [0,1) per segment), and the fragment is mapped into shape-local space by the region transform's
// inverse homography before the SDF. A boundary carrying a cubic segment is sampled to a polygon on the
// CPU and routed to region_stencil.frag instead — this shader only ever sees linear + quadratic.
//
// SDL_GPU HLSL conventions: one fragment sampled texture + sampler in space2 (t0/s0 = the source); the
// uniform buffer in space3.

Texture2D<float4> SrcTexture : register(t0, space2);
SamplerState      SrcSampler : register(s0, space2);

// Per-segment control points ride the cbuffer two registers each (up to kCurveRegionMaxSegments = 32);
// the inverse homography + misc tail mirror region_select_curve.frag exactly; uStencil adds the two
// stencil scalars.
cbuffer CurveStencilUniforms : register(b0, space3) {
    float4 uSegs[64]; // 2 regs/segment (registers 0..63), xy/zw packed
    float4 uInvRow0;  // region transform inverse homography, row 0 (xyz; w = invert flag) — register 64
    float4 uInvRow1;  //                          row 1 (xyz; w = stroke band width, px) — register 65
    float4 uInvRow2;  //                                       row 2             — register 66
    float4 uMisc;     // x = 1/viewportW, y = 1/viewportH, z = segment count, w = radius — register 67
    float4 uStencil;  // x = mode (0 TransparentInside, 1 TransparentOutside), y = feather (px),
                      //   z = snap (1 = viewport grid, crisp); w pad                             — register 68
};

#include "curve_sdf.hlsli"  // segment accessors, pointSegDist, cbrtf, quadDist, sdCurve — reads uSegs above

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 src = SrcTexture.Sample(SrcSampler, uv);

    uint  segCount = (uint)(uMisc.z + 0.5);
    float radius   = uMisc.w;
    uint  mode     = (uint)(uStencil.x + 0.5);
    float feather  = uStencil.y;
    float stroke   = uInvRow1.w;  // > 0 → make a band along the boundary see-through (a ring), not the fill

    // Coverage = how far inside the boundary, ramped over `feather` (mirror of stencilCoverage).
    // segCount 0 → whole viewport inside (coverage 1) — the no-region degenerate.
    float coverage;
    if (segCount == 0u) {
        coverage = 1.0;
    } else {
        // Fragment UV → viewport pixels → shape-local via the inverse homography (perspective divide).
        float2 fragPx = float2(uv.x / uMisc.x, uv.y / uMisc.y);
        // Crisp evaluation (uStencil.z): snap the fragment to its viewport-cell center before the SDF, so
        // the stencil resolves per viewport pixel (pixel-identical upscale). A no-op at compose scale 1.
        if (uStencil.z != 0.0) fragPx = floor(fragPx) + 0.5;
        float  wgt    = uInvRow2.x * fragPx.x + uInvRow2.y * fragPx.y + uInvRow2.z;
        float2 local  = float2(uInvRow0.x * fragPx.x + uInvRow0.y * fragPx.y + uInvRow0.z,
                               uInvRow1.x * fragPx.x + uInvRow1.y * fragPx.y + uInvRow1.z) / wgt;
        float signedDist = sdCurve(local, segCount) - radius;
        if (stroke > 0.0) signedDist = abs(signedDist) - stroke * 0.5;  // boundary → band (mirror of bandSignedDistance)
        if (uInvRow0.w > 0.5) signedDist = -signedDist;  // region invert: make the opposite side see-through
        coverage = feather > 0.0 ? clamp(0.5 - signedDist / feather, 0.0, 1.0)
                                 : (signedDist <= 0.0 ? 1.0 : 0.0);
    }

    // Survival = mode-selected (mirror of stencilSurvival). Scale all four premultiplied channels.
    float survival = (mode == 0u) ? (1.0 - coverage) : coverage;
    return src * survival;
}
