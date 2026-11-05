// Halo composite — adds a sprite layer's whole-silhouette Bloom / Glow halos over the layer's content, one
// quad per field, in place. The quad covers the field's inner box; the source is the content as it stood
// before this pass, copied out of the target (a pass cannot read the texture it writes), and the halo is
// read from the field through the same rect-relative sample the art fragment uses for a region Bloom.
//
//   out.rgb = src.rgb + e.rgb
//   Bloom:  out.a = saturate(src.a + e.a · (1 − src.a))
//   Glow:   out.a = saturate(src.a + e.a)
//
// the arithmetic of retropp::emissionComposite. SDL_GPU HLSL conventions: sampled textures first (t0/s0
// the copied content, t1/s1 the emission atlas), then the storage texture (t2 the rect table); b0 in space3.
Texture2D<float4>      SourceTexture    : register(t0, space2);
SamplerState           SourceSampler    : register(s0, space2);
Texture2DArray<float4> uEmissionAtlas   : register(t1, space2);
SamplerState           uEmissionSampler : register(s1, space2);   // linear, CLAMP
Texture2D<float4>      uEmissionRects   : register(t2, space2);
cbuffer HaloCompositeUniforms : register(b0, space3) {
    float uComposeScale;
    float uEvalSnap;
    float _pad0;
    float _pad1;
};
static const float kEmissionSampleSteps = 256.0f;
#include "emission_field.hlsli"
float4 main(float2 uv : TEXCOORD0, nointerpolation float2 rowGlow : TEXCOORD1, float4 pos : SV_Position) : SV_Target0 {
    float4 src = SourceTexture.Sample(SourceSampler, uv);
    float4 e   = sampleEmissionField(pos.xy, rowGlow.x);
    float3 rgb = src.rgb + e.rgb;
    float  a   = rowGlow.y != 0.0f ? saturate(src.a + e.a) : saturate(src.a + e.a * (1.0f - src.a));
    return float4(rgb, a);
}
