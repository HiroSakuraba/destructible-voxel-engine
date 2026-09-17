#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/voxel_material_policy.hpp"

namespace dve {

inline constexpr std::uint32_t kBrickPaletteFormatVersion = 2U;
inline constexpr std::uint32_t kBrickPaletteMinimumSupportedVersion = 0U;
inline constexpr std::uint8_t kBrickPaletteMaximumSlots = 4U;
inline constexpr std::uint8_t kBrickPaletteWeightDenominator = 255U;
inline constexpr std::string_view kBrickPaletteMagic = "DVEBPAL";

enum class BrickPaletteEncoding : std::uint8_t {
    Empty,
    Single,
    Palette2,
    Palette4,
};

struct BrickMaterialContribution {
    std::uint32_t globalMaterialId{};
    float weight{};
    friend bool operator==(const BrickMaterialContribution&, const BrickMaterialContribution&) = default;
};

struct BrickSourceSample {
    std::vector<BrickMaterialContribution> contributions;
    friend bool operator==(const BrickSourceSample&, const BrickSourceSample&) = default;
};

struct BrickSourceSurface {
    std::uint64_t brickId{};
    std::string assetName;
    std::vector<BrickSourceSample> samples;
};

struct BrickPaletteCookConfig {
    std::uint8_t maximumPaletteSlots{kBrickPaletteMaximumSlots};
    VoxelMaterialPaletteOverflowPolicy overflowPolicy{VoxelMaterialPaletteOverflowPolicy::BakeProperties};
    float weightEpsilon{1.0e-4F};
    // Preserve full, canonicalized per-sample contributions so a wider palette can be recooked
    // without returning to external authoring data.
    bool preserveCanonicalSource{true};
};

struct BrickPaletteSampleEncoding {
    std::array<std::uint8_t, kBrickPaletteMaximumSlots> slotIndices{};
    std::array<std::uint8_t, kBrickPaletteMaximumSlots> quantizedWeights{};
    std::uint8_t usedSlots{};
};

struct CookedBrickPalette {
    std::uint32_t formatVersion{kBrickPaletteFormatVersion};
    std::uint32_t sourceFormatVersion{kBrickPaletteFormatVersion};
    std::uint64_t brickId{};
    std::string assetName;
    BrickPaletteEncoding encoding{BrickPaletteEncoding::Empty};
    VoxelMaterialRuntimePath runtimePath{VoxelMaterialRuntimePath::NoPath};
    std::vector<std::uint32_t> slots;
    // Retained for diagnostics and migration. Full recooking uses canonicalSamples.
    std::vector<std::uint32_t> canonicalMaterials;
    std::vector<BrickSourceSample> canonicalSamples;
    std::vector<BrickPaletteSampleEncoding> samples;
    std::uint32_t sourceMaterialCount{};
    std::uint32_t overflowSampleCount{};
    std::uint32_t degenerateSampleCount{};
    float maximumWeightError{};
    float maximumReductionError{};
    bool overflowed{};
    bool recookRequested{};
    bool valid{};
    std::string reason;
    std::uint64_t contentHash{};
};

struct BrickPaletteCookReport {
    std::uint64_t brickCount{};
    std::uint64_t emptyBrickCount{};
    std::uint64_t singleMaterialBrickCount{};
    std::uint64_t palette2BrickCount{};
    std::uint64_t palette4BrickCount{};
    std::uint64_t bakedBrickCount{};
    std::uint64_t rejectedBrickCount{};
    std::uint64_t recookRequestCount{};
    std::uint64_t overflowBrickCount{};
    std::uint64_t sampleCount{};
    std::uint64_t paletteSlotCount{};
    std::uint64_t payloadBytes{};
    float maximumWeightError{};
    float maximumReductionError{};
    std::vector<CookedBrickPalette> bricks;
    std::uint64_t contentHash{};
};

struct BrickPaletteRemapTable {
    std::vector<std::uint32_t> globalMaterialIds;
};

[[nodiscard]] CookedBrickPalette cook_brick_palette(
    const BrickSourceSurface& surface, const BrickPaletteCookConfig& config);
[[nodiscard]] BrickPaletteCookReport build_brick_palette_cook_report(
    std::span<const BrickSourceSurface> surfaces, const BrickPaletteCookConfig& config);
[[nodiscard]] std::string brick_palette_cook_json(const BrickPaletteCookReport& report);

[[nodiscard]] BrickPaletteRemapTable build_brick_palette_remap(
    std::span<const CookedBrickPalette> bricks);
[[nodiscard]] std::vector<std::uint32_t> remap_brick_palette_slots(
    const CookedBrickPalette& brick, const BrickPaletteRemapTable& table);

[[nodiscard]] float brick_palette_sample_weight(
    const BrickPaletteSampleEncoding& sample, std::uint8_t encodedIndex) noexcept;

[[nodiscard]] std::vector<std::uint8_t> serialize_brick_palette(
    const CookedBrickPalette& palette, std::uint32_t formatVersion = kBrickPaletteFormatVersion);
[[nodiscard]] bool deserialize_brick_palette(
    std::span<const std::uint8_t> bytes, CookedBrickPalette& out, std::string* error = nullptr);

[[nodiscard]] std::uint64_t brick_palette_content_hash(const CookedBrickPalette& palette) noexcept;
[[nodiscard]] const char* to_string(BrickPaletteEncoding value) noexcept;

} // namespace dve
