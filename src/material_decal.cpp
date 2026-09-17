#include "dve/material_decal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dve {
namespace {

[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }
[[nodiscard]] bool finite(Float3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}
[[nodiscard]] float dot3(Float3 a, Float3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] float length3(Float3 value) noexcept { return std::sqrt(std::max(0.0F, dot3(value, value))); }
[[nodiscard]] Float3 subtract3(Float3 a, Float3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] float saturate(float value) noexcept { return std::clamp(value, 0.0F, 1.0F); }

[[nodiscard]] float axis_coverage(float coordinate, float extent, float feather) noexcept {
    if (!(extent > 0.0F)) return 0.0F;
    const float distance = extent - std::abs(coordinate);
    if (distance <= 0.0F) return 0.0F;
    if (!(feather > 0.0F)) return 1.0F;
    return saturate(distance / std::min(extent, feather));
}

} // namespace

const char* material_decal_semantic_label(MaterialDecalSemantic semantic) noexcept {
    switch (semantic) {
        case MaterialDecalSemantic::BulletImpact: return "Bullet Impact";
        case MaterialDecalSemantic::Scorch: return "Scorch";
        case MaterialDecalSemantic::Crack: return "Crack";
        case MaterialDecalSemantic::Dirt: return "Dirt";
        case MaterialDecalSemantic::Blood: return "Blood";
        case MaterialDecalSemantic::Paint: return "Paint";
        case MaterialDecalSemantic::StructuralStress: return "Structural Stress";
        case MaterialDecalSemantic::Wetness: return "Wetness";
        case MaterialDecalSemantic::FluidResidue: return "Fluid Residue";
    }
    return "Unknown";
}

bool validate_material_decal(const MaterialDecal& decal, std::string* error) noexcept {
    auto fail = [&](const char* message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (decal.id == 0U) return fail("material decal id must be non-zero");
    if (static_cast<unsigned>(decal.semantic) >
        static_cast<unsigned>(MaterialDecalSemantic::FluidResidue))
        return fail("material decal semantic is invalid");
    if (!finite(decal.center) || !finite(decal.tangent) || !finite(decal.bitangent) ||
        !finite(decal.projectionNormal) || !finite(decal.halfExtents) ||
        !finite(decal.edgeFeather) || !finite(decal.normalFadeStart))
        return fail("material decal contains a non-finite value");
    if (!(decal.halfExtents.x > 0.0F) || !(decal.halfExtents.y > 0.0F) ||
        !(decal.halfExtents.z > 0.0F) || decal.edgeFeather < 0.0F ||
        decal.normalFadeStart < -1.0F || decal.normalFadeStart > 1.0F)
        return fail("material decal projection controls are invalid");
    if (length3(decal.tangent) < 0.5F || length3(decal.bitangent) < 0.5F ||
        length3(decal.projectionNormal) < 0.5F)
        return fail("material decal projection basis is degenerate");
    return validate_material_layer_sample(decal.layer, error);
}

float material_decal_projection_coverage(
    const MaterialDecal& decal,
    Float3 worldPosition,
    Float3 worldNormal) noexcept {
    if (!decal.enabled) return 0.0F;
    const Float3 relative = subtract3(worldPosition, decal.center);
    const float x = dot3(relative, decal.tangent);
    const float y = dot3(relative, decal.bitangent);
    const float z = dot3(relative, decal.projectionNormal);
    const float feather = decal.edgeFeather;
    const float boxCoverage = axis_coverage(x, decal.halfExtents.x, feather) *
                              axis_coverage(y, decal.halfExtents.y, feather) *
                              axis_coverage(z, decal.halfExtents.z, feather);
    if (!(boxCoverage > 0.0F)) return 0.0F;
    const float orientation = std::abs(dot3(worldNormal, decal.projectionNormal));
    const float normalCoverage = decal.normalFadeStart >= 1.0F
        ? (orientation >= 1.0F ? 1.0F : 0.0F)
        : saturate((orientation - decal.normalFadeStart) /
                   std::max(1.0e-5F, 1.0F - decal.normalFadeStart));
    return saturate(boxCoverage * normalCoverage * decal.layer.mask);
}

MaterialSurfaceSample composite_material_decals(
    MaterialSurfaceSample base,
    Float3 worldPosition,
    Float3 worldNormal,
    std::span<const MaterialDecal> decals,
    MaterialDecalCompositeStats* stats) {
    std::vector<const MaterialDecal*> ordered;
    ordered.reserve(decals.size());
    for (const MaterialDecal& decal : decals) if (decal.enabled) ordered.push_back(&decal);
    std::stable_sort(ordered.begin(), ordered.end(), [](const MaterialDecal* left, const MaterialDecal* right) {
        if (left->priority != right->priority) return left->priority < right->priority;
        return left->id < right->id;
    });

    MaterialDecalCompositeStats local{};
    local.candidates = static_cast<std::uint32_t>(ordered.size());
    for (const MaterialDecal* decal : ordered) {
        const float projectedCoverage = material_decal_projection_coverage(*decal, worldPosition, worldNormal);
        if (!(projectedCoverage > 0.0F)) continue;
        ++local.projected;

        MaterialLayerSample layer = decal->layer;
        layer.mask = projectedCoverage;
        const bool affectsNormal = has_decal_channel(decal->channels, MaterialDecalChannel::Normal);
        const Float3 tangentDetailNormal = layer.surface.normal;
        if (!has_decal_channel(decal->channels, MaterialDecalChannel::BaseColor))
            layer.surface.baseColor = base.baseColor;
        // Scalar/channel blending must not reinterpret a world-space base normal as tangent space.
        // Apply the projected decal normal separately through the decal's authored frame.
        layer.surface.normal = {0.0F, 0.0F, 1.0F};
        if (!has_decal_channel(decal->channels, MaterialDecalChannel::Roughness))
            layer.surface.roughness = base.roughness;
        if (!has_decal_channel(decal->channels, MaterialDecalChannel::Metallic))
            layer.surface.metallic = base.metallic;
        if (!has_decal_channel(decal->channels, MaterialDecalChannel::Emissive))
            layer.surface.emissive = {};
        if (!has_decal_channel(decal->channels, MaterialDecalChannel::Opacity)) {
            layer.opacityPolicy = MaterialLayerOpacityPolicy::PreserveBase;
            layer.surface.opacity = base.opacity;
        }
        if (!has_decal_channel(decal->channels, MaterialDecalChannel::Height)) {
            layer.surface.height = base.height;
            layer.heightBlendStrength = 0.0F;
        }

        const Float3 baseNormal = base.normal;
        float coverage{};
        base = blend_material_surface_layer(base, layer, &coverage);
        if (affectsNormal && coverage > 0.0F) {
            base.normal = apply_reoriented_tangent_material_normal(
                baseNormal, decal->tangent, decal->bitangent, tangentDetailNormal, coverage);
        }
        local.maximumCoverage = std::max(local.maximumCoverage, coverage);
        if (coverage > 0.0F) ++local.applied;
    }
    if (stats != nullptr) *stats = local;
    return base;
}

MaterialDecalGrid::MaterialDecalGrid(float cellSizeMeters)
    : cellSizeMeters_(std::max(0.01F, cellSizeMeters)) {}

std::size_t MaterialDecalGrid::CellHash::operator()(const CellKey& key) const noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    auto mix = [&](std::uint32_t value) {
        for (unsigned shift = 0U; shift < 32U; shift += 8U) {
            hash ^= static_cast<std::uint8_t>(value >> shift);
            hash *= 1099511628211ULL;
        }
    };
    mix(static_cast<std::uint32_t>(key.x));
    mix(static_cast<std::uint32_t>(key.y));
    mix(static_cast<std::uint32_t>(key.z));
    return static_cast<std::size_t>(hash);
}

MaterialDecalGrid::CellKey MaterialDecalGrid::cell_for(Float3 position) const noexcept {
    return {
        static_cast<std::int32_t>(std::floor(position.x / cellSizeMeters_)),
        static_cast<std::int32_t>(std::floor(position.y / cellSizeMeters_)),
        static_cast<std::int32_t>(std::floor(position.z / cellSizeMeters_)),
    };
}

bool MaterialDecalGrid::rebuild(std::span<const MaterialDecal> decals, std::string* error) {
    std::vector<MaterialDecal> validated;
    validated.reserve(decals.size());
    for (const MaterialDecal& decal : decals) {
        std::string local;
        if (!validate_material_decal(decal, &local)) {
            if (error != nullptr) *error = local;
            return false;
        }
        validated.push_back(decal);
    }

    std::unordered_map<CellKey, std::vector<std::uint32_t>, CellHash> rebuilt;
    for (std::uint32_t index = 0U; index < validated.size(); ++index) {
        const MaterialDecal& decal = validated[index];
        const float radius = std::sqrt(decal.halfExtents.x * decal.halfExtents.x +
                                       decal.halfExtents.y * decal.halfExtents.y +
                                       decal.halfExtents.z * decal.halfExtents.z);
        const CellKey minimum = cell_for({decal.center.x - radius, decal.center.y - radius,
                                         decal.center.z - radius});
        const CellKey maximum = cell_for({decal.center.x + radius, decal.center.y + radius,
                                         decal.center.z + radius});
        for (std::int32_t z = minimum.z; z <= maximum.z; ++z)
            for (std::int32_t y = minimum.y; y <= maximum.y; ++y)
                for (std::int32_t x = minimum.x; x <= maximum.x; ++x)
                    rebuilt[{x, y, z}].push_back(index);
    }
    decals_ = std::move(validated);
    cells_ = std::move(rebuilt);
    return true;
}

std::vector<std::uint32_t> MaterialDecalGrid::query(Float3 worldPosition) const {
    const auto found = cells_.find(cell_for(worldPosition));
    if (found == cells_.end()) return {};
    std::vector<std::uint32_t> result = found->second;
    std::sort(result.begin(), result.end(), [&](std::uint32_t left, std::uint32_t right) {
        const MaterialDecal& a = decals_[left];
        const MaterialDecal& b = decals_[right];
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.id < b.id;
    });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace dve
