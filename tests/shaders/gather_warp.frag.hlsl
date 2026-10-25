// Gather equivalence-test shader — the SOURCE-DEPENDENT twin (the GATHERING route). A displacement + tint
// warp: out = sampleSource(uv + duv) + tint. Its output depends on the SOURCE, so the additive path's
// zero-source delta extraction cannot express it — this is exactly the replace class gathering exists for
// (a reality-warp shape). It carries NO declaration, so the build compiles its GATHER variant
// automatically and the renderer collapses eligible same-stage regions into one union-shape pass. The `tint`
// is distinct per region, proving per-region params ride the gather records. The gather_warp_nogather twin is
// this file with the no-gather opt-out added (the per-region reference route); the GPU equivalence test
// renders both at each confined site over WELL-SEPARATED shapes and asserts they match within Tol::OneStep.
// (This comment deliberately avoids spelling the opt-out token so the build's raw-source scan does not
// mistake this prose for the declaration and suppress the gather variant.)

// Unique param names (gwarp*) so this test fixture does not collide with — and reorder — the shared
// ScreenSpaceEffect param union that a real consumer (e.g. Ferryman's warpAmp/warpFreq) contributes to.
cbuffer Params : register(b1, space3) {
    float  gwarpAmp;   // displacement strength (UV units)
    float  gwarpFreq;  // wave frequency across the viewport
    float3 gwarpTint;  // color added after sampling (per-region — rides the gather records)
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float2 duv;
    duv.x = sin(uv.y * gwarpFreq) * gwarpAmp;
    duv.y = cos(uv.x * gwarpFreq) * gwarpAmp;
    float4 c = sampleSource(uv + duv);
    c.rgb = saturate(c.rgb + gwarpTint);
    return c;
}
