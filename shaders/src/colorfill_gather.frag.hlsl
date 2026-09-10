// ColorFill gather fragment — collapses N ColorFill-confined regions into ONE fullscreen pass.
//
// The built-in ColorFill peer of the custom-shader gather variant: instead of the ~2N serialized
// passes the per-region path issues (one scissored colorfill.frag pass + one region_select.frag gate
// per region), one pass walks the run's per-region records from a storage buffer and, per fragment,
// composites EVERY covering record over the running value IN SUBMISSION ORDER — the sequential
// semantics the per-region path produces, not the custom gather's last-covering-wins union. In-order
// compositing is what lets any region alpha and any blend mode ride the record: overlapping grades
// compound and translucent fills stack exactly as sequential passes stacked them.
//
// Byte-identical parity with the per-region path is the contract, held by three mirrors:
//   1. The membership math is region_select.frag's, verbatim — the same sdPolygon (winding sign +
//      min-edge distance, degenerating to circle at n=1 / capsule at n=2), the same inverse-homography
//      fragment mapping computed through the SAME arithmetic (division by the reciprocal viewport,
//      matching regionParams' CPU-side 1/width), the same CRISP snap, stroke band, and invert flip.
//   2. The per-record composite fuses the two per-region passes exactly: paint rgb keep alpha
//      (colorfill.frag), then the gate's blend grade + alpha mix (region_select.frag), with the fill
//      quantized to float16 first — colorfill.frag's output crossed a float16 scratch texture.
//   3. Between records the running value round-trips through float16 (round-toward-zero, matching
//      the render-target down-conversion), replicating the float16 intermediate each per-region step
//      wrote and the next step sampled. Never after the last record — the render-target write
//      performs that final quantization itself.
//
// Record layout (float4s; stride = uRecordStride, resolved per run — see colorFillGatherRecordBytes,
// the unit-tested CPU packer this shader must match):
//   0 : uvBox u0, v0, u1, v1      (the covering quad, the quick-reject)
//   1 : fill r, g, b, regionAlpha
//   2 : invRow0.xyz, invert flag
//   3 : invRow1.xyz, strokeWidth
//   4 : invRow2.xyz, blend mode
//   5 : vertex count, radius, 0, 0
//   6+: vertices, two per float4
//
// SDL_GPU HLSL conventions — the custom GATHER variant's exact resource layout (the one register
// assignment whose indices satisfy every backend's binding rules at once: on Metal the storage buffer
// must land after the two uniform buffers in the shared [[buffer]] namespace, which register t2
// produces): the sampled source + sampler at t0/s0 space2; the row-data store at t1 space2 (declared
// for layout parity, never read — ColorFill has no per-row data table); the record storage buffer at
// t2 space2; uniforms in space3 — b0 mirrors the engine effect cbuffer (only the snap flag + viewport
// dims are read), b1 is the run header (region count + record stride).

Texture2D<float4> SourceTexture : register(t0, space2);
SamplerState      SourceSampler : register(s0, space2);
Texture2D<float4> RowDataStore  : register(t1, space2);
ByteAddressBuffer RegionRecords : register(t2, space2);  // packed float4 records; byte address = idx*16 (loadRec)

cbuffer RetroppEngineEffect : register(b0, space3) {
    uint  uEdgeClamp;      // unread — ColorFill never resamples the source at an offset
    uint  uRowTableY;      // unread — ColorFill has no per-row data table
    uint  uRowTableRows;   // unread
    uint  uSnap;           // 1 = snap the gate to the viewport grid (crisp), 0 = per-output-pixel
    float uViewportW;      // logical viewport dimensions for the fragment→pixel mapping
    float uViewportH;
    float uEnginePad0;
    float uEnginePad1;
};

cbuffer RetroppGatherInfo : register(b1, space3) {
    uint uRegionCount;     // how many records the run's storage buffer holds (the loop bound)
    uint uRecordStride;    // one record's size in float4s (6-float4 header + the run's vertex allotment)
    uint uGatherPad1;
    uint uGatherPad2;
};

#include "blend_ops.hlsli"  // blendOp — the separable BlendMode operator, mirror of retropp::blendChannel

// One float4 record slot by index — byte-addressed (idx * 16), NOT a StructuredBuffer index: SDL's
// D3D12 backend leaves StructureByteStride 0, which AMD uses to index a StructuredBuffer, collapsing
// every dynamic index to element 0. Byte addressing is stride-independent; reads the same bytes, so
// the Vulkan/Metal output is unchanged.
float4 loadRec(uint idx) { return asfloat(RegionRecords.Load4(idx * 16u)); }

// One record's i-th vertex: packed two-per-float4 from the record's vertex region at `vbase`.
float2 recPoint(uint vbase, uint i) {
    float4 packed = loadRec(vbase + (i >> 1u));
    return (i & 1u) != 0u ? packed.zw : packed.xy;
}

// Mirror of region_select.frag's sdPolygon (itself the retropp::sdPolygon mirror): winding-number
// sign + min-edge distance; degenerates to point (n==1) and segment (n==2) distance so one routine
// covers circle / capsule / polygon. Reads the record's vertices instead of a cbuffer array.
float sdPolygonRec(float2 p, uint n, uint vbase) {
    float2 v0 = recPoint(vbase, 0u);
    if (n == 1u) {
        return length(p - v0);
    }
    float d = dot(p - v0, p - v0);  // squared distance, seeded at vertex 0
    float s = 1.0;
    uint j = n - 1u;
    for (uint i = 0u; i < n; ++i) {
        float2 vi = recPoint(vbase, i);
        float2 vj = recPoint(vbase, j);
        float2 e = vj - vi;
        float2 w = p - vi;
        float ee = dot(e, e);
        float t = ee > 0.0 ? clamp(dot(w, e) / ee, 0.0, 1.0) : 0.0;
        float2 b = w - e * t;
        d = min(d, dot(b, b));
        if (n >= 3u) {
            bool c1 = p.y >= vi.y;
            bool c2 = p.y <  vj.y;
            bool c3 = (e.x * w.y) > (e.y * w.x);
            if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
        }
        j = i;
    }
    return s * sqrt(d);
}

// Round-trip through float16 — the quantization a value picks up crossing an R16G16B16A16_FLOAT
// intermediate between per-region passes. The render-target down-conversion rounds TOWARD ZERO,
// realized here in plain float/bit arithmetic: floor the value against the f16 lattice step at its
// magnitude. No pack/unpack intrinsics — their rounding is implementation-defined per backend (and a
// pack∘unpack pair invites an identity fold), while every operation here is exact in float32: the
// step is a power of two, so the divide, the floor (quotient < 2^11), and the multiply are exact.
// Values here are non-negative (the per-region path never stores a negative intermediate); f16's
// largest finite is the ceiling — rounding toward zero never reaches infinity.
float quantizeF16Lane(float v) {
    if (!(v > 0.0)) return 0.0;
    if (v >= 65504.0) return 65504.0;
    uint  bits = asuint(v);
    int   e = int((bits >> 23u) & 0xFFu) - 127;          // v's unbiased float32 exponent
    int   eStep = e < -14 ? -24 : e - 10;                // f16 step: 2^-24 below normals, else 2^(e-10)
    float step = asfloat(uint((eStep + 127) << 23));
    return floor(v / step) * step;
}
float4 quantizeF16(float4 v) {
    return float4(quantizeF16Lane(v.x), quantizeF16Lane(v.y), quantizeF16Lane(v.z), quantizeF16Lane(v.w));
}

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float4 acc = SourceTexture.Sample(SourceSampler, uv);

    // Fragment UV → viewport pixels through the reciprocal, the exact arithmetic region_select.frag
    // uses (uv / uMisc.x where uMisc.x = 1/viewportW) — a multiply by the dimension can differ in the
    // last ULP. Crisp evaluation snaps the test point to its viewport-cell centre, mirroring the gate.
    float2 invVp = 1.0 / float2(uViewportW, uViewportH);
    float2 fragPx = uv / invVp;
    if (uSnap != 0u) fragPx = floor(fragPx) + 0.5;

    bool composited = false;
    for (uint i = 0u; i < uRegionCount; ++i) {
        uint base = i * uRecordStride;
        float4 box = loadRec(base);
        if (uv.x < box.x || uv.y < box.y || uv.x > box.z || uv.y > box.w) continue;

        float4 inv0 = loadRec(base + 2u);
        float4 inv1 = loadRec(base + 3u);
        float4 inv2 = loadRec(base + 4u);
        float4 misc = loadRec(base + 5u);
        uint count = (uint)(misc.x + 0.5);

        // Viewport pixels → shape-local via the record's inverse homography (perspective divide),
        // then the SDF gate: fill distance − radius, the stroke band, the invert flip — all
        // region_select.frag verbatim.
        float  wgt = inv2.x * fragPx.x + inv2.y * fragPx.y + inv2.z;
        float2 local = float2(inv0.x * fragPx.x + inv0.y * fragPx.y + inv0.z,
                              inv1.x * fragPx.x + inv1.y * fragPx.y + inv1.z) / wgt;
        float sd = sdPolygonRec(local, count, base + 6u) - misc.y;
        if (inv1.w > 0.0) sd = abs(sd) - inv1.w * 0.5;  // boundary distance → band (the outline)
        bool inside = sd <= 0.0;
        if (inv0.w > 0.5) inside = !inside;             // region invert: confine to the OUTSIDE
        if (!inside) continue;

        // The running value crossed a float16 intermediate between per-region passes — replicate it
        // before this record consumes the previous record's output (never after the last record; the
        // render-target write quantizes that one).
        if (composited) acc = quantizeF16(acc);

        // colorfill.frag: emit the fill, opaque. Its output crossed a float16 scratch before the gate
        // sampled it — quantize the fill the same way.
        float4 fillA = loadRec(base + 1u);
        float4 eff = float4(quantizeF16(float4(fillA.rgb, 0.0)).rgb, 1.0);

        // region_select.frag: the blend grade over the scene, then the region-alpha mix.
        uint   mode   = (uint)(inv2.w + 0.5);
        float4 graded = eff;
        if (mode != 0u) {
            float  sa  = eff.a;
            float3 rgb = saturate((1.0 - sa) * acc.rgb + sa * blendOp(mode, acc.rgb, eff.rgb));
            float  a   = saturate(sa + acc.a * (1.0 - sa));
            graded = float4(rgb, a);
        }
        acc = lerp(acc, graded, fillA.w);
        composited = true;
    }
    return acc;
}
