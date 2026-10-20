#include "dve/polygon_collision.hpp"
#include "dve/collision_hull.hpp"

#include <cmath>
#include <algorithm>

namespace dve {
namespace {
PolygonBounds bounds(const CookedPolygonAsset& asset,std::span<const Float3> points) {
    if(points.empty())return asset.bounds;
    PolygonBounds b{points[0],points[0]};for(auto p:points){b.minimum={std::min(b.minimum.x,p.x),std::min(b.minimum.y,p.y),std::min(b.minimum.z,p.z)};b.maximum={std::max(b.maximum.x,p.x),std::max(b.maximum.y,p.y),std::max(b.maximum.z,p.z)};}return b;
}
std::vector<Float3> all_points(std::span<const ConvexPrism> pieces) {
    std::vector<Float3> out;
    for (const auto& piece : pieces) { auto p = piece.points(); out.insert(out.end(), p.begin(), p.end()); }
    return out;
}
bool finite3(Float3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
}

// Dynamic body from exact convex pieces: shape, mass, center of mass and inertia all
// come from the prisms. The bounds box is kept for the reference solver's queries.
static std::optional<RigidBodyCreateDesc> make_convex_set_dynamic_desc(
    const CookedPolygonAsset& asset, const RigidTransform& authoredTransform,
    const PolygonCollisionOptions& options, Float3* outLocalCenterOfMass) noexcept {
    if (!validate_convex_set(options.convexPieces)) return std::nullopt;
    const auto props = convex_set_mass_properties(options.convexPieces);
    if (!props) return std::nullopt;
    const auto points = all_points(options.convexPieces);
    const auto shapeBounds = bounds(asset, points);
    const Float3 com = props->centerOfMass;
    const Float3 boundsCenter = multiply(add(shapeBounds.minimum, shapeBounds.maximum), 0.5F);
    Float3 half = multiply(subtract(shapeBounds.maximum, shapeBounds.minimum), 0.5F);
    const float minimumHalf = options.minimumThicknessMeters * 0.5F;
    half = {std::max(half.x, minimumHalf), std::max(half.y, minimumHalf), std::max(half.z, minimumHalf)};
    const double density = options.densityKilogramsPerCubicMeter;
    RigidBodyCreateDesc desc;
    desc.transform = authoredTransform;
    desc.transform.position = add(authoredTransform.position, rotate(authoredTransform.rotation, com));
    desc.massKilograms = props->volumeCubicMeters * density;
    const auto& i = props->inertiaPerDensity;
    desc.inertiaKilogramMetersSquared = {i.xx * density, i.yy * density, i.zz * density,
                                         i.xy * density, i.xz * density, i.yz * density};
    desc.boxes.push_back({subtract(boundsCenter, com), half});
    for (const auto& piece : options.convexPieces) {
        std::vector<Float3> local;
        for (auto p : piece.points()) local.push_back(subtract(p, com));
        desc.convexPieces.push_back(std::move(local));
    }
    desc.allowSleeping = options.allowSleeping;
    desc.useContinuousCollision = options.useContinuousCollision;
    desc.collisionClass = options.structural ? RigidBodyCollisionClass::Full : RigidBodyCollisionClass::DebrisNoSelf;
    if (!validate_rigid_body_desc(desc)) return std::nullopt;
    if (outLocalCenterOfMass != nullptr) *outLocalCenterOfMass = com;
    return desc;
}

std::optional<StaticRigidBodyCreateDesc> make_polygon_static_body_desc(
    const CookedPolygonAsset& asset, const RigidTransform& authoredTransform, std::span<const Float3> hullPoints,
    std::span<const ConvexPrism> convexPieces) noexcept {
    if (!validate_polygon_asset(asset) || (!hullPoints.empty() && !validate_collision_hull_points(hullPoints))) return std::nullopt;
    if (!convexPieces.empty() && !validate_convex_set(convexPieces)) return std::nullopt;
    const auto piecePoints = all_points(convexPieces);
    const auto shapeBounds = piecePoints.empty() ? bounds(asset, hullPoints) : bounds(asset, piecePoints);
    const Float3 center = multiply(add(shapeBounds.minimum, shapeBounds.maximum), 0.5F);
    Float3 half = multiply(subtract(shapeBounds.maximum, shapeBounds.minimum), 0.5F);
    constexpr float kMinimumHalfThickness = 0.005F;
    half.x = std::max(half.x, kMinimumHalfThickness);
    half.y = std::max(half.y, kMinimumHalfThickness);
    half.z = std::max(half.z, kMinimumHalfThickness);
    if (!finite3(center) || !finite3(half)) return std::nullopt;
    StaticRigidBodyCreateDesc desc;
    desc.transform = authoredTransform;
    desc.boxes.push_back({center, half});
    if (!convexPieces.empty()) for (const auto& piece : convexPieces) desc.convexPieces.push_back(piece.points());
    else desc.convexHullPoints.assign(hullPoints.begin(),hullPoints.end());
    return validate_static_rigid_body_desc(desc) ? std::optional<StaticRigidBodyCreateDesc>(desc)
                                                  : std::nullopt;
}

std::optional<RigidBodyCreateDesc> make_polygon_dynamic_body_desc(
    const CookedPolygonAsset& asset, const RigidTransform& authoredTransform,
    const PolygonCollisionOptions& options, Float3* outLocalCenterOfMass) noexcept {
    if (!validate_polygon_asset(asset) || !(options.densityKilogramsPerCubicMeter > 0.0) ||
        !std::isfinite(options.densityKilogramsPerCubicMeter)) return std::nullopt;
    if(!options.hullPoints.empty() && !validate_collision_hull_points(options.hullPoints))return std::nullopt;
    if(!options.convexPieces.empty())return make_convex_set_dynamic_desc(asset,authoredTransform,options,outLocalCenterOfMass);
    const auto shapeBounds=bounds(asset,options.hullPoints);
    const Float3 center = multiply(add(shapeBounds.minimum, shapeBounds.maximum), 0.5F);
    Float3 half = multiply(subtract(shapeBounds.maximum, shapeBounds.minimum), 0.5F);
    if (!(options.minimumThicknessMeters > 0.0F) || !std::isfinite(options.minimumThicknessMeters)) return std::nullopt;
    const float minimumHalfThickness = options.minimumThicknessMeters * 0.5F;
    half.x = std::max(half.x, minimumHalfThickness);
    half.y = std::max(half.y, minimumHalfThickness);
    half.z = std::max(half.z, minimumHalfThickness);
    if (!finite3(center) || !finite3(half)) return std::nullopt;
    const double volume = 8.0 * static_cast<double>(half.x) * half.y * half.z;
    const double mass = volume * options.densityKilogramsPerCubicMeter;
    if (!(mass > 0.0) || !std::isfinite(mass)) return std::nullopt;
    RigidBodyCreateDesc desc;
    desc.transform = authoredTransform;
    desc.transform.position = add(authoredTransform.position, rotate(authoredTransform.rotation, center));
    desc.massKilograms = mass;
    const double hx = half.x, hy = half.y, hz = half.z;
    desc.inertiaKilogramMetersSquared.xx = mass * (hy * hy + hz * hz) / 3.0;
    desc.inertiaKilogramMetersSquared.yy = mass * (hx * hx + hz * hz) / 3.0;
    desc.inertiaKilogramMetersSquared.zz = mass * (hx * hx + hy * hy) / 3.0;
    desc.boxes.push_back({{}, half});
    for(auto point:options.hullPoints)desc.convexHullPoints.push_back(subtract(point,center));
    desc.allowSleeping = options.allowSleeping;
    desc.useContinuousCollision = options.useContinuousCollision;
    desc.collisionClass = options.structural ? RigidBodyCollisionClass::Full
                                             : RigidBodyCollisionClass::DebrisNoSelf;
    if (!validate_rigid_body_desc(desc)) return std::nullopt;
    if (outLocalCenterOfMass != nullptr) *outLocalCenterOfMass = center;
    return desc;
}

} // namespace dve
