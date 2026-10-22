#include "dve/render/caustic_map.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <cstdio>
#include <cstdlib>

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
constexpr int kLevels = 8;
std::uint64_t cell_key(int level, std::int64_t x, std::int64_t y, std::int64_t z) noexcept {
    const auto m = [](std::int64_t a) { return static_cast<std::uint64_t>(a + (1 << 19)) & 0xFFFFFU; };
    return (static_cast<std::uint64_t>(level) << 60U) | (m(x) << 40U) | (m(y) << 20U) | m(z);
}
// Area of the intersection of two convex polygons (2D, counter-clockwise), by clipping.
float clipped_area(std::vector<Float2> subject, const Float2* clip, int clipCount) {
    std::vector<Float2> next;
    for (int e = 0; e < clipCount && !subject.empty(); ++e) {
        const Float2 a = clip[e], b = clip[(e + 1) % clipCount];
        const auto inside = [&](Float2 p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x) >= 0.0F; };
        next.clear();
        for (std::size_t i = 0; i < subject.size(); ++i) {
            const Float2 p = subject[i], q = subject[(i + 1) % subject.size()];
            const bool pin = inside(p), qin = inside(q);
            if (pin) next.push_back(p);
            if (pin != qin) {
                const float d1 = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
                const float d2 = (b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x);
                const float t = d1 / (d1 - d2);
                next.push_back({p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t});
            }
        }
        subject.swap(next);
    }
    float area = 0.0F;
    for (std::size_t i = 0; i < subject.size(); ++i) {
        const Float2 p = subject[i], q = subject[(i + 1) % subject.size()];
        area += p.x * q.y - q.x * p.y;
    }
    return 0.5F * std::abs(area);
}
constexpr int kFilterSides = 16;
// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5).
Float3 closest_on_triangle(Float3 p, Float3 a, Float3 b, Float3 c) noexcept {
    const Float3 ab = sub3(b, a), ac = sub3(c, a), ap = sub3(p, a);
    const float d1 = dot3(ab, ap), d2 = dot3(ac, ap);
    if (d1 <= 0.0F && d2 <= 0.0F) return a;
    const Float3 bp = sub3(p, b);
    const float d3 = dot3(ab, bp), d4 = dot3(ac, bp);
    if (d3 >= 0.0F && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) return add3(a, scale3(ab, d1 / (d1 - d3)));
    const Float3 cp = sub3(p, c);
    const float d5 = dot3(ab, cp), d6 = dot3(ac, cp);
    if (d6 >= 0.0F && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) return add3(a, scale3(ac, d2 / (d2 - d6)));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F)
        return add3(b, scale3(sub3(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
    const float denom = 1.0F / (va + vb + vc);
    return add3(a, add3(scale3(ab, vb * denom), scale3(ac, vc * denom)));
}
}  // namespace

CausticMap build_caustic_map(const OpticalScene& scene, const SunLight& sun, const CausticMapSettings& settings) {
    CausticMap map;
    const CausticLightRegion region = caustic_light_region(scene, sun.towardSun, settings.margin);
    if (!region.valid() || settings.resolution == 0U) return map;
    const std::uint32_t n = std::min<std::uint32_t>(settings.resolution, 4096U);
    const std::size_t side = static_cast<std::size_t>(n) + 1U;
    const Float3 down = scale3(unit3(region.towardSun), -1.0F);
    std::vector<LightPathResult> vertices(side * side);
    for (std::size_t j = 0; j < side; ++j)
        for (std::size_t i = 0; i < side; ++i) {
            const float a = static_cast<float>(i) / static_cast<float>(n), b = static_cast<float>(j) / static_cast<float>(n);
            const Float3 origin = add3(add3(region.corner, scale3(region.edgeU, a)), scale3(region.edgeV, b));
            vertices[j * side + i] = follow_light_transmitted(scene, origin, down, settings.path);
            ++map.stats_.rays;
            if (vertices[j * side + i].landed && vertices[j * side + i].refractions > 0U) ++map.stats_.causticRays;
        }
    const float cellSize = std::sqrt(region.area()) / static_cast<float>(n);
    const float sourceArea = region.area() / static_cast<float>(n * n) * 0.5F;
    const float tanSun = std::tan(std::max(sun.angularRadius, 0.0F));
    const auto emit = [&](const LightPathResult& p0, const LightPathResult& p1, const LightPathResult& p2) {
        const LightPathResult* v[3] = {&p0, &p1, &p2};
        int caustic = 0;
        for (const auto* p : v) caustic += p->landed && p->refractions > 0U ? 1 : 0;
        if (caustic == 0) return;
        if (caustic < 3) { ++map.stats_.splitTriangles; return; }
        for (const auto* p : v)
            if (p->receiverObject != p0.receiverObject || p->refractions != p0.refractions || p->reflections != p0.reflections ||
                dot3(p->normal, p0.normal) < settings.sameSurfaceCosine ||
                std::abs(dot3(sub3(p->position, p0.position), p0.normal)) > settings.planeToleranceCells * cellSize) {
                ++map.stats_.splitTriangles;
                return;
            }
        CausticTriangle t;
        t.normal = unit3(add3(add3(p0.normal, p1.normal), p2.normal));
        for (int k = 0; k < 3; ++k)   // onto the receiver's plane through the first vertex
            t.points[k] = sub3(v[k]->position, scale3(t.normal, dot3(sub3(v[k]->position, p0.position), t.normal)));
        const Float3 c = cross3(sub3(t.points[1], t.points[0]), sub3(t.points[2], t.points[0]));
        t.area = 0.5F * std::sqrt(dot3(c, c));
        const Float3 throughput = scale3(add3(add3(p0.throughput, p1.throughput), p2.throughput), 1.0F / 3.0F);
        t.flux = scale3(throughput, sourceArea);
        t.filterRadius = std::max(tanSun * (p0.pathLength + p1.pathLength + p2.pathLength) / 3.0F,
                                  settings.minimumFilterCells * cellSize);
        t.receiver = p0.receiverObject;
        map.triangles_.push_back(t);
    };
    for (std::size_t j = 0; j < n; ++j)
        for (std::size_t i = 0; i < n; ++i) {
            const LightPathResult& a = vertices[j * side + i];
            const LightPathResult& b = vertices[j * side + i + 1U];
            const LightPathResult& c = vertices[(j + 1U) * side + i];
            const LightPathResult& d = vertices[(j + 1U) * side + i + 1U];
            emit(a, b, c);
            emit(b, d, c);
        }
    map.stats_.triangles = map.triangles_.size();
    map.cell_ = std::max(4.0F * cellSize, 1.0e-4F);
    map.index();
    return map;
}

void CausticMap::index() {
    std::vector<std::pair<std::uint64_t, std::uint32_t>> pairs;
    for (std::uint32_t i = 0; i < triangles_.size(); ++i) {
        const CausticTriangle& t = triangles_[i];
        const float pad = t.filterRadius + 0.5F * cell_;   // + the plane tolerance of a query
        Float3 lo = t.points[0], hi = t.points[0];
        for (const Float3& p : t.points) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        lo = {lo.x - pad, lo.y - pad, lo.z - pad};
        hi = {hi.x + pad, hi.y + pad, hi.z + pad};
        const float extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
        // The level whose cells are at least as big as the triangle: it touches at most 2x2x2.
        int level = 0;
        float size = cell_;
        while (level + 1 < kLevels && size < extent) { ++level; size *= 4.0F; }
        for (std::int64_t z = static_cast<std::int64_t>(std::floor(lo.z / size)); z <= static_cast<std::int64_t>(std::floor(hi.z / size)); ++z)
            for (std::int64_t y = static_cast<std::int64_t>(std::floor(lo.y / size)); y <= static_cast<std::int64_t>(std::floor(hi.y / size)); ++y)
                for (std::int64_t x = static_cast<std::int64_t>(std::floor(lo.x / size)); x <= static_cast<std::int64_t>(std::floor(hi.x / size)); ++x)
                    pairs.emplace_back(cell_key(level, x, y, z), i);
    }
    std::sort(pairs.begin(), pairs.end());
    keys_.resize(pairs.size());
    entries_.resize(pairs.size());
    for (std::size_t i = 0; i < pairs.size(); ++i) { keys_[i] = pairs[i].first; entries_[i] = pairs[i].second; }
}

Float3 CausticMap::irradiance(Float3 point, Float3 normal) const {
    if (triangles_.empty()) return {};
    const Float3 n = unit3(normal);
    Float3 sum{};
    std::vector<Float2> subject(3);
    Float2 kernel[kFilterSides];
    float size = cell_;
    for (int level = 0; level < kLevels; ++level, size *= 4.0F) {
        const std::uint64_t key = cell_key(level, static_cast<std::int64_t>(std::floor(point.x / size)),
                                           static_cast<std::int64_t>(std::floor(point.y / size)),
                                           static_cast<std::int64_t>(std::floor(point.z / size)));
        for (auto it = std::lower_bound(keys_.begin(), keys_.end(), key); it != keys_.end() && *it == key; ++it) {
            const CausticTriangle& t = triangles_[entries_[static_cast<std::size_t>(it - keys_.begin())]];
            if (dot3(t.normal, n) < 0.9F || !(t.area > 0.0F)) continue;
            const float plane = dot3(sub3(point, t.points[0]), t.normal);
            if (std::abs(plane) > 0.5F * cell_) continue;
            // 2D frame in the receiver's plane, centred on the point.
            const Float3 helper = std::abs(t.normal.y) < 0.9F ? Float3{0.0F, 1.0F, 0.0F} : Float3{1.0F, 0.0F, 0.0F};
            const Float3 u = unit3(cross3(helper, t.normal)), v = cross3(t.normal, u);
            const float r = t.filterRadius;
            bool near = false;
            for (int k = 0; k < 3; ++k) {
                const Float3 d = sub3(t.points[k], point);
                subject[static_cast<std::size_t>(k)] = {dot3(d, u), dot3(d, v)};
            }
            // Quick reject: the triangle's 2D bounds against the filter's.
            float x0 = subject[0].x, x1 = x0, y0 = subject[0].y, y1 = y0;
            for (const Float2& p : subject) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
            near = x0 <= r && x1 >= -r && y0 <= r && y1 >= -r;
            if (!near) continue;
            const float orientation = (subject[1].x - subject[0].x) * (subject[2].y - subject[0].y) -
                                      (subject[1].y - subject[0].y) * (subject[2].x - subject[0].x);
            if (orientation < 0.0F) std::swap(subject[1], subject[2]);
            for (int k = 0; k < kFilterSides; ++k) {
                const float a = 6.2831853F * (static_cast<float>(k) + 0.5F) / kFilterSides;
                kernel[k] = {r * std::cos(a), r * std::sin(a)};
            }
            const float kernelArea = 0.5F * kFilterSides * r * r * std::sin(6.2831853F / kFilterSides);
            const float covered = clipped_area(subject, kernel, kFilterSides);
            if (!(covered > 0.0F)) continue;
            sum = add3(sum, scale3(t.flux, covered / (t.area * kernelArea)));
        }
    }
    return sum;
}

Float3 CausticMap::total_power() const noexcept {
    Float3 sum{};
    for (const CausticTriangle& t : triangles_) sum = add3(sum, t.flux);
    return sum;
}

} // namespace dve::render
