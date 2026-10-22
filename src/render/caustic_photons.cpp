#include "dve/render/caustic_photons.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>


namespace dve::render {
namespace {
Float3 scale3(Float3 v, float s) noexcept { return {v.x * s, v.y * s, v.z * s}; }
Float3 add3(Float3 a, Float3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Float3 sub3(Float3 a, Float3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot3(Float3 a, Float3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 cross3(Float3 a, Float3 b) noexcept { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Float3 unit3(Float3 v) noexcept {
    const float l = std::sqrt(dot3(v, v));
    return l > 0.0F ? scale3(v, 1.0F / l) : Float3{0.0F, 1.0F, 0.0F};
}
void basis(Float3 n, Float3& u, Float3& v) noexcept {
    const Float3 helper = std::abs(n.y) < 0.9F ? Float3{0.0F, 1.0F, 0.0F} : Float3{1.0F, 0.0F, 0.0F};
    u = unit3(cross3(helper, n));
    v = cross3(n, u);
}
// Uniform point on the unit disc (Shirley-Chiu concentric map of the unit square).
void square_to_disk(float a, float b, float& x, float& y) noexcept {
    const float ox = 2.0F * a - 1.0F, oy = 2.0F * b - 1.0F;
    if (ox == 0.0F && oy == 0.0F) { x = y = 0.0F; return; }
    float r = 0.0F, theta = 0.0F;
    if (std::abs(ox) > std::abs(oy)) { r = ox; theta = 0.78539816F * (oy / ox); }
    else { r = oy; theta = 1.57079633F - 0.78539816F * (ox / oy); }
    x = r * std::cos(theta);
    y = r * std::sin(theta);
}
std::uint64_t cell_key(std::int64_t x, std::int64_t y, std::int64_t z) noexcept {
    const auto m = [](std::int64_t a) { return static_cast<std::uint64_t>(a + (1 << 20)) & 0x1FFFFFU; };
    return (m(x) << 42U) | (m(y) << 21U) | m(z);
}
}  // namespace

float CausticLightRegion::area() const noexcept {
    const Float3 c = cross3(edgeU, edgeV);
    return std::sqrt(dot3(c, c));
}
bool CausticLightRegion::valid() const noexcept { return area() > 0.0F && std::isfinite(area()); }

CausticLightRegion caustic_light_region(const OpticalScene& scene, Float3 towardSun, float margin) {
    CausticLightRegion region;
    Float3 lo{}, hi{};
    if (!scene.refractive_bounds(lo, hi)) return region;
    Float3 all0{}, all1{};
    (void)scene.bounds(all0, all1);
    const Float3 n = unit3(towardSun);
    Float3 u{}, v{};
    basis(n, u, v);
    float u0 = std::numeric_limits<float>::max(), u1 = -u0, v0 = u0, v1 = -u0, top = -u0;
    for (int c = 0; c < 8; ++c) {
        const Float3 p{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
        u0 = std::min(u0, dot3(p, u)); u1 = std::max(u1, dot3(p, u));
        v0 = std::min(v0, dot3(p, v)); v1 = std::max(v1, dot3(p, v));
    }
    for (int c = 0; c < 8; ++c) {
        const Float3 p{(c & 1) ? all1.x : all0.x, (c & 2) ? all1.y : all0.y, (c & 4) ? all1.z : all0.z};
        top = std::max(top, dot3(p, n));
    }
    top += 1.0F;
    u0 -= margin; v0 -= margin; u1 += margin; v1 += margin;
    region.corner = add3(add3(scale3(u, u0), scale3(v, v0)), scale3(n, top));
    region.edgeU = scale3(u, u1 - u0);
    region.edgeV = scale3(v, v1 - v0);
    region.towardSun = n;
    return region;
}

CausticPhotonResult trace_caustic_photons(const OpticalScene& scene, const SunLight& sun, const CausticLightRegion& region,
                                          const CausticPhotonSettings& settings) {
    CausticPhotonResult result;
    if (!region.valid() || settings.photonCount == 0U) return result;
    // Photons travel along the sun's direction; the region must have been built for it
    // (caustic_light_region with the same direction), or it would not cover what that sun lights.
    const Float3 towardSun = unit3(sun.towardSun);
    if (dot3(towardSun, unit3(region.towardSun)) < 0.9999F) { result.regionMismatch = true; return result; }
    std::mt19937_64 rng(settings.seed);
    std::uniform_real_distribution<float> uniform01(0.0F, 1.0F);
    auto random = [&]() -> double { return uniform01(rng); };
    // Stratified over the region: a jittered grid of side x side cells.
    const auto side = static_cast<std::uint64_t>(std::ceil(std::sqrt(static_cast<double>(settings.photonCount))));
    const std::uint64_t count = side * side;
    const float power = region.area() / static_cast<float>(count);
    const Float3 down = scale3(towardSun, -1.0F);
    Float3 du{}, dv{};
    basis(towardSun, du, dv);
    const float tanRadius = std::tan(std::max(sun.angularRadius, 0.0F));
    for (std::uint64_t j = 0; j < side; ++j)
        for (std::uint64_t i = 0; i < side; ++i) {
            ++result.emitted;
            const float a = (static_cast<float>(i) + uniform01(rng)) / static_cast<float>(side);
            const float b = (static_cast<float>(j) + uniform01(rng)) / static_cast<float>(side);
            const Float3 origin = add3(add3(region.corner, scale3(region.edgeU, a)), scale3(region.edgeV, b));
            Float3 direction = down;
            if (tanRadius > 0.0F) {
                float dx = 0.0F, dy = 0.0F;
                const float sa = uniform01(rng), sb = uniform01(rng);
                square_to_disk(sa, sb, dx, dy);
                direction = unit3(add3(down, add3(scale3(du, dx * tanRadius), scale3(dv, dy * tanRadius))));
            }
            const LightPathResult path = settings.includeReflections
                                             ? follow_light_sampled(scene, origin, direction, random, settings.path)
                                             : follow_light_transmitted(scene, origin, direction, settings.path);
            if (!path.landed) { ++result.lost; continue; }
            if (path.refractions == 0U && path.reflections == 0U) { ++result.direct; continue; }
            result.photons.push_back({path.position, path.normal, scale3(path.throughput, power), path.receiverObject});
        }
    return result;
}

CausticPhotonMap::CausticPhotonMap(std::vector<CausticPhoton> photons, float cellSize)
    : photons_(std::move(photons)), cell_(cellSize > 0.0F ? cellSize : 1.0F) {
    keys_.resize(photons_.size());
    order_.resize(photons_.size());
    std::iota(order_.begin(), order_.end(), 0U);
    std::vector<std::uint64_t> key(photons_.size());
    for (std::size_t i = 0; i < photons_.size(); ++i) {
        const Float3 p = photons_[i].position;
        key[i] = cell_key(static_cast<std::int64_t>(std::floor(p.x / cell_)), static_cast<std::int64_t>(std::floor(p.y / cell_)),
                          static_cast<std::int64_t>(std::floor(p.z / cell_)));
    }
    std::sort(order_.begin(), order_.end(), [&](std::uint32_t a, std::uint32_t b) { return key[a] < key[b]; });
    for (std::size_t i = 0; i < order_.size(); ++i) keys_[i] = key[order_[i]];
}

Float3 CausticPhotonMap::irradiance(Float3 point, Float3 normal, float radius) const {
    if (!(radius > 0.0F)) return {};
    const Float3 n = unit3(normal);
    const float r2 = radius * radius;
    Float3 sum{};
    const auto lo = [&](float c) { return static_cast<std::int64_t>(std::floor((c - radius) / cell_)); };
    const auto hi = [&](float c) { return static_cast<std::int64_t>(std::floor((c + radius) / cell_)); };
    for (std::int64_t z = lo(point.z); z <= hi(point.z); ++z)
        for (std::int64_t y = lo(point.y); y <= hi(point.y); ++y)
            for (std::int64_t x = lo(point.x); x <= hi(point.x); ++x) {
                const std::uint64_t k = cell_key(x, y, z);
                auto it = std::lower_bound(keys_.begin(), keys_.end(), k);
                for (; it != keys_.end() && *it == k; ++it) {
                    const CausticPhoton& photon = photons_[order_[static_cast<std::size_t>(it - keys_.begin())]];
                    const Float3 d = sub3(photon.position, point);
                    if (dot3(photon.normal, n) < 0.5F) continue;
                    const float along = dot3(d, n);
                    if (std::abs(along) > 0.25F * radius) continue;
                    const Float3 inPlane = sub3(d, scale3(n, along));
                    if (dot3(inPlane, inPlane) > r2) continue;
                    sum = add3(sum, photon.power);
                }
            }
    return scale3(sum, 1.0F / (3.14159265358979F * r2));
}

Float3 CausticPhotonMap::total_power() const noexcept {
    Float3 sum{};
    for (const CausticPhoton& p : photons_) sum = add3(sum, p.power);
    return sum;
}

} // namespace dve::render
