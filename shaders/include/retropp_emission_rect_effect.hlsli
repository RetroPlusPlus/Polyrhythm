// Emission-rect preamble — the below WRITE counterpart to retropp_sprite_below_effect.hlsli. A generated
// <ns>_emission_rect variant (gen_shader.cmake EMISSION_RECT mode) injects this at emission_extract_rect.frag's
// `@retropp:emission-rect-hook`, so an emission-declared game shader's `float4 emission(float2 uv)` body runs
// to AUTHOR a Below-scope Custom lens's field over the field's rect. Where retropp_sprite_below_effect's
// sampleSource() reads the scene for a lens's color, here it reads the scene for the lens's EMISSION content —
// the SAME body a frame-class emission extract runs (gen_shader EMISSION_EXTRACT), just over a rect.
//
//   * sampleSource(uv) reads the SCENE (SourceTexture — the accumulator this extract pass binds) under the
//     effect step's edge (Blank -> transparent outside the scene; Stretch -> clamp), quantizing a requested
//     displacement to whole viewport px on the Viewport grid, mirroring retropp_effect.hlsli exactly. The
//     shader never decides the edge — the effect step's `edge` does (the wrapper reads it off the record).
//   * Params are float-typed (the sprite-effect record store is a float texture) — the same gate the sprite
//     variants apply. The body's params load from the fx record lanes (register k at texel 2 + k).
//   * The per-row data table is unavailable on this path — paramRow / paramRowAtUv return 0.
//
// SourceTexture / SourceSampler are declared in emission_extract_rect.frag (t0/s0 space2); this preamble adds
// uFxStore, the ONE storage texture the custom rect pipeline binds (the sprite-effect records), at t1 space2.

Texture2D<float4> uFxStore : register(t1, space2);   // the below Custom lens's effect record (its packed params)

// Per-fragment evaluation context (set by the generated wrapper before the body runs; the standard preamble's
// retroppTrueUv / retroppEvalUv). On the Viewport grid the body evaluates at the snapped viewport-cell center
// and sampleSource() quantizes a requested displacement to whole viewport px; on the Output grid it passes
// through.
static float2 retroppTrueUv;
static float2 retroppEvalUv;

// Engine-cbuffer stand-ins as emission-rect values, set by the wrapper before the body: the scene is a
// viewport-resolution image, so the crisp snap uses the viewport dims (uSnap on by default, mirroring the stock
// rect extract's per-cell scene math); uEdgeClamp comes from the effect step (0 = Blank, 1 = Stretch). No row
// table applies on this path.
static uint  uSnap         = 1u;
static uint  uEdgeClamp    = 0u;
static float uViewportW    = 0.0f;
static float uViewportH    = 0.0f;
static uint  uRowTableY    = 0u;
static uint  uRowTableRows = 0u;

// The center of the viewport cell `uv` falls in — the crisp evaluation point (mirrors retropp_effect.hlsli's
// retroppSnapToCellCenter). A non-positive dimension leaves that axis unchanged.
float2 retroppSnapToCellCenter(float2 uv) {
    float2 c = uv;
    if (uViewportW > 0.0f) c.x = (floor(uv.x * uViewportW) + 0.5f) / uViewportW;
    if (uViewportH > 0.0f) c.y = (floor(uv.y * uViewportH) + 0.5f) / uViewportH;
    return c;
}

// THE sample function — the SCENE at `uv` under the step's edge, quantizing a requested displacement to whole
// viewport px on the Viewport grid (the exact op order of retropp_effect.hlsli's sampleSource). SourceTexture /
// SourceSampler come from emission_extract_rect.frag. CPU mirror: customSampleSourceUv (postprocess.h).
float4 sampleSource(float2 uv) {
    if (uSnap != 0u && uViewportW > 0.0f && uViewportH > 0.0f) {
        float2 offPx = (uv - retroppEvalUv) * float2(uViewportW, uViewportH);
        float2 qPx   = float2(floor(offPx.x + 0.5f), floor(offPx.y + 0.5f));
        uv = retroppTrueUv + qPx / float2(uViewportW, uViewportH);
    }
    if (uEdgeClamp == 0u && (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f))
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    return SourceTexture.Sample(SourceSampler, uv);  // in-bounds, OR Stretch -> CLAMP_TO_EDGE
}

// No per-row data table on this path.
float4 paramRow(uint i) { return float4(0.0f, 0.0f, 0.0f, 0.0f); }
float4 paramRowAtUv(float2 uv) { return float4(0.0f, 0.0f, 0.0f, 0.0f); }
