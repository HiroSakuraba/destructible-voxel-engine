#pragma once

#include <optional>
#include <span>
#include <vector>

#include "dve/damage.hpp"
#include "dve/surface.hpp"

namespace dve {

enum class DestructionSurfaceKind : std::uint8_t {
    Exterior,
    Interior,
    Fracture,
};

struct DestructionMaterialPolicy {
    std::optional<MaterialId> interiorMaterial;
    std::optional<MaterialId> fractureMaterial;
    // When present, keep the selected base/interior material and apply this material as a
    // fracture-exposure layer. This is useful for fresh concrete aggregate, torn wood fibres,
    // glowing heat damage, and similar overlays without replacing the physical material.
    std::optional<MaterialId> fractureLayerMaterial;
};

struct DestructionSurfaceAssignment {
    SurfaceQuad surface{};
    DestructionSurfaceKind kind{DestructionSurfaceKind::Exterior};
    MaterialId inheritedMaterial{kAirMaterial};
    MaterialId renderMaterial{kAirMaterial};
    std::optional<MaterialId> overlayLayerMaterial;
};

[[nodiscard]] DestructionSurfaceAssignment assign_destruction_surface_material(
    SurfaceQuad surface,
    DestructionSurfaceKind kind,
    const DestructionMaterialPolicy& policy) noexcept;

// Identifies only faces that became visible because voxels in the supplied edits were removed.
// The before object is immutable authority state immediately before destruction; after is the
// committed state. Policies are indexed by MaterialId. Missing policies inherit the exterior.
[[nodiscard]] std::vector<DestructionSurfaceAssignment> extract_newly_exposed_fracture_surfaces(
    const VoxelObject& before,
    const VoxelObject& after,
    std::span<const AppliedBrickEdit> edits,
    std::span<const DestructionMaterialPolicy> policies = {});

} // namespace dve
