// DVE v1.2 reference compute contract for immediate 8^3 brick face extraction.
// Shader floor: SM 6.0. No shader-int64 dependency. One 8x8 group handles one brick;
// each lane owns an (x,y) column and loops over z. Six 8x8 halo planes are supplied by CPU.

struct BrickDispatch {
    uint occupancyWordOffset; // 16 uint32 words (512 bits)
    uint haloWordOffset;      // 12 uint32 words: 2 words per face, face order -X,+X,-Y,+Y,-Z,+Z
    uint materialPayloadOffsetBytes;
    uint outputOffset;
    uint sourceGeneration;
    uint brickId;
    uint materialEncoding;    // 1=UniformSolid, 2=MaskUniform, 3=LocalPalette4, 4=Palette8
    uint uniformMaterial;
};

struct FaceRecord {
    uint packedVoxel; // x:3, y:3, z:3, face:3, material:8
    uint brickId;
    uint generation;
    uint reserved;
};

StructuredBuffer<uint> gOccupancy32 : register(t0);
StructuredBuffer<uint> gHalo32 : register(t1);
ByteAddressBuffer gMaterialPayloads : register(t2);
StructuredBuffer<BrickDispatch> gDispatch : register(t3);
RWStructuredBuffer<FaceRecord> gFaces : register(u0);
RWStructuredBuffer<uint> gFaceCounts : register(u1);

groupshared uint sOccupancy[16];
groupshared uint sHalo[12];

uint LoadByte(uint byteOffset) {
    uint alignedOffset = byteOffset & ~3u;
    uint shift = (byteOffset & 3u) * 8u;
    return (gMaterialPayloads.Load(alignedOffset) >> shift) & 0xFFu;
}

bool TestOccupancy(uint index) {
    return ((sOccupancy[index >> 5u] >> (index & 31u)) & 1u) != 0u;
}

bool TestHalo(uint face, uint u, uint v) {
    uint faceIndex = u + 8u * v;
    uint word = sHalo[face * 2u + (faceIndex >> 5u)];
    return ((word >> (faceIndex & 31u)) & 1u) != 0u;
}

bool OccupiedNeighbor(uint x, uint y, uint z, uint face) {
    if (face == 0u) return x > 0u ? TestOccupancy((x - 1u) + 8u * y + 64u * z) : TestHalo(0u, y, z);
    if (face == 1u) return x < 7u ? TestOccupancy((x + 1u) + 8u * y + 64u * z) : TestHalo(1u, y, z);
    if (face == 2u) return y > 0u ? TestOccupancy(x + 8u * (y - 1u) + 64u * z) : TestHalo(2u, x, z);
    if (face == 3u) return y < 7u ? TestOccupancy(x + 8u * (y + 1u) + 64u * z) : TestHalo(3u, x, z);
    if (face == 4u) return z > 0u ? TestOccupancy(x + 8u * y + 64u * (z - 1u)) : TestHalo(4u, x, y);
    return z < 7u ? TestOccupancy(x + 8u * y + 64u * (z + 1u)) : TestHalo(5u, x, y);
}

uint LoadMaterial(BrickDispatch dispatch, uint voxelIndex) {
    if (dispatch.materialEncoding <= 2u) return dispatch.uniformMaterial & 0xFFu;
    if (dispatch.materialEncoding == 3u) {
        uint packed = LoadByte(dispatch.materialPayloadOffsetBytes + (voxelIndex >> 1u));
        uint paletteIndex = (voxelIndex & 1u) == 0u ? (packed & 0xFu) : (packed >> 4u);
        return LoadByte(dispatch.materialPayloadOffsetBytes + 256u + paletteIndex);
    }
    return LoadByte(dispatch.materialPayloadOffsetBytes + voxelIndex);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex) {
    BrickDispatch dispatch = gDispatch[groupId.x];

    if (groupIndex < 16u) sOccupancy[groupIndex] = gOccupancy32[dispatch.occupancyWordOffset + groupIndex];
    if (groupIndex < 12u) sHalo[groupIndex] = gHalo32[dispatch.haloWordOffset + groupIndex];
    GroupMemoryBarrierWithGroupSync();

    uint x = threadId.x;
    uint y = threadId.y;
    [unroll]
    for (uint z = 0u; z < 8u; ++z) {
        uint voxelIndex = x + 8u * y + 64u * z;
        bool occupied = TestOccupancy(voxelIndex);
        uint material = occupied ? LoadMaterial(dispatch, voxelIndex) : 0u;

        [unroll]
        for (uint face = 0u; face < 6u; ++face) {
            bool emit = occupied && !OccupiedNeighbor(x, y, z, face);
            uint waveCount = WaveActiveCountBits(emit);
            uint waveBase = 0u;
            if (WaveIsFirstLane() && waveCount != 0u) {
                InterlockedAdd(gFaceCounts[groupId.x], waveCount, waveBase);
            }
            waveBase = WaveReadLaneFirst(waveBase);
            if (emit) {
                uint localIndex = waveBase + WavePrefixCountBits(emit);
                FaceRecord record;
                record.packedVoxel = x | (y << 3u) | (z << 6u) | (face << 9u) | (material << 12u);
                record.brickId = dispatch.brickId;
                record.generation = dispatch.sourceGeneration;
                record.reserved = 0u;
                gFaces[dispatch.outputOffset + localIndex] = record;
            }
        }
    }
}
