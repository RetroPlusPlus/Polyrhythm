// Emission extract — stage 1 of the Bloom / Glow chain.
//
// Computes the per-pixel EMISSION of the source into a scratch target: the light a pixel contributes to
// the halo, before any blur. Everything the developer authored except the reach folds in here — the
// threshold that keys what emits, the intensity that scales it, and (for Glow) the tint that colors it —
// so every downstream stage is parameter-free and one emission buffer can carry many emitters at once.
//
//   Bloom:  e = src · f(lum(src), threshold) · intensity          // the brightpass, full rgba
//           f(c) = saturate((lum − threshold) / max(1 − threshold, 1/255))    // Rec. 601 luminance
//   Glow:   m = mask(src, threshold);  e = (tint · m · intensity, m · intensity)
//           mask(c) = c.a · survive(lumStraight(c), threshold)     // scalar; survive = 1 at threshold 0
//
// Bloom radiates the source's OWN color, so its emission keeps the source chroma; Glow radiates a scalar
// mask times the authored tint, so no source hue ever enters its halo. Both write PREMULTIPLIED rgba —
// the alpha carries the halo's coverage, which the composite stage lifts onto the source.
//
// The extract always runs at full resolution, even when the blur that follows is downsampled: the
// threshold is a NONLINEAR key, so evaluating it on true pixels (rather than on an averaged, already-
// reduced image) is what keeps a lone bright pixel from either vanishing or flickering as it moves. Only
// the linear emission field is ever reduced.
//
// intensity 0 yields a zero emission, which the composite adds as nothing — the identity the whole chain
// preserves. The CPU mirror is retropp::emissionExtractBloom / emissionExtractGlow.
//
// SDL_GPU HLSL conventions: the fragment's sampled texture + sampler in space2, the uniform buffer in
// space3.

Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);

cbuffer EmissionExtractUniforms : register(b0, space3) {
    float uGlow;        // 0 = Bloom (brightpass, source chroma); 1 = Glow (scalar mask × tint)
    float uThreshold;   // emission floor, normalized 0..1
    float uIntensity;   // strength, normalized 0..1; 0 = a zero emission
    float _pad0;        //                                                                   — register 0
    float uTintR;       // Glow only: the authored tint (fill.r/255 · fillIntensity)
    float uTintG;
    float uTintB;
    float _pad1;        //                                                                   — register 1
};

#include "emission_mask.hlsli"  // glowMask — the emission keying function

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 src = SourceTexture.Sample(SourceSampler, uv);

    if (uGlow != 0.0f) {
        float m = glowMask(src, uThreshold) * uIntensity;
        return float4(uTintR * m, uTintG * m, uTintB * m, m);
    }

    float  lum    = src.r * 0.299f + src.g * 0.587f + src.b * 0.114f;
    float  den    = max(1.0f - uThreshold, 1.0f / 255.0f);
    float  f      = saturate((lum - uThreshold) / den);
    float4 bright = src * f;              // the brightpass, exactly retropp::applyBrightpass
    return bright * uIntensity;
}
