// Packed-data conformance kernel for the v2.26 brick-palette upload contract. This executes the
// same analytic material model as the CPU reference. It is intentionally separate from the final
// texture-resident fragment path so packet decoding and blend semantics can be certified first.

#include "common/brick_palette_resolve.hlsli"

StructuredBuffer<BrickPaletteGpuBrickRecord> gBrickPaletteRecords : register(t0);
StructuredBuffer<uint> gBrickPaletteRemapGlobalMaterialIds : register(t1);
StructuredBuffer<uint> gBrickPaletteSlots : register(t2);
StructuredBuffer<BrickPaletteGpuSampleEncoding> gBrickPaletteSamples : register(t3);
StructuredBuffer<BrickPaletteGpuBakedSample> gBrickPaletteBakedSamples : register(t4);
StructuredBuffer<uint> gBrickPaletteSingleMaterialIds : register(t5);
StructuredBuffer<BrickPaletteGpuMaterialRecord> gBrickPaletteMaterials : register(t6);
StructuredBuffer<BrickPaletteGpuResolveRequest> gBrickPaletteResolveRequests : register(t7);
RWStructuredBuffer<BrickPaletteGpuBakedSample> gBrickPaletteResolveOutputs : register(u8);

bool BrickPaletteFindMaterial(uint globalMaterialId, out BrickPaletteGpuMaterialRecord material) {
    uint materialCount;
    uint materialStride;
    gBrickPaletteMaterials.GetDimensions(materialCount, materialStride);
    for (uint index = 0u; index < materialCount; ++index) {
        BrickPaletteGpuMaterialRecord candidate = gBrickPaletteMaterials[index];
        if (candidate.globalMaterialId == globalMaterialId) {
            material = candidate;
            return true;
        }
    }
    material = (BrickPaletteGpuMaterialRecord)0;
    return false;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint requestCount;
    uint requestStride;
    gBrickPaletteResolveRequests.GetDimensions(requestCount, requestStride);
    const uint requestIndex = dispatchThreadId.x;
    if (requestIndex >= requestCount) return;

    const BrickPaletteGpuResolveRequest request = gBrickPaletteResolveRequests[requestIndex];
    BrickPaletteGpuBakedSample invalid = BrickPaletteInvalidResolve(request);
    uint recordCount;
    uint recordStride;
    gBrickPaletteRecords.GetDimensions(recordCount, recordStride);
    if (request.brickRecordIndex >= recordCount) {
        gBrickPaletteResolveOutputs[requestIndex] = invalid;
        return;
    }

    const BrickPaletteGpuBrickRecord record = gBrickPaletteRecords[request.brickRecordIndex];
    if (request.sampleIndex >= record.sampleCount ||
        (record.mappingMode != kBrickPaletteMappingWorldTriplanar &&
         record.mappingMode != kBrickPaletteMappingAssetUv)) {
        gBrickPaletteResolveOutputs[requestIndex] = invalid;
        return;
    }

    if (record.runtimePath == kVoxelMaterialBakedProperties) {
        uint bakedCount;
        uint bakedStride;
        gBrickPaletteBakedSamples.GetDimensions(bakedCount, bakedStride);
        const uint bakedIndex = record.bakedOffset + request.sampleIndex;
        gBrickPaletteResolveOutputs[requestIndex] =
            record.bakedOffset != kBrickPaletteInvalidOffset && bakedIndex < bakedCount
                ? gBrickPaletteBakedSamples[bakedIndex] : invalid;
        return;
    }

    if (record.runtimePath == kVoxelMaterialSingleMaterial) {
        uint singleCount;
        uint singleStride;
        gBrickPaletteSingleMaterialIds.GetDimensions(singleCount, singleStride);
        const uint singleIndex = record.singleOffset + request.sampleIndex;
        if (record.singleOffset == kBrickPaletteInvalidOffset || singleIndex >= singleCount) {
            gBrickPaletteResolveOutputs[requestIndex] = invalid;
            return;
        }
        BrickPaletteGpuMaterialRecord material;
        if (!BrickPaletteFindMaterial(gBrickPaletteSingleMaterialIds[singleIndex], material)) {
            gBrickPaletteResolveOutputs[requestIndex] = invalid;
            return;
        }
        const BrickPaletteEvaluatedMaterial evaluated =
            BrickPaletteEvaluateMaterial(material, request, record.mappingMode);
        gBrickPaletteResolveOutputs[requestIndex] = BrickPalettePackResolved(
            evaluated.baseColor, evaluated.opacity, evaluated.worldNormal, evaluated.roughness,
            evaluated.emissive, evaluated.metallic, 1u, record.mappingMode);
        return;
    }

    if (record.runtimePath != kVoxelMaterialDeferredPalette2 &&
        record.runtimePath != kVoxelMaterialDeferredPalette4) {
        gBrickPaletteResolveOutputs[requestIndex] = invalid;
        return;
    }

    uint sampleCount;
    uint sampleStride;
    gBrickPaletteSamples.GetDimensions(sampleCount, sampleStride);
    const uint encodedIndex = record.sampleOffset + request.sampleIndex;
    if (record.sampleOffset == kBrickPaletteInvalidOffset || encodedIndex >= sampleCount) {
        gBrickPaletteResolveOutputs[requestIndex] = invalid;
        return;
    }
    const BrickPaletteGpuSampleEncoding encoding = gBrickPaletteSamples[encodedIndex];
    const uint pathMaximum = record.runtimePath == kVoxelMaterialDeferredPalette2 ? 2u : 4u;
    const uint slotLimit = min(min(record.paletteSlotCount, pathMaximum), kBrickPaletteMaximumSlots);

    uint slotArenaCount;
    uint slotArenaStride;
    gBrickPaletteSlots.GetDimensions(slotArenaCount, slotArenaStride);
    uint remapCount;
    uint remapStride;
    gBrickPaletteRemapGlobalMaterialIds.GetDimensions(remapCount, remapStride);

    float3 accumulatedBaseColor = 0.0F.xxx;
    float accumulatedRoughness = 0.0F;
    float accumulatedMetallic = 0.0F;
    float3 accumulatedEmissive = 0.0F.xxx;
    float accumulatedOpacity = 0.0F;
    float3 accumulatedNormal = 0.0F.xxx;
    float accumulatedWeight = 0.0F;
    uint slotsEvaluated = 0u;

    [unroll] for (uint encodedSlot = 0u; encodedSlot < kBrickPaletteMaximumSlots; ++encodedSlot) {
        if (encodedSlot >= slotLimit) break;
        const uint quantizedWeight = BrickPaletteUnpackByte(encoding.packedWeights, encodedSlot);
        if (quantizedWeight == 0u) continue;
        const uint localSlot = BrickPaletteUnpackByte(encoding.packedSlotIndices, encodedSlot);
        if (localSlot >= record.paletteSlotCount) continue;
        const uint slotIndex = record.paletteSlotOffset + localSlot;
        if (record.paletteSlotOffset == kBrickPaletteInvalidOffset || slotIndex >= slotArenaCount) continue;
        const uint remapIndex = gBrickPaletteSlots[slotIndex];
        if (remapIndex >= remapCount) continue;
        BrickPaletteGpuMaterialRecord material;
        if (!BrickPaletteFindMaterial(gBrickPaletteRemapGlobalMaterialIds[remapIndex], material)) continue;

        const float weight = float(quantizedWeight) / 255.0F;
        const BrickPaletteEvaluatedMaterial evaluated =
            BrickPaletteEvaluateMaterial(material, request, record.mappingMode);
        accumulatedBaseColor += evaluated.baseColor * weight;
        accumulatedRoughness += evaluated.roughness * weight;
        accumulatedMetallic += evaluated.metallic * weight;
        accumulatedEmissive += evaluated.emissive * weight;
        accumulatedOpacity += evaluated.opacity * weight;
        accumulatedNormal += evaluated.worldNormal * weight;
        accumulatedWeight += weight;
        ++slotsEvaluated;
    }

    if (slotsEvaluated == 0u || !(accumulatedWeight > 0.0F)) {
        gBrickPaletteResolveOutputs[requestIndex] = invalid;
        return;
    }
    const float inverseWeight = rcp(accumulatedWeight);
    gBrickPaletteResolveOutputs[requestIndex] = BrickPalettePackResolved(
        accumulatedBaseColor * inverseWeight,
        accumulatedOpacity * inverseWeight,
        accumulatedNormal * inverseWeight,
        accumulatedRoughness * inverseWeight,
        accumulatedEmissive * inverseWeight,
        accumulatedMetallic * inverseWeight,
        slotsEvaluated,
        record.mappingMode);
}
