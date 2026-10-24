// The prepared-emission-field read: a rect on the layer's paged atlas, sampled rect-relative.
//
// Engine-internal header — see blend_ops.hlsli for why shaders/common is separate from shaders/include.
//
// REQUIRES the includer to have already declared uEmissionRects, uEmissionAtlas, uEmissionSampler,
// uComposeScale, uEvalSnap and kEmissionSampleSteps, so this header is included AFTER the shader's
// resource and cbuffer declarations.
//
// This is the GPU mirror of retropp::emissionFieldSamplePoint (postprocess.h), which is the CPU
// authority — the two must move together.

#ifndef RETROPP_COMMON_EMISSION_FIELD_HLSLI
#define RETROPP_COMMON_EMISSION_FIELD_HLSLI

// On the Viewport grid the point snaps to the cell center, so the read is crisp; on the Output grid it stays
// continuous and the halo resolves smoothly between stored texels. Quantizing to kEmissionSampleSteps per
// texel makes the crisp case exact on every backend rather than dependent on each one's subtexel precision;
// round-half-up is the form that matches the CPU mirror, because HLSL round() is round-to-even and would
// disagree at a tie.
//
// The point is held inside the field's content box grown by one texel: the tap at the quad's edge draws from
// that ring, and emissionMargin sizes the rect so the ring holds this field's own light and no neighbor's. The
// clamp also covers the one degenerate case — if the atlas could not be allocated the layer binds the 1×1
// stand-in and a one-row table, whose zeroes collapse every read to a single transparent texel, so a step that
// named a field adds no light instead of reading somewhere arbitrary.
float4 sampleEmissionField(float2 pos, float index) {
    int    row  = max((int)index, 0);
    float4 head = uEmissionRects.Load(int3(0, row, 0));   // (offsetX, offsetY, innerX, innerY)
    float4 span = uEmissionRects.Load(int3(1, row, 0));   // (innerW, innerH, sheet, _)
    float2 p    = pos / uComposeScale;
    if (uEvalSnap > 0.0f) p = floor(p) + 0.5f;            // the viewport cell center — the crisp read
    p += float2(head.x, head.y);
    p = floor(p * kEmissionSampleSteps + 0.5f) / kEmissionSampleSteps;
    float2 lo = float2(head.z, head.w) - 0.5f;
    float2 hi = float2(head.z + span.x, head.w + span.y) + 0.5f;
    p = clamp(p, lo, hi);
    uint   w, h, sheets, levels;
    uEmissionAtlas.GetDimensions(0u, w, h, sheets, levels);
    float  sheet = clamp(span.z, 0.0f, (float)sheets - 1.0f);
    float2 uv    = p / float2((float)w, (float)h);
    return uEmissionAtlas.SampleLevel(uEmissionSampler, float3(uv, sheet), 0.0f);
}

#endif  // RETROPP_COMMON_EMISSION_FIELD_HLSLI
