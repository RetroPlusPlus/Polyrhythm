// Region-select post-process fragment shader — curve boundary (analytic linear + quadratic).
//
// The curve-boundary peer of region_select.frag: the same gate (inside(region) ? eff : src, run after
// the effect is rendered full-frame, touching no effect shader), but the boundary is a CLOSED CURVE of
// Linear and Quadratic Bezier segments instead of a straight-edged polygon — exact between control
// points, no facets, no vertex cap. Containment mirrors retropp::sdCurveAnalytic exactly
// (postprocess.h): the unsigned distance is the closed-form Bezier distance (a depressed-cubic solve
// for the quadratic), the sign is an even-odd +x ray cast (half-open [0,1) per segment), and the
// fragment is mapped into shape-local space by the region transform's inverse homography before the
// SDF. A boundary carrying a cubic segment is sampled to a polygon on the CPU and routed to
// region_select.frag instead — this shader only ever sees linear + quadratic segments.
//
// SDL_GPU HLSL conventions: fragment sampled textures + samplers in space2 (t0/s0 = eff, t1/s1 = src);
// the uniform buffer in space3.

Texture2D<float4> EffTexture : register(t0, space2);
SamplerState      EffSampler : register(s0, space2);
Texture2D<float4> SrcTexture : register(t1, space2);
SamplerState      SrcSampler : register(s1, space2);

// Per-segment control points ride the cbuffer two registers each (up to kCurveRegionMaxSegments = 32):
//   uSegs[2i]   = { start.xy, control.xy }   (control live for quadratics)
//   uSegs[2i+1] = { end.xy, degree, pad }    (degree 1 = linear, 2 = quadratic)
// The renderer truncates a longer boundary and warns. uMisc carries the inverse-viewport, segment
// count, and radius; uInvRow* the region transform inverse homography (mirrors retropp::curveRegionParams).
cbuffer CurveRegionUniforms : register(b0, space3) {
    float4 uSegs[64]; // 2 regs/segment (registers 0..63), xy/zw packed
    float4 uInvRow0;  // region transform inverse homography, row 0 (xyz; w = invert flag) — register 64
    float4 uInvRow1;  //                          row 1 (xyz; w = stroke band width, px) — register 65
    float4 uInvRow2;  //                          row 2 (xyz; w = region alpha)          — register 66
    float4 uMisc;     // x = 1/viewportW, y = 1/viewportH, z = segment count, w = radius — register 67
    float4 uBlend;    // x = blend mode (BlendMode as float, rounded to uint); y = snap (1 = viewport
                      //   grid, crisp); zw unused                                        — register 68
};

#include "blend_ops.hlsli"  // blendOp — the separable BlendMode operator, mirror of retropp::blendChannel

#include "curve_sdf.hlsli"  // segment accessors, pointSegDist, cbrtf, quadDist, sdCurve — reads uSegs above

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 eff = EffTexture.Sample(EffSampler, uv);
    float4 src = SrcTexture.Sample(SrcSampler, uv);

    uint  segCount = (uint)(uMisc.z + 0.5);
    float radius   = uMisc.w;
    float stroke   = uInvRow1.w;  // > 0 → confine to a band of this width along the boundary (the outline)
    if (segCount == 0u) {
        return eff;  // no boundary → effect everywhere (the renderer never takes this path empty)
    }

    // Fragment UV → viewport pixels → shape-local via the inverse homography (perspective divide).
    float2 fragPx = float2(uv.x / uMisc.x, uv.y / uMisc.y);
    // Crisp evaluation (uBlend.y): snap the fragment to its viewport-cell center before the SDF, so the
    // gate resolves per viewport pixel (pixel-identical upscale). A no-op at compose scale 1.
    if (uBlend.y != 0.0) fragPx = floor(fragPx) + 0.5;
    float  wgt    = uInvRow2.x * fragPx.x + uInvRow2.y * fragPx.y + uInvRow2.z;
    float2 local  = float2(uInvRow0.x * fragPx.x + uInvRow0.y * fragPx.y + uInvRow0.z,
                           uInvRow1.x * fragPx.x + uInvRow1.y * fragPx.y + uInvRow1.z) / wgt;

    float sd = sdCurve(local, segCount) - radius;
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
