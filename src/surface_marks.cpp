#include "dve/surface_marks.hpp"

#include <algorithm>
#include <cmath>

#include "dve/material_surface_detail.hpp"
#include "dve/transform.hpp"

namespace dve {
namespace {

float smoothstep(float a, float b, float x) noexcept {
    if (!(b > a)) return x >= b ? 1.0F : 0.0F;
    const float t = std::clamp((x - a) / (b - a), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}
std::uint32_t mix(std::uint32_t h) noexcept {
    h ^= h >> 16; h *= 0x7feb352dU; h ^= h >> 15; h *= 0x846ca68bU; h ^= h >> 16;
    return h;
}
float unit(std::uint32_t h) noexcept { return static_cast<float>(h >> 8) * (1.0F / 16777215.0F); }

// F2 - F1 of jittered 3D cells (cell units): 0 on a cell border.
float voronoi_edge(Float3 q, std::uint32_t seed) noexcept {
    const int ix = static_cast<int>(std::floor(q.x)), iy = static_cast<int>(std::floor(q.y)), iz = static_cast<int>(std::floor(q.z));
    float f1 = 1e9F, f2 = 1e9F;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                const std::uint32_t h = mix(static_cast<std::uint32_t>(ix + x) * 0x8da6b343U ^ static_cast<std::uint32_t>(iy + y) * 0xd8163841U ^
                                            static_cast<std::uint32_t>(iz + z) * 0xcb1ab31fU ^ seed);
                const Float3 point{static_cast<float>(ix + x) + unit(h), static_cast<float>(iy + y) + unit(mix(h + 1U)),
                                   static_cast<float>(iz + z) + unit(mix(h + 2U))};
                const float d = length(subtract(q, point));
                if (d < f1) { f2 = f1; f1 = d; } else if (d < f2) f2 = d;
            }
    return f2 - f1;
}

} // namespace

bool surface_mark_valid(const SurfaceMark& m) noexcept {
    const auto unitRange = [](float v) { return v >= 0.0F && v <= 1.0F; };
    return std::isfinite(m.centerMeters.x) && std::isfinite(m.centerMeters.y) && std::isfinite(m.centerMeters.z) && m.radiusMeters > 0.0F &&
           m.radiusMeters <= 1.0e4F && unitRange(m.exposure) && unitRange(m.scorch) && unitRange(m.cracks) && m.reachScale >= 1.0F &&
           m.reachScale <= 10.0F && m.crackReach >= 1.0F && m.crackReach <= 10.0F;
}

float surface_mark_reach(const SurfaceMark& m) noexcept {
    return m.radiusMeters * std::max({m.reachScale, m.crackReach, 1.25F}) * 1.05F;
}

bool surface_mark_settings_valid(const SurfaceMarkSettings& s) noexcept {
    const auto unitRange = [](float v) { return v >= 0.0F && v <= 1.0F; };
    return unitRange(s.exposure) && unitRange(s.scorch) && unitRange(s.cracks) && s.reachScale >= 1.0F && s.reachScale <= 10.0F &&
           s.crackReach >= 1.0F && s.crackReach <= 10.0F && s.maximumMarks <= 65536U;
}

SurfaceMarkSample sample_surface_marks(std::span<const SurfaceMark> marks, std::uint64_t objectId, Float3 p, float footprint) noexcept {
    SurfaceMarkSample out;
    for (const SurfaceMark& m : marks) {
        if (m.objectId != objectId || !surface_mark_valid(m)) continue;
        const float r = m.radiusMeters;
        const float d = length(subtract(p, m.centerMeters));
        if (d > surface_mark_reach(m)) continue;
        // The broken interior: everything the crater sphere cut (with a little slack for the
        // voxel staircase).
        out.exposure = std::max(out.exposure, m.exposure * (1.0F - smoothstep(r * 1.0F, r * 1.25F, d)));
        // Soot: full at the rim, fading to the reach, with a ragged edge.
        if (m.scorch > 0.0F) {
            MaterialSurfaceDetail noise;
            noise.pattern = SurfacePattern::Mineral;
            noise.periodMeters = std::max(r * 0.4F, 1.0e-4F);
            noise.seed = m.seed;
            noise.bumpHeightMeters = 0.0F;
            const float ragged = 0.35F * sample_surface_pattern(noise, p, footprint).value;
            const float reach = r * m.reachScale;
            // Full inside the crater; past the rim the ragged distance fades it out by the reach.
            const float effective = r + std::max(d - r, 0.0F) * (1.0F - ragged);
            out.scorch = std::max(out.scorch, m.scorch * (1.0F - smoothstep(r, reach, effective)));
        }
        // Fracture lines in the ring outside the rim.
        if (m.cracks > 0.0F && d > r * 0.95F) {
            const float cell = r * 0.35F;
            const float width = 0.045F;   // in cells
            // Thinner than a pixel: fade rather than alias.
            const float visible = footprint > 0.0F ? std::clamp(width * cell / footprint, 0.0F, 1.0F) : 1.0F;
            const float edge = voronoi_edge(multiply(subtract(p, m.centerMeters), 1.0F / cell), m.seed ^ 0x6372616bU);
            const float line = 1.0F - smoothstep(width * 0.5F, width, edge);
            const float fade = 1.0F - smoothstep(r, r * m.crackReach, d);
            out.cracks = std::max(out.cracks, m.cracks * line * fade * visible);
        }
    }
    return out;
}

void apply_surface_marks(VoxelMaterialDefinition& m, const SurfaceMarkSample& s) noexcept {
    if (!s.any()) return;
    const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    // Exposure: lighter and duller, toward the colour's own grey (fresh, unweathered stone).
    if (s.exposure > 0.0F) {
        const float grey = 0.2126F * m.baseColor.x + 0.7152F * m.baseColor.y + 0.0722F * m.baseColor.z;
        const auto fresh = [&](float c) { return std::clamp(lerp(c, grey, 0.35F) * 1.25F + 0.04F, 0.0F, 1.0F); };
        m.baseColor = {lerp(m.baseColor.x, fresh(m.baseColor.x), s.exposure), lerp(m.baseColor.y, fresh(m.baseColor.y), s.exposure),
                       lerp(m.baseColor.z, fresh(m.baseColor.z), s.exposure), m.baseColor.w};
        m.roughness = lerp(m.roughness, std::max(m.roughness, 0.9F), s.exposure);
        m.metallic = lerp(m.metallic, m.metallic * 0.5F, s.exposure);
    }
    // Soot and cracks darken; soot also roughens and kills metal.
    const float soot = std::clamp(s.scorch, 0.0F, 1.0F);
    const float dark = lerp(1.0F, 0.07F, soot) * lerp(1.0F, 0.15F, std::clamp(s.cracks, 0.0F, 1.0F));
    m.baseColor = {m.baseColor.x * dark, m.baseColor.y * dark, m.baseColor.z * dark, m.baseColor.w};
    m.roughness = lerp(m.roughness, 0.95F, std::max(soot, s.cracks));
    m.metallic *= 1.0F - soot;
    m.specular *= lerp(1.0F, 0.5F, soot);
}

bool SurfaceMarkList::push(const SurfaceMark& mark) {
    if (settings_.maximumMarks == 0U || !surface_mark_valid(mark)) return false;
    if (marks_.size() >= settings_.maximumMarks) marks_.erase(marks_.begin(), marks_.begin() + static_cast<std::ptrdiff_t>(marks_.size() - settings_.maximumMarks + 1U));
    marks_.push_back(mark);
    return true;
}

bool SurfaceMarkList::add(const SurfaceMark& mark) {
    return settings_.enabled && push(mark);
}

void SurfaceMarkList::remove_object(std::uint64_t objectId) {
    std::erase_if(marks_, [&](const SurfaceMark& m) { return m.objectId == objectId; });
}

std::size_t SurfaceMarkList::copy_marks(std::uint64_t from, std::uint64_t to, Float3 lo, Float3 hi) {
    std::vector<SurfaceMark> copies;
    for (const SurfaceMark& m : marks_) {
        if (m.objectId != from) continue;
        const Float3 c = m.centerMeters;
        const float dx = std::max({lo.x - c.x, 0.0F, c.x - hi.x}), dy = std::max({lo.y - c.y, 0.0F, c.y - hi.y}),
                    dz = std::max({lo.z - c.z, 0.0F, c.z - hi.z});
        if (dx * dx + dy * dy + dz * dz > surface_mark_reach(m) * surface_mark_reach(m)) continue;
        SurfaceMark copy = m;
        copy.objectId = to;
        copies.push_back(copy);
    }
    for (const SurfaceMark& m : copies) push(m);
    return copies.size();
}

void SurfaceMarkList::restore(std::vector<SurfaceMark> marks, std::uint32_t nextSeed) {
    std::erase_if(marks, [](const SurfaceMark& m) { return !surface_mark_valid(m); });
    if (marks.size() > settings_.maximumMarks)
        marks.erase(marks.begin(), marks.begin() + static_cast<std::ptrdiff_t>(marks.size() - settings_.maximumMarks));
    marks_ = std::move(marks);
    next_ = std::max(nextSeed, 1U);
}

bool SurfaceMarkList::add_crater(std::uint64_t objectId, Float3 centerMeters, float radiusMeters) {
    SurfaceMark m;
    m.objectId = objectId;
    m.centerMeters = centerMeters;
    m.radiusMeters = radiusMeters;
    m.exposure = settings_.exposure;
    m.scorch = settings_.scorch;
    m.cracks = settings_.cracks;
    m.reachScale = settings_.reachScale;
    m.crackReach = settings_.crackReach;
    if (!settings_.enabled) return false;
    m.seed = mix(next_++ * 0x9e3779b9U);
    return add(m);
}

void SurfaceMarkList::set_settings(const SurfaceMarkSettings& settings) {
    settings_ = settings;
    if (marks_.size() > settings_.maximumMarks)
        marks_.erase(marks_.begin(), marks_.begin() + static_cast<std::ptrdiff_t>(marks_.size() - settings_.maximumMarks));
}

} // namespace dve
