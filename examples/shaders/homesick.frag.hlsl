// Homesick pixels — a deliberately USELESS custom effect (custom-stage demo). Every pixel slowly creeps toward
// the center of the screen and back by `amount` (the game oscillates it gently), so the whole image breathes
// inward and outward forever, for no reason (a slow zoom about the center). The engine injects the plumbing
// (retropp_effect.hlsli); this file declares its OWN param + body. It samples through sampleSource(), so when
// the breath pushes the sample past the frame edge it is BLANK by default (the effect's edge setting decides
// — not this shader). The game sets the param
// inline:
//   ScreenSpaceEffect{ .kind = ScreenSpaceEffectKind::Custom, .customShader = homesick, .amount = ... }

cbuffer Params : register(b1, space3) {
    float amount;   // creep toward (+) / away from (-) the center
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    const float2 center = float2(0.5f, 0.5f);
    return sampleSource(uv + amount * (center - uv));
}
