#include "dve/render/subsurface_random_walk.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "dve/render/optical_scene.hpp"

namespace dve::render {
namespace {
constexpr float kTwoPi = 6.28318530717958647692F;
Float3 scale3(Float3 v, float s) noexcept { return {v.x * s, v.y * s, v.z * s}; }
Float3 add3(Float3 a, Float3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
float dot3(Float3 a, Float3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 cross3(Float3 a, Float3 b) noexcept { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Float3 unit3(Float3 v, Float3 fallback = {0.0F, 1.0F, 0.0F}) noexcept {
    const float l = std::sqrt(dot3(v, v));
    return l > 1.0e-20F ? scale3(v, 1.0F / l) : fallback;
}
// A direction at angle acos(cosTheta) from `axis`, at azimuth phi.
Float3 around(Float3 axis, float cosTheta, float phi) noexcept {
    const Float3 helper = std::abs(axis.y) < 0.9F ? Float3{0.0F, 1.0F, 0.0F} : Float3{1.0F, 0.0F, 0.0F};
    const Float3 u = unit3(cross3(helper, axis));
    const Float3 v = cross3(axis, u);
    const float sinTheta = std::sqrt(std::max(0.0F, 1.0F - cosTheta * cosTheta));
    return unit3(add3(scale3(axis, cosTheta), add3(scale3(u, sinTheta * std::cos(phi)), scale3(v, sinTheta * std::sin(phi)))));
}
// Henyey-Greenstein about the current direction (g = 0: uniform sphere).
Float3 sample_phase(Float3 direction, float g, float u1, float u2) noexcept {
    float cosTheta;
    if (std::abs(g) < 1.0e-3F) cosTheta = 1.0F - 2.0F * u1;
    else {
        const float q = (1.0F - g * g) / (1.0F - g + 2.0F * g * u1);
        cosTheta = std::clamp((1.0F + g * g - q * q) / (2.0F * g), -1.0F, 1.0F);
    }
    return around(direction, cosTheta, kTwoPi * u2);
}
}  // namespace

SubsurfaceMedium subsurface_medium_from_albedo(float albedo, float scatterDistanceMeters, float metersPerUnit) noexcept {
    SubsurfaceMedium medium;
    const float a = std::clamp(albedo, 0.0F, 0.999F);
    medium.albedo = 1.0F - std::exp(-5.09406F * a + 2.61188F * a * a - 4.31805F * a * a * a);
    const float s = 1.9F - a + 3.5F * (a - 0.8F) * (a - 0.8F);
    const float distance = std::max(scatterDistanceMeters, 1.0e-6F) / (metersPerUnit > 0.0F ? metersPerUnit : kDefaultMetersPerVoxel);
    medium.extinction = 1.0F / (distance * s);
    return medium;
}

SubsurfaceExit SlabSubsurfaceDomain::march(Float3 point, Float3 direction, float maximumDistance) const {
    SubsurfaceExit exit;
    direction = unit3(direction);
    float t = std::numeric_limits<float>::infinity();
    Float3 normal{};
    if (direction.y > 0.0F && std::isfinite(top_)) { t = (top_ - point.y) / direction.y; normal = {0.0F, 1.0F, 0.0F}; }
    if (direction.y < 0.0F && std::isfinite(bottom_)) { t = (bottom_ - point.y) / direction.y; normal = {0.0F, -1.0F, 0.0F}; }
    if (!(t <= maximumDistance)) return exit;
    t = std::max(t, 0.0F);
    exit.found = true;
    exit.open = true;
    exit.chord = t;
    exit.position = add3(point, scale3(direction, t));
    exit.normal = normal;
    exit.thickness = std::abs(t * direction.y);
    return exit;
}

SubsurfaceExit VoxelSubsurfaceDomain::march(Float3 point, Float3 direction, float maximumDistance) const {
    return march_voxel_subsurface(*instance_, material_, point, direction, maximumDistance);
}

SubsurfaceExit PolygonSubsurfaceDomain::march(Float3 point, Float3 direction, float maximumDistance) const {
    return march_polygon_subsurface(*bvh_, *asset_, transform_, point, direction, maximumDistance);
}

RandomWalkResult random_walk_impl(const SubsurfaceDomain& domain, const SubsurfaceMedium& medium, Float3 entryPoint,
                                  Float3 entryNormal, Float3 direction, float (*uniform)(void*), void* state,
                                  const RandomWalkSettings& settings) {
    RandomWalkResult result;
    Float3 position = add3(entryPoint, scale3(unit3(entryNormal), -settings.surfaceOffset));
    Float3 dir = unit3(direction);
    const float eta = medium.boundaryIndex > 0.0F ? medium.boundaryIndex : 1.0F;
    const auto random = [&] { return std::min(uniform(state), 0.99999994F); };
    for (std::uint32_t step = 0; step < settings.maximumEvents; ++step) {
        const float flight = -std::log(1.0F - random()) / std::max(medium.extinction, 1.0e-12F);
        const SubsurfaceExit exit = domain.march(position, dir, flight);
        if (exit.found) {
            if (!exit.open) { result.end = RandomWalkResult::End::Blocked; return result; }
            const Float3 n = unit3(exit.normal);
            const float cosI = std::clamp(dot3(dir, n), 0.0F, 1.0F);
            std::optional<Float3> out = dir;
            if (eta != 1.0F) {
                const float fresnel = dielectric_fresnel(cosI, eta, 1.0F);
                out = refract_direction(dir, scale3(n, -1.0F), eta);
                if (!out || random() < fresnel) {
                    dir = reflect_direction(dir, scale3(n, -1.0F));
                    position = add3(exit.position, scale3(n, -settings.surfaceOffset));
                    continue;
                }
            }
            result.end = RandomWalkResult::End::Exited;
            result.position = exit.position;
            result.normal = n;
            result.direction = *out;
            return result;
        }
        position = add3(position, scale3(dir, flight));
        ++result.events;
        if (random() >= medium.albedo) { result.end = RandomWalkResult::End::Absorbed; return result; }
        const float u1 = random(), u2 = random();
        dir = sample_phase(dir, medium.anisotropy, u1, u2);
    }
    result.end = RandomWalkResult::End::Dropped;
    return result;
}

std::optional<RandomWalkResult> enter_and_walk_impl(const SubsurfaceDomain& domain, const SubsurfaceMedium& medium,
                                                    Float3 entryPoint, Float3 entryNormal, Float3 incoming,
                                                    float (*uniform)(void*), void* state, const RandomWalkSettings& settings) {
    const Float3 n = unit3(entryNormal);
    Float3 dir = unit3(incoming);
    const float eta = medium.boundaryIndex > 0.0F ? medium.boundaryIndex : 1.0F;
    if (eta != 1.0F) {
        const float fresnel = dielectric_fresnel(std::clamp(-dot3(dir, n), 0.0F, 1.0F), 1.0F, eta);
        if (std::min(uniform(state), 0.99999994F) < fresnel) return std::nullopt;
        const auto refracted = refract_direction(dir, n, 1.0F / eta);
        if (!refracted) return std::nullopt;
        dir = *refracted;
    }
    return random_walk_impl(domain, medium, entryPoint, n, dir, uniform, state, settings);
}

} // namespace dve::render
