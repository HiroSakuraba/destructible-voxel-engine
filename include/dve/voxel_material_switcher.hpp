#pragma once

#include "dve/brick_palette.hpp"
#include "dve/render/brick_palette_shading.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dve {

struct VoxelMaterialRepresentationSet {
    std::optional<std::vector<render::BrickPaletteShadedSample>> bakedProperties;
    std::optional<std::vector<std::uint32_t>> singleMaterialIds;
    std::optional<CookedBrickPalette> deferredPalette2;
    std::optional<CookedBrickPalette> deferredPalette4;
    std::optional<BrickSourceSurface> canonicalSource;
    std::uint64_t generation{};
};

struct VoxelMaterialSwitcherState {
    VoxelMaterialRuntimePath activePath{VoxelMaterialRuntimePath::NoPath};
    VoxelMaterialRuntimePath requestedPath{VoxelMaterialRuntimePath::NoPath};
    VoxelMaterialRuntimePath pendingRecookPath{VoxelMaterialRuntimePath::NoPath};
    std::uint64_t appliedGeneration{};
};

struct VoxelMaterialSwitchResult {
    bool valid{};
    bool completedImmediately{};
    bool usedFallback{};
    bool recookRequested{};
    VoxelMaterialRuntimePath activePath{VoxelMaterialRuntimePath::NoPath};
    VoxelMaterialRuntimePath requestedPath{VoxelMaterialRuntimePath::NoPath};
    std::string reason;
};

[[nodiscard]] bool voxel_material_representation_available(
    const VoxelMaterialRepresentationSet& representations,
    VoxelMaterialRuntimePath path) noexcept;

[[nodiscard]] std::size_t voxel_material_retained_bytes(
    const VoxelMaterialRepresentationSet& representations);

[[nodiscard]] VoxelMaterialSwitchResult request_voxel_material_switch(
    VoxelMaterialSwitcherState& state,
    const VoxelMaterialRepresentationSet& representations,
    VoxelMaterialRuntimePath requestedPath,
    bool allowFallback = true);

[[nodiscard]] bool recook_voxel_material_representation(
    VoxelMaterialRepresentationSet& representations,
    VoxelMaterialRuntimePath requestedPath,
    VoxelMaterialPaletteOverflowPolicy overflowPolicy,
    std::string* error = nullptr);

[[nodiscard]] VoxelMaterialSwitchResult complete_voxel_material_recook(
    VoxelMaterialSwitcherState& state,
    const VoxelMaterialRepresentationSet& representations,
    VoxelMaterialRuntimePath completedPath);

[[nodiscard]] render::BrickPaletteShadedSample shade_active_voxel_material_sample(
    const VoxelMaterialSwitcherState& state,
    const VoxelMaterialRepresentationSet& representations,
    std::size_t sampleIndex,
    std::span<const render::BrickPaletteMaterialRecord> materials,
    const render::BrickPaletteSurfacePoint& point,
    const render::BrickPaletteShadingConfig& config,
    render::BrickPaletteShadingStats* stats = nullptr);

} // namespace dve
