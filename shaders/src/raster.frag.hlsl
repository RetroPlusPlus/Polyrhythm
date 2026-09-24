// Raster layer fragment shader (a raster of pixels: a hosted machine's picture, or one the program drew).
//
// Per output pixel: reconstruct the layer-local pixel from the interpolated UV × the layer size, apply
// the per-layer transform, add the layer scroll, and Load the texel of the raster under that pixel —
// the pixel itself when the raster is shown at its own size, the texel the fit maps it to otherwise.
// Everything is an integer Load — there is NO sampler on this path, so the raster reaches the screen at
// exactly the texels its source produced however far it is scaled up.
//
// A raster is FINITE: outside the size it fills there is nothing, so the fragment discards and the
// layers below show through. That is the whole of the edge policy — a raster does not wrap.
//
// SDL_GPU HLSL conventions (see SDL_CreateGPUShader docs): with no sampled textures, the read-only
// storage texture takes t0 in space2; the uniform buffer is b0 in space3.
//   - t0 space2 : the raster (RGBA8; integer Load; row-major, the source's own dimensions)
//   - b0 space3 : per-layer uniforms

Texture2D<float4> uRaster : register(t0, space2);

cbuffer RasterUniforms : register(b0, space3) {
    float2 uScroll;        // layer scroll, pixels                                              — reg 0
    float2 uLayerSize;     // layer destination size, viewport pixels
    float2 uRasterSize;    // the raster's dimensions, pixels                                   — reg 1
    float  uAlpha;              // layer alpha, [0,1]
    float  uComposeScale;       // compose grid ÷ viewport (1 = faithful); output pixel → viewport
    float2 uFitSize;            // the size the raster fills, viewport pixels                       — reg 2
    float  uSnap;               // 1 = snap the transform's destination pixel to the viewport grid
    float  _pad2;
    float4 uInvRow0;            // inverse transform homography, row 0 (m00,m01,m02, _)         — reg 3
    float4 uInvRow1;            //   row 1 (m10,m11,m12, _)                                     — reg 4
    float4 uInvRow2;            //   row 2 (m20,m21,m22, _) — perspective terms in .x/.y        — reg 5
    uint4  uTransformCtl;       //   x = hasTransform (0/1), y = footprint edge (0 Blank / 1 Stretch)
};                              //                                                              — reg 6

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    // uLayerSize is the compose grid — the output resolution on the interpolation path (viewport ×
    // uComposeScale). Floor the output pixel, then divide by uComposeScale to land in VIEWPORT-content
    // space at output granularity. vpSize is the viewport-content size the transform math is authored
    // in. At uComposeScale == 1: vpSize == uLayerSize and local is the integer output pixel.
    float2 vpSize = uLayerSize / uComposeScale;
    float2 local  = floor(uv * uLayerSize) / uComposeScale;   // viewport-content pixel (output-granular)

    // Per-layer geometric transform, identical to the tile path: inverse-map the destination pixel
    // through the inverse homography (perspective divide included) to the CONTENT pixel, then apply the
    // FOOTPRINT edge policy outside [0, vpSize) — Blank discards, Stretch clamps to the footprint edge.
    // Identity leaves `sample` as `local` and the path below is the untransformed one.
    float2 sample = local;
    if (uTransformCtl.x != 0u) {
        if (uSnap != 0.0f) local = floor(local);
        float cw = uInvRow2.x * local.x + uInvRow2.y * local.y + uInvRow2.z;   // perspective weight
        if (cw <= 0.0f) discard;                               // behind the projection: no content
        float cx = (uInvRow0.x * local.x + uInvRow0.y * local.y + uInvRow0.z) / cw;
        float cy = (uInvRow1.x * local.x + uInvRow1.y * local.y + uInvRow1.z) / cw;
        if (cx < 0.0f || cx >= vpSize.x || cy < 0.0f || cy >= vpSize.y) {
            if (uTransformCtl.y == 0u) discard;                // Blank → transparent, reveal below
            cx = clamp(cx, 0.0f, vpSize.x - 1.0f);             // Stretch → clamp-to-edge
            cy = clamp(cy, 0.0f, vpSize.y - 1.0f);
        }
        sample = float2(cx, cy);
    }

    float2 world = sample + uScroll;   // scrolled pixel of the picture the raster fills (may be negative)
    if (world.x < 0.0f || world.x >= uFitSize.x || world.y < 0.0f || world.y >= uFitSize.y) {
        discard;                        // off the picture: nothing was drawn there
    }

    // The texel under this pixel: the raster's own size over the size it fills, multiplied before it is
    // divided, so a fit equal to the raster's size lands on exactly the texel the pixel names.
    float2 texel = (world * uRasterSize) / uFitSize;
    float4 color = uRaster.Load(int3((int)texel.x, (int)texel.y, 0));
    return float4(color.rgb, color.a * uAlpha);
}
