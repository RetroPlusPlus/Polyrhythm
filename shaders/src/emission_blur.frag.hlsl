// Emission blur — stage 2 of the Bloom / Glow chain, run twice (horizontal, then vertical).
//
// One axis of a separable Gaussian over the emission field the extract stage wrote:
//
//   out = Σ w(k) · emission(base + k · step) · uInvNorm,   k ∈ [−K, K]
//   w(k) = exp(−k² · uInv2Sigma2)
//
// Two such passes compose into the 2-D Gaussian exactly, because the kernel is a product of its axes:
// Σ_dy w(dy) · [Σ_dx w(dx) · e(p + (dx,dy))] = Σ_dx Σ_dy w(dx)·w(dy) · e(p + (dx,dy)). Each pass applies
// uInvNorm once, so the pair applies the invNorm² a single 2-D gather would. The saving is the reason the
// chain exists: a radius-20 halo costs 2·41 = 82 taps per fragment instead of 41² = 1681.
//
// `step` is the per-tap offset in UV, so the caller chooses the sampling grid: one viewport pixel per tap
// on the full-resolution path, four on the quarter-resolution path (where uInv2Sigma2 is likewise resolved
// from radius/4, keeping the halo the same size in the image).
//
// uSnapW / uSnapH carry the viewport cell grid on the crisp evaluation path: taps walk from the fragment's
// viewport-cell center, so every output pixel of one cell sees the same blur and the halo stays crisp under
// upscale. Both 0 evaluates at the exact fragment position (the smooth path, and always the path taken at
// quarter resolution, where a viewport cell can be smaller than a texel). Off-frame taps clamp to the
// border texel (the pass sampler's CLAMP_TO_EDGE).
//
// The CPU mirror of the weights is retropp::gaussianKernelWeight / gaussianKernel, resolved for a pass by
// retropp::emissionChainPlan.
//
// SDL_GPU HLSL conventions: the fragment's sampled texture + sampler in space2, the uniform buffer in
// space3.

Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);

cbuffer EmissionBlurUniforms : register(b0, space3) {
    float uStepU;       // per-tap UV offset, u component (0 on the vertical pass)
    float uStepV;       // per-tap UV offset, v component (0 on the horizontal pass)
    float uTaps;        // K, per side (resolved on the CPU); 0 = a single center tap
    float uInvNorm;     // 1 / Σ w(k) over k ∈ [−K, K] — this axis's normalization  — register 0
    float uInv2Sigma2;  // 1 / (2σ²) for this pass's resolved σ
    float uSnapW;       // viewport cells across, or 0 for no snap
    float uSnapH;       // viewport cells down, or 0 for no snap
    float _pad0;        //                                                          — register 1
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float2 base = uv;
    if (uSnapW > 0.0f) base.x = (floor(uv.x * uSnapW) + 0.5f) / uSnapW;
    if (uSnapH > 0.0f) base.y = (floor(uv.y * uSnapH) + 0.5f) / uSnapH;

    int    K   = (int)uTaps;
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    [loop]
    for (int k = -K; k <= K; k++) {
        float w = exp(-((float)(k * k)) * uInv2Sigma2);
        sum += w * SourceTexture.Sample(SourceSampler,
                                        float2(base.x + (float)k * uStepU,
                                               base.y + (float)k * uStepV));
    }
    return sum * uInvNorm;
}
