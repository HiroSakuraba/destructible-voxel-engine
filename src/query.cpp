#include "dve/query.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace dve {

namespace {

struct Aabb {
    Float3 min{};
    Float3 max{};
};

[[nodiscard]] float int_boundary(float value, float direction) noexcept {
    if (direction > 0.0F) return (std::floor(value) + 1.0F - value) / direction;
    if (direction < 0.0F) return (value - std::floor(value)) / -direction;
    return std::numeric_limits<float>::infinity();
}

[[nodiscard]] Aabb capsule_aabb(const Capsule& capsule) noexcept {
    const float radius = std::max(0.0F, capsule.radius);
    return {
        {
            std::min(capsule.pointA.x, capsule.pointB.x) - radius,
            std::min(capsule.pointA.y, capsule.pointB.y) - radius,
            std::min(capsule.pointA.z, capsule.pointB.z) - radius,
        },
        {
            std::max(capsule.pointA.x, capsule.pointB.x) + radius,
            std::max(capsule.pointA.y, capsule.pointB.y) + radius,
            std::max(capsule.pointA.z, capsule.pointB.z) + radius,
        },
    };
}

[[nodiscard]] Capsule inverse_transform_capsule(
    const RigidTransform& transform,
    const Capsule& worldCapsule) noexcept {
    return {
        inverse_transform_point(transform, worldCapsule.pointA),
        inverse_transform_point(transform, worldCapsule.pointB),
        std::max(0.0F, worldCapsule.radius),
    };
}

[[nodiscard]] float point_aabb_distance_squared(Float3 point, const Aabb& box) noexcept {
    const float dx = std::max({box.min.x - point.x, 0.0F, point.x - box.max.x});
    const float dy = std::max({box.min.y - point.y, 0.0F, point.y - box.max.y});
    const float dz = std::max({box.min.z - point.z, 0.0F, point.z - box.max.z});
    return dx * dx + dy * dy + dz * dz;
}

// Squared distance from a segment to an AABB. The objective is convex and
// piecewise quadratic, so fixed-iteration ternary search is deterministic and
// sufficiently accurate for overlap classification with a small epsilon.
[[nodiscard]] float segment_aabb_distance_squared(Float3 a, Float3 b, const Aabb& box) noexcept {
    float low = 0.0F;
    float high = 1.0F;
    const Float3 delta = subtract(b, a);
    for (int iteration = 0; iteration < 36; ++iteration) {
        const float t1 = low + (high - low) / 3.0F;
        const float t2 = high - (high - low) / 3.0F;
        const float d1 = point_aabb_distance_squared(add(a, multiply(delta, t1)), box);
        const float d2 = point_aabb_distance_squared(add(a, multiply(delta, t2)), box);
        if (d1 <= d2) high = t2;
        else low = t1;
    }
    const float middle = 0.5F * (low + high);
    return std::min({
        point_aabb_distance_squared(a, box),
        point_aabb_distance_squared(b, box),
        point_aabb_distance_squared(add(a, multiply(delta, middle)), box),
    });
}

[[nodiscard]] bool capsule_overlaps_voxel(const Capsule& capsule, Int3 voxel) noexcept {
    const Aabb box{
        {static_cast<float>(voxel.x), static_cast<float>(voxel.y), static_cast<float>(voxel.z)},
        {static_cast<float>(voxel.x + 1), static_cast<float>(voxel.y + 1), static_cast<float>(voxel.z + 1)},
    };
    const float radius = std::max(0.0F, capsule.radius);
    return segment_aabb_distance_squared(capsule.pointA, capsule.pointB, box) <= radius * radius + 1.0e-6F;
}

[[nodiscard]] bool sweep_aabb_against_aabb(
    const Aabb& moving,
    Float3 displacement,
    const Aabb& obstacle,
    float& entryTime,
    Float3& normal) noexcept {
    float tEnter = 0.0F;
    float tExit = 1.0F;
    int enterAxis = -1;
    float enterNormal = 0.0F;

    const std::array<float, 3> movingMin{{moving.min.x, moving.min.y, moving.min.z}};
    const std::array<float, 3> movingMax{{moving.max.x, moving.max.y, moving.max.z}};
    const std::array<float, 3> obstacleMin{{obstacle.min.x, obstacle.min.y, obstacle.min.z}};
    const std::array<float, 3> obstacleMax{{obstacle.max.x, obstacle.max.y, obstacle.max.z}};
    const std::array<float, 3> delta{{displacement.x, displacement.y, displacement.z}};

    for (int axis = 0; axis < 3; ++axis) {
        if (delta[axis] == 0.0F) {
            if (movingMax[axis] <= obstacleMin[axis] || movingMin[axis] >= obstacleMax[axis]) return false;
            continue;
        }
        float axisEnter;
        float axisExit;
        float axisNormal;
        if (delta[axis] > 0.0F) {
            axisEnter = (obstacleMin[axis] - movingMax[axis]) / delta[axis];
            axisExit = (obstacleMax[axis] - movingMin[axis]) / delta[axis];
            axisNormal = -1.0F;
        } else {
            axisEnter = (obstacleMax[axis] - movingMin[axis]) / delta[axis];
            axisExit = (obstacleMin[axis] - movingMax[axis]) / delta[axis];
            axisNormal = 1.0F;
        }
        if (axisEnter > axisExit) std::swap(axisEnter, axisExit);
        if (axisEnter > tEnter || (enterAxis < 0 && axisEnter >= tEnter - 1.0e-7F)) {
            tEnter = axisEnter;
            enterAxis = axis;
            enterNormal = axisNormal;
        }
        tExit = std::min(tExit, axisExit);
        if (tEnter > tExit) return false;
    }
    if (tExit < 0.0F || tEnter > 1.0F) return false;
    entryTime = std::clamp(tEnter, 0.0F, 1.0F);
    normal = {};
    if (enterAxis == 0) normal.x = enterNormal;
    else if (enterAxis == 1) normal.y = enterNormal;
    else if (enterAxis == 2) normal.z = enterNormal;
    return true;
}

[[nodiscard]] Float3 conservative_overlap_correction(
    const Aabb& moving,
    const Aabb& obstacle,
    float skin) noexcept {
    const float pushNegX = obstacle.min.x - moving.max.x - skin;
    const float pushPosX = obstacle.max.x - moving.min.x + skin;
    const float pushNegY = obstacle.min.y - moving.max.y - skin;
    const float pushPosY = obstacle.max.y - moving.min.y + skin;
    const float pushNegZ = obstacle.min.z - moving.max.z - skin;
    const float pushPosZ = obstacle.max.z - moving.min.z + skin;

    std::array<Float3, 6> candidates{{
        {pushNegX, 0.0F, 0.0F}, {pushPosX, 0.0F, 0.0F},
        {0.0F, pushNegY, 0.0F}, {0.0F, pushPosY, 0.0F},
        {0.0F, 0.0F, pushNegZ}, {0.0F, 0.0F, pushPosZ},
    }};
    return *std::min_element(candidates.begin(), candidates.end(), [](Float3 a, Float3 b) {
        return length_squared(a) < length_squared(b);
    });
}

[[nodiscard]] Aabb moved_aabb(Aabb box, Float3 displacement) noexcept {
    box.min = add(box.min, displacement);
    box.max = add(box.max, displacement);
    return box;
}

} // namespace

std::optional<RayHit> raycast_voxels(
    const VoxelObject& object,
    Float3 origin,
    Float3 direction,
    float maxDistance) {
    const float magnitude = length(direction);
    if (!(magnitude > 0.0F) || !(maxDistance >= 0.0F)) return std::nullopt;
    // Traverse with the caller's original float direction. Avoiding normalization
    // removes a cross-translation-unit/GPU rounding source that can change which
    // voxel owns an exact edge hit. DDA time is parametric; reported distance is
    // converted back to world units only when a hit is returned.
    const float maximumParameter = maxDistance / magnitude;

    Int3 voxel{
        static_cast<std::int32_t>(std::floor(origin.x)),
        static_cast<std::int32_t>(std::floor(origin.y)),
        static_cast<std::int32_t>(std::floor(origin.z)),
    };
    const Int3 step{
        direction.x > 0.0F ? 1 : (direction.x < 0.0F ? -1 : 0),
        direction.y > 0.0F ? 1 : (direction.y < 0.0F ? -1 : 0),
        direction.z > 0.0F ? 1 : (direction.z < 0.0F ? -1 : 0),
    };

    Float3 tMax{
        int_boundary(origin.x, direction.x),
        int_boundary(origin.y, direction.y),
        int_boundary(origin.z, direction.z),
    };
    const Float3 tDelta{
        direction.x == 0.0F ? std::numeric_limits<float>::infinity() : std::abs(1.0F / direction.x),
        direction.y == 0.0F ? std::numeric_limits<float>::infinity() : std::abs(1.0F / direction.y),
        direction.z == 0.0F ? std::numeric_limits<float>::infinity() : std::abs(1.0F / direction.z),
    };

    float parameter = 0.0F;
    Int3 normal{};
    while (parameter <= maximumParameter) {
        const MaterialId materialId = object.material_at(voxel);
        if (materialId != kAirMaterial) return RayHit{voxel, normal, parameter * magnitude, materialId};

        if (tMax.x <= tMax.y && tMax.x <= tMax.z) {
            voxel.x += step.x;
            parameter = tMax.x;
            tMax.x += tDelta.x;
            normal = {-step.x, 0, 0};
        } else if (tMax.y <= tMax.z) {
            voxel.y += step.y;
            parameter = tMax.y;
            tMax.y += tDelta.y;
            normal = {0, -step.y, 0};
        } else {
            voxel.z += step.z;
            parameter = tMax.z;
            tMax.z += tDelta.z;
            normal = {0, 0, -step.z};
        }
    }
    return std::nullopt;
}

std::optional<TransformedRayHit> raycast_voxels_transformed(
    const VoxelObject& object,
    const RigidTransform& objectTransform,
    Float3 worldOrigin,
    Float3 worldDirection,
    float maxDistance) {
    const Float3 localOrigin = inverse_transform_point(objectTransform, worldOrigin);
    const Float3 localDirection = inverse_transform_vector(objectTransform, worldDirection);
    const auto hit = raycast_voxels(object, localOrigin, localDirection, maxDistance);
    if (!hit) return std::nullopt;
    const Float3 localPosition = add(localOrigin, multiply(normalize(localDirection), hit->distance));
    const Float3 localNormal{
        static_cast<float>(hit->normal.x),
        static_cast<float>(hit->normal.y),
        static_cast<float>(hit->normal.z),
    };
    return TransformedRayHit{
        *hit,
        transform_point(objectTransform, localPosition),
        normalize(transform_vector(objectTransform, localNormal)),
    };
}

bool overlaps_voxels(
    const VoxelObject& object,
    Float3 minCorner,
    Float3 maxCorner) {
    const std::int32_t minX = static_cast<std::int32_t>(std::floor(std::min(minCorner.x, maxCorner.x)));
    const std::int32_t minY = static_cast<std::int32_t>(std::floor(std::min(minCorner.y, maxCorner.y)));
    const std::int32_t minZ = static_cast<std::int32_t>(std::floor(std::min(minCorner.z, maxCorner.z)));
    const std::int32_t maxX = static_cast<std::int32_t>(std::ceil(std::max(minCorner.x, maxCorner.x))) - 1;
    const std::int32_t maxY = static_cast<std::int32_t>(std::ceil(std::max(minCorner.y, maxCorner.y))) - 1;
    const std::int32_t maxZ = static_cast<std::int32_t>(std::ceil(std::max(minCorner.z, maxCorner.z))) - 1;

    for (std::int32_t z = minZ; z <= maxZ; ++z) {
        for (std::int32_t y = minY; y <= maxY; ++y) {
            for (std::int32_t x = minX; x <= maxX; ++x) {
                if (object.occupied_at({x, y, z})) return true;
            }
        }
    }
    return false;
}

bool capsule_overlaps_voxels(
    const VoxelObject& object,
    const RigidTransform& objectTransform,
    const Capsule& worldCapsule) {
    const Capsule local = inverse_transform_capsule(objectTransform, worldCapsule);
    const Aabb bounds = capsule_aabb(local);
    const std::int32_t minX = static_cast<std::int32_t>(std::floor(bounds.min.x));
    const std::int32_t minY = static_cast<std::int32_t>(std::floor(bounds.min.y));
    const std::int32_t minZ = static_cast<std::int32_t>(std::floor(bounds.min.z));
    const std::int32_t maxX = static_cast<std::int32_t>(std::ceil(bounds.max.x)) - 1;
    const std::int32_t maxY = static_cast<std::int32_t>(std::ceil(bounds.max.y)) - 1;
    const std::int32_t maxZ = static_cast<std::int32_t>(std::ceil(bounds.max.z)) - 1;

    for (std::int32_t z = minZ; z <= maxZ; ++z) {
        for (std::int32_t y = minY; y <= maxY; ++y) {
            for (std::int32_t x = minX; x <= maxX; ++x) {
                const Int3 voxel{x, y, z};
                if (object.occupied_at(voxel) && capsule_overlaps_voxel(local, voxel)) return true;
            }
        }
    }
    return false;
}

std::optional<CapsuleSweepHit> sweep_capsule_conservative(
    const VoxelObject& object,
    const RigidTransform& objectTransform,
    const Capsule& worldCapsule,
    Float3 worldDisplacement) {
    const Capsule localCapsule = inverse_transform_capsule(objectTransform, worldCapsule);
    const Float3 localDisplacement = inverse_transform_vector(objectTransform, worldDisplacement);
    const Aabb startBounds = capsule_aabb(localCapsule);
    const Aabb endBounds = moved_aabb(startBounds, localDisplacement);
    const Aabb broad{
        {
            std::min(startBounds.min.x, endBounds.min.x),
            std::min(startBounds.min.y, endBounds.min.y),
            std::min(startBounds.min.z, endBounds.min.z),
        },
        {
            std::max(startBounds.max.x, endBounds.max.x),
            std::max(startBounds.max.y, endBounds.max.y),
            std::max(startBounds.max.z, endBounds.max.z),
        },
    };

    const std::int32_t minX = static_cast<std::int32_t>(std::floor(broad.min.x));
    const std::int32_t minY = static_cast<std::int32_t>(std::floor(broad.min.y));
    const std::int32_t minZ = static_cast<std::int32_t>(std::floor(broad.min.z));
    const std::int32_t maxX = static_cast<std::int32_t>(std::ceil(broad.max.x)) - 1;
    const std::int32_t maxY = static_cast<std::int32_t>(std::ceil(broad.max.y)) - 1;
    const std::int32_t maxZ = static_cast<std::int32_t>(std::ceil(broad.max.z)) - 1;

    float bestTime = std::numeric_limits<float>::infinity();
    Float3 bestNormal{};
    Int3 bestVoxel{};
    MaterialId bestMaterial = kAirMaterial;
    for (std::int32_t z = minZ; z <= maxZ; ++z) {
        for (std::int32_t y = minY; y <= maxY; ++y) {
            for (std::int32_t x = minX; x <= maxX; ++x) {
                const Int3 voxel{x, y, z};
                const MaterialId material = object.material_at(voxel);
                if (material == kAirMaterial) continue;
                const Aabb obstacle{
                    {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)},
                    {static_cast<float>(x + 1), static_cast<float>(y + 1), static_cast<float>(z + 1)},
                };
                float time = 0.0F;
                Float3 normal{};
                if (sweep_aabb_against_aabb(startBounds, localDisplacement, obstacle, time, normal) &&
                    time < bestTime) {
                    bestTime = time;
                    bestNormal = normal;
                    bestVoxel = voxel;
                    bestMaterial = material;
                }
            }
        }
    }
    if (!std::isfinite(bestTime)) return std::nullopt;
    const Float3 worldNormal = normalize(transform_vector(objectTransform, bestNormal));
    const Float3 worldPosition = add(worldCapsule.pointA, multiply(worldDisplacement, bestTime));
    return CapsuleSweepHit{bestTime, worldPosition, worldNormal, bestVoxel, bestMaterial};
}

std::optional<CapsuleSweepHit> sweep_capsule_against_moving_body(
    const VoxelObject& object,
    const MovingRigidTransform& bodyTransform,
    float alpha,
    const Capsule& worldCapsule,
    Float3 worldDisplacement) {
    const RigidTransform sampled = interpolate_rigid_transform(bodyTransform.previous, bodyTransform.current, alpha);
    return sweep_capsule_conservative(object, sampled, worldCapsule, worldDisplacement);
}

Float3 depenetrate_capsule_conservative(
    const VoxelObject& object,
    const RigidTransform& objectTransform,
    const Capsule& worldCapsule,
    std::size_t maxIterations,
    float skin,
    std::size_t* iterationsUsed) {
    Capsule local = inverse_transform_capsule(objectTransform, worldCapsule);
    Float3 totalLocalCorrection{};
    std::size_t completedIterations = 0;
    for (std::size_t iteration = 0; iteration < maxIterations; ++iteration) {
        const Aabb bounds = capsule_aabb(local);
        const std::int32_t minX = static_cast<std::int32_t>(std::floor(bounds.min.x));
        const std::int32_t minY = static_cast<std::int32_t>(std::floor(bounds.min.y));
        const std::int32_t minZ = static_cast<std::int32_t>(std::floor(bounds.min.z));
        const std::int32_t maxX = static_cast<std::int32_t>(std::ceil(bounds.max.x)) - 1;
        const std::int32_t maxY = static_cast<std::int32_t>(std::ceil(bounds.max.y)) - 1;
        const std::int32_t maxZ = static_cast<std::int32_t>(std::ceil(bounds.max.z)) - 1;

        bool found = false;
        Float3 bestCorrection{};
        float bestLength = std::numeric_limits<float>::infinity();
        for (std::int32_t z = minZ; z <= maxZ; ++z) {
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (!object.occupied_at({x, y, z})) continue;
                    const Aabb obstacle{
                        {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)},
                        {static_cast<float>(x + 1), static_cast<float>(y + 1), static_cast<float>(z + 1)},
                    };
                    const bool aabbOverlap = bounds.max.x > obstacle.min.x && bounds.min.x < obstacle.max.x &&
                                             bounds.max.y > obstacle.min.y && bounds.min.y < obstacle.max.y &&
                                             bounds.max.z > obstacle.min.z && bounds.min.z < obstacle.max.z;
                    if (!aabbOverlap || !capsule_overlaps_voxel(local, {x, y, z})) continue;
                    const Float3 correction = conservative_overlap_correction(bounds, obstacle, skin);
                    const float correctionLength = length_squared(correction);
                    if (correctionLength < bestLength) {
                        found = true;
                        bestCorrection = correction;
                        bestLength = correctionLength;
                    }
                }
            }
        }
        if (!found) break;
        local.pointA = add(local.pointA, bestCorrection);
        local.pointB = add(local.pointB, bestCorrection);
        totalLocalCorrection = add(totalLocalCorrection, bestCorrection);
        ++completedIterations;
    }
    if (iterationsUsed != nullptr) *iterationsUsed = completedIterations;
    return transform_vector(objectTransform, totalLocalCorrection);
}

PlayerEditResolution resolve_player_after_voxel_edit(
    const VoxelObject& object,
    const RigidTransform& objectTransform,
    PlayerCollisionState& player,
    float groundProbeDistance,
    float skin) {
    PlayerEditResolution result;
    const bool wasGrounded = player.grounded;
    result.correction = depenetrate_capsule_conservative(
        object, objectTransform, player.capsule, 8, skin, &result.depenetrationIterations);
    if (length_squared(result.correction) > 0.0F) {
        player.capsule.pointA = add(player.capsule.pointA, result.correction);
        player.capsule.pointB = add(player.capsule.pointB, result.correction);
    }

    const auto ground = sweep_capsule_conservative(
        object,
        objectTransform,
        player.capsule,
        {0.0F, 0.0F, -std::max(0.0F, groundProbeDistance)});
    player.grounded = ground.has_value() && ground->worldNormal.z > 0.5F;
    result.grounded = player.grounded;
    result.floorRemoved = wasGrounded && !player.grounded;
    if (player.grounded && player.velocity.z < 0.0F) player.velocity.z = 0.0F;
    return result;
}

} // namespace dve
