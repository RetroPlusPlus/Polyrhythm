// Per-pixel color operators shared by the sprite shaders.
//
// Engine-internal header — see blend_ops.hlsli for why shaders/common is separate from shaders/include.
// Self-contained: reads no shader-declared state.

#ifndef RETROPP_COMMON_SPRITE_COLOR_HLSLI
#define RETROPP_COMMON_SPRITE_COLOR_HLSLI

// Luminance-keyed diagonal sheen boost. Mirrors retropp::applyGleam (same op order, luma weights, 0.6 lift).
float3 applyGleam(float3 c, float u, float v, float4 gp) {
    float d    = u + v * gp.w;               // gp = (sweep, width, gain, slant)
    float ad   = abs(d - gp.x);
    float band = saturate(1.0f - ad / gp.y);
    float crest = band * band;
    float lum  = c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;
    float g    = gp.z * crest;
    float lift = lum * g * 0.6f;
    return c * (1.0f + g) + lift;
}

// Cross-channel desaturation — pull each channel toward the pixel's own luminance. Mirrors
// retropp::applySaturation (same op order, luma weights). sat == 1 is a byte-exact identity (amount == 0).
float3 applySaturation(float3 c, float sat) {
    float lum    = c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;
    float amount = 1.0f - sat;
    return c - (c - lum) * amount;
}

#endif  // RETROPP_COMMON_SPRITE_COLOR_HLSLI
