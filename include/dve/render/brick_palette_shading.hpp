#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "dve/brick_palette.hpp"

namespace dve::render {

// Deterministic CPU reference for deferred brick-palette shading. This module resolves cooked
// palette slots to global material records, applies a mapping policy, blends physical channels
// correctly, and counts real texture samples. It exists so that the GPU palette pipelines added in
// a later package have a byte-comparable reference to be certified against.
//
// This is not a production renderer. No GPU execution, shader certification, or timing claim is
// made here. Material records are analytic stand-ins for authored texture sets so that blending
// behavior can be tested without image assets or a device.

inline constexpr std::uint32_t kBrickPaletteShadingChannelCount = 5U;
inline constexpr std::uint32_t kBrickPaletteTriplanarProjectionCount = 3U;

struct BrickPaletteFloat3 {
    float x{};
    float y{};
    float z{};
};

enum class BrickPaletteMappingMode : std::uint8_t {
    WorldTriplanar,
    AssetUv,
};

struct BrickPaletteMaterialRecord {
    std::uint32_t globalMaterialId{};
    BrickPaletteFloat3 baseColor{1.0F, 1.0F, 1.0F};
    float roughness{1.0F};
    float metallic{};
    BrickPaletteFloat3 emissive{};
    float opacity{1.0F};
    // Tangent-space normal. Expected to be roughly unit length with a positive z component.
    BrickPaletteFloat3 normal{0.0F, 0.0F, 1.0F};
    float textureScale{1.0F};
    // Strength of the analytic detail modulation. Zero yields a flat material.
    float detailContrast{};
};

struct BrickPaletteShadingConfig {
    BrickPaletteMappingMode mapping{BrickPaletteMappingMode::WorldTriplanar};
    float triplanarSharpness{4.0F};
    // When false the reference reproduces the common defect of averaging packed normal values
    // instead of blending unpacked vectors. Retained so the defect can be measured, not shipped.
    bool blendNormalsAsVectors{true};
    // When false, base color is blended in encoded space rather than linear space.
    bool blendBaseColorInLinearSpace{true};
};

struct BrickPaletteSurfacePoint {
    BrickPaletteFloat3 worldPosition{};
    BrickPaletteFloat3 worldNormal{0.0F, 1.0F, 0.0F};
    float assetU{};
    float assetV{};
};

struct BrickPaletteShadedSample {
    BrickPaletteFloat3 baseColor{};
    float roughness{};
    float metallic{};
    BrickPaletteFloat3 emissive{};
    float opacity{};
    BrickPaletteFloat3 worldNormal{0.0F, 1.0F, 0.0F};
    std::uint8_t slotsEvaluated{};
    std::uint32_t textureSamples{};
    bool valid{};
};

struct BrickPaletteShadingStats {
    std::uint64_t shadedSamples{};
    std::uint64_t slotEvaluations{};
    std::uint64_t textureSamples{};
    std::uint64_t unresolvedSlots{};
    std::uint64_t invalidSamples{};
};

[[nodiscard]] const BrickPaletteMaterialRecord* find_brick_palette_material(
    std::span<const BrickPaletteMaterialRecord> records, std::uint32_t globalMaterialId) noexcept;

// Real per-sample texture-sample count for a given slot count and mapping policy. This replaces the
// v2.24 policy estimate, which assumed one projection per channel and therefore understates
// world-triplanar cost by the projection count.
[[nodiscard]] std::uint32_t brick_palette_texture_sample_count(
    std::uint8_t slotsEvaluated, BrickPaletteMappingMode mapping) noexcept;

[[nodiscard]] BrickPaletteShadedSample shade_brick_palette_encoding(
    const CookedBrickPalette& palette,
    std::span<const BrickPaletteMaterialRecord> records,
    const BrickPaletteSampleEncoding& encoding,
    const BrickPaletteSurfacePoint& point,
    const BrickPaletteShadingConfig& config,
    BrickPaletteShadingStats* stats = nullptr);

[[nodiscard]] BrickPaletteShadedSample shade_brick_palette_sample(
    const CookedBrickPalette& palette,
    std::span<const BrickPaletteMaterialRecord> records,
    std::size_t sampleIndex,
    const BrickPaletteSurfacePoint& point,
    const BrickPaletteShadingConfig& config,
    BrickPaletteShadingStats* stats = nullptr);

// Conventional baked-properties reference for the same sample. The baked path stores one resolved
// property set per sample, so it costs one lookup and carries no palette identity.
[[nodiscard]] BrickPaletteShadedSample bake_brick_palette_sample(
    const CookedBrickPalette& palette,
    std::span<const BrickPaletteMaterialRecord> records,
    std::size_t sampleIndex,
    const BrickPaletteSurfacePoint& point,
    const BrickPaletteShadingConfig& config,
    BrickPaletteShadingStats* stats = nullptr);

// Deferred contribution across the near/far transition. Returns 1 well inside the deferred range,
// 0 well outside it, and is continuous and non-increasing across the band.
[[nodiscard]] float brick_palette_deferred_weight(
    float cameraDistance, float deferredMaximumDistance, float transitionBand) noexcept;

// Endpoint-exact interpolation between a deferred and a baked result.
[[nodiscard]] BrickPaletteShadedSample blend_brick_palette_samples(
    const BrickPaletteShadedSample& deferred,
    const BrickPaletteShadedSample& baked,
    float deferredWeight);

[[nodiscard]] const char* to_string(BrickPaletteMappingMode value) noexcept;

} // namespace dve::render
