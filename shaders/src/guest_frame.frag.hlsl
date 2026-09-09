// Guest-frame layer fragment shader (a hosted machine's completed picture).
//
// Per output pixel: reconstruct the layer-local pixel from the interpolated UV × the layer size, apply
// the per-layer transform, add the layer scroll, and Load the pixel the machine drew. Everything is an
// integer Load — there is NO sampler on this path, so the picture reaches the screen at exactly the
// texels the machine produced however far it is scaled up.
//
// A picture is FINITE: outside its own dimensions there is nothing, so the fragment discards and the
// layers below show through. That is the whole of the edge policy — a machine's screen does not wrap.
//
// SDL_GPU HLSL conventions (see SDL_CreateGPUShader docs): with no sampled textures, the read-only
// storage texture takes t0 in space2; the uniform buffer is b0 in space3.
//   - t0 space2 : the machine's picture (RGBA8; integer Load; row-major, the machine's own dimensions)
//   - b0 space3 : per-layer uniforms

Texture2D<float4> uPicture : register(t0, space2);

cbuffer GuestFrameUniforms : register(b0, space3) {
    float2 uScroll;        // layer scroll, pixels                                              — reg 0
    float2 uLayerSize;     // layer destination size, viewport pixels
    float2 uPictureSize;   // the machine's picture dimensions, pixels                          — reg 1
    float  uAlpha;              // layer alpha, [0,1]
    float  uComposeScale;       // compose grid ÷ viewport (1 = faithful); output pixel → viewport
    float  uSnap;               // 1 = snap the transform's destination pixel to the viewport grid — reg 2
    float3 _pad2;
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

    float2 world = sample + uScroll;   // scrolled picture pixel (may be negative)
    if (world.x < 0.0f || world.x >= uPictureSize.x || world.y < 0.0f || world.y >= uPictureSize.y) {
        discard;                        // off the machine's screen: nothing was drawn there
    }

    float4 colour = uPicture.Load(int3((int)world.x, (int)world.y, 0));
    return float4(colour.rgb, colour.a * uAlpha);
}
