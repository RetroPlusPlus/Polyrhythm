// Blend composite post-process fragment shader.
//
// Composites a container's isolated render `src` over what it sits on, `dst` (the accumulator), under a
// BlendMode. The programmable peer of the fixed-function premultiplied-over composite: where the over
// path can only alpha-blend, this evaluates the separable blend operator the container selects (Add /
// Subtract / Multiply / Screen / Half, plus Normal). It samples both operands, writes the full blended
// RGBA (the pass REPLACES its target — the caller swaps it into the accumulator).
//
// `src` arrives PREMULTIPLIED: the container renders alone into a transparent scratch, so its rgb holds
// color·alpha and its alpha holds the coverage. The operator B(dst, color) takes the STRAIGHT source
// color, so the shader un-premultiplies (rgb / a) before evaluating it, then applies alpha once as the
// weight. `dst` is premultiplied too: the opaque accumulator, or the transparent scratch of a layer that
// composites on its own, whose empty area a mode other than Normal leaves empty. The result matches
// retropp::applyBlendMode(dst, {color, a}, mode) — the CPU authority over straight-color operands — through
// blendCombine.
//
// SDL_GPU HLSL conventions: fragment sampled textures + samplers in space2 (t0/s0 = dst accumulator,
// t1/s1 = src container render); the uniform buffer in space3.

Texture2D<float4> DstTexture : register(t0, space2);
SamplerState      DstSampler : register(s0, space2);
Texture2D<float4> SrcTexture : register(t1, space2);
SamplerState      SrcSampler : register(s1, space2);

cbuffer BlendUniforms : register(b0, space3) {
    float4 uBlend;  // x = blend mode (BlendMode as float, rounded to uint); yzw unused
};

#include "blend_ops.hlsli"  // blendCombine — the BlendMode combine, mirror of retropp::applyBlendMode

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 dst = DstTexture.Sample(DstSampler, uv);
    float4 src = SrcTexture.Sample(SrcSampler, uv);

    uint  mode = (uint)(uBlend.x + 0.5);
    float sa   = src.a;
    // src is premultiplied — recover the straight source color for the operator (sa == 0 ⇒ no coverage).
    float3 sc  = sa > 0.0 ? src.rgb / sa : float3(0.0, 0.0, 0.0);
    // At sa == 1, sc == src.rgb, so full-coverage composites equal the premultiplied form; Normal reduces to
    // the exact premultiplied-over equation.
    return blendCombine(mode, dst, sc, sa);
}
