// ColorSaturation post-process fragment — a BUILT-IN engine effect stage. A cross-channel color grade: each
// pixel is pulled toward its own luminance, so uSaturation == 1 is an exact identity, 0 is grayscale, and
// values between desaturate. The identity is byte-exact — at uSaturation == 1 the amount is 0, so the
// multiply-by-zero + subtract-zero returns the source color unchanged (the same structural exactness Gleam's
// gain == 0 has). The unit-tested CPU mirror is retropp::applySaturation (postprocess.h), sharing Gleam's
// Rec. 601 luma weights so the engine has one luminance authority. Engine stage contract (identical to
// gleam/colorfill): one sampled source in space2 (t0/s0), one uniform cbuffer in space3 (b0); the shared
// fullscreen-triangle postprocess.vert is the vertex stage.
Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);
cbuffer SaturationUniforms : register(b0, space3) {
    float uSaturation;   // 0 = grayscale, 1 = identity (normalized from the developer's uint8 field ÷255)
};
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 c           = SourceTexture.Sample(SourceSampler, uv);
    const float lum    = dot(c.rgb, float3(0.299f, 0.587f, 0.114f));
    const float amount = 1.0f - uSaturation;           // 0 at identity
    c.rgb = c.rgb - (c.rgb - lum) * amount;            // amount == 0 → c.rgb unchanged (exact identity)
    return c;                                          // the pixel's own alpha is kept
}
