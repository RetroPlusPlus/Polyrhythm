// Sprite-path preamble — the sprite-variant counterpart to retropp_effect.hlsli. A generated sprite-custom
// variant (gen_shader.cmake SPRITE mode) injects this at sprite.frag's `@retropp:sprite-custom-hook` marker
// so a game's custom body (main → retroppCustomMain) runs INLINE in the sprite fragment — zero added render
// passes, N-flat in the sprite count. The custom body samples through sampleSource() and reads its own params
// exactly as on the layer path; the only differences the sprite path imposes are provided here:
//
//   * sampleSource(uv) reads the SPRITE'S OWN ART (the infinite transparent field via retroppSpriteArtSample),
//     not a composited frame. A Layer-scope custom effect transforms the sprite's own pixels; a custom that
//     must read the composited SCENE beneath the sprite is a Below-scope effect (a separate, scene-facing path).
//   * The art read is nearest at art-pixel granularity, so there is no compose-grid quantization — the engine
//     cbuffer values are sprite-path constants and paramRow / paramRowAtUv return 0 (no row table on the sprite
//     path). A shader that reads SourceTexture / RowDataTexture DIRECTLY (bypassing sampleSource) can't compile
//     as a sprite variant — those registers are the sprite's atlas / palette stores — so it declares
//     // @retropp:no-sprite and stays off the sprite path.
//   * Params are float-typed (float / float2 / float3 / float4); a shader whose cbuffer carries an int / uint
//     field gets no sprite variant (the sprite-effect store is a float texture; float values round-trip
//     bit-exact, arbitrary integer bit patterns risk denormal flush). Layer / Below use of that shader is
//     unaffected.
//
// The sprite context (gSprite*) this reads is published by sprite.frag's main() before the effect loop; this
// block is injected AFTER retroppSpriteArtSample so sampleSource resolves it.

// Per-fragment evaluation context (set by the generated wrapper before the body runs; the standard preamble's
// retroppTrueUv / retroppEvalUv). On the sprite path the body evaluates at the within-sprite quad coordinate
// directly (the art read is already nearest), so the two coincide.
static float2 retroppTrueUv;
static float2 retroppEvalUv;

// The edge sampleSource() obeys for the CURRENT custom step (from the effect's `edge`): 0 = Blank (transparent
// outside the art, the default), 1 = Stretch (clamp to the border texel). The wrapper reads it off the record.
static uint retroppSpriteEdgeStretch;

// Engine-cbuffer stand-ins (the standard preamble's RetroppEngineEffect fields) as sprite-path constants, so a
// body that references them compiles unchanged. No compose-grid snap and no row table apply on the sprite path.
static uint  uEdgeClamp    = 0u;
static uint  uRowTableY    = 0u;
static uint  uRowTableRows = 0u;
static uint  uSnap         = 0u;
static float uViewportW    = 0.0f;
static float uViewportH    = 0.0f;

// The center of the ART cell `uv` falls in (the sprite's own art-pixel grid) — the sprite-path analog of the
// standard preamble's retroppSnapToCellCenter (which snaps to the viewport grid). A body that snaps gets the
// sprite's art granularity. A non-positive dimension leaves that axis unchanged.
float2 retroppSnapToCellCenter(float2 uv) {
    int2  szc  = int2((int)(gSpritePackedSize >> 16), (int)(gSpritePackedSize & 0xFFFFu));
    float2 dims = float2(szc);
    float2 c    = uv;
    if (dims.x > 0.0f) c.x = (floor(uv.x * dims.x) + 0.5f) / dims.x;
    if (dims.y > 0.0f) c.y = (floor(uv.y * dims.y) + 0.5f) / dims.y;
    return c;
}

// THE sample function — the sprite's own art at `uv` under the current step's edge (Blank = transparent outside
// the art, the default; Stretch = clamp to the border). Nearest at art-pixel granularity. CPU mirror: the
// sprite path reads through retropp::retroppSpriteArtSample's equivalent (the sprite fragment's own read).
float4 sampleSource(float2 uv) {
    return retroppSpriteArtSample(uv, retroppSpriteEdgeStretch != 0u);
}

// The per-row data table is unavailable on the sprite path (no row store is bound to the sprite fragment) —
// the helpers return 0, so a body that reads a table simply gets no contribution.
float4 paramRow(uint i) { return float4(0.0f, 0.0f, 0.0f, 0.0f); }
float4 paramRowAtUv(float2 uv) { return float4(0.0f, 0.0f, 0.0f, 0.0f); }
