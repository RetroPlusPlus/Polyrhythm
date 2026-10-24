// Ferryman — the REALITY-WARP WAKE (shared by the ferry and the mutant, ONE shader with a switch).
// A moving thing leaves a trail of warped space behind it that refracts the WHOLE VIEW beneath it —
// an animated wavy displacement of the sampled image, like a heat-haze wake. The game runs this on
// a content-less layer JUST BELOW the moving sprite at BELOW scope (so it distorts everything under
// it, never the sprite itself), confined to a trailing capsule behind the mover via a Region.
//
// `warpChroma` is the switch: 0 = pure distortion (the ferry's clean wake); > 0 = ALSO cycle the
// hue of the refracted view, animated + position-varying, for the mutant's PSYCHEDELIC trail (a
// luma-preserving rotation about the gray axis, blended in by warpChroma). The engine injects the
// plumbing (sampleSource + the cbuffer packer); this file declares its own params + body.

cbuffer Params : register(b1, space3) {
    float warpPhase;        // animation phase (radians, tick-advanced) — the ripple crawls
    float warpAmp;          // displacement strength, UV units
    float warpFreq;         // wave frequency — cycles across the viewport
    float warpChroma;       // 0 = distortion only; > 0 = psychedelic hue-cycle amount (mutant)
    float warpChromaSpeed;  // how fast the hue cycles (multiplies the phase — lower = lazier)
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    // The distortion: sample the view a little away from where each pixel sits, so the world
    // beneath smears and swims. The Region confines this to the trail behind the mover.
    float2 duv;
    duv.x = sin(uv.y * warpFreq + warpPhase) * warpAmp;
    duv.y = cos(uv.x * warpFreq * 0.8f - warpPhase * 1.2f) * warpAmp * 0.7f;
    float4 c = sampleSource(uv + duv);

    if (warpChroma > 0.0f) {
        // Psychedelic hue cycle: rotate the sampled color about the gray axis by an angle that
        // sweeps with time and position — rainbow bands crawling down the trail.
        float  ang = warpPhase * warpChromaSpeed + (uv.x * 8.0f + uv.y * 6.0f);
        float  s   = sin(ang);
        float  cc  = cos(ang);
        const float3 k = float3(0.5773503f, 0.5773503f, 0.5773503f);  // gray axis, 1/sqrt(3)
        float3 rot = c.rgb * cc + cross(k, c.rgb) * s + k * dot(k, c.rgb) * (1.0f - cc);
        c.rgb = lerp(c.rgb, rot, warpChroma);
    }
    return c;
}
