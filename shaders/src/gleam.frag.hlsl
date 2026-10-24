// Gleam post-process fragment — a BUILT-IN engine effect stage. A luminance-keyed diagonal sheen sweep:
// a soft band leaning along the axis d = uv.x + uv.y·slant multiplies each pixel by its OWN brightness at
// the band crest (plus a white lift scaled by that brightness), so bright content catches the light and
// dark stays dark — the marquee/logo "shine". The whole contribution scales by `gain`, so gain == 0 is an
// exact identity. The unit-tested CPU mirror is retropp::applyGleam (postprocess.h). Engine stage contract
// (identical to displace/ripple/colorfill): one sampled source in space2 (t0/s0), one uniform cbuffer in
// space3 (b0); the shared fullscreen-triangle postprocess.vert is the vertex stage.
Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);
cbuffer GleamUniforms : register(b0, space3) {
    float uSweep;   // band center along the slant axis, UV
    float uWidth;   // band half-width, UV (> 0)
    float uGain;    // sheen boost at the crest — 0 = identity
    float uSlant;   // diagonal lean of the band axis
};
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 c          = SourceTexture.Sample(SourceSampler, uv);
    const float d     = uv.x + uv.y * uSlant;
    const float band  = saturate(1.0f - abs(d - uSweep) / uWidth);
    const float crest = band * band;                 // soft shoulders, bright core
    const float lum   = dot(c.rgb, float3(0.299f, 0.587f, 0.114f));
    const float g     = uGain * crest;               // whole contribution scaled by gain → gain==0 is identity
    c.rgb = c.rgb * (1.0f + g) + lum * g * 0.6f;
    return c;                                         // the pixel's own alpha is kept
}
