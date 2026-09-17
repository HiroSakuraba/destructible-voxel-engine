#pragma once

#include <cstddef>
#include <cstdint>

#include "dve/polygon_asset.hpp"

namespace dve {

inline constexpr std::uint32_t kNoGpuTextureIndex = 0xFFFFFFFFU;
inline constexpr std::uint32_t kGpuMaterialMappingDoubleSided = 1U << 0U;
inline constexpr std::uint32_t kGpuMaterialMappingBaseColorSrgb = 1U << 1U;
inline constexpr std::uint32_t kGpuMaterialMappingEmissiveSrgb = 1U << 2U;
inline constexpr std::uint32_t kGpuMaterialMappingDetailBaseColorSrgb = 1U << 3U;

// Byte-exact mirror of shaders/common/material_mapping.hlsli. Texture indices refer to the
// renderer-owned polygon texture table; 0xFFFFFFFF means that the channel is unbound.
struct GpuPolygonMaterialMappingRecord {
    std::uint32_t mappingMode{};
    std::uint32_t heightMode{};
    std::uint32_t baseColorTexture{kNoGpuTextureIndex};
    std::uint32_t metallicRoughnessTexture{kNoGpuTextureIndex};

    std::uint32_t normalTexture{kNoGpuTextureIndex};
    std::uint32_t emissiveTexture{kNoGpuTextureIndex};
    std::uint32_t opacityTexture{kNoGpuTextureIndex};
    std::uint32_t heightTexture{kNoGpuTextureIndex};

    std::uint32_t detailBaseColorTexture{kNoGpuTextureIndex};
    std::uint32_t detailNormalTexture{kNoGpuTextureIndex};
    std::uint32_t detailRoughnessTexture{kNoGpuTextureIndex};
    std::uint32_t baseTexcoord{};

    std::uint32_t normalTexcoord{};
    std::uint32_t heightTexcoord{};
    std::uint32_t detailTexcoord{};
    std::uint32_t mappingFlags{};

    float baseScaleX{1.0F};
    float baseScaleY{1.0F};
    float baseOffsetX{};
    float baseOffsetY{};

    float baseRotationRadians{};
    float triplanarScale{1.0F};
    float triplanarBlendSharpness{4.0F};
    float detailScaleX{8.0F};

    float detailScaleY{8.0F};
    float detailOffsetX{};
    float detailOffsetY{};
    float detailRotationRadians{};

    float detailColorStrength{};
    float detailNormalStrength{1.0F};
    float detailRoughnessStrength{};
    float detailFadeStartMeters{2.0F};

    float detailFadeEndMeters{20.0F};
    float heightScale{0.04F};
    float heightReferencePlane{0.5F};
    float maximumParallaxDistanceMeters{20.0F};

    std::uint32_t minimumHeightSteps{8U};
    std::uint32_t maximumHeightSteps{32U};
    std::uint32_t refinementSteps{4U};
    float normalScale{1.0F};

    float detailNormalScale{1.0F};
    float alphaCutoff{0.5F};
    std::uint32_t reserved1{};
    std::uint32_t reserved2{};
};

static_assert(sizeof(GpuPolygonMaterialMappingRecord) == 44U * sizeof(std::uint32_t));
static_assert(alignof(GpuPolygonMaterialMappingRecord) == 4U);

[[nodiscard]] GpuPolygonMaterialMappingRecord pack_gpu_polygon_material_mapping(
    const PolygonMaterialBinding& material) noexcept;

} // namespace dve
