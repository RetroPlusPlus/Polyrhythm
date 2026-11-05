// Halo composite vertex shader (instanced quads) — engine-internal.
//
// One quad per sprite-halo field, drawn as an instance. Each quad covers its field's inner box on the compose
// grid and hands the fragment the frame-global uv at that point, plus the field's rect-table row and its
// kind. 6 × SV_VertexID unit-quad corners; the per-instance record is read from a storage buffer indexed by
// SV_InstanceID — no vertex buffer.
//
// Storage-only, with no uniform buffer: a vertex stage that binds both a storage buffer and a uniform buffer
// collides in Metal's [[buffer]] namespace under the HLSL→SPIR-V→MSL toolchain, so the box is baked in uv
// space on the CPU and each draw starts at instance 0. Resource: t0 space0 = the records as a
// ByteAddressBuffer (byte address = instanceID * 32), which indexes correctly on every driver whatever
// stride the backend reports for the buffer.

// Mirrors retropp::GpuHaloComposite (32 bytes): uvBox = the field's inner box in normalized compose uv
// (u0, v0, u1, v1); rowGlow = (rect-table row, 1 for Glow / 0 for Bloom, 0, 0).
struct HaloCompositeRecord {
    float4 uvBox;
    float4 rowGlow;
};

ByteAddressBuffer uRecords : register(t0, space0);

struct Output {
    float2 uv                      : TEXCOORD0;  // frame-global uv
    nointerpolation float2 rowGlow : TEXCOORD1;  // (rect-table row, glow)
    float4 pos                     : SV_Position;
};

Output main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) {
    // Two triangles of a unit quad: {(0,0),(1,0),(0,1)} and {(0,1),(1,0),(1,1)}.
    const float2 corners[6] = {
        float2(0.0f, 0.0f), float2(1.0f, 0.0f), float2(0.0f, 1.0f),
        float2(0.0f, 1.0f), float2(1.0f, 0.0f), float2(1.0f, 1.0f),
    };
    float2 corner = corners[vertexID];

    uint base = instanceID * 32u;
    HaloCompositeRecord rec;
    rec.uvBox   = asfloat(uRecords.Load4(base + 0u));
    rec.rowGlow = asfloat(uRecords.Load4(base + 16u));

    // The corner's frame-global uv, then uv→NDC with the top-left-origin V-flip.
    float2 uv = float2(lerp(rec.uvBox.x, rec.uvBox.z, corner.x),
                       lerp(rec.uvBox.y, rec.uvBox.w, corner.y));

    Output output;
    output.pos     = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.uv      = uv;
    output.rowGlow = rec.rowGlow.xy;
    return output;
}
