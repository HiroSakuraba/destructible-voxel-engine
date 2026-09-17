#include "dve/voxel_material_policy.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <tuple>
#include <type_traits>

namespace dve {
namespace {

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= 1099511628211ULL;
}

template<class Integer>
void hash_integer(std::uint64_t& hash, Integer value) noexcept {
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t shift = 0U; shift < sizeof(Unsigned) * 8U; shift += 8U)
        hash_byte(hash, static_cast<std::uint8_t>((bits >> shift) & static_cast<Unsigned>(0xFFU)));
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_integer(hash, value.size());
    for (const char character : value) hash_byte(hash, static_cast<std::uint8_t>(character));
}

void hash_float(std::uint64_t& hash, float value) noexcept {
    std::uint32_t bits{};
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    hash_integer(hash, bits);
}

[[nodiscard]] std::uint32_t samples_for(VoxelMaterialRuntimePath path) noexcept {
    switch (path) {
        case VoxelMaterialRuntimePath::NoPath: return 0U;
        case VoxelMaterialRuntimePath::BakedProperties: return 1U;
        case VoxelMaterialRuntimePath::SingleMaterial: return 5U;
        case VoxelMaterialRuntimePath::DeferredPalette2: return 30U;
        case VoxelMaterialRuntimePath::DeferredPalette4: return 60U;
    }
    return 0U;
}

[[nodiscard]] bool available(VoxelMaterialRuntimePath path,
                             const VoxelMaterialCookedRepresentations& cooked) noexcept {
    switch (path) {
        case VoxelMaterialRuntimePath::NoPath: return false;
        case VoxelMaterialRuntimePath::BakedProperties: return cooked.bakedProperties;
        case VoxelMaterialRuntimePath::SingleMaterial: return cooked.singleMaterial;
        case VoxelMaterialRuntimePath::DeferredPalette2: return cooked.deferredPalette2;
        case VoxelMaterialRuntimePath::DeferredPalette4: return cooked.deferredPalette4;
    }
    return false;
}

[[nodiscard]] VoxelMaterialRuntimePath best_existing_fallback(
    const VoxelMaterialCookedRepresentations& cooked) noexcept {
    if (cooked.bakedProperties) return VoxelMaterialRuntimePath::BakedProperties;
    if (cooked.singleMaterial) return VoxelMaterialRuntimePath::SingleMaterial;
    if (cooked.deferredPalette2) return VoxelMaterialRuntimePath::DeferredPalette2;
    if (cooked.deferredPalette4) return VoxelMaterialRuntimePath::DeferredPalette4;
    return VoxelMaterialRuntimePath::NoPath;
}

void finalize(VoxelMaterialSelectionDecision& decision,
              const VoxelMaterialBrickContext& brick) noexcept {
    switch (decision.runtimePath) {
        case VoxelMaterialRuntimePath::NoPath:
        case VoxelMaterialRuntimePath::BakedProperties:
            decision.materialSlotsUsed = 0U;
            break;
        case VoxelMaterialRuntimePath::SingleMaterial:
            decision.materialSlotsUsed = 1U;
            break;
        case VoxelMaterialRuntimePath::DeferredPalette2:
            decision.materialSlotsUsed = std::min<std::uint8_t>(brick.sourceMaterialCount, 2U);
            break;
        case VoxelMaterialRuntimePath::DeferredPalette4:
            decision.materialSlotsUsed = std::min<std::uint8_t>(brick.sourceMaterialCount, 4U);
            break;
    }
    decision.estimatedTextureSamples = samples_for(decision.runtimePath);
    decision.additionalTextureSamples = decision.estimatedTextureSamples > 5U
        ? decision.estimatedTextureSamples - 5U : 0U;
    decision.valid = decision.runtimePath != VoxelMaterialRuntimePath::NoPath;

    std::uint64_t hash = 1469598103934665603ULL;
    hash_integer(hash, decision.brickId);
    hash_string(hash, decision.assetName);
    hash_integer(hash, static_cast<std::uint8_t>(decision.requestedMode));
    hash_integer(hash, static_cast<std::uint8_t>(decision.effectiveMode));
    hash_integer(hash, static_cast<std::uint8_t>(decision.runtimePath));
    hash_integer(hash, decision.materialSlotsUsed);
    hash_integer(hash, decision.estimatedTextureSamples);
    hash_integer(hash, decision.additionalTextureSamples);
    hash_byte(hash, static_cast<std::uint8_t>(decision.usedFallback));
    hash_byte(hash, static_cast<std::uint8_t>(decision.recookRequested));
    hash_byte(hash, static_cast<std::uint8_t>(decision.runtimeSwitchAvailable));
    hash_byte(hash, static_cast<std::uint8_t>(decision.sourceDataSufficient));
    hash_byte(hash, static_cast<std::uint8_t>(decision.valid));
    hash_string(hash, decision.reason);
    hash_float(hash, brick.cameraDistance);
    hash_integer(hash, brick.lod);
    hash_integer(hash, brick.sourceMaterialCount);
    decision.contentHash = hash;
}

void choose_or_fallback(VoxelMaterialSelectionDecision& decision,
                        VoxelMaterialRuntimePath wanted,
                        const VoxelMaterialSelectionRequest& request,
                        std::string reason) {
    const auto& brick = request.brick;
    if (available(wanted, brick.cooked)) {
        decision.runtimePath = wanted;
        decision.reason = std::move(reason);
        return;
    }

    decision.usedFallback = true;
    decision.sourceDataSufficient = brick.canonicalMaterialMembershipAvailable;
    if (brick.canonicalMaterialMembershipAvailable) decision.recookRequested = true;
    decision.runtimePath = best_existing_fallback(brick.cooked);
    if (decision.runtimePath == VoxelMaterialRuntimePath::NoPath) {
        decision.reason = std::move(reason) +
            "; required representation is missing and no fallback is cooked";
    } else {
        decision.reason = std::move(reason) + "; using cooked " +
            std::string(to_string(decision.runtimePath)) + " fallback";
    }
}

[[nodiscard]] VoxelMaterialRuntimePath wanted_deferred_path(
    const VoxelMaterialPolicyConfig& policy,
    const VoxelMaterialBrickContext& brick) noexcept {
    if (brick.sourceMaterialCount <= 1U) return VoxelMaterialRuntimePath::SingleMaterial;
    if (brick.sourceMaterialCount <= 2U) return VoxelMaterialRuntimePath::DeferredPalette2;
    if (policy.enableFourWayBlending && policy.maximumPaletteSlots >= 4U &&
        brick.sourceMaterialCount <= 4U)
        return VoxelMaterialRuntimePath::DeferredPalette4;
    return VoxelMaterialRuntimePath::NoPath;
}

void choose_overflow_path(VoxelMaterialSelectionDecision& decision,
                          const VoxelMaterialSelectionRequest& request,
                          const VoxelMaterialPolicyConfig& policy) {
    switch (policy.overflowPolicy) {
        case VoxelMaterialPaletteOverflowPolicy::DominantMaterial:
            choose_or_fallback(decision, VoxelMaterialRuntimePath::SingleMaterial, request,
                               "palette overflow reduced to the dominant material");
            break;
        case VoxelMaterialPaletteOverflowPolicy::BakeProperties:
            choose_or_fallback(decision, VoxelMaterialRuntimePath::BakedProperties, request,
                               "palette overflow resolved through conventional baked properties");
            break;
        case VoxelMaterialPaletteOverflowPolicy::RequestRecook:
            decision.usedFallback = true;
            decision.recookRequested = request.brick.canonicalMaterialMembershipAvailable;
            decision.sourceDataSufficient = request.brick.canonicalMaterialMembershipAvailable;
            decision.runtimePath = best_existing_fallback(request.brick.cooked);
            decision.reason = "palette overflow requests an asynchronous recook";
            if (decision.runtimePath != VoxelMaterialRuntimePath::NoPath)
                decision.reason += "; current frame uses cooked " +
                    std::string(to_string(decision.runtimePath)) + " fallback";
            break;
        case VoxelMaterialPaletteOverflowPolicy::Reject:
            decision.runtimePath = VoxelMaterialRuntimePath::NoPath;
            decision.reason = "palette overflow rejected by policy";
            break;
    }
}

[[nodiscard]] VoxelMaterialMode mode_for_path(VoxelMaterialRuntimePath path) noexcept {
    switch (path) {
        case VoxelMaterialRuntimePath::NoPath: return VoxelMaterialMode::Hybrid;
        case VoxelMaterialRuntimePath::BakedProperties: return VoxelMaterialMode::BakedProperties;
        case VoxelMaterialRuntimePath::SingleMaterial: return VoxelMaterialMode::SingleMaterial;
        case VoxelMaterialRuntimePath::DeferredPalette2:
        case VoxelMaterialRuntimePath::DeferredPalette4:
            return VoxelMaterialMode::DeferredPalette;
    }
    return VoxelMaterialMode::Hybrid;
}

} // namespace

VoxelMaterialPolicyConfig resolve_voxel_material_policy(
    const VoxelMaterialPolicyConfig& project,
    const VoxelMaterialAssetOverride& asset) noexcept {
    VoxelMaterialPolicyConfig result = project;
    if (asset.mode) result.mode = *asset.mode;
    if (asset.deferredMaximumDistance)
        result.deferredMaximumDistance = std::max(0.0F, *asset.deferredMaximumDistance);
    if (asset.deferredMaximumLod) result.deferredMaximumLod = *asset.deferredMaximumLod;
    if (asset.maximumPaletteSlots)
        result.maximumPaletteSlots = std::clamp<std::uint8_t>(*asset.maximumPaletteSlots, 1U, 4U);
    if (asset.overflowPolicy) result.overflowPolicy = *asset.overflowPolicy;
    if (asset.retainBakedFallback) result.retainBakedFallback = *asset.retainBakedFallback;
    if (asset.retainSingleMaterialFallback)
        result.retainSingleMaterialFallback = *asset.retainSingleMaterialFallback;
    if (asset.allowRuntimeSwitching) result.allowRuntimeSwitching = *asset.allowRuntimeSwitching;
    if (asset.enableFourWayBlending) result.enableFourWayBlending = *asset.enableFourWayBlending;
    if (result.maximumPaletteSlots < 4U) result.enableFourWayBlending = false;
    return result;
}

VoxelMaterialSelectionDecision select_voxel_material_path(
    const VoxelMaterialSelectionRequest& request) {
    const VoxelMaterialPolicyConfig policy = resolve_voxel_material_policy(request.project, request.asset);
    VoxelMaterialSelectionDecision decision;
    decision.brickId = request.brick.brickId;
    decision.assetName = request.brick.assetName;
    decision.requestedMode = request.editorPreviewMode.value_or(policy.mode);
    decision.effectiveMode = decision.requestedMode;
    decision.sourceDataSufficient = request.brick.canonicalMaterialMembershipAvailable;
    decision.runtimeSwitchAvailable = policy.allowRuntimeSwitching &&
        request.brick.canonicalMaterialMembershipAvailable;

    if (policy.platform == VoxelMaterialPlatformProfile::DedicatedServer) {
        decision.effectiveMode = VoxelMaterialMode::SingleMaterial;
        choose_or_fallback(decision, VoxelMaterialRuntimePath::SingleMaterial, request,
                           "dedicated-server profile strips texture evaluation");
        finalize(decision, request.brick);
        return decision;
    }

    if (decision.requestedMode == VoxelMaterialMode::BakedProperties) {
        choose_or_fallback(decision, VoxelMaterialRuntimePath::BakedProperties, request,
                           "conventional baked-properties mode selected");
    } else if (decision.requestedMode == VoxelMaterialMode::SingleMaterial) {
        choose_or_fallback(decision, VoxelMaterialRuntimePath::SingleMaterial, request,
                           "single-material indirection mode selected");
    } else if (decision.requestedMode == VoxelMaterialMode::DeferredPalette) {
        const auto wanted = wanted_deferred_path(policy, request.brick);
        if (wanted == VoxelMaterialRuntimePath::NoPath) choose_overflow_path(decision, request, policy);
        else choose_or_fallback(decision, wanted, request, "deferred brick-palette mode selected");
    } else {
        const bool constrainedPlatform = policy.platform == VoxelMaterialPlatformProfile::Mobile ||
            policy.platform == VoxelMaterialPlatformProfile::IntegratedGpu;
        const bool farOrCoarse = request.brick.cameraDistance > policy.deferredMaximumDistance ||
            request.brick.lod > policy.deferredMaximumLod;
        const bool pressureFallback = policy.fallBackUnderMemoryPressure && request.brick.memoryPressure;
        const bool oneMaterial = request.brick.sourceMaterialCount <= 1U;
        const bool preferDeferred = request.brick.recentlyModified ||
            (policy.preferDeferredForDestructible && request.brick.destructible);

        if (pressureFallback) {
            decision.effectiveMode = VoxelMaterialMode::BakedProperties;
            choose_or_fallback(decision, VoxelMaterialRuntimePath::BakedProperties, request,
                               "hybrid mode selected a baked fallback under memory pressure");
        } else if (farOrCoarse) {
            decision.effectiveMode = VoxelMaterialMode::BakedProperties;
            choose_or_fallback(decision, VoxelMaterialRuntimePath::BakedProperties, request,
                               "hybrid mode selected conventional baked properties for distant or coarse LOD data");
        } else if (oneMaterial) {
            decision.effectiveMode = VoxelMaterialMode::SingleMaterial;
            choose_or_fallback(decision, VoxelMaterialRuntimePath::SingleMaterial, request,
                               "hybrid mode selected the one-material path");
        } else if (constrainedPlatform && !preferDeferred) {
            decision.effectiveMode = VoxelMaterialMode::BakedProperties;
            choose_or_fallback(decision, VoxelMaterialRuntimePath::BakedProperties, request,
                               "hybrid mode selected conventional baking for a constrained platform");
        } else {
            decision.effectiveMode = VoxelMaterialMode::DeferredPalette;
            const auto wanted = wanted_deferred_path(policy, request.brick);
            if (wanted == VoxelMaterialRuntimePath::NoPath) choose_overflow_path(decision, request, policy);
            else choose_or_fallback(decision, wanted, request,
                                    preferDeferred
                                        ? "hybrid mode retained deferred material identity for a dynamic destructible brick"
                                        : "hybrid mode selected deferred palette blending for a nearby brick");
        }
    }

    if (decision.runtimePath != VoxelMaterialRuntimePath::NoPath)
        decision.effectiveMode = mode_for_path(decision.runtimePath);
    finalize(decision, request.brick);
    return decision;
}

VoxelMaterialPolicyReport build_voxel_material_policy_report(
    std::span<const VoxelMaterialSelectionRequest> requests) {
    VoxelMaterialPolicyReport report;
    report.decisions.reserve(requests.size());
    for (const auto& request : requests) report.decisions.push_back(select_voxel_material_path(request));
    std::stable_sort(report.decisions.begin(), report.decisions.end(),
                     [](const auto& left, const auto& right) {
                         return std::tie(left.brickId, left.assetName, left.contentHash) <
                                std::tie(right.brickId, right.assetName, right.contentHash);
                     });

    report.brickCount = report.decisions.size();
    for (const auto& decision : report.decisions) {
        switch (decision.runtimePath) {
            case VoxelMaterialRuntimePath::NoPath: break;
            case VoxelMaterialRuntimePath::BakedProperties: ++report.bakedBrickCount; break;
            case VoxelMaterialRuntimePath::SingleMaterial: ++report.singleMaterialBrickCount; break;
            case VoxelMaterialRuntimePath::DeferredPalette2: ++report.deferredPalette2BrickCount; break;
            case VoxelMaterialRuntimePath::DeferredPalette4: ++report.deferredPalette4BrickCount; break;
        }
        if (decision.usedFallback) ++report.fallbackBrickCount;
        if (decision.recookRequested) ++report.recookRequestCount;
        if (!decision.valid) ++report.invalidBrickCount;
        report.estimatedTextureSamples += decision.estimatedTextureSamples;
        report.additionalTextureSamples += decision.additionalTextureSamples;
    }

    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto value : {report.brickCount, report.bakedBrickCount,
                             report.singleMaterialBrickCount, report.deferredPalette2BrickCount,
                             report.deferredPalette4BrickCount, report.fallbackBrickCount,
                             report.recookRequestCount, report.invalidBrickCount,
                             report.estimatedTextureSamples, report.additionalTextureSamples})
        hash_integer(hash, value);
    for (const auto& decision : report.decisions) hash_integer(hash, decision.contentHash);
    report.contentHash = hash;
    return report;
}

std::string voxel_material_policy_json(const VoxelMaterialPolicyReport& report) {
    std::ostringstream stream;
    stream << "{\n"
           << "  \"brick_count\": " << report.brickCount << ",\n"
           << "  \"baked_bricks\": " << report.bakedBrickCount << ",\n"
           << "  \"single_material_bricks\": " << report.singleMaterialBrickCount << ",\n"
           << "  \"deferred_palette2_bricks\": " << report.deferredPalette2BrickCount << ",\n"
           << "  \"deferred_palette4_bricks\": " << report.deferredPalette4BrickCount << ",\n"
           << "  \"fallback_bricks\": " << report.fallbackBrickCount << ",\n"
           << "  \"recook_requests\": " << report.recookRequestCount << ",\n"
           << "  \"invalid_bricks\": " << report.invalidBrickCount << ",\n"
           << "  \"estimated_texture_samples\": " << report.estimatedTextureSamples << ",\n"
           << "  \"additional_texture_samples\": " << report.additionalTextureSamples << ",\n"
           << "  \"content_hash\": " << report.contentHash << ",\n"
           << "  \"decisions\": [\n";
    for (std::size_t index = 0U; index < report.decisions.size(); ++index) {
        const auto& decision = report.decisions[index];
        stream << "    {\"brick_id\": " << decision.brickId
               << ", \"asset\": \"" << decision.assetName
               << "\", \"requested_mode\": \"" << to_string(decision.requestedMode)
               << "\", \"effective_mode\": \"" << to_string(decision.effectiveMode)
               << "\", \"runtime_path\": \"" << to_string(decision.runtimePath)
               << "\", \"slots\": " << static_cast<unsigned>(decision.materialSlotsUsed)
               << ", \"fallback\": " << (decision.usedFallback ? "true" : "false")
               << ", \"recook\": " << (decision.recookRequested ? "true" : "false")
               << ", \"valid\": " << (decision.valid ? "true" : "false")
               << ", \"reason\": \"" << decision.reason << "\"}";
        if (index + 1U != report.decisions.size()) stream << ',';
        stream << '\n';
    }
    stream << "  ]\n}\n";
    return stream.str();
}

const char* to_string(VoxelMaterialMode value) noexcept {
    switch (value) {
        case VoxelMaterialMode::BakedProperties: return "BakedProperties";
        case VoxelMaterialMode::SingleMaterial: return "SingleMaterial";
        case VoxelMaterialMode::DeferredPalette: return "DeferredPalette";
        case VoxelMaterialMode::Hybrid: return "Hybrid";
    }
    return "Hybrid";
}

const char* to_string(VoxelMaterialPlatformProfile value) noexcept {
    switch (value) {
        case VoxelMaterialPlatformProfile::Editor: return "Editor";
        case VoxelMaterialPlatformProfile::HighEndDesktop: return "HighEndDesktop";
        case VoxelMaterialPlatformProfile::Console: return "Console";
        case VoxelMaterialPlatformProfile::IntegratedGpu: return "IntegratedGpu";
        case VoxelMaterialPlatformProfile::Mobile: return "Mobile";
        case VoxelMaterialPlatformProfile::DedicatedServer: return "DedicatedServer";
    }
    return "HighEndDesktop";
}

const char* to_string(VoxelMaterialPaletteOverflowPolicy value) noexcept {
    switch (value) {
        case VoxelMaterialPaletteOverflowPolicy::DominantMaterial: return "DominantMaterial";
        case VoxelMaterialPaletteOverflowPolicy::BakeProperties: return "BakeProperties";
        case VoxelMaterialPaletteOverflowPolicy::RequestRecook: return "RequestRecook";
        case VoxelMaterialPaletteOverflowPolicy::Reject: return "Reject";
    }
    return "BakeProperties";
}

const char* to_string(VoxelMaterialRuntimePath value) noexcept {
    switch (value) {
        case VoxelMaterialRuntimePath::NoPath: return "None";
        case VoxelMaterialRuntimePath::BakedProperties: return "BakedProperties";
        case VoxelMaterialRuntimePath::SingleMaterial: return "SingleMaterial";
        case VoxelMaterialRuntimePath::DeferredPalette2: return "DeferredPalette2";
        case VoxelMaterialRuntimePath::DeferredPalette4: return "DeferredPalette4";
    }
    return "None";
}

} // namespace dve
