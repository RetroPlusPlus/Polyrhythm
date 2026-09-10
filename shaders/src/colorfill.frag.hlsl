// ColorFill post-process fragment — a BUILT-IN engine effect stage. A fill is a pure source colour, so
// the stage emits it directly: out = fill, opaque. A Region confines it to a shape, so the colour FILLS
// that shape (a stroked region → a coloured line/path, a filled region → a solid shape); the region gate
// does the confining, and the region's alpha and blend mode compose it over the scene. The unit-tested
// CPU mirror is retropp::applyColorFill (postprocess.h). Engine stage contract: one uniform cbuffer in
// space3 (b0); shared postprocess.vert.
cbuffer ColorFillUniforms : register(b0, space3) {
    float3 uFill; float uPad;  // register 0 — fill colour (rgb), normalized
};
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return float4(uFill, 1.0);
}
