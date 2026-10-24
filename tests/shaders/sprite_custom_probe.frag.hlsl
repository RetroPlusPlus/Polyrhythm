// Sprite-inline custom probe (sprite_effects_custom_test). Simple, exactly-computable math so a device
// capture can be checked against a CPU expected: multiply the sampled color by `tint` and add `lift`,
// keeping the sampled alpha. On a sprite, sampleSource(uv) reads the sprite's own art at uv, so the output
// is saturate(art.rgb * tint + lift) with the art's alpha. Float-only params, so the shader earns a sprite
// variant. The engine injects the plumbing; params live at b1/space3.

cbuffer Params : register(b1, space3) {
    float3 tint;   // per-channel multiply
    float  lift;   // per-channel add
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 c = sampleSource(uv);
    return float4(saturate(c.rgb * tint + lift), c.a);
}
