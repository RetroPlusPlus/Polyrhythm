// Tile layer fragment shader (indexed atlas + runtime palettes).
//
// Per output pixel: reconstruct the layer-local pixel from the interpolated UV × the layer size, add
// the layer scroll, wrap into the tilemap (mirroring retropp::sampleTilemap), Load the two-word tilemap
// cell (tile + flip in word0, atlas + palette handle in word1 — mirroring retropp::unpackTileCell),
// resolve the cell's atlas to its region in the flat store via the global atlas-region table, Load the
// palette INDEX from the indexed atlas, Load the color from the palette store at the cell's palette
// offset, and scale by the layer alpha. Everything is integer Load — there is NO sampler on the tile
// path. Faithful GB/C model: color is an index plus a palette chosen at render time, applied per-pixel.
//
// SDL_GPU HLSL conventions (see SDL_CreateGPUShader docs): with no sampled textures, the read-only
// storage textures take t0..t3 in space2; the uniform buffer is b0 in space3.
//   - t0 space2 : flat ATLAS STORE (R32_UINT; integer Load; all sheets stacked vertically)
//   - t1 space2 : tilemap cells (R32G32_UINT; integer Load; packTileCell two-word layout)
//   - t2 space2 : palette store (RGBA8; integer Load; FLAT colors wrapped W wide → texel (flat%W, flat/W))
//   - t3 space2 : global atlas-region table (R32G32B32A32_UINT; texel x = AtlasId → (place = column | row << 16, cols, transpMaskLo, transpMaskHi))
//   - b0 space3 : per-layer uniforms

Texture2D<uint>   uAtlas        : register(t0, space2);
Texture2D<uint2>  uTilemap      : register(t1, space2);
Texture2D<float4> uPaletteStore : register(t2, space2);
Texture2D<uint4>  uAtlasRegions : register(t3, space2);

cbuffer TileUniforms : register(b0, space3) {
    float2 uScroll;       // layer scroll, pixels                                              — reg 0
    float2 uLayerSize;    // layer destination size, viewport pixels
    float2 uTilemapSize;  // tilemap dimensions, tiles (width, height)                         — reg 1
    float  uTilePx;            // tile edge length, pixels (8)
    float  uAlpha;             // layer alpha, [0,1]
    float  uPaletteStoreW;     // palette-store row width (colors); flat offset → (f%W, f/W)  — reg 2
    float  uComposeScale;      // compose grid ÷ viewport (1 = faithful); output pixel → viewport
    float  uSnap;              // 1 = snap the transform's destination pixel to the viewport grid (crisp)
    float  _pad2;
    float4 uInvRow0;           // inverse transform homography, row 0 (m00,m01,m02, _)         — reg 3
    float4 uInvRow1;           //   row 1 (m10,m11,m12, _)                                          — reg 4
    float4 uInvRow2;           //   row 2 (m20,m21,m22, _) — perspective terms in .x/.y             — reg 5
    uint4  uTransformCtl;      //   x = hasTransform (0/1), y = footprint edge (0 Blank / 1 Stretch),
                               //   z = tilemap wrap (0 Repeat / 1 Clamp / 2 Blank)               — reg 6
};

// Floored modulo via floor() — well-defined for any sign, unlike HLSL integer %.
float floorModF(float a, float p) { return a - floor(a / p) * p; }

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    // uLayerSize is the compose grid — the output resolution on the interpolation path (viewport ×
    // uComposeScale). Floor the output pixel, then divide by uComposeScale to land in VIEWPORT-content
    // space at output granularity: local advances in 1/uComposeScale-viewport-pixel steps, so a fractional
    // uScroll shifts the sampled tile/pixel boundary by whole output pixels between refreshes — smooth
    // motion, crisp texels. vpSize is the viewport-content size the transform math + world are authored in.
    // At uComposeScale == 1: vpSize == uLayerSize, local is the integer output pixel, byte-identical.
    float2 vpSize = uLayerSize / uComposeScale;
    float2 local  = floor(uv * uLayerSize) / uComposeScale;   // viewport-content pixel (output-granular)

    // Per-layer geometric transform (authored in viewport space). When present, inverse-map the
    // destination pixel through the inverse homography (perspective divide included → the Mode-7-style
    // floor) to the CONTENT pixel to sample, then apply the FOOTPRINT edge policy outside [0, vpSize):
    // Blank discards (transparent, the layers below show through), Stretch clamps to the footprint edge.
    // When absent (identity), `sample` stays `local` and the path below is byte-identical to the
    // untransformed faithful behavior.
    float2 sample = local;
    if (uTransformCtl.x != 0u) {
        // Crisp evaluation (uSnap): floor the destination pixel to the viewport grid before the inverse
        // homography, so the transform samples exactly the integer content pixels the viewport-resolution
        // rasterization would — the upscale stays pixel-identical, no per-output-pixel homography softening.
        // A no-op when `local` is already integer (compose grid == viewport). uSnap 0 (Output grid) evaluates
        // the homography per output pixel (smooth). The untransformed path is unaffected — it set `sample`
        // before this branch and carries the smooth interpolated scroll.
        if (uSnap != 0.0f) local = floor(local);
        float cw = uInvRow2.x * local.x + uInvRow2.y * local.y + uInvRow2.z;   // perspective weight
        // Behind the projection (above the Mode-7 horizon, w <= 0): NO content exists there — always
        // blank, in EITHER edge mode. The perspective divide flips sign here, so Stretch's clamp would
        // pin to the (0,0) content corner and smear it across the whole upper wedge. Discard first.
        if (cw <= 0.0f) discard;
        float cx = (uInvRow0.x * local.x + uInvRow0.y * local.y + uInvRow0.z) / cw;
        float cy = (uInvRow1.x * local.x + uInvRow1.y * local.y + uInvRow1.z) / cw;
        if (cx < 0.0f || cx >= vpSize.x || cy < 0.0f || cy >= vpSize.y) {
            if (uTransformCtl.y == 0u) discard;                    // Blank → transparent, reveal below
            cx = clamp(cx, 0.0f, vpSize.x - 1.0f);                 // Stretch → clamp-to-edge
            cy = clamp(cy, 0.0f, vpSize.y - 1.0f);
        }
        sample = float2(cx, cy);
    }

    float2 world = sample + uScroll;           // scrolled world pixel (may be negative)
    float2 mapPx = uTilemapSize * uTilePx;     // tilemap size in pixels
    int tilePx = (int)uTilePx;

    // Per-layer tilemap wrap mode (uTransformCtl.z), mirroring retropp::sampleTilemap:
    //   0 Repeat — toroidal floorMod (the faithful default)
    //   1 Clamp  — clamp the world coord to the map's last pixel (smear the edge tile)
    //   2 Blank  — finite map: discard outside [0, mapPx) on either axis → transparent, reveal below
    float wx, wy;
    if (uTransformCtl.z == 2u) {               // Blank
        if (world.x < 0.0f || world.x >= mapPx.x || world.y < 0.0f || world.y >= mapPx.y) discard;
        wx = world.x;
        wy = world.y;
    } else if (uTransformCtl.z == 1u) {        // Clamp
        wx = clamp(world.x, 0.0f, mapPx.x - 1.0f);
        wy = clamp(world.y, 0.0f, mapPx.y - 1.0f);
    } else {                                    // Repeat
        wx = floorModF(world.x, mapPx.x);      // wrapped to [0, mapPx) → non-negative
        wy = floorModF(world.y, mapPx.y);
    }
    int ix = (int)wx;
    int iy = (int)wy;

    int tileX  = ix / tilePx;
    int tileY  = iy / tilePx;
    int pixelX = ix % tilePx;
    int pixelY = iy % tilePx;

    // Load + unpack the two-word tilemap cell (mirrors retropp::unpackTileCell's bit layout exactly:
    // tile 0..15 | flipX 16 | flipY 17 | rotation 18..19 in word0).
    uint2 packed       = uTilemap.Load(int3(tileX, tileY, 0));
    uint  tileIndex    = packed.x & 0xFFFF;
    bool  flipX        = ((packed.x >> 16) & 1) != 0;
    bool  flipY        = ((packed.x >> 17) & 1) != 0;
    uint  rotation     = (packed.x >> 18) & 3u;
    uint  atlasId      = packed.y & 0xFFFF;
    uint  paletteOffset= packed.y >> 16;

    // Orient the within-tile offset before addressing the atlas cell: flip first, then the 90° rotation
    // (mirrors retropp::sourceCellTexel; tiles are square so w = h = tilePx).
    if (flipX) pixelX = tilePx - 1 - pixelX;
    if (flipY) pixelY = tilePx - 1 - pixelY;
    int rn = tilePx - 1;
    if (rotation == 1u)      { int rt = pixelX; pixelX = pixelY;      pixelY = rn - rt; }  // Rot90
    else if (rotation == 2u) { pixelX = rn - pixelX; pixelY = rn - pixelY; }               // Rot180
    else if (rotation == 3u) { int rt = pixelX; pixelX = rn - pixelY;  pixelY = rt; }      // Rot270

    // The cell's sheet region in the atlas store, looked up by its atlas handle in the global table:
    // (place, cols, transpMaskLo, transpMaskHi) — place is the sheet's column in its low 16 bits and its row
    // in the high 16. An unused/invalid handle reads region 0 → cols 0 → discard (nothing to draw, and
    // never a divide-by-zero).
    uint4 region    = uAtlasRegions.Load(int3((int)atlasId, 0, 0));
    int   storeX    = (int)(region.x & 0xFFFFu);
    int   storeY    = (int)(region.x >> 16);
    int   atlasCols = (int)region.y;
    if (atlasCols == 0) discard;
    int atlasCol    = (int)tileIndex % atlasCols;
    int atlasRow    = (int)tileIndex / atlasCols;
    int2 atlasTexel = int2(storeX + atlasCol * tilePx + pixelX, storeY + atlasRow * tilePx + pixelY);

    uint colorIndex = uAtlas.Load(int3(atlasTexel, 0));           // palette index 0..N-1

    // Structural transparency: the sheet's transparent-index set is a 64-bit bitmask split across
    // region.z (indices 0–31) and region.w (indices 32–63). When this index is a member it is a HOLE —
    // discard so the lower layer shows through. The empty set (the default) discards nothing.
    bool hole = (colorIndex < 32u) ? (((region.z >> colorIndex)         & 1u) != 0u)
              : (colorIndex < 64u) ? (((region.w >> (colorIndex - 32u)) & 1u) != 0u)
                                   : false;   // indices ≥ 64 are alpha-only, never a structural hole
    if (hole) discard;

    uint   flat   = paletteOffset + colorIndex;                  // flat index into the palette store
    int    W      = (int)uPaletteStoreW;
    float4 colour = uPaletteStore.Load(int3((int)(flat % (uint)W), (int)(flat / (uint)W), 0));
    if (colour.a == 0.0f) discard;   // material transparency: a fully-transparent palette entry is a hole

    return float4(colour.rgb, colour.a * uAlpha);
}
