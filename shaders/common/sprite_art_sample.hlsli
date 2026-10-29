// The sprite's own art read — the transparent-field sample — and the per-fragment context it reads.
//
// Engine-internal header — see blend_ops.hlsli for why shaders/common is separate from shaders/include.
//
// REQUIRES the includer to have already declared uAtlasRegions, uAtlas and uPaletteStore, so this header
// is included AFTER the shader's resource declarations.
//
// The statics are the context the function reads, and they live here with it so a shader that includes the
// read also gets the state it needs. main() sets them once per fragment, before the read.

#ifndef RETROPP_COMMON_SPRITE_ART_SAMPLE_HLSLI
#define RETROPP_COMMON_SPRITE_ART_SAMPLE_HLSLI

// The per-fragment sprite context main() publishes so retroppSpriteArtSample — and, in a generated
// sprite-custom variant, a game shader's sampleSource() — can read the sprite's own art with no shader
// inputs threaded through. main() sets these once before the read.
static uint  gSpriteTile;          // top-left atlas cell within the sprite's sheet
static uint  gSpriteAtlasPalette;  // atlas (low 16) | palette flat offset (high 16)
static uint  gSpriteFlags;         // flip / rotation bits (the coverage bits are irrelevant to the read)
static uint  gSpritePackedSize;    // (width << 16) | height
static float gSpriteTilePx;        // tile edge length, px
static float gSpritePaletteW;      // palette-store row width

// Read the sprite's OWN art at a within-sprite quad coordinate `uv` (∈ [0,1]² over the art) — the sprite
// sits in an infinite transparent field, so a read outside the art is transparent (Blank) unless `stretch`
// clamps it to the border texel. Returns straight rgba (the palette color); a structural hole or a fully
// transparent palette entry returns (0,0,0,0). This is the sprite's material read, shared by main()'s own
// pixel and a custom shader's sampleSource() (the transparent-field domain, one authority).
float4 retroppSpriteArtSample(float2 uv, bool stretch) {
    int2 sz = int2((int)(gSpritePackedSize >> 16), (int)(gSpritePackedSize & 0xFFFFu));
    if ((uv.x < 0.0f || uv.x >= 1.0f || uv.y < 0.0f || uv.y >= 1.0f) && !stretch)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);                 // off-art, Blank → transparent
    int2 px = clamp(int2(floor(uv * float2(sz))), int2(0, 0), sz - int2(1, 1));  // Stretch clamps to the border
    uint flags    = gSpriteFlags;
    bool flipX    = (flags & 1u) != 0u;
    bool flipY    = (flags & 2u) != 0u;
    uint rotation = (flags >> 2u) & 3u;
    if (flipX) px.x = sz.x - 1 - px.x;
    if (flipY) px.y = sz.y - 1 - px.y;
    if (rotation == 1u)      { int rt = px.x; px.x = px.y;            px.y = sz.x - 1 - rt; }  // Rot90
    else if (rotation == 2u) { px.x = sz.x - 1 - px.x; px.y = sz.y - 1 - px.y; }               // Rot180
    else if (rotation == 3u) { int rt = px.x; px.x = sz.y - 1 - px.y;  px.y = rt; }            // Rot270

    uint atlasId       = gSpriteAtlasPalette & 0xFFFFu;
    uint paletteOffset = gSpriteAtlasPalette >> 16;
    uint4 region       = uAtlasRegions.Load(int3((int)atlasId, 0, 0));
    int   storeX       = (int)(region.x & 0xFFFFu);
    int   storeY       = (int)(region.x >> 16);
    int   atlasCols    = (int)region.y;
    if (atlasCols == 0) return float4(0.0f, 0.0f, 0.0f, 0.0f);

    int  tilePx = (int)gSpriteTilePx;
    int  col    = (int)gSpriteTile % atlasCols;
    int  row    = (int)gSpriteTile / atlasCols;
    int2 texel  = int2(storeX + col * tilePx + px.x, storeY + row * tilePx + px.y);
    uint idx    = uAtlas.Load(int3(texel, 0));
    bool hole = (idx < 32u) ? (((region.z >> idx)         & 1u) != 0u)
              : (idx < 64u) ? (((region.w >> (idx - 32u)) & 1u) != 0u)
                            : false;
    if (hole) return float4(0.0f, 0.0f, 0.0f, 0.0f);           // structural transparency
    uint   flat = paletteOffset + idx;
    int    W    = (int)gSpritePaletteW;
    return uPaletteStore.Load(int3((int)(flat % (uint)W), (int)(flat / (uint)W), 0));  // a==0 = material hole
}

#endif  // RETROPP_COMMON_SPRITE_ART_SAMPLE_HLSLI
