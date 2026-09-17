#include "common/gabor_volume.hlsli"
cbuffer GaborFrameConstants : register(b4) {
    uint primitiveCount; uint tileCountX; uint tileCountY; uint maximumPrimitivesPerTile;
    uint viewportWidth; uint viewportHeight; float lodBias; float projectedDiameterPixels;
};
StructuredBuffer<GaborPrimitive> gGaborPrimitives : register(t20);
RWStructuredBuffer<uint> gGaborTileCounts : register(u10);
RWStructuredBuffer<uint> gGaborTileIndices : register(u11);
RWStructuredBuffer<uint> gGaborOverflowCounter : register(u12);
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= primitiveCount) return;
    // Conservative bounded assignment. Production backends replace this with projected ellipsoid bounds.
    uint tile = id.x % max(1u, tileCountX * tileCountY);
    uint slot;
    InterlockedAdd(gGaborTileCounts[tile], 1u, slot);
    if (slot < maximumPrimitivesPerTile) gGaborTileIndices[tile * maximumPrimitivesPerTile + slot] = id.x;
    else InterlockedAdd(gGaborOverflowCounter[0], 1u);
}
