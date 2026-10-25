// Rect-instanced emission extract fragment shader (below-scope fields) — engine-internal.
//
// The scene-sourced sibling of emission_extract.frag. That one runs fullscreen for the frame-class chain;
// this one runs over ONE FIELD'S RECT in the emission atlas, so a layer's below lenses extract together in
// a single instanced draw and their fields sit in the same atlas the region path packs into.
//
// The rect is a VIEWPORT-grid image: an atlas texel is a viewport cell, and `offset` carries that texel
// back to the viewport position it holds (retropp::emissionRectEntry writes the same offset the read
// applies, so the texel a fragment reads is the texel this pass wrote). The accumulator is a
// COMPOSE-resolution image, so the cell's center scales up by uComposeScale and point-samples there — the
// analytic evaluation point, and the blur that follows band-limits whatever a point sample aliases.
//
// The whole rect is filled, margin ring included — NOT just the content box. A below halo is scene-sourced,
// so light just OUTSIDE a lens's quad is real light that belongs in its halo, and the ring is where a tap
// from the quad's edge lands. (The region path's raster leaves its ring at zero for the opposite reason:
// there the light is the sprite's own art, which does not extend past the quad.) The packer's margin is
// what keeps a tap inside the rect it started in, so a ring carrying this field's own light reaches no
// neighbor.
//
// The extract is NEUTRAL — intensity 1, white tint — because one field serves lenses of differing strength
// and color; each record applies its own as it samples. That is why neither appears here.
//
// SDL_GPU HLSL conventions: the fragment's sampled texture + sampler in space2, the uniform buffer in
// space3.
//   - t0 space2 : the accumulator (the scene beneath this layer), SAMPLED (nearest, CLAMP)
//   - s0 space2 : its sampler — CLAMP, so a rect hanging off the frame extends the border rather than
//                 punching a hole of black into the halo
//   - b0 space3 : compose scale

Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);

cbuffer EmissionExtractRectUniforms : register(b0, space3) {
    float uComposeScale;   // compose grid ÷ viewport — viewport cell → accumulator position
    float _pad0;
    float _pad1;
    float _pad2;
};

#include "emission_mask.hlsli"  // glowMask — the emission keying function

// ── Custom (Below-scope emission) hook ──────────────────────────────────────────────────────
//
// A Below-scope Custom lens that is an emission consumer authors its OWN field content: a generated
// <ns>_emission_rect variant (gen_shader.cmake EMISSION_RECT mode) replaces the marker below with the shader's
// `emission()` body + its scene-reading sampleSource (retropp_emission_rect_effect.hlsli) + a record-lane param
// loader, and #defines RETROPP_EMISSION_RECT_CUSTOM so main() dispatches to it. On the base (stock) pipeline
// this is a sentinel — the stock brightpass / glow path below runs instead, and `read.w` is the 0/1 Bloom/Glow
// flag; on a custom pipeline `read.w` carries the fx record ROW the wrapper loads params from. `sceneUv` is the
// scene position this rect texel holds, `viewportDim` the viewport size (the displacement quantization unit),
// `ri` the lens's Custom record row.
// @retropp:emission-rect-hook
#ifndef RETROPP_EMISSION_RECT_CUSTOM
float4 retroppEmissionRect(float2 sceneUv, float2 viewportDim, int ri) { return float4(0.0f, 0.0f, 0.0f, 0.0f); }
#endif

float4 main(nointerpolation float4 read : TEXCOORD0, float4 pos : SV_Position) : SV_Target0 {
    // The viewport cell this atlas texel holds, at its center (SV_Position is already texel-centered and the
    // offset is a whole number of texels), then up to the accumulator's grid.
    float2 composeDim;
    SourceTexture.GetDimensions(composeDim.x, composeDim.y);
    float2 cell   = pos.xy - read.xy;
    float2 sceneUv = cell * uComposeScale / composeDim;

#ifdef RETROPP_EMISSION_RECT_CUSTOM
    // The game shader authors this field: it reads the scene through sampleSource and its own params (loaded
    // from the record row `read.w` carries), returning the emission content for this texel.
    float2 viewportDim = composeDim / uComposeScale;
    return retroppEmissionRect(sceneUv, viewportDim, int(read.w));
#else
    float4 src     = SourceTexture.Sample(SourceSampler, clamp(sceneUv, float2(0.0f, 0.0f), float2(1.0f, 1.0f)));

    float threshold = read.z;
    if (read.w != 0.0f) {                      // Glow — a scalar coverage mask; the lens supplies the color
        float m = glowMask(src, threshold);
        return float4(m, m, m, m);
    }
    float lum = src.r * 0.299f + src.g * 0.587f + src.b * 0.114f;   // Bloom — the scene's own light
    float den = max(1.0f - threshold, 1.0f / 255.0f);
    return src * saturate((lum - threshold) / den);                 // the brightpass, retropp::applyBrightpass
#endif
}
