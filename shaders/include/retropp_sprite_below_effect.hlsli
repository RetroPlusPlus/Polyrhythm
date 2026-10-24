// Below-custom preamble — the scene-facing counterpart to retropp_sprite_effect.hlsli. A generated
// sprite-below-custom variant (gen_shader.cmake SPRITE_BELOW mode) injects this at sprite_below.frag's
// `@retropp:sprite-below-custom-hook` so a game's custom body (main -> retroppCustomMain) runs to distort /
// grade the COMPOSITED SCENE beneath the sprite's layer, confined to the sprite silhouette (its art alpha
// coverage). Where the Layer-scope sprite preamble's sampleSource() reads the sprite's OWN art, here it reads
// the SCENE (SourceTexture — the accumulator the below pass binds) — so the SAME shader that is a frame /
// layer custom post-process works unchanged as a below-custom, its output confined to the silhouette and
// composited as a lens.
//
//   * sampleSource(uv) reads the scene at `uv` under the effect step's edge (Blank -> transparent outside the
//     scene, revealing the frame backdrop; Stretch -> clamp), mirroring retropp_effect.hlsli exactly. The
//     shader never decides the edge — the effect's `edge` does (the wrapper reads it off the record).
//   * Params are float-typed (the sprite-effect record store is a float texture; float values round-trip
//     bit-exact) — the same gate the Layer-scope sprite variant applies. A no-sprite / int-uint shader gets no
//     below-custom variant either.
//   * The per-row data table is unavailable (no row store is bound to the below fragment) — paramRow /
//     paramRowAtUv return 0, so a body that reads a table simply gets no contribution.
//
// SourceTexture / SourceSampler and uFxStore are already declared in sprite_below.frag; this block is injected
// after them, so sampleSource and the param loader resolve. The wrapper (generated below this include) sets
// the evaluation context — the viewport dims + the step's edge — before the body runs.

// Per-fragment evaluation context (set by the generated wrapper before the body runs; the standard preamble's
// retroppTrueUv / retroppEvalUv). On the Viewport grid the body evaluates at the snapped viewport-cell center
// and sampleSource() quantizes a requested displacement to whole viewport px, so the lens reproduces the
// composeScale = 1 rasterization; on the Output grid the true uv passes through.
static float2 retroppTrueUv;
static float2 retroppEvalUv;

// Engine-cbuffer stand-ins as sprite-below-path values, set by the wrapper before the body: the scene is a
// viewport-resolution image, so the crisp snap uses the viewport dims (uSnap on by default, mirroring the
// built-in below's per-cell scene math); uEdgeClamp comes from the effect step (0 = Blank, 1 = Stretch). No
// row table applies on the sprite path.
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
// SourceSampler come from sprite_below.frag. CPU mirror: customSampleSourceUv (postprocess.h).
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

// No per-row data table on the sprite path.
float4 paramRow(uint i) { return float4(0.0f, 0.0f, 0.0f, 0.0f); }
float4 paramRowAtUv(float2 uv) { return float4(0.0f, 0.0f, 0.0f, 0.0f); }
