#include "dve/render/subsurface.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "dve/polygon_bvh.hpp"
#include "dve/render/subsurface_transmission.hpp"
#include "dve/voxel_metric.hpp"

namespace dve::render {
namespace {
Float3 scale3(Float3 v, float s) noexcept { return {v.x * s, v.y * s, v.z * s}; }
Float3 add3(Float3 a, Float3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Float3 sub3(Float3 a, Float3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot3(Float3 a, Float3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 unit3(Float3 v, Float3 fallback) noexcept {
    const float l = std::sqrt(dot3(v, v));
    return l > 1.0e-20F ? scale3(v, 1.0F / l) : fallback;
}
void finish(SubsurfaceExit& exit, Float3 start) noexcept {
    exit.thickness = std::clamp(dot3(sub3(exit.position, start), exit.normal), 0.0F, exit.chord);
}
}  // namespace

float burley_transmittance(float thickness, float scatterDistance) noexcept {
    if (!(scatterDistance > 0.0F)) return 0.0F;
    const float x = std::max(thickness, 0.0F) / scatterDistance;
    return 0.25F * std::exp(-x) + 0.75F * std::exp(-x / 3.0F);
}

SubsurfaceExit march_voxel_subsurface(const VoxelReferenceInstance& instance, MaterialId material, Float3 point,
                                      Float3 direction, float maximumDistance) {
    SubsurfaceExit exit;
    if (!instance.object || !(maximumDistance > 0.0F)) return exit;
    direction = unit3(direction, {0.0F, 1.0F, 0.0F});
    const Float3 p0 = world_to_voxel(instance.transform, instance.spacing, point);
    // Not normalised: with a unit world direction the DDA parameter is world distance.
    const Float3 dl = divide_components(inverse_transform_vector(instance.transform, direction), instance.spacing);
    Int3 cell{static_cast<std::int32_t>(std::floor(p0.x)), static_cast<std::int32_t>(std::floor(p0.y)),
              static_cast<std::int32_t>(std::floor(p0.z))};
    if (instance.object->material_at(cell) != material) return exit;   // not inside it
    const float inf = std::numeric_limits<float>::infinity();
    const auto setup = [&](float p, float d, std::int32_t c, std::int32_t& step, float& tMax, float& tDelta) {
        if (d > 0.0F) { step = 1; tMax = (static_cast<float>(c) + 1.0F - p) / d; tDelta = 1.0F / d; }
        else if (d < 0.0F) { step = -1; tMax = (static_cast<float>(c) - p) / d; tDelta = -1.0F / d; }
        else { step = 0; tMax = inf; tDelta = inf; }
    };
    Int3 step{};
    float tMax[3], tDelta[3];
    setup(p0.x, dl.x, cell.x, step.x, tMax[0], tDelta[0]);
    setup(p0.y, dl.y, cell.y, step.y, tMax[1], tDelta[1]);
    setup(p0.z, dl.z, cell.z, step.z, tMax[2], tDelta[2]);
    for (int guard = 0; guard < 1 << 16; ++guard) {
        const int axis = tMax[0] <= tMax[1] && tMax[0] <= tMax[2] ? 0 : (tMax[1] <= tMax[2] ? 1 : 2);
        const float t = tMax[axis];
        if (!(t <= maximumDistance)) return exit;
        Float3 localNormal{};
        if (axis == 0) { cell.x += step.x; tMax[0] += tDelta[0]; localNormal.x = static_cast<float>(step.x); }
        if (axis == 1) { cell.y += step.y; tMax[1] += tDelta[1]; localNormal.y = static_cast<float>(step.y); }
        if (axis == 2) { cell.z += step.z; tMax[2] += tDelta[2]; localNormal.z = static_cast<float>(step.z); }
        const MaterialId beyond = instance.object->material_at(cell);
        if (beyond == material) continue;
        exit.found = true;
        exit.open = beyond == kAirMaterial;
        exit.chord = t;
        exit.position = add3(point, scale3(direction, t));
        exit.normal = voxel_normal_to_world(instance.transform, instance.spacing, localNormal);
        finish(exit, point);
        return exit;
    }
    return exit;
}

namespace {
// The hit's interpolated vertex normal (local space), or its face normal.
Float3 interpolated_normal(const CookedPolygonAsset& asset, const PolygonBvhHit& hit) noexcept {
    Float3 normal = hit.normal;
    const std::size_t t = hit.firstIndex;   // the triangle's place in the index buffer
    if (t + 2U >= asset.indices.size()) return normal;
    const PolygonVertex& a = asset.vertices[asset.indices[t]];
    const PolygonVertex& b = asset.vertices[asset.indices[t + 1U]];
    const PolygonVertex& c = asset.vertices[asset.indices[t + 2U]];
    const Float3 e1 = sub3(b.position, a.position), e2 = sub3(c.position, a.position);
    const Float3 q = sub3(hit.position, a.position);
    const float d00 = dot3(e1, e1), d01 = dot3(e1, e2), d11 = dot3(e2, e2), d20 = dot3(q, e1), d21 = dot3(q, e2);
    const float denominator = d00 * d11 - d01 * d01;
    if (!(std::abs(denominator) > 1.0e-20F)) return normal;
    const float v = (d11 * d20 - d01 * d21) / denominator, w = (d00 * d21 - d01 * d20) / denominator;
    const float u = 1.0F - v - w;
    return unit3(add3(add3(scale3(a.normal, u), scale3(b.normal, v)), scale3(c.normal, w)), normal);
}
}  // namespace

SubsurfaceExit march_polygon_subsurface(const PolygonBvh& bvh, const CookedPolygonAsset& asset, const RigidTransform& transform,
                                        Float3 point, Float3 direction, float maximumDistance) {
    SubsurfaceExit exit;
    if (!(maximumDistance > 0.0F)) return exit;
    direction = unit3(direction, {0.0F, 1.0F, 0.0F});
    const Float3 localDirection = inverse_transform_vector(transform, direction);
    const auto hit = bvh.raycast(inverse_transform_point(transform, point), localDirection, maximumDistance);
    if (!hit) return exit;
    Float3 normal = interpolated_normal(asset, *hit);
    // Leaving the mesh: the outward normal points along the march.
    if (dot3(normal, localDirection) < 0.0F) normal = scale3(normal, -1.0F);
    exit.found = true;
    exit.open = true;
    exit.chord = hit->distance;
    exit.position = add3(point, scale3(direction, hit->distance));
    exit.normal = unit3(transform_vector(transform, normal), direction);
    finish(exit, point);
    return exit;
}

std::optional<SubsurfaceSurfacePoint> probe_polygon_surface(const PolygonBvh& bvh, const CookedPolygonAsset& asset,
                                                            const RigidTransform& transform, Float3 origin, Float3 direction,
                                                            float maximumDistance) {
    direction = unit3(direction, {0.0F, -1.0F, 0.0F});
    const auto hit = bvh.raycast(inverse_transform_point(transform, origin), inverse_transform_vector(transform, direction),
                                 maximumDistance);
    if (!hit) return std::nullopt;
    SubsurfaceSurfacePoint point;
    point.position = add3(origin, scale3(direction, hit->distance));
    point.normal = unit3(transform_vector(transform, interpolated_normal(asset, *hit)), scale3(direction, -1.0F));
    point.materialIndex = hit->materialIndex;
    return point;
}

float burley_sample_radius(float u, float d) noexcept {
    u = std::clamp(u, 0.0F, 0.9999999F);
    return u < 0.25F ? -d * std::log(1.0F - u / 0.25F) : -3.0F * d * std::log(1.0F - (u - 0.25F) / 0.75F);
}

float burley_half_plane_share(float x, float d) noexcept {
    if (!(d > 0.0F)) return x > 0.0F ? 0.0F : 1.0F;
    if (x < 0.0F) return 1.0F - burley_half_plane_share(-x, d);
    // Share beyond the line = Int over r of (profile mass on the ring) x (the ring's share beyond
    // the line, acos(x / r) / pi for r > x). Each exponential: substitute r = x + scale y.
    const auto component = [&](float scale) {
        double sum = 0.0;
        const int n = 4096;
        for (int i = 0; i < n; ++i) {
            const double y = (i + 0.5) / n * 40.0;   // up to 40 scale lengths past the line
            const double r = x + scale * y;
            sum += std::exp(-(r) / scale) / scale * std::acos(std::min(1.0, x / r)) / 3.14159265358979323846 *
                   (40.0 / n) * scale;
        }
        return sum;
    };
    return static_cast<float>(0.25 * component(d) + 0.75 * component(3.0F * d));
}

Float3 subsurface_transmitted_radiance(const VoxelMaterialDefinition& material, const SubsurfaceExit& exit,
                                       Float3 towardSun, float metersPerUnit, float sunVisibility,
                                       const RenderEnvironment& environment) noexcept {
    if (!material_has_subsurface(material) || !exit.found || !exit.open || !(sunVisibility > 0.0F)) return {};
    const float cosEntry = std::max(0.0F, dot3(exit.normal, unit3(towardSun, {0.0F, 1.0F, 0.0F})));
    if (!(cosEntry > 0.0F)) return {};
    // Per channel: the share of light scattered through a slab of this thickness, from the
    // random-walk ground truth (subsurface_transmission.hpp), for that channel's albedo.
    const float x = exit.thickness * metersPerUnit / material.subsurfaceScatterDistanceMeters;
    const float k = environment.sunIntensity * cosEntry * sunVisibility;
    return {environment.sunColor.x * k * subsurface_slab_transmission(material.subsurfaceColor.x, x, cosEntry),
            environment.sunColor.y * k * subsurface_slab_transmission(material.subsurfaceColor.y, x, cosEntry),
            environment.sunColor.z * k * subsurface_slab_transmission(material.subsurfaceColor.z, x, cosEntry)};
}

} // namespace dve::render
