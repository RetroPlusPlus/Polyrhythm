// Region-batching equivalence-test shader (DECLARED twin). An ADDITIVE custom effect: its output is its
// source PLUS a source-independent radial term (out = sampleSource(uv) + tint·addGain·fall(uv)), the exact
// contract the batched-additive fast path requires. The additive declaration below is the entire
// opt-in — the build compiles a second BATCHED variant and the renderer routes eligible same-shader
// regions through one instanced-additive pass. additive_glow_plain.frag.hlsl is this file minus the
// declaration (the per-region reference twin); the GPU equivalence test renders both at each confined
// site and asserts the readbacks match within Tol::OneStep.
//
// @retropp:additive

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
