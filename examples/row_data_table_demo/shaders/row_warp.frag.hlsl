// Reads a per-scanline value the game cannot express as one sine: a hand-built, digitized per-line
// HORIZONTAL SCALE ramp, supplied through the effect's per-row data table (paramRowAtUv reads the row
// under this fragment). The x lane is the per-line scale about the screen center, so each scanline
// stretches or squeezes horizontally by its own amount — a domed warp whose profile is whatever the
// game wrote into the table that frame. It samples through sampleSource(), so the edge policy is the
// effect's (Blank reveals what is behind; Stretch clamps). Slow drift, no strobe.
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float scale = paramRowAtUv(uv).x;            // per-line horizontal scale from the table
    float2 c = float2(0.5f, 0.5f);
    return sampleSource(c + (uv - c) * float2(scale, 1.0f));
}
