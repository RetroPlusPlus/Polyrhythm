// Radial ripple post-process fragment shader — a BUILT-IN engine effect stage.
//
// The engine's second built-in screen-space effect (peer to displace.frag's RowDisplacement): a RADIAL,
// concentric ripple — a water droplet dropped into `center`, rings expanding outward. It does something
// the axis-aligned RowDisplacement cannot: each fragment's sample UV is displaced ALONG THE RADIUS from
// the center by a sine of the distance (so crests form rings), with the crest traveling outward as the
// game advances `uPhase`, and a gentle distance decay so it fades with radius like a real droplet. Aspect-
// corrected (the viewport need not be square) so the rings stay circular in screen space. The game names
// ScreenSpaceEffectKind::Ripple and sets the parameters (ScreenSpaceEffect::ripple) — no registration; the
// engine owns this shader, resolves the uniform from retropp::rippleParams, and binds the ripple_ pipeline.
//
// This is the byte-for-byte mirror of retropp::rippleSourceUv (postprocess.h) — the unit-tested CPU side.
// Engine stage fragment CONTRACT (identical to displace.frag): one sampled source texture + sampler in
// space2 (t0/s0 — the composited viewport, or the prior chain pass), and one uniform cbuffer in space3
// (b0). The engine's shared fullscreen-triangle postprocess.vert is the vertex stage.

Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);

cbuffer RippleUniforms : register(b0, space3) {
    float uCenterX;       // ripple center, UV
    float uCenterY;
    float uAmplitude;     // displacement magnitude, viewport pixels
    float uFrequency;     // ring count across the distance field   — register 0
    float uPhase;         // expansion phase (game-advanced, slow)
    float uInvViewportW;  // 1 / viewport width  (amplitude px→UV, x)
    float uInvViewportH;  // 1 / viewport height (amplitude px→UV, y; also the aspect via H/W)
    float uDecay;         // radial falloff rate                    — register 1
    float uSnap;          // 1 = snap to the viewport grid (crisp); 0 = per-output-pixel (smooth) — register 2
    float _pad0; float _pad1; float _pad2;
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    const float kTwoPi = 6.283185307179586f;

    const float2 center = float2(uCenterX, uCenterY);

    // Crisp evaluation (uSnap): evaluate the ripple at the fragment's viewport-cell center and quantize the
    // resulting per-axis offset to whole viewport pixels (round-half-up), then displace the UNSNAPPED uv.
    // Under the pass's nearest sampler this reproduces the viewport-resolution rasterization exactly (a no-op
    // at compose scale 1); at a finer compose grid it copies each source cell's interior intact. HLSL round()
    // is round-to-even and would break scale-1 parity at .5 ties, so floor(v + 0.5) is used. uSnap 0 (Output
    // grid) keeps the shipped continuous per-output-pixel displacement, unchanged.
    float2 evalUv = uv;
    if (uSnap != 0.0f) {
        float vpW = uInvViewportW > 0.0f ? 1.0f / uInvViewportW : 0.0f;
        float vpH = uInvViewportH > 0.0f ? 1.0f / uInvViewportH : 0.0f;
        if (vpW > 0.0f) evalUv.x = (floor(uv.x * vpW) + 0.5f) / vpW;
        if (vpH > 0.0f) evalUv.y = (floor(uv.y * vpH) + 0.5f) / vpH;
    }
    const float2 delta = evalUv - center;

    // Aspect-correct the distance so the rings are circular in screen space (a 160-wide / 144-tall
    // viewport would otherwise make ellipses): scale x by aspect = W/H = invH/invW.
    const float  aspect    = uInvViewportH / uInvViewportW;
    const float2 corrected = float2(delta.x * aspect, delta.y);
    const float  dist      = length(corrected);

    // Expanding ring: the crest travels outward as uPhase grows; exp(-decay·dist) fades it with radius.
    const float wave   = sin(kTwoPi * (uFrequency * dist - uPhase));
    const float env    = exp(-uDecay * dist);
    const float offset = uAmplitude * wave * env;  // viewport pixels

    // Displace the sample radially (outward from the center), amplitude px→UV per axis.
    const float2 dir = dist > 1e-5f ? (delta / dist) : float2(0.0f, 0.0f);
    float2 src;
    if (uSnap != 0.0f) {
        src = uv + float2(floor(dir.x * offset + 0.5f) * uInvViewportW,
                          floor(dir.y * offset + 0.5f) * uInvViewportH);
    } else {
        src = uv + dir * float2(offset * uInvViewportW, offset * uInvViewportH);
    }

    return SourceTexture.Sample(SourceSampler, src);
}
