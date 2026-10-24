// Sprite emission fragment — the raster stage of a sprite's Layer-scope Bloom / Glow.
//
// A glowing sprite draws TWICE: once through sprite.frag for its art, and once through this shader for the
// light it radiates. This pass writes only the EMISSION — the halo's source field — into the shared
// emission buffer, additively, so every sprite in the same (kind, reach) bucket accumulates into one image
// that is blurred once and composited once. That is what makes the cost of a field of glowing sprites the
// cost of its distinct reaches rather than of its sprite count.
//
// The sprite's own chain runs here up to the emission step, which is named by bits 8..15 of the record's
// flags word (retropp::kSpriteEmissionIndexShift; the renderer ORs it into the emission copy of the art
// record). Color steps before it transform the pixel exactly as they do for the art, so a recolored
// sprite radiates its new color; the step at that index resolves the emission from the running color:
//
//   Bloom:  e = premultiply(c) · f(lum(premultiply(c)), threshold) · intensity     // its own light
//   Glow:   m = c.a · survive(lum(c), threshold);  e = (tint · m · intensity, m · intensity)
//
// mirroring retropp::emissionExtractBloom / emissionExtractGlow with the sprite's art as the source. The
// result scales by the layer alpha and the per-sprite alpha, so a fading sprite's halo fades with it.
//
// A Custom chain step is a no-op here — a game shader has no emission variant, so the emission sees the
// color as it stood before that step (the renderer warns). Region steps never precede a chain step in the
// record slice, and are skipped defensively.
//
// The displacement pre-pass, the art read, and the transparent-field semantics are the art shader's, quoted
// verbatim: the halo radiates from wherever the art actually landed. There is no footprint inflation — this
// pass covers the art's own quad and the blur that follows spreads the light in buffer space.
//
// SDL_GPU HLSL conventions match sprite.frag exactly (t0..t3 space2 storage textures, b0 space3 uniform),
// so the two share one vertex stage and one set of bindings.

Texture2D<uint>   uAtlas        : register(t0, space2);
Texture2D<float4> uPaletteStore : register(t1, space2);
Texture2D<uint4>  uAtlasRegions : register(t2, space2);
Texture2D<float4> uFxStore      : register(t3, space2);

cbuffer SpriteFragUniforms : register(b0, space3) {
    float uTilePx;          // tile edge length, pixels (8)
    float uAlpha;           // layer alpha, [0,1]
    float uPaletteStoreW;   // palette-store row width (colors); flat offset → (f%W, f/W)
    float uComposeScale;    // compose grid ÷ viewport (1 = faithful); output pixel → viewport
};

// ── Effect math (mirrors the retropp:: CPU authorities in postprocess.h) ──────────────────

#include "sprite_color.hlsli"  // applyGleam, applySaturation — the per-pixel color operators

// ── Displacement re-read (identical to sprite.frag — the halo follows the displaced art) ──

#include "rounding.hlsli"  // roundHalfUp — the CPU mirror's tie rule

#include "sprite_displace.hlsli"  // snapArt, spriteDisplace, spriteRipple, spriteSwirl — art-space re-reads

// ── Sprite art read (identical to sprite.frag's transparent-field sample) ─────────────────

#include "sprite_art_sample.hlsli"  // the sprite context + retroppSpriteArtSample — reads the atlas resources above

float4 main(float2 spriteUV : TEXCOORD0,
            nointerpolation uint tile         : TEXCOORD1,
            nointerpolation uint atlasPalette : TEXCOORD2,
            nointerpolation uint flags        : TEXCOORD3,
            nointerpolation uint packedSize   : TEXCOORD4,
            nointerpolation float3 inv0       : TEXCOORD5,
            nointerpolation float3 inv1       : TEXCOORD6,
            nointerpolation float3 inv2       : TEXCOORD7,
            nointerpolation float  spriteAlpha : TEXCOORD8,
            nointerpolation uint fxOffset     : TEXCOORD9,
            nointerpolation uint fxCount      : TEXCOORD10,
            float4 pos : SV_Position) : SV_Target0 {
    int2   sz    = int2((int)(packedSize >> 16), (int)(packedSize & 0xFFFFu));
    float2 fdims = float2(sz);

    // Which record carries the emission — the renderer's index, in the flags word's high byte.
    uint emitIndex = (flags >> 8u) & 0xFFu;
    if (emitIndex >= fxCount) discard;   // no emission step on this record: nothing to radiate

    bool analytic = (flags & 16u) != 0u;
    float2 fxUv;
    if (analytic) {
        float2 c  = floor(pos.xy / uComposeScale) + 0.5f;      // viewport-cell center
        float  cw = inv2.x * c.x + inv2.y * c.y + inv2.z;
        if (cw <= 0.0f) discard;                               // behind the projection
        fxUv = float2((inv0.x * c.x + inv0.y * c.y + inv0.z) / cw,
                      (inv1.x * c.x + inv1.y * c.y + inv1.z) / cw);
    } else {
        fxUv = spriteUV;
    }

    // Displacement pre-pass over the WHOLE chain, exactly as the art pass runs it, so the halo radiates
    // from where the art landed rather than from where it was authored.
    float2 readUv   = fxUv;
    bool   hasDisp  = false;
    uint   dispEdge = 0u;   // 0 = Blank (out-of-art ⇒ transparent), 1 = Stretch (clamp to the border)
    if ((flags & 32u) != 0u) {
        [loop]
        for (uint di = 0u; di < fxCount; di++) {
            int    dri  = int(fxOffset + di);
            float4 dh   = uFxStore.Load(int3(0, dri, 0));   // head: kind, flags
            uint   dk   = (uint)dh.x;
            uint   dfl  = (uint)dh.y;
            if ((dfl & 1u) != 0u) continue;                 // a region step is never a displacing re-read
            if (dk == 1u || dk == 2u || dk == 10u) {        // RowDisplacement / Ripple / Swirl
                float4 dp = uFxStore.Load(int3(2, dri, 0));
                if (dk == 1u) {
                    readUv = spriteDisplace(readUv, dp, fdims);
                } else if (dk == 2u) {
                    float4 dg = uFxStore.Load(int3(1, dri, 0));
                    readUv = spriteRipple(readUv, dp, dg, fdims);
                } else {
                    float4 dg = uFxStore.Load(int3(1, dri, 0));
                    readUv = spriteSwirl(readUv, dp, dg, fdims);
                }
                hasDisp  = true;
                dispEdge = ((dfl & 4u) != 0u) ? 1u : 0u;    // kSpriteFxEdgeStretch
            }
        }
    }

    if (!hasDisp && analytic) {
        if (fxUv.x < 0.0f || fxUv.x >= 1.0f || fxUv.y < 0.0f || fxUv.y >= 1.0f) discard;
    }

    gSpriteTile         = tile;
    gSpriteAtlasPalette = atlasPalette;
    gSpriteFlags        = flags;
    gSpritePackedSize   = packedSize;
    gSpriteTilePx       = uTilePx;
    gSpritePaletteW     = uPaletteStoreW;
    float4 c = retroppSpriteArtSample(readUv, hasDisp && dispEdge == 1u);
    if (c.a == 0.0f) discard;            // an uncovered pixel radiates nothing

    // Color steps BEFORE the emission step, in chain order — what the sprite looks like is what it
    // radiates. Kinds with no color transform (displacement, another glow) pass through.
    [loop]
    for (uint i = 0u; i < emitIndex; i++) {
        int    ri     = int(fxOffset + i);
        float4 head   = uFxStore.Load(int3(0, ri, 0));   // kind, flags, blend, pointCount
        float4 params = uFxStore.Load(int3(2, ri, 0));
        uint   kind   = (uint)head.x;
        if (((uint)head.y & 1u) != 0u) continue;         // region steps never precede a chain step
        if (kind == 5u)      c.rgb = params.xyz;                                     // ColorFill
        else if (kind == 6u) c.rgb = applyGleam(c.rgb, fxUv.x, fxUv.y, params);      // Gleam
        else if (kind == 7u) c.rgb = applySaturation(c.rgb, params.x);               // ColorSaturation
        else if (kind == 4u) {                                                        // Transparency
            float surv = (uint)params.x == 0u ? 0.0f : 1.0f;   // whole silhouette: coverage is 1 everywhere
            if (surv <= 0.0f) discard;
            c.a *= surv;
        }
    }

    // The emission step itself.
    int    eri    = int(fxOffset + emitIndex);
    float4 ehead  = uFxStore.Load(int3(0, eri, 0));
    float4 egate  = uFxStore.Load(int3(1, eri, 0));   // Glow: the authored tint on the idle chain lanes
    float4 eparam = uFxStore.Load(int3(2, eri, 0));   // (radius art px, threshold, intensity, invNorm)
    float  threshold = eparam.y;
    float  intensity = eparam.z;
    float  scale     = uAlpha * spriteAlpha;          // the sprite's light fades as the sprite does

    if ((uint)ehead.x == 9u) {                        // Glow — a scalar mask times the authored tint
        float lum  = c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;
        float den  = max(1.0f - threshold, 1.0f / 255.0f);
        float surv = threshold <= 0.0f ? 1.0f : saturate((lum - threshold) / den);
        float m    = c.a * surv * intensity * scale;
        return float4(egate.y * m, egate.z * m, egate.w * m, m);
    }

    float4 pm  = float4(c.rgb * c.a, c.a);            // Bloom — the brightpass of its own light
    float  lum = pm.r * 0.299f + pm.g * 0.587f + pm.b * 0.114f;
    float  den = max(1.0f - threshold, 1.0f / 255.0f);
    float  f   = saturate((lum - threshold) / den);
    return pm * (f * intensity * scale);
}
