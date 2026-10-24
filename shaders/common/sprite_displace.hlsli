// The coordinate re-read kinds in the sprite's OWN art space — RowDisplacement, Ripple, Swirl.
//
// Engine-internal header — see blend_ops.hlsli for why shaders/common is separate from shaders/include.
//
// REQUIRES rounding.hlsli (roundHalfUp) to have been included first. Reads no shader-declared state
// otherwise — the art size arrives as `dims`.
//
// A sprite displacement is in the sprite's own art px, and the read snaps to art cells so a row/column
// shifts as one, crisp. Mirrors retropp::displaceSourceUv / rippleSourceUv with the art size as the
// normalization. The scene-space siblings in sprite_below.frag are separate functions on purpose: they
// normalize by the viewport, not the art.

#ifndef RETROPP_COMMON_SPRITE_DISPLACE_HLSLI
#define RETROPP_COMMON_SPRITE_DISPLACE_HLSLI

// The center of the art cell a within-sprite coordinate falls in — the point the wave is evaluated at, so all
// pixels of one art row/column share a displacement (mirrors snapUvToCellCenter with dims = art size).
float2 snapArt(float2 uv, float2 dims) {
    return float2((floor(uv.x * dims.x) + 0.5f) / dims.x, (floor(uv.y * dims.y) + 0.5f) / dims.y);
}

// RowDisplacement: the modulated axis offsets by amplitude·sin(2π(freq·otherAxis + phase)), quantized to whole
// art px. params = (amplitude, frequency, phase, axis: 0 Horizontal / 1 Vertical). amplitude 0 ⇒ identity.
float2 spriteDisplace(float2 uv, float4 params, float2 dims) {
    if (params.x == 0.0f) return uv;
    const float kTwoPi = 6.283185307179586f;
    float2 e = snapArt(uv, dims);
    if ((uint)params.w == 0u) {  // Horizontal: offset in u, wave over v
        float s   = sin(kTwoPi * (params.y * e.y + params.z));
        float off = roundHalfUp(params.x * s) / dims.x;
        return float2(uv.x + off, uv.y);
    }
    float s   = sin(kTwoPi * (params.y * e.x + params.z));  // Vertical: offset in v, wave over u
    float off = roundHalfUp(params.x * s) / dims.y;
    return float2(uv.x, uv.y + off);
}

// Ripple: a radial re-read pushed along the radius from `center` (art px, in gate.yz) by
// amplitude·sin(2π(freq·dist − phase))·exp(−decay·dist), quantized to whole art px. params = (amplitude,
// frequency, phase, _); gate.w = decay. amplitude 0 or the center pixel ⇒ identity.
float2 spriteRipple(float2 uv, float4 params, float4 gate, float2 dims) {
    if (params.x == 0.0f) return uv;
    const float kTwoPi = 6.283185307179586f;
    float invW = 1.0f / dims.x, invH = 1.0f / dims.y;
    float cu = gate.y * invW, cv = gate.z * invH;   // center art px → within-sprite uv
    float2 e   = snapArt(uv, dims);
    float  dx  = e.x - cu, dy = e.y - cv;
    float  cx  = dx * (invH / invW);                // aspect-correct so the rings stay circular in art space
    float  dist = sqrt(cx * cx + dy * dy);
    if (dist <= 1e-5f) return uv;                   // the center has no radial direction
    float  wave   = sin(kTwoPi * (params.y * dist - params.z));
    float  env    = exp(-gate.w * dist);
    float  offset = params.x * wave * env;          // art px
    return float2(uv.x + roundHalfUp(dx / dist * offset) * invW,
                  uv.y + roundHalfUp(dy / dist * offset) * invH);
}

// Swirl: an angular re-read rotating the sample about `center` (art px, in gate.yz) by twist·(1 − t²)²,
// t = dist/radius — the full turn at the center easing to none at the rim. params = (twist, radius, _, _);
// `twist` arrives already resolved to RADIANS and signed (swirlParams does the degrees conversion), and
// `radius` is in art px. Working space is art px (square units, so the disc stays circular). The evaluation
// point snaps to the art cell center; the read itself is the exact rotated position (nearest sampling makes
// it crisp). twist 0, radius ≤ 0, or a fragment at or beyond the rim ⇒ identity, its own coordinate exactly.
float2 spriteSwirl(float2 uv, float4 params, float4 gate, float2 dims) {
    if (params.x == 0.0f || params.y <= 0.0f) return uv;
    float2 e = snapArt(uv, dims) * dims;             // evaluate from the art cell center, in art px
    float2 c = float2(gate.y, gate.z);
    float2 d = e - c;
    float  r = length(d);
    if (r >= params.y) return uv;                    // outside the disc: its own coordinate
    float  t     = r / params.y;
    float  f     = 1.0f - t * t;
    float  theta = params.x * f * f;
    float  s, cs;
    sincos(theta, s, cs);
    float2 rd = float2(cs * d.x - s * d.y, s * d.x + cs * d.y);
    return (c + rd) / dims;
}

#endif  // RETROPP_COMMON_SPRITE_DISPLACE_HLSLI
