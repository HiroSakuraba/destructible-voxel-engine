#include "dve/voxel_normal_map.hpp"

#include <algorithm>
#include <cmath>

#include "dve/transform.hpp"
#include "dve/triplanar_normal.hpp"

namespace dve {
namespace {
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
void fnv(std::uint64_t& h, std::uint8_t b) noexcept { h = (h ^ b) * kFnvPrime; }
void fnv32(std::uint64_t& h, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) fnv(h, static_cast<std::uint8_t>(v >> (8 * i)));
}
float wrap(float x) noexcept { return x - std::floor(x); }
} // namespace

std::shared_ptr<const VoxelNormalMap> make_voxel_normal_map(std::string name, std::uint32_t width, std::uint32_t height,
                                                            std::vector<std::uint8_t> rgba8, std::string* error) {
    if (width == 0U || height == 0U || width > kMaximumVoxelNormalMapDimension || height > kMaximumVoxelNormalMapDimension) {
        if (error) *error = "normal map size must be 1.." + std::to_string(kMaximumVoxelNormalMapDimension) + " per side";
        return nullptr;
    }
    if (rgba8.size() != static_cast<std::size_t>(width) * height * 4U) {
        if (error) *error = "normal map texel count does not match its size";
        return nullptr;
    }
    auto map = std::make_shared<VoxelNormalMap>();
    map->name = std::move(name);
    map->width = width;
    map->height = height;
    map->rgba8 = std::move(rgba8);
    std::uint64_t h = kFnvOffset;
    fnv32(h, width);
    fnv32(h, height);
    for (const std::uint8_t b : map->rgba8) fnv(h, b);
    map->contentHash = h;
    return map;
}

Float3 sample_voxel_normal_map(const VoxelNormalMap& map, float u, float v) noexcept {
    if (map.rgba8.empty() || !std::isfinite(u) || !std::isfinite(v)) return {0.0F, 0.0F, 1.0F};
    const float x = wrap(u) * static_cast<float>(map.width) - 0.5F;
    const float y = wrap(v) * static_cast<float>(map.height) - 0.5F;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    const auto texel = [&](int ix, int iy) {
        const auto w = static_cast<int>(map.width), hgt = static_cast<int>(map.height);
        ix = ((ix % w) + w) % w;
        iy = ((iy % hgt) + hgt) % hgt;
        const std::size_t i = (static_cast<std::size_t>(iy) * map.width + static_cast<std::size_t>(ix)) * 4U;
        return Float3{map.rgba8[i] / 127.5F - 1.0F, map.rgba8[i + 1] / 127.5F - 1.0F, map.rgba8[i + 2] / 127.5F - 1.0F};
    };
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const Float3 a = texel(x0, y0), b = texel(x0 + 1, y0), c = texel(x0, y0 + 1), d = texel(x0 + 1, y0 + 1);
    const auto mix = [](Float3 p, Float3 q, float t) {
        return Float3{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t};
    };
    return mix(mix(a, b, tx), mix(c, d, tx), ty);
}

Float3 voxel_normal_map_normal(const VoxelMaterialDefinition& material, Float3 positionMeters, Float3 normal) noexcept {
    if (!material.normalMap || !(material.normalMapStrength > 0.0F)) return normal;
    const VoxelNormalMap& map = *material.normalMap;
    const float scale = 1.0F / std::max(material.normalMapTileMeters, 1.0e-4F);
    const Float3 p = multiply(positionMeters, scale);
    const float sharpness = std::clamp(material.normalMapSharpness, 1.0F, 64.0F);
    Float3 w{std::pow(std::abs(normal.x), sharpness), std::pow(std::abs(normal.y), sharpness),
             std::pow(std::abs(normal.z), sharpness)};
    const float sum = w.x + w.y + w.z;
    if (!(sum > 0.0F)) return normal;
    w = multiply(w, 1.0F / sum);
    const float k = material.normalMapStrength;
    // Tangent-space (tx, ty, tz) per projection (X: u = z, v = y; Y: u = x, v = z; Z: u = x,
    // v = y), blended with the polygon renderer's whiteout triplanar blend: a flat map keeps the
    // geometric normal even where projections overlap.
    const auto tangent = [&](float u, float v) {
        const Float3 t = sample_voxel_normal_map(map, u, v);
        return Float3{t.x * k, t.y * k, std::max(t.z, 0.0F)};
    };
    const Float3 zero{0.0F, 0.0F, 0.0F};
    Float3 out = triplanar_whiteout_normal(normal, w.x > 0.0F ? tangent(p.z, p.y) : zero,
                                                 w.y > 0.0F ? tangent(p.x, p.z) : zero,
                                                 w.z > 0.0F ? tangent(p.x, p.y) : zero, w);
    if (!(length(out) > 0.5F)) return normal;
    // Never bend past the surface (a grazing map on a face would light it from behind).
    if (dot(out, normal) < 0.05F) out = normalize(add(out, multiply(normal, 0.05F - dot(out, normal))));
    return out;
}

} // namespace dve
