// BuildRadianceCascadeProbes: choose the same centre texel as the CPU SPWI reference.
// Host guarantees nonzero dimensions and a probe grid of ceil(width/spacing) by
// ceil(height/spacing). The compact primary-hit input is populated by the host for now.
struct SurfaceSample {
    float3 position;
    float viewDistance;
    float3 normal;
    uint valid;
};

StructuredBuffer<SurfaceSample> gPrimarySamples : register(t0);
RWStructuredBuffer<SurfaceSample> gRcProbes : register(u1);
cbuffer RcProbeConstants : register(b2) {
    uint gWidth;
    uint gHeight;
    uint gSpacing;
    uint gProbesX;
    uint gProbesY;
    uint3 gPadding;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint index = dispatchThreadId.x;
    if (index >= gProbesX * gProbesY) return;
    uint x = min((index % gProbesX) * gSpacing + gSpacing / 2u, gWidth - 1u);
    uint y = min((index / gProbesX) * gSpacing + gSpacing / 2u, gHeight - 1u);
    gRcProbes[index] = gPrimarySamples[y * gWidth + x];
}
