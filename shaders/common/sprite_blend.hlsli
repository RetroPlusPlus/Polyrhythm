// The per-channel blend operator and the over-composite built on it, shared by the sprite shaders.
//
// Engine-internal header — see blend_ops.hlsli for why shaders/common is separate from shaders/include.
// Self-contained: reads no shader-declared state.
//
// Distinct from blend_ops.hlsli's blendOp, which takes float3 and serves the frame-class passes. This is
// the scalar form the sprite path mirrors from retropp::blendChannel.

#ifndef RETROPP_COMMON_SPRITE_BLEND_HLSLI
#define RETROPP_COMMON_SPRITE_BLEND_HLSLI

// The separable blend operator B(d, s) per BlendMode (Normal 0 / Add 1 / Subtract 2 / Multiply 3 /
// Screen 4 / Half 5 / HalfSubtract 6). Mirrors retropp::blendChannel.
float blendChannel(uint mode, float d, float s) {
    if (mode == 1u) return d + s;
    if (mode == 2u) return d - s;
    if (mode == 3u) return d * s;
    if (mode == 4u) return 1.0f - (1.0f - d) * (1.0f - s);
    if (mode == 5u) return (d + s) * 0.5f;
    if (mode == 6u) return (d - s) * 0.5f;
    return s;  // Normal
}

// Combine src over dst under `mode`, source-alpha-weighted, standard over alpha. Mirrors applyBlendMode.
float4 applyBlendMode(float4 dst, float4 src, uint mode) {
    float sa = src.w;
    float3 b = float3(blendChannel(mode, dst.x, src.x),
                      blendChannel(mode, dst.y, src.y),
                      blendChannel(mode, dst.z, src.z));
    float3 rgb = saturate((1.0f - sa) * dst.xyz + sa * b);
    float a = saturate(sa + dst.w * (1.0f - sa));
    return float4(rgb, a);
}

#endif  // RETROPP_COMMON_SPRITE_BLEND_HLSLI
