#include "dve/destruction_material.hpp"

#include <algorithm>
#include <array>

namespace dve {
namespace {

struct NeighborFace {
    Int3 delta{};
    FaceDirection face{};
};

constexpr std::array<NeighborFace, 6> kNeighbors{{
    NeighborFace{{-1, 0, 0}, FaceDirection::PosX},
    NeighborFace{{ 1, 0, 0}, FaceDirection::NegX},
    NeighborFace{{ 0,-1, 0}, FaceDirection::PosY},
    NeighborFace{{ 0, 1, 0}, FaceDirection::NegY},
    NeighborFace{{ 0, 0,-1}, FaceDirection::PosZ},
    NeighborFace{{ 0, 0, 1}, FaceDirection::NegZ},
}};

[[nodiscard]] Int3 add3(Int3 a, Int3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] const DestructionMaterialPolicy& policy_for(
    MaterialId material,
    std::span<const DestructionMaterialPolicy> policies) noexcept {
    static const DestructionMaterialPolicy fallback{};
    const std::size_t index = static_cast<std::size_t>(material);
    return index < policies.size() ? policies[index] : fallback;
}

} // namespace

DestructionSurfaceAssignment assign_destruction_surface_material(
    SurfaceQuad surface,
    DestructionSurfaceKind kind,
    const DestructionMaterialPolicy& policy) noexcept {
    DestructionSurfaceAssignment assignment;
    assignment.surface = surface;
    assignment.kind = kind;
    assignment.inheritedMaterial = surface.material;
    assignment.renderMaterial = surface.material;

    if (kind == DestructionSurfaceKind::Interior && policy.interiorMaterial)
        assignment.renderMaterial = *policy.interiorMaterial;
    if (kind == DestructionSurfaceKind::Fracture) {
        if (policy.fractureMaterial) assignment.renderMaterial = *policy.fractureMaterial;
        else if (policy.interiorMaterial) assignment.renderMaterial = *policy.interiorMaterial;
        assignment.overlayLayerMaterial = policy.fractureLayerMaterial;
    }
    assignment.surface.material = assignment.renderMaterial;
    return assignment;
}

std::vector<DestructionSurfaceAssignment> extract_newly_exposed_fracture_surfaces(
    const VoxelObject& before,
    const VoxelObject& after,
    std::span<const AppliedBrickEdit> edits,
    std::span<const DestructionMaterialPolicy> policies) {
    std::vector<DestructionSurfaceAssignment> result;

    for (const AppliedBrickEdit& edit : edits) {
        edit.changedMask.for_each_set([&](std::uint16_t localIndex) {
            const Int3 removed = global_from_local(edit.key, local_from_index_unchecked(localIndex));
            if (!before.occupied_at(removed) || after.occupied_at(removed)) return;

            for (const NeighborFace& neighborFace : kNeighbors) {
                const Int3 neighbor = add3(removed, neighborFace.delta);
                const MaterialId materialAfter = after.material_at(neighbor);
                if (materialAfter == kAirMaterial || !before.occupied_at(neighbor)) continue;

                SurfaceQuad surface{neighbor, neighborFace.face, materialAfter};
                result.push_back(assign_destruction_surface_material(
                    surface, DestructionSurfaceKind::Fracture,
                    policy_for(materialAfter, policies)));
            }
        });
    }

    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.surface.voxel != right.surface.voxel) return left.surface.voxel < right.surface.voxel;
        if (left.surface.face != right.surface.face) return left.surface.face < right.surface.face;
        if (left.renderMaterial != right.renderMaterial) return left.renderMaterial < right.renderMaterial;
        return left.overlayLayerMaterial < right.overlayLayerMaterial;
    });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.surface.voxel == right.surface.voxel &&
               left.surface.face == right.surface.face;
    }), result.end());
    return result;
}

} // namespace dve
