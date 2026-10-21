// Shared blend operators for the engine's own shaders.
//
// Engine-internal header — reachable via #include because shaders/common is on the -I path of both HLSL
// frontends (glslang for SPIR-V / metallib, dxc for DXIL). NOT one of the shaders/include/*.hlsli game
// preambles: those are textually PREPENDED to a game-authored custom shader by gen_shader.cmake and are
// deliberately kept off the include path.
//
// Include guards rather than #pragma once — a plain guard is the one construct both frontends' C
// preprocessors are guaranteed to honour.

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

#endif  // RETROPP_COMMON_BLEND_OPS_HLSLI
