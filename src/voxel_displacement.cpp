#include "dve/voxel_displacement.hpp"

#include <algorithm>
#include <cmath>

#include "dve/transform.hpp"
#include "dve/voxel_textures.hpp"

namespace dve {
namespace {
float component(Float3 v, int a) noexcept { return a == 0 ? v.x : a == 1 ? v.y : v.z; }
Float3 unit_axis(int a) noexcept { return a == 0 ? Float3{1, 0, 0} : a == 1 ? Float3{0, 1, 0} : Float3{0, 0, 1}; }
// The projection's (u, v) axes for a face across axis a: X -> (z, y), Y -> (x, z), Z -> (x, y).
int u_axis(int a) noexcept { return a == 0 ? 2 : 0; }
int v_axis(int a) noexcept { return a == 1 ? 2 : 1; }
int dominant(Float3 n) noexcept {
    const float x = std::abs(n.x), y = std::abs(n.y), z = std::abs(n.z);
    return x >= y && x >= z ? 0 : y >= z ? 1 : 2;
}
float texel_meters(const VoxelMaterialDefinition& m, float level) noexcept {
    // A procedural height has no texels: differentiate over a hundredth of its period.
    if (!m.heightMap) return std::max(m.surfaceDetail.periodMeters, 1.0e-5F) * 0.01F * std::exp2(std::max(0.0F, level));
    const auto size = static_cast<float>(std::max(m.heightMap->width, m.heightMap->height));
    return std::max(m.textureTileMeters, 1.0e-4F) / size * std::exp2(std::max(0.0F, level));
}
} // namespace

float voxel_face_height(const VoxelMaterialDefinition& m, Float3 p, int axis, float level) noexcept {
    if (!m.heightMap) return surface_pattern_height(m.surfaceDetail, p);
    const float s = 1.0F / std::max(m.textureTileMeters, 1.0e-4F);
    const float u = component(p, u_axis(axis)) * s, v = component(p, v_axis(axis)) * s;
    return sample_voxel_image(*m.heightMap, u, v, level).x;
}

float voxel_displacement_depth(const VoxelMaterialDefinition& m, float metersPerVoxel) noexcept {
    if (!material_has_displacement(m)) return 0.0F;
    return std::min(m.displacementDepthMeters, 0.5F * std::max(metersPerVoxel, 1.0e-6F));
}

Float3 voxel_displaced_normal(const VoxelMaterialDefinition& m, Float3 p, Float3 faceNormal, float depthMeters, float level) noexcept {
    if (!material_has_height_source(m) || !(depthMeters > 0.0F)) return faceNormal;
    const int a = dominant(faceNormal);
    const int ua = u_axis(a), va = v_axis(a);
    const float d = texel_meters(m, level);
    const auto h = [&](int axis, float delta) {
        Float3 q = p;
        if (axis == 0) q.x += delta; else if (axis == 1) q.y += delta; else q.z += delta;
        return voxel_face_height(m, q, a, level);
    };
    const float du = (h(ua, d) - h(ua, -d)) / (2.0F * d), dv = (h(va, d) - h(va, -d)) / (2.0F * d);
    // The surface sits depth x h along the outward normal: its normal is n - depth x grad(h).
    const Float3 gradient = add(multiply(unit_axis(ua), du), multiply(unit_axis(va), dv));
    return normalize(subtract(faceNormal, multiply(gradient, depthMeters)));
}

std::optional<VoxelDisplacedHit> trace_displaced_voxel(const VoxelMaterialDefinition& m, Int3 cell, std::uint8_t exposedFaces,
                                                       Float3 origin, Float3 direction, float tEnter, float tExit, Float3 metersPerCell,
                                                       float level, std::uint32_t steps) {
    const float mpc[3] = {metersPerCell.x, metersPerCell.y, metersPerCell.z};
    const float minMpc = std::min({mpc[0], mpc[1], mpc[2]});
    const float depthMeters = voxel_displacement_depth(m, minMpc);
    if (!(depthMeters > 0.0F) || exposedFaces == 0U) return VoxelDisplacedHit{tEnter, {}, -1};
    const int c[3] = {cell.x, cell.y, cell.z};
    // Slack of the most nearly violated face constraint (>= 0: solid) and which face it is.
    const auto slack = [&](float t, int* faceOut) {
        const Float3 p = add(origin, multiply(direction, t));
        const float pc[3] = {p.x, p.y, p.z};
        const Float3 meters{p.x * mpc[0], p.y * mpc[1], p.z * mpc[2]};
        float worst = 1.0e30F;
        int face = -1;
        for (int bit = 0; bit < 6; ++bit) {
            if ((exposedFaces & (1U << bit)) == 0U) continue;
            const int a = bit / 2;
            const bool positive = (bit & 1) != 0;
            const float depthCells = positive ? static_cast<float>(c[a] + 1) - pc[a] : pc[a] - static_cast<float>(c[a]);
            // Heights are read on the face plane, so each face is a height field (as in parallax).
            Float3 onFace = meters;
            const float plane = static_cast<float>(positive ? c[a] + 1 : c[a]) * mpc[a];
            if (a == 0) onFace.x = plane; else if (a == 1) onFace.y = plane; else onFace.z = plane;
            const float carve = (1.0F - voxel_face_height(m, onFace, a, level)) * depthMeters / mpc[a];
            const float s = depthCells - carve;
            if (s < worst) { worst = s; face = bit; }
        }
        if (faceOut) *faceOut = face;
        return worst;
    };
    steps = std::max(2U, steps);
    float previous = tEnter;
    if (slack(tEnter, nullptr) >= 0.0F) {
        // Solid where the ray enters: entered through an unexposed side (a wall the carving
        // opened) or a face whose height is 1 there.
        int face = -1;
        (void)slack(tEnter, &face);
        return VoxelDisplacedHit{tEnter, {}, -1};
    }
    for (std::uint32_t i = 1; i <= steps; ++i) {
        const float t = tEnter + (tExit - tEnter) * static_cast<float>(i) / static_cast<float>(steps);
        if (slack(t, nullptr) < 0.0F) { previous = t; continue; }
        float lo = previous, hi = t;
        for (int k = 0; k < 10; ++k) {
            const float mid = 0.5F * (lo + hi);
            (slack(mid, nullptr) >= 0.0F ? hi : lo) = mid;
        }
        int face = -1;
        (void)slack(hi, &face);
        if (face < 0) return VoxelDisplacedHit{hi, {}, -1};
        const int a = face / 2;
        Float3 faceNormal = unit_axis(a);
        if ((face & 1) == 0) faceNormal = multiply(faceNormal, -1.0F);
        const Float3 p = add(origin, multiply(direction, hi));
        Float3 meters{p.x * mpc[0], p.y * mpc[1], p.z * mpc[2]};
        const float plane = static_cast<float>((face & 1) != 0 ? c[a] + 1 : c[a]) * mpc[a];
        if (a == 0) meters.x = plane; else if (a == 1) meters.y = plane; else meters.z = plane;
        return VoxelDisplacedHit{hi, voxel_displaced_normal(m, meters, faceNormal, depthMeters, level), a};
    }
    return std::nullopt;
}

VoxelParallaxResult voxel_parallax(const VoxelMaterialDefinition& m, Float3 p, Float3 faceNormal, Float3 view, float depthMeters,
                                   float level, std::uint32_t steps) noexcept {
    VoxelParallaxResult out{p, faceNormal, 0.0F, 0U};
    if (!material_has_height_source(m) || !(depthMeters > 0.0F)) return out;
    const int a = dominant(faceNormal);
    steps = std::max(2U, steps);
    // The view ray below the face: each unit of depth moves the read point lateral / viewN.
    const float viewN = std::max(dot(view, faceNormal), 0.1F);
    const Float3 lateral = subtract(view, multiply(faceNormal, dot(view, faceNormal)));
    const Float3 perDepth = multiply(lateral, -1.0F / viewN);
    const auto at = [&](float z) { return add(p, multiply(perDepth, z)); };
    const auto surface = [&](float z) { return (1.0F - voxel_face_height(m, at(z), a, level)) * depthMeters; };
    float zPrevious = 0.0F, sPrevious = surface(0.0F);
    out.samples = 1U;
    float hitZ = depthMeters;
    if (sPrevious <= 0.0F) {
        hitZ = 0.0F;
    } else {
        for (std::uint32_t i = 1; i <= steps; ++i) {
            const float z = depthMeters * static_cast<float>(i) / static_cast<float>(steps);
            const float s = surface(z);
            ++out.samples;
            if (z >= s) {
                // Linear refinement between the last layer above the surface and this one.
                const float before = sPrevious - zPrevious, after = z - s;
                const float w = before + after > 0.0F ? before / (before + after) : 0.0F;
                hitZ = zPrevious + (z - zPrevious) * w;
                break;
            }
            zPrevious = z;
            sPrevious = s;
        }
    }
    out.depthMeters = hitZ;
    out.positionMeters = at(hitZ);
    out.normal = voxel_displaced_normal(m, out.positionMeters, faceNormal, depthMeters, level);
    return out;
}

} // namespace dve
