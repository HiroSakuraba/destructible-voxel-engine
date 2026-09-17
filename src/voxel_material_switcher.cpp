#include "dve/voxel_material_switcher.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace dve {
namespace {

VoxelMaterialRuntimePath first_available_fallback(
    const VoxelMaterialRepresentationSet& representations,
    VoxelMaterialRuntimePath preferred) noexcept {
    if (voxel_material_representation_available(representations, preferred)) return preferred;
    constexpr std::array<VoxelMaterialRuntimePath, 4U> order{
        VoxelMaterialRuntimePath::BakedProperties,
        VoxelMaterialRuntimePath::SingleMaterial,
        VoxelMaterialRuntimePath::DeferredPalette2,
        VoxelMaterialRuntimePath::DeferredPalette4,
    };
    for (const auto path : order)
        if (voxel_material_representation_available(representations, path)) return path;
    return VoxelMaterialRuntimePath::NoPath;
}

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

const render::BrickPaletteMaterialRecord* find_material(
    std::span<const render::BrickPaletteMaterialRecord> materials,
    std::uint32_t materialId) noexcept {
    return render::find_brick_palette_material(materials, materialId);
}

} // namespace

bool voxel_material_representation_available(
    const VoxelMaterialRepresentationSet& representations,
    VoxelMaterialRuntimePath path) noexcept {
    switch (path) {
        case VoxelMaterialRuntimePath::NoPath: return false;
        case VoxelMaterialRuntimePath::BakedProperties:
            return representations.bakedProperties.has_value();
        case VoxelMaterialRuntimePath::SingleMaterial:
            return representations.singleMaterialIds.has_value();
        case VoxelMaterialRuntimePath::DeferredPalette2:
            return representations.deferredPalette2.has_value();
        case VoxelMaterialRuntimePath::DeferredPalette4:
            return representations.deferredPalette4.has_value();
    }
    return false;
}

std::size_t voxel_material_retained_bytes(
    const VoxelMaterialRepresentationSet& representations) {
    std::size_t bytes = 0U;
    if (representations.bakedProperties.has_value())
        bytes += representations.bakedProperties->size() * sizeof(render::BrickPaletteShadedSample);
    if (representations.singleMaterialIds.has_value())
        bytes += representations.singleMaterialIds->size() * sizeof(std::uint32_t);
    if (representations.deferredPalette2.has_value())
        bytes += serialize_brick_palette(*representations.deferredPalette2).size();
    if (representations.deferredPalette4.has_value())
        bytes += serialize_brick_palette(*representations.deferredPalette4).size();
    if (representations.canonicalSource.has_value()) {
        for (const auto& sample : representations.canonicalSource->samples)
            bytes += sample.contributions.size() * sizeof(BrickMaterialContribution);
    }
    return bytes;
}

VoxelMaterialSwitchResult request_voxel_material_switch(
    VoxelMaterialSwitcherState& state,
    const VoxelMaterialRepresentationSet& representations,
    VoxelMaterialRuntimePath requestedPath,
    bool allowFallback) {
    VoxelMaterialSwitchResult result;
    result.requestedPath = requestedPath;
    state.requestedPath = requestedPath;

    if (requestedPath == VoxelMaterialRuntimePath::NoPath) {
        result.reason = "NoPath is not a renderable material representation";
        result.activePath = state.activePath;
        return result;
    }
    if (voxel_material_representation_available(representations, requestedPath)) {
        state.activePath = requestedPath;
        state.pendingRecookPath = VoxelMaterialRuntimePath::NoPath;
        state.appliedGeneration = representations.generation;
        result.valid = true;
        result.completedImmediately = true;
        result.activePath = requestedPath;
        result.reason = "requested representation was already retained";
        return result;
    }

    const auto fallback = allowFallback
        ? first_available_fallback(representations, state.activePath)
        : VoxelMaterialRuntimePath::NoPath;
    if (representations.canonicalSource.has_value()) {
        state.pendingRecookPath = requestedPath;
        result.recookRequested = true;
        result.reason = "requested representation is missing; recook requested";
    } else {
        state.pendingRecookPath = VoxelMaterialRuntimePath::NoPath;
        result.reason = "requested representation is missing and canonical source was stripped";
    }

    if (fallback != VoxelMaterialRuntimePath::NoPath) {
        state.activePath = fallback;
        state.appliedGeneration = representations.generation;
        result.valid = true;
        result.usedFallback = true;
        result.activePath = fallback;
        result.reason += "; retained fallback remains active";
    } else {
        state.activePath = VoxelMaterialRuntimePath::NoPath;
        result.activePath = VoxelMaterialRuntimePath::NoPath;
        result.reason += "; no fallback representation is available";
    }
    return result;
}

bool recook_voxel_material_representation(
    VoxelMaterialRepresentationSet& representations,
    VoxelMaterialRuntimePath requestedPath,
    VoxelMaterialPaletteOverflowPolicy overflowPolicy,
    std::string* error) {
    if (!representations.canonicalSource.has_value()) {
        set_error(error, "canonical source is unavailable");
        return false;
    }
    std::uint8_t slots = 0U;
    switch (requestedPath) {
        case VoxelMaterialRuntimePath::DeferredPalette2: slots = 2U; break;
        case VoxelMaterialRuntimePath::DeferredPalette4: slots = 4U; break;
        default:
            set_error(error, "only deferred palette representations can be recooked");
            return false;
    }

    BrickPaletteCookConfig config;
    config.maximumPaletteSlots = slots;
    config.overflowPolicy = overflowPolicy;
    config.preserveCanonicalSource = true;
    auto cooked = cook_brick_palette(*representations.canonicalSource, config);
    if (!cooked.valid || cooked.runtimePath != requestedPath) {
        set_error(error, cooked.reason.empty() ? "recook did not produce the requested path" : cooked.reason);
        return false;
    }
    if (requestedPath == VoxelMaterialRuntimePath::DeferredPalette2)
        representations.deferredPalette2 = std::move(cooked);
    else
        representations.deferredPalette4 = std::move(cooked);
    ++representations.generation;
    return true;
}

VoxelMaterialSwitchResult complete_voxel_material_recook(
    VoxelMaterialSwitcherState& state,
    const VoxelMaterialRepresentationSet& representations,
    VoxelMaterialRuntimePath completedPath) {
    VoxelMaterialSwitchResult result;
    result.requestedPath = state.requestedPath;
    result.activePath = state.activePath;
    if (state.pendingRecookPath != completedPath) {
        result.reason = "completed representation does not match the pending recook";
        return result;
    }
    if (!voxel_material_representation_available(representations, completedPath)) {
        result.reason = "recook completion was reported before the representation was installed";
        return result;
    }
    state.activePath = completedPath;
    state.pendingRecookPath = VoxelMaterialRuntimePath::NoPath;
    state.appliedGeneration = representations.generation;
    result.valid = true;
    result.completedImmediately = true;
    result.activePath = completedPath;
    result.reason = "recooked representation installed atomically";
    return result;
}

render::BrickPaletteShadedSample shade_active_voxel_material_sample(
    const VoxelMaterialSwitcherState& state,
    const VoxelMaterialRepresentationSet& representations,
    std::size_t sampleIndex,
    std::span<const render::BrickPaletteMaterialRecord> materials,
    const render::BrickPaletteSurfacePoint& point,
    const render::BrickPaletteShadingConfig& config,
    render::BrickPaletteShadingStats* stats) {
    render::BrickPaletteShadedSample result;
    result.worldNormal = point.worldNormal;
    switch (state.activePath) {
        case VoxelMaterialRuntimePath::NoPath:
            return result;
        case VoxelMaterialRuntimePath::BakedProperties:
            if (!representations.bakedProperties.has_value() ||
                sampleIndex >= representations.bakedProperties->size()) return result;
            result = (*representations.bakedProperties)[sampleIndex];
            if (stats != nullptr) {
                ++stats->shadedSamples;
                stats->textureSamples += result.valid ? 1U : 0U;
                if (!result.valid) ++stats->invalidSamples;
            }
            return result;
        case VoxelMaterialRuntimePath::SingleMaterial: {
            if (!representations.singleMaterialIds.has_value() ||
                sampleIndex >= representations.singleMaterialIds->size()) return result;
            const std::uint32_t materialId = (*representations.singleMaterialIds)[sampleIndex];
            if (find_material(materials, materialId) == nullptr) return result;
            CookedBrickPalette single;
            single.valid = true;
            single.encoding = BrickPaletteEncoding::Single;
            single.runtimePath = VoxelMaterialRuntimePath::SingleMaterial;
            single.slots = {materialId};
            BrickPaletteSampleEncoding encoding;
            encoding.usedSlots = 1U;
            encoding.quantizedWeights[0] = kBrickPaletteWeightDenominator;
            single.samples = {encoding};
            return render::shade_brick_palette_sample(single, materials, 0U, point, config, stats);
        }
        case VoxelMaterialRuntimePath::DeferredPalette2:
            if (!representations.deferredPalette2.has_value()) return result;
            return render::shade_brick_palette_sample(
                *representations.deferredPalette2, materials, sampleIndex, point, config, stats);
        case VoxelMaterialRuntimePath::DeferredPalette4:
            if (!representations.deferredPalette4.has_value()) return result;
            return render::shade_brick_palette_sample(
                *representations.deferredPalette4, materials, sampleIndex, point, config, stats);
    }
    return result;
}

} // namespace dve
