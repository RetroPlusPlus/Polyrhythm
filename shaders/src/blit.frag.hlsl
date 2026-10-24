// Blit fragment shader.
//
// Samples the internal viewport texture at the interpolated UV and writes it to the swapchain — a plain
// passthrough. Scaling/letterboxing is the pipeline viewport + scissor (set engine-side); nearest vs
// bilinear is the bound sampler (set engine-side at sampler creation). Whole-frame color (day/night,
// fades, flash, tints) is a screen-space effect (a ColorFill region with the blend mode and alpha the look
// wants), composited before this blit — the blit itself does no color.
//
// Authored to SDL_GPU's HLSL conventions (see SDL_CreateGPUShader docs): a fragment shader's sampled
// texture + sampler live in register space2.

Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return SourceTexture.Sample(SourceSampler, uv);
}
