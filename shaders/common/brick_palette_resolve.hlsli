#ifndef DVE_BRICK_PALETTE_RESOLVE_HLSLI
#define DVE_BRICK_PALETTE_RESOLVE_HLSLI

static const uint kBrickPaletteInvalidOffset = 0xFFFFFFFFu;
static const uint kVoxelMaterialNoPath = 0u;
static const uint kVoxelMaterialBakedProperties = 1u;
static const uint kVoxelMaterialSingleMaterial = 2u;
static const uint kVoxelMaterialDeferredPalette2 = 3u;
static const uint kVoxelMaterialDeferredPalette4 = 4u;
static const uint kBrickPaletteMappingWorldTriplanar = 0u;
static const uint kBrickPaletteMappingAssetUv = 1u;
static const uint kBrickPaletteMaximumSlots = 4u;
static const uint kBrickPaletteShadingChannelCount = 5u;
static const float kBrickPalettePi = 3.14159265358979323846F;

struct BrickPaletteGpuBrickRecord {
    int3 key;
    uint sourceGeneration;
    uint runtimePath;
    uint paletteSlotOffset;
    uint paletteSlotCount;
    uint sampleOffset;
    uint sampleCount;
    uint bakedOffset;
    uint singleOffset;
    uint mappingMode;
};

struct BrickPaletteGpuSampleEncoding {
    uint packedSlotIndices;
    uint packedWeights;
};

struct BrickPaletteGpuBakedSample {
    float4 baseColorOpacity;
    float4 normalRoughness;
    float4 emissiveMetallic;
    uint valid;
    uint textureSamples;
    uint slotsEvaluated;
    uint reserved;
};

struct BrickPaletteGpuMaterialRecord {
    uint globalMaterialId;
    float baseColorX;
    float baseColorY;
    float baseColorZ;
    float roughness;
    float metallic;
    float emissiveX;
    float emissiveY;
    float emissiveZ;
    float opacity;
    float normalX;
    float normalY;
    float normalZ;
    float textureScale;
    float detailContrast;
    uint reserved;
};

struct BrickPaletteGpuResolveRequest {
    uint brickRecordIndex;
    uint sampleIndex;
    float worldPositionX;
    float worldPositionY;
    float worldPositionZ;
    float assetU;
    float worldNormalX;
    float worldNormalY;
    float worldNormalZ;
    float assetV;
    uint reserved0;
    uint reserved1;
};

struct BrickPaletteEvaluatedMaterial {
    float3 baseColor;
    float roughness;
    float metallic;
    float3 emissive;
    float opacity;
    float3 worldNormal;
};

float3 BrickPaletteNormalizeOr(float3 value, float3 fallbackValue) {
    const float magnitudeSquared = dot(value, value);
    return magnitudeSquared > 1.0e-16F ? value * rsqrt(magnitudeSquared) : fallbackValue;
}

uint BrickPaletteUnpackByte(uint packed, uint index) {
    return (packed >> (index * 8u)) & 0xFFu;
}

float BrickPaletteDetailFactor(BrickPaletteGpuMaterialRecord material, float u, float v) {
    if (!(material.detailContrast > 0.0F)) return 1.0F;
    const float pattern = 0.5F + 0.5F * sin(u * kBrickPalettePi) * sin(v * kBrickPalettePi);
    return max(1.0F + material.detailContrast * (pattern * 2.0F - 1.0F), 0.0F);
}

void BrickPaletteAccumulateProjection(
    BrickPaletteGpuMaterialRecord material,
    float weight,
    float u,
    float v,
    float3 tangent,
    float3 bitangent,
    float3 projectionNormal,
    float3 tangentNormal,
    inout BrickPaletteEvaluatedMaterial evaluated,
    inout float3 accumulatedNormal) {
    if (!(weight > 0.0F)) return;
    const float factor = BrickPaletteDetailFactor(material, u, v);
    evaluated.baseColor += float3(material.baseColorX, material.baseColorY, material.baseColorZ) *
                           (weight * factor);
    evaluated.roughness += saturate(material.roughness * factor) * weight;
    evaluated.metallic += saturate(material.metallic) * weight;
    evaluated.emissive += float3(material.emissiveX, material.emissiveY, material.emissiveZ) *
                          (weight * factor);
    evaluated.opacity += saturate(material.opacity) * weight;
    const float3 rotated = tangent * tangentNormal.x + bitangent * tangentNormal.y +
                           projectionNormal * tangentNormal.z;
    accumulatedNormal += rotated * weight;
}

BrickPaletteEvaluatedMaterial BrickPaletteEvaluateMaterial(
    BrickPaletteGpuMaterialRecord material,
    BrickPaletteGpuResolveRequest request,
    uint mappingMode) {
    BrickPaletteEvaluatedMaterial evaluated = (BrickPaletteEvaluatedMaterial)0;
    const float3 position = float3(
        request.worldPositionX, request.worldPositionY, request.worldPositionZ);
    const float3 fallbackNormal = float3(0.0F, 1.0F, 0.0F);
    const float3 surfaceNormal = BrickPaletteNormalizeOr(
        float3(request.worldNormalX, request.worldNormalY, request.worldNormalZ), fallbackNormal);
    const float3 tangentNormal = BrickPaletteNormalizeOr(
        float3(material.normalX, material.normalY, material.normalZ), float3(0.0F, 0.0F, 1.0F));
    const float textureScale = material.textureScale > 0.0F ? material.textureScale : 1.0F;
    float3 accumulatedNormal = 0.0F.xxx;

    if (mappingMode == kBrickPaletteMappingAssetUv) {
        BrickPaletteAccumulateProjection(
            material, 1.0F, request.assetU * textureScale, request.assetV * textureScale,
            float3(1.0F, 0.0F, 0.0F), float3(0.0F, 1.0F, 0.0F), surfaceNormal,
            tangentNormal, evaluated, accumulatedNormal);
    } else {
        const float3 powered = pow(max(abs(surfaceNormal), 1.0e-6F.xxx), 4.0F.xxx);
        const float total = powered.x + powered.y + powered.z;
        const float3 weights = total > 1.0e-8F
            ? powered / total : float3(1.0F, 0.0F, 0.0F);
        BrickPaletteAccumulateProjection(
            material, weights.x, position.y * textureScale, position.z * textureScale,
            float3(0.0F, 1.0F, 0.0F), float3(0.0F, 0.0F, 1.0F),
            float3(surfaceNormal.x >= 0.0F ? 1.0F : -1.0F, 0.0F, 0.0F),
            tangentNormal, evaluated, accumulatedNormal);
        BrickPaletteAccumulateProjection(
            material, weights.y, position.z * textureScale, position.x * textureScale,
            float3(0.0F, 0.0F, 1.0F), float3(1.0F, 0.0F, 0.0F),
            float3(0.0F, surfaceNormal.y >= 0.0F ? 1.0F : -1.0F, 0.0F),
            tangentNormal, evaluated, accumulatedNormal);
        BrickPaletteAccumulateProjection(
            material, weights.z, position.x * textureScale, position.y * textureScale,
            float3(1.0F, 0.0F, 0.0F), float3(0.0F, 1.0F, 0.0F),
            float3(0.0F, 0.0F, surfaceNormal.z >= 0.0F ? 1.0F : -1.0F),
            tangentNormal, evaluated, accumulatedNormal);
    }
    evaluated.worldNormal = BrickPaletteNormalizeOr(accumulatedNormal, surfaceNormal);
    return evaluated;
}

BrickPaletteGpuBakedSample BrickPaletteInvalidResolve(BrickPaletteGpuResolveRequest request) {
    BrickPaletteGpuBakedSample output = (BrickPaletteGpuBakedSample)0;
    output.normalRoughness.xyz = BrickPaletteNormalizeOr(
        float3(request.worldNormalX, request.worldNormalY, request.worldNormalZ),
        float3(0.0F, 1.0F, 0.0F));
    return output;
}

BrickPaletteGpuBakedSample BrickPalettePackResolved(
    float3 baseColor,
    float opacity,
    float3 worldNormal,
    float roughness,
    float3 emissive,
    float metallic,
    uint slotsEvaluated,
    uint mappingMode) {
    BrickPaletteGpuBakedSample output = (BrickPaletteGpuBakedSample)0;
    output.baseColorOpacity = float4(baseColor, saturate(opacity));
    output.normalRoughness = float4(BrickPaletteNormalizeOr(worldNormal, float3(0.0F, 1.0F, 0.0F)),
                                    saturate(roughness));
    output.emissiveMetallic = float4(emissive, saturate(metallic));
    output.valid = 1u;
    output.slotsEvaluated = slotsEvaluated;
    const uint projections = mappingMode == kBrickPaletteMappingWorldTriplanar ? 3u : 1u;
    output.textureSamples = slotsEvaluated * kBrickPaletteShadingChannelCount * projections;
    return output;
}

#endif // DVE_BRICK_PALETTE_RESOLVE_HLSLI
