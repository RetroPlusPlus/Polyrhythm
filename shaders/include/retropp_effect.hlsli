// Standard preamble PREPENDED to every game-authored custom post-process fragment. It declares
// the engine plumbing — the composited source frame + the engine-controlled edge mode. A custom shader
// declares its OWN parameter cbuffer (its own named fields, at b1/space3) and writes
//   float4 main(float2 uv : TEXCOORD0) : SV_Target0 { ... }
// using sampleSource() + its own params. The game sets those params as inline named fields on the
// ScreenSpaceEffect (.kind = Custom, .customShader = <path handle>, .<param> = ...), exactly like a
// built-in effect; the build reads the shader's cbuffer and surfaces those names. Low-friction by design:
// drop in a .hlsl with a cbuffer + body, reference its path, set params inline — nothing else.
//
//   sampleSource(uv)  — THE sample function. Inside [0,1] it samples the composited frame (or the prior
//                       chain pass). OUTSIDE [0,1] its behavior is the EFFECT's edge setting, NOT the
//                       shader's choice: ScreenSpaceEffect::edge == Blank (the default) returns BLANK
//                       (transparent — the backdrop / layers below reveal through, never a smeared edge);
//                       edge == Stretch clamps (smears the border). A layer that doesn't want clamping
//                       never gets it — the same edge rule the engine's built-in effects obey. Custom
//                       shaders should ALWAYS sample through this, not SourceTexture directly.
//
// Evaluation grid: on the Viewport grid (uSnap, the crisp default) the generated entry point hands the
// shader's main() the CENTER of the viewport cell its fragment falls in, and sampleSource() quantizes the
// requested displacement to whole viewport pixels — so the shader's spatial math and sampling reproduce
// the viewport-resolution rasterization exactly, nearest-upscaled, with no change to the shader itself.
// On the Output grid (uSnap == 0) main() receives the fragment's true uv and sampleSource() samples the
// requested coordinate directly — smooth output-resolution evaluation, wholesale.

// Engine-filled (b0): values the engine controls for every effect. uEdgeClamp is the edge mode for
// sampleSource (0 = Blank, the default; 1 = Stretch/clamp). uRowTableY / uRowTableRows locate this
// effect's per-row data table in the shared data store (see RowDataTexture below); uRowTableRows == 0
// means the effect carries no table. uSnap selects the evaluation grid (1 = Viewport/crisp, 0 = Output/
// smooth); uViewportW/uViewportH are the logical viewport dimensions the snap math needs.
cbuffer RetroppEngineEffect : register(b0, space3) {
    uint  uEdgeClamp;
    uint  uRowTableY;
    uint  uRowTableRows;
    uint  uSnap;
    float uViewportW;
    float uViewportH;
    float uEnginePad0;
    float uEnginePad1;
};

// Per-fragment evaluation context, set by the generated entry point before the shader's main() runs.
// retroppTrueUv is the fragment's actual uv; retroppEvalUv is what main() received (the viewport-cell
// center on the Viewport grid, the true uv on the Output grid). sampleSource() reads both to quantize a
// requested displacement relative to the evaluation point.
static float2 retroppTrueUv;
static float2 retroppEvalUv;

// The center of the viewport cell `uv` falls in: (floor(uv·dim) + 0.5) / dim per axis — the exact point
// the composeScale = 1 fragment evaluates at, so per-cell math matches the viewport-resolution
// rasterization. A non-positive dimension leaves that axis unchanged. CPU mirror: snapUvToCellCenter
// (postprocess.h).
float2 retroppSnapToCellCenter(float2 uv) {
    float2 c = uv;
    if (uViewportW > 0.0f) c.x = (floor(uv.x * uViewportW) + 0.5f) / uViewportW;
    if (uViewportH > 0.0f) c.y = (floor(uv.y * uViewportH) + 0.5f) / uViewportH;
    return c;
}

Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);

// Sample the composited source with the EFFECT's edge policy. Blank (default) → transparent outside the
// frame; Stretch (uEdgeClamp == 1) → clamp (CLAMP_TO_EDGE smears the border). The shader never decides
// this — the layer/effect does.
//
// On the Viewport grid (uSnap) the displacement the shader asked for — the requested coordinate relative
// to the evaluation point main() received — is quantized to whole viewport pixels with round-half-up
// (floor(v + 0.5); HLSL round() is round-to-even and breaks scale-1 parity at .5 ties) and applied to the
// fragment's TRUE uv. Per viewport cell the offset is constant, so each source cell's output-resolution
// interior is copied intact (sub-pixel-placed content survives the effect), and at scale 1 the sampled
// texel is unchanged. The edge policy applies to the final coordinate on both grids. CPU mirror:
// customSampleSourceUv (postprocess.h).
float4 sampleSource(float2 uv) {
    if (uSnap != 0u && uViewportW > 0.0f && uViewportH > 0.0f) {
        float2 offPx = (uv - retroppEvalUv) * float2(uViewportW, uViewportH);
        float2 qPx   = float2(floor(offPx.x + 0.5f), floor(offPx.y + 0.5f));
        uv = retroppTrueUv + qPx / float2(uViewportW, uViewportH);
    }
    if (uEdgeClamp == 0u && (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return SourceTexture.Sample(SourceSampler, uv);  // in-bounds, OR Stretch → CLAMP_TO_EDGE
}

// Emission consumer (an `// @retropp:emission`-declared stage): the engine prepares this stage's blurred
// emission field and binds it as a SAMPLED texture at t1/s1 (the second sampler, after SourceTexture at
// t0/s0), so the shader's main() may read it through sampleEmission(). The rect / scratch translation is
// hidden here exactly as sampleSource() hides edge policy and crisp quantization — the shader writes a uv
// (its own sampling space) and never learns there is a translation. The declaration is compiled by
// prepending `#define RETROPP_EMISSION`; undeclared stages take the #else branch below, which is
// byte-identical to the historical single-sampler layout (RowDataTexture at t1), so their bytecode never
// moves. The emission blur runs before this fragment ever executes — a stage obtains its blur by DECLARING
// its reach (`.radius`), never by computing one in the body.
#ifdef RETROPP_EMISSION
Texture2D<float4> EmissionTexture : register(t1, space2);
SamplerState      EmissionSampler : register(s1, space2);
// The blurred emission the engine prepared for this stage, at `uv` in the shader's own sampling space
// (the screen uv at a fullscreen site). saturate() clamps a stray sample to the field's edge — the field
// covers the viewport, so an out-of-range read has no defined content of its own.
float4 sampleEmission(float2 uv) { return EmissionTexture.Sample(EmissionSampler, saturate(uv)); }
// The row-data store follows the two sampled textures (t0, t1) at the first storage slot, t2, for these
// variants only — the SDL_GPU convention: sampled textures first, then storage textures, numbered
// sequentially.
Texture2D<float4> RowDataTexture : register(t2, space2);
#else
// A read-only per-frame data table the effect samples by row — an arbitrary float4 array the game fills
// each frame (a per-scanline value, or a per-region value indexed by id). Bound for the custom path; an
// effect with no table forwards uRowTableRows == 0 and the helpers return 0 (no contribution). Storage
// texture (integer Load, no sampler) — the same delivery as the tile / atlas / palette stores. It is the
// fragment stage's storage texture, so it follows the sampled SourceTexture (t0) at the next slot, t1,
// in the shared fragment texture space (space2) — the SDL_GPU convention: sampled textures first, then
// storage textures, numbered sequentially.
Texture2D<float4> RowDataTexture : register(t1, space2);
#endif

// Row i of this effect's table (i in [0, uRowTableRows)). Returns 0 when the effect has no table.
float4 paramRow(uint i) {
    if (uRowTableRows == 0u) return float4(0, 0, 0, 0);
    return RowDataTexture.Load(int3(0, uRowTableY + i, 0));
}

// The table row under this fragment's scanline — maps uv.y in [0,1) to a row in [0, uRowTableRows). The
// per-scanline case (a per-line scroll / warp / color). Returns 0 when the effect has no table.
float4 paramRowAtUv(float2 uv) {
    if (uRowTableRows == 0u) return float4(0, 0, 0, 0);
    uint i = min((uint)(uv.y * (float)uRowTableRows), uRowTableRows - 1u);
    return paramRow(i);
}
