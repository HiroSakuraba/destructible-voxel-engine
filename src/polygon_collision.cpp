#include "dve/polygon_collision.hpp"

#include <cmath>
#include <algorithm>

namespace dve {
namespace {
bool finite3(Float3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
}

std::optional<StaticRigidBodyCreateDesc> make_polygon_static_body_desc(
    const CookedPolygonAsset& asset, const RigidTransform& authoredTransform) noexcept {
    if (!validate_polygon_asset(asset)) return std::nullopt;
    const Float3 center = multiply(add(asset.bounds.minimum, asset.bounds.maximum), 0.5F);
    Float3 half = multiply(subtract(asset.bounds.maximum, asset.bounds.minimum), 0.5F);
    constexpr float kMinimumHalfThickness = 0.005F;
    half.x = std::max(half.x, kMinimumHalfThickness);
    half.y = std::max(half.y, kMinimumHalfThickness);
    half.z = std::max(half.z, kMinimumHalfThickness);
    if (!finite3(center) || !finite3(half)) return std::nullopt;
    StaticRigidBodyCreateDesc desc;
    desc.transform = authoredTransform;
    desc.boxes.push_back({center, half});
    return validate_static_rigid_body_desc(desc) ? std::optional<StaticRigidBodyCreateDesc>(desc)
                                                  : std::nullopt;
}

std::optional<RigidBodyCreateDesc> make_polygon_dynamic_body_desc(
    const CookedPolygonAsset& asset, const RigidTransform& authoredTransform,
    const PolygonCollisionOptions& options, Float3* outLocalCenterOfMass) noexcept {
    if (!validate_polygon_asset(asset) || !(options.densityKilogramsPerCubicMeter > 0.0) ||
        !std::isfinite(options.densityKilogramsPerCubicMeter)) return std::nullopt;
    const Float3 center = multiply(add(asset.bounds.minimum, asset.bounds.maximum), 0.5F);
    Float3 half = multiply(subtract(asset.bounds.maximum, asset.bounds.minimum), 0.5F);
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
    desc.allowSleeping = options.allowSleeping;
    desc.useContinuousCollision = options.useContinuousCollision;
    desc.collisionClass = options.structural ? RigidBodyCollisionClass::Full
                                             : RigidBodyCollisionClass::DebrisNoSelf;
    if (!validate_rigid_body_desc(desc)) return std::nullopt;
    if (outLocalCenterOfMass != nullptr) *outLocalCenterOfMass = center;
    return desc;
}

} // namespace dve
