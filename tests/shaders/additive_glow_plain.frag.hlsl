// Region-batching equivalence-test shader (PLAIN twin — the per-region reference). Identical to
// additive_glow.frag.hlsl EXCEPT it carries no additive declaration, so no batched variant is compiled and
// every region takes the existing per-region path (runEffect + region-select gate). The GPU equivalence test
// renders this against the declared twin at each confined site; the two must match within Tol::OneStep
// (float-rounding / additive-order only). Keep the body in lockstep with the declared twin.
//
// The no-gather declaration below keeps this twin on the TRUE per-region route: without it, the automatic
// gather routing would compile a gather variant and collapse its same-stage Normal-blend regions into a
// union-shape pass — no longer the per-region reference the additive-batching proof compares against. This
// one line defends the RegionBatchEquiv* tests, unchanged, from the routing landing beside it.
//
// @retropp:no-gather

cbuffer Params : register(b1, space3) {
    float addGain;   // additive peak at the region center
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 c = sampleSource(uv);
    const float2 d    = uv - 0.5f;
    const float  r    = saturate(length(d) * 2.0f);   // 0 at the (frame-global) center → 1 at the edge
    const float  fall = (1.0f - r) * (1.0f - r);       // soft shoulders, bright core
    const float3 tint = float3(0.9f, 0.6f, 0.3f);
    c.rgb += tint * (addGain * fall);                  // additive — brightens whatever's behind
    return c;
}
