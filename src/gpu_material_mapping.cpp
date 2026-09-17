#include "dve/gpu_material_mapping.hpp"

namespace dve {
namespace {
static_assert(static_cast<std::uint32_t>(MaterialMappingMode::UV0) == 0U);
static_assert(static_cast<std::uint32_t>(MaterialMappingMode::UV1) == 1U);
static_assert(static_cast<std::uint32_t>(MaterialMappingMode::WorldTriplanar) == 2U);
static_assert(static_cast<std::uint32_t>(MaterialMappingMode::ObjectTriplanar) == 3U);
static_assert(static_cast<std::uint32_t>(HeightMappingMode::Off) == 0U);
static_assert(static_cast<std::uint32_t>(HeightMappingMode::OffsetParallax) == 1U);
static_assert(static_cast<std::uint32_t>(HeightMappingMode::SteepParallax) == 2U);
static_assert(static_cast<std::uint32_t>(HeightMappingMode::ParallaxOcclusion) == 3U);
std::uint32_t texture_index(const PolygonTextureBinding& binding) noexcept {
    return binding.texture.value_or(kNoGpuTextureIndex);
}
} // namespace

GpuPolygonMaterialMappingRecord pack_gpu_polygon_material_mapping(
    const PolygonMaterialBinding& material) noexcept {
    GpuPolygonMaterialMappingRecord record;
    record.mappingMode = static_cast<std::uint32_t>(material.mapping.mappingMode);
    record.heightMode = static_cast<std::uint32_t>(material.mapping.heightMode);
    record.baseColorTexture = texture_index(material.baseColor);
    record.metallicRoughnessTexture = texture_index(material.metallicRoughness);
    record.normalTexture = texture_index(material.normal);
    record.emissiveTexture = texture_index(material.emissive);
    record.opacityTexture = texture_index(material.opacity);
    record.heightTexture = texture_index(material.height);
    record.detailBaseColorTexture = texture_index(material.detailBaseColor);
    record.detailNormalTexture = texture_index(material.detailNormal);
    record.detailRoughnessTexture = texture_index(material.detailRoughness);
    record.baseTexcoord = material.baseColor.texcoord;
    record.normalTexcoord = material.normal.texcoord;
    record.heightTexcoord = material.height.texcoord;
    record.detailTexcoord = material.detailBaseColor.texture
        ? material.detailBaseColor.texcoord
        : material.detailNormal.texture ? material.detailNormal.texcoord
                                        : material.detailRoughness.texcoord;
    record.mappingFlags = material.doubleSided ? kGpuMaterialMappingDoubleSided : 0U;
    if (material.baseColor.colorSpace == PolygonTextureColorSpace::Srgb)
        record.mappingFlags |= kGpuMaterialMappingBaseColorSrgb;
    if (material.emissive.colorSpace == PolygonTextureColorSpace::Srgb)
        record.mappingFlags |= kGpuMaterialMappingEmissiveSrgb;
    if (material.detailBaseColor.colorSpace == PolygonTextureColorSpace::Srgb)
        record.mappingFlags |= kGpuMaterialMappingDetailBaseColorSrgb;
    record.baseScaleX = material.mapping.baseTransform.scale.x;
    record.baseScaleY = material.mapping.baseTransform.scale.y;
    record.baseOffsetX = material.mapping.baseTransform.offset.x;
    record.baseOffsetY = material.mapping.baseTransform.offset.y;
    record.baseRotationRadians = material.mapping.baseTransform.rotationRadians;
    record.triplanarScale = material.mapping.triplanarScale;
    record.triplanarBlendSharpness = material.mapping.triplanarBlendSharpness;
    record.detailScaleX = material.mapping.detailTransform.scale.x;
    record.detailScaleY = material.mapping.detailTransform.scale.y;
    record.detailOffsetX = material.mapping.detailTransform.offset.x;
    record.detailOffsetY = material.mapping.detailTransform.offset.y;
    record.detailRotationRadians = material.mapping.detailTransform.rotationRadians;
    record.detailColorStrength = material.mapping.detailColorStrength;
    record.detailNormalStrength = material.mapping.detailNormalStrength;
    record.detailRoughnessStrength = material.mapping.detailRoughnessStrength;
    record.detailFadeStartMeters = material.mapping.detailFadeStartMeters;
    record.detailFadeEndMeters = material.mapping.detailFadeEndMeters;
    record.heightScale = material.mapping.heightScale;
    record.heightReferencePlane = material.mapping.heightReferencePlane;
    record.maximumParallaxDistanceMeters = material.mapping.maximumParallaxDistanceMeters;
    record.minimumHeightSteps = material.mapping.minimumHeightSteps;
    record.maximumHeightSteps = material.mapping.maximumHeightSteps;
    record.refinementSteps = material.mapping.refinementSteps;
    record.normalScale = material.normalScale;
    record.detailNormalScale = material.detailNormalScale;
    record.alphaCutoff = material.alphaCutoff;
    return record;
}

} // namespace dve
