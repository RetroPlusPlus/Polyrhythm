// Shared blend operators for the engine's own shaders.
//
// Engine-internal header — reachable via #include because shaders/common is on the -I path of both HLSL
// frontends (glslang for SPIR-V / metallib, dxc for DXIL). NOT one of the shaders/include/*.hlsli game
// preambles: those are textually PREPENDED to a game-authored custom shader by gen_shader.cmake and are
// deliberately kept off the include path.
//
// Include guards rather than #pragma once — a plain guard is the one construct both frontends' C
// preprocessors are guaranteed to honor.

#ifndef RETROPP_COMMON_BLEND_OPS_HLSLI
#define RETROPP_COMMON_BLEND_OPS_HLSLI

// The separable blend operator B(d, s) per BlendMode (mirror of retropp::blendChannel). Normal returns s.
float3 blendOp(uint mode, float3 d, float3 s) {
    if (mode == 1u) return d + s;                        // Add
    if (mode == 2u) return d - s;                        // Subtract
    if (mode == 3u) return d * s;                        // Multiply
    if (mode == 4u) return 1.0 - (1.0 - d) * (1.0 - s);  // Screen
    if (mode == 5u) return (d + s) * 0.5;                // Half
    if (mode == 6u) return (d - s) * 0.5;                // HalfSubtract
    return s;                                            // Normal
}

// Combine a straight source color `src` of alpha `sa` over a PREMULTIPLIED destination `dst` under `mode`, and
// return the result premultiplied (mirror of retropp::applyBlendMode). Normal is the alpha-over: it paints
// wherever the source has coverage. Every other mode reads its destination, so it grades the destination's
// straight color and keeps the destination's coverage — a transparent destination stays transparent. Over an
// opaque destination (the accumulator, the frame, a Below grade) premultiplied and straight coincide and the
// result is the plain separable blend.
float4 blendCombine(uint mode, float4 dst, float3 src, float sa) {
    if (mode == 0u) {
        return float4(saturate((1.0 - sa) * dst.rgb + sa * src), saturate(sa + dst.a * (1.0 - sa)));
    }
    float3 d      = dst.a > 0.0 ? dst.rgb / dst.a : float3(0.0, 0.0, 0.0);
    float3 graded = saturate((1.0 - sa) * d + sa * blendOp(mode, d, src));
    return float4(graded * dst.a, dst.a);
}

#endif  // RETROPP_COMMON_BLEND_OPS_HLSLI
