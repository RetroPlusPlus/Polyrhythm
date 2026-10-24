// Gather equivalence-test shader — the SOURCE-DEPENDENT twin (the PER-REGION reference route). Body
// identical to gather_warp.frag.hlsl EXCEPT for the no-gather declaration below, which suppresses the GATHER
// variant so every region takes the existing per-region path (runEffect + region-select gate). The GPU
// equivalence test renders this against the gathering twin at each confined site over WELL-SEPARATED shapes;
// the two must match within Tol::OneStep (float rounding only). Keep the body in lockstep with the gathering
// twin.
//
// @retropp:no-gather

cbuffer Params : register(b1, space3) {
    float  gwarpAmp;   // displacement strength (UV units)
    float  gwarpFreq;  // wave frequency across the viewport
    float3 gwarpTint;  // color added after sampling (per-region)
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float2 duv;
    duv.x = sin(uv.y * gwarpFreq) * gwarpAmp;
    duv.y = cos(uv.x * gwarpFreq) * gwarpAmp;
    float4 c = sampleSource(uv + duv);
    c.rgb = saturate(c.rgb + gwarpTint);
    return c;
}
