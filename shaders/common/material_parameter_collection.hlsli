#ifndef DVE_MATERIAL_PARAMETER_COLLECTION_HLSLI
#define DVE_MATERIAL_PARAMETER_COLLECTION_HLSLI

// Stable-slot global material parameters. The CPU-side mirror is
// include/dve/gpu_material_parameter_collection.hpp. Scalars are packed four per cbuffer
// register to avoid compiler-specific scalar-array strides; vectors occupy one register each.
cbuffer MaterialParameterCollectionConstants : register(b4) {
    float4 gMaterialGlobalScalarGroups[8];
    float4 gMaterialGlobalVectors[16];
    uint gMaterialGlobalScalarCount;
    uint gMaterialGlobalVectorCount;
    uint gMaterialGlobalReserved0;
    uint gMaterialGlobalReserved1;
}

static const uint kMpcTimeSeconds = 0u;
static const uint kMpcWindStrength = 1u;
static const uint kMpcWetness = 2u;
static const uint kMpcSnowAmount = 3u;
static const uint kMpcTimeOfDayTint = 0u;
static const uint kMpcWindDirection = 1u;

float MaterialGlobalScalar(uint slot) {
    if (slot >= gMaterialGlobalScalarCount) return 0.0F;
    float4 group = gMaterialGlobalScalarGroups[slot >> 2u];
    uint component = slot & 3u;
    return component == 0u ? group.x : component == 1u ? group.y : component == 2u ? group.z : group.w;
}

float4 MaterialGlobalVector(uint slot) {
    return slot < gMaterialGlobalVectorCount ? gMaterialGlobalVectors[slot] : float4(0.0F, 0.0F, 0.0F, 0.0F);
}

#endif // DVE_MATERIAL_PARAMETER_COLLECTION_HLSLI
