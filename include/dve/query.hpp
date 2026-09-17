#pragma once

#include <cstddef>
#include <optional>

#include "dve/transform.hpp"
#include "dve/voxel_object.hpp"

namespace dve {

struct RayHit {
    Int3 voxel{};
    Int3 normal{};
    float distance{};
    MaterialId material{};
};

struct TransformedRayHit {
    RayHit objectHit{};
    Float3 worldPosition{};
    Float3 worldNormal{};
};

struct Capsule {
    Float3 pointA{};
    Float3 pointB{};
    float radius{0.5F};
};

struct CapsuleSweepHit {
    float time{}; // normalized [0,1] along displacement
    Float3 worldPosition{};
    Float3 worldNormal{};
    Int3 voxel{};
    MaterialId material{};
};

struct PlayerCollisionState {
    Capsule capsule{};
    Float3 velocity{};
    bool grounded{};
};

struct PlayerEditResolution {
    Float3 correction{};
    bool grounded{};
    bool floorRemoved{};
    std::size_t depenetrationIterations{};
};

[[nodiscard]] std::optional<RayHit> raycast_voxels(
    const VoxelObject& object,
    Float3 origin,
    Float3 direction,
    float maxDistance);

[[nodiscard]] std::optional<TransformedRayHit> raycast_voxels_transformed(
    const VoxelObject& object,
    const RigidTransform& objectTransform,
    Float3 worldOrigin,
    Float3 worldDirection,
    float maxDistance);

[[nodiscard]] bool overlaps_voxels(
    const VoxelObject& object,
    Float3 minCorner,
    Float3 maxCorner);

[[nodiscard]] bool capsule_overlaps_voxels(
    const VoxelObject& object,
    const RigidTransform& objectTransform,
    const Capsule& worldCapsule);

// Conservative sweep: the capsule's object-local AABB is swept against voxel
// AABBs. It may report early corner contacts, but cannot miss a true capsule
// contact for a rigidly transformed voxel object.
[[nodiscard]] std::optional<CapsuleSweepHit> sweep_capsule_conservative(
    const VoxelObject& object,
    const RigidTransform& objectTransform,
    const Capsule& worldCapsule,
    Float3 worldDisplacement);

// Previous/current transform hook for moving voxel bodies. Translation and
// rotation are sampled at alpha; callers can substep if body rotation is large.
[[nodiscard]] std::optional<CapsuleSweepHit> sweep_capsule_against_moving_body(
    const VoxelObject& object,
    const MovingRigidTransform& bodyTransform,
    float alpha,
    const Capsule& worldCapsule,
    Float3 worldDisplacement);

[[nodiscard]] Float3 depenetrate_capsule_conservative(
    const VoxelObject& object,
    const RigidTransform& objectTransform,
    const Capsule& worldCapsule,
    std::size_t maxIterations = 8,
    float skin = 0.001F,
    std::size_t* iterationsUsed = nullptr);

[[nodiscard]] PlayerEditResolution resolve_player_after_voxel_edit(
    const VoxelObject& object,
    const RigidTransform& objectTransform,
    PlayerCollisionState& player,
    float groundProbeDistance = 0.08F,
    float skin = 0.001F);

} // namespace dve
