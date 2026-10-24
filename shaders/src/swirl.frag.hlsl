// Swirl — an angular twist about a center, one pass. The whirlpool / vortex / drain: content inside the
// disc rotates about `center`, most at the center and easing to nothing at the rim; content outside the
// disc is untouched. Ripple's angular sibling — where Ripple pushes the sample along the radius
// (concentric rings), Swirl carries it around the center (rotation). A displacing re-read: one sample per
// pixel at the rotated position.
//
//   d       = px − center                       // the fragment's offset from the center, viewport px
//   t       = |d| / radius                      // normalized distance; ≥ 1 = outside the disc
//   θ(t)    = twist · (1 − t²)²                 // the twist angle: full at the center, smoothly 0 at the
//                                               //   rim (zero slope at both ends — no crease, no seam)
//   out     = src(center + rotate(d, θ))        // read the source at the rotated offset
//
// `twist` is the total angle in RADIANS (the CPU resolves amplitude + phase into it — the game advances
// `phase` per frame to spin the vortex). Positive twist rotates the read offset clockwise on screen
// (y-down, +x toward +y), so the visible content turns counter-clockwise; negate for the other direction.
// The exact center is a fixed point (rotating a zero offset moves nothing). twist 0 or radius ≤ 0 is a
// byte-exact identity (early-out passthrough), and every pixel outside the disc is byte-identical always
// (sampled at its own exact coordinate).
//
// Working space is viewport PIXELS (uv ÷ the inverse viewport dimensions) — square units, so the disc
// stays circular on any aspect. Under uSnap (the crisp Viewport grid) the twist evaluates from the
// fragment's viewport-cell center, so every output pixel of one viewport cell reads the same source;
// uSnap 0 (the Output grid) evaluates at the exact fragment position for a smooth vortex. A read that
// leaves the frame (a disc crossing the border) clamps to the border texel (the pass sampler's
// CLAMP_TO_EDGE), matching Ripple. The CPU mirror is retropp::swirlReadPx / swirlParams.
//
// SDL_GPU HLSL conventions: the fragment's sampled texture + sampler in space2, the uniform buffer in
// space3.

Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);

cbuffer SwirlUniforms : register(b0, space3) {
    float uCenterX;       // swirl center, viewport px
    float uCenterY;       //
    float uTwist;         // total twist angle at the center, radians (amplitude + phase, CPU-resolved)
    float uRadius;        // disc radius, viewport px; ≤ 0 = identity                          — register 0
    float uInvViewportW;  // 1 / viewport width
    float uInvViewportH;  // 1 / viewport height
    float uSnap;          // 1 = evaluate from the viewport-cell center (crisp); 0 = per output pixel
    float _pad0;          //                                                                   — register 1
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    if (uTwist == 0.0f || uRadius <= 0.0f) return SourceTexture.Sample(SourceSampler, uv);

    float2 base = uv;
    if (uSnap != 0.0f) {
        float vpW = uInvViewportW > 0.0f ? 1.0f / uInvViewportW : 0.0f;
        float vpH = uInvViewportH > 0.0f ? 1.0f / uInvViewportH : 0.0f;
        if (vpW > 0.0f) base.x = (floor(uv.x * vpW) + 0.5f) / vpW;
        if (vpH > 0.0f) base.y = (floor(uv.y * vpH) + 0.5f) / vpH;
    }

    float2 px = float2(base.x / uInvViewportW, base.y / uInvViewportH);
    float2 d  = px - float2(uCenterX, uCenterY);
    float  r  = length(d);
    if (r >= uRadius) return SourceTexture.Sample(SourceSampler, uv);  // outside the disc: byte-identical

    float t = r / uRadius;
    float f = 1.0f - t * t;
    f = f * f;                                  // (1 − t²)² — smooth at the center AND the rim
    float theta = uTwist * f;
    float s, c;
    sincos(theta, s, c);
    float2 rd = float2(c * d.x - s * d.y, s * d.x + c * d.y);
    float2 sp = float2(uCenterX, uCenterY) + rd;
    return SourceTexture.Sample(SourceSampler,
                                float2(sp.x * uInvViewportW, sp.y * uInvViewportH));
}
