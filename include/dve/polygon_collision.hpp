#pragma once

#include <optional>

#include "dve/collision_convex_set.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/rigid_body_adapter.hpp"

namespace dve {

struct PolygonCollisionOptions {
    double densityKilogramsPerCubicMeter{1000.0};
    bool structural{true};
    bool allowSleeping{true};
    bool useContinuousCollision{};
    // AABB proxy thickness used for planar polygon assets such as floors and leaves.
    float minimumThicknessMeters{0.01F};
    std::vector<Float3> hullPoints;
    // Exact convex pieces (dve.collision_convex_set). When present they are the shape,
    // and mass, center of mass and inertia come from them instead of the bounds.
    std::vector<ConvexPrism> convexPieces;
};

[[nodiscard]] std::optional<StaticRigidBodyCreateDesc> make_polygon_static_body_desc(
    const CookedPolygonAsset& asset,
    const RigidTransform& authoredTransform, std::span<const Float3> hullPoints = {},
    std::span<const ConvexPrism> convexPieces = {}) noexcept;

[[nodiscard]] std::optional<RigidBodyCreateDesc> make_polygon_dynamic_body_desc(
    const CookedPolygonAsset& asset,
    const RigidTransform& authoredTransform,
    const PolygonCollisionOptions& options = {},
    Float3* outLocalCenterOfMass = nullptr) noexcept;

} // namespace dve
