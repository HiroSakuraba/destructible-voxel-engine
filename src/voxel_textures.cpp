#include "dve/voxel_textures.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "dve/transform.hpp"
#include "dve/voxel_normal_map.hpp"

namespace dve {
namespace {
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
void fnv(std::uint64_t& h, std::uint8_t b) noexcept { h = (h ^ b) * kFnvPrime; }
void fnv32(std::uint64_t& h, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) fnv(h, static_cast<std::uint8_t>(v >> (8 * i)));
}

float srgb_to_linear(float c) noexcept { return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F); }
float linear_to_srgb(float c) noexcept {
    c = std::clamp(c, 0.0F, 1.0F);
    return c <= 0.0031308F ? c * 12.92F : 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F;
}
const std::array<float, 256>& srgb_table() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) t[static_cast<std::size_t>(i)] = srgb_to_linear(static_cast<float>(i) / 255.0F);
        return t;
    }();
    return table;
}
std::uint8_t quantize(float v) noexcept { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F)); }

// One 2 x 2 box reduction (the last row / column repeats for odd sizes), by role.
VoxelImageLevel reduce(const VoxelImageLevel& src, VoxelImageRole role) {
    VoxelImageLevel dst;
    dst.width = std::max(1U, src.width / 2U);
    dst.height = std::max(1U, src.height / 2U);
    dst.rgba8.resize(static_cast<std::size_t>(dst.width) * dst.height * 4U);
    const auto& lin = srgb_table();
    for (std::uint32_t y = 0; y < dst.height; ++y)
        for (std::uint32_t x = 0; x < dst.width; ++x) {
            float sum[4]{};
            for (std::uint32_t dy = 0; dy < 2U; ++dy)
                for (std::uint32_t dx = 0; dx < 2U; ++dx) {
                    const std::uint32_t sx = std::min(src.width - 1U, 2U * x + dx), sy = std::min(src.height - 1U, 2U * y + dy);
                    const std::uint8_t* t = &src.rgba8[(static_cast<std::size_t>(sy) * src.width + sx) * 4U];
                    for (int c = 0; c < 4; ++c) {
                        float v = static_cast<float>(t[c]) / 255.0F;
                        if (role == VoxelImageRole::Color && c < 3) v = lin[t[c]];
                        if (role == VoxelImageRole::Normal && c < 3) v = v * 2.0F - 1.0F;
                        sum[c] += 0.25F * v;
                    }
                }
            std::uint8_t* out = &dst.rgba8[(static_cast<std::size_t>(y) * dst.width + x) * 4U];
            if (role == VoxelImageRole::Normal) {
                float l = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
                if (!(l > 1.0e-6F)) { sum[0] = sum[1] = 0.0F; sum[2] = l = 1.0F; }
                for (int c = 0; c < 3; ++c) out[c] = quantize(0.5F * (sum[c] / l) + 0.5F);
            } else {
                for (int c = 0; c < 3; ++c) out[c] = quantize(role == VoxelImageRole::Color ? linear_to_srgb(sum[c]) : sum[c]);
            }
            out[3] = quantize(sum[3]);
        }
    return dst;
}

Float4 texel(const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h, int x, int y, bool color) noexcept {
    const int iw = static_cast<int>(w), ih = static_cast<int>(h);
    x = ((x % iw) + iw) % iw;
    y = ((y % ih) + ih) % ih;
    const std::uint8_t* t = rgba + (static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 4U;
    if (color) {
        const auto& lin = srgb_table();
        return {lin[t[0]], lin[t[1]], lin[t[2]], t[3] / 255.0F};
    }
    return {t[0] / 255.0F, t[1] / 255.0F, t[2] / 255.0F, t[3] / 255.0F};
}

Float4 bilinear(const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h, float u, float v, bool color) noexcept {
    const float x = (u - std::floor(u)) * static_cast<float>(w) - 0.5F;
    const float y = (v - std::floor(v)) * static_cast<float>(h) - 0.5F;
    const float fx = std::floor(x), fy = std::floor(y), tx = x - fx, ty = y - fy;
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const Float4 a = texel(rgba, w, h, x0, y0, color), b = texel(rgba, w, h, x0 + 1, y0, color);
    const Float4 c = texel(rgba, w, h, x0, y0 + 1, color), d = texel(rgba, w, h, x0 + 1, y0 + 1, color);
    const auto mix = [](Float4 p, Float4 q, float t) {
        return Float4{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t, p.w + (q.w - p.w) * t};
    };
    return mix(mix(a, b, tx), mix(c, d, tx), ty);
}

Float4 mix4(Float4 a, Float4 b, float t) noexcept {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}
Float4 add4(Float4 a, Float4 b, float w) noexcept { return {a.x + b.x * w, a.y + b.y * w, a.z + b.z * w, a.w + b.w * w}; }

struct Projection {
    Float3 weights{};
    float u[3]{}, v[3]{};
};
Projection project(Float3 p, Float3 normal, float tileMeters, float sharpness) noexcept {
    Projection out;
    const float s = 1.0F / std::max(tileMeters, 1.0e-4F);
    sharpness = std::clamp(sharpness, 1.0F, 64.0F);
    Float3 w{std::pow(std::abs(normal.x), sharpness), std::pow(std::abs(normal.y), sharpness), std::pow(std::abs(normal.z), sharpness)};
    const float sum = w.x + w.y + w.z;
    w = sum > 0.0F ? multiply(w, 1.0F / sum) : Float3{0.0F, 1.0F, 0.0F};
    out.weights = w;
    // The voxel normal map's projections: X -> (z, y), Y -> (x, z), Z -> (x, y).
    out.u[0] = p.z * s; out.v[0] = p.y * s;
    out.u[1] = p.x * s; out.v[1] = p.z * s;
    out.u[2] = p.x * s; out.v[2] = p.y * s;
    return out;
}

Float4 triplanar(const VoxelNormalMap& image, const Projection& pr, float level) noexcept {
    Float4 sum{0.0F, 0.0F, 0.0F, 0.0F};
    const float w[3] = {pr.weights.x, pr.weights.y, pr.weights.z};
    for (int k = 0; k < 3; ++k)
        if (w[k] > 0.0F) sum = add4(sum, sample_voxel_image(image, pr.u[k], pr.v[k], level), w[k]);
    return sum;
}

float level_for(const VoxelNormalMap* image, float tileMeters, float footprintMeters) noexcept {
    if (!image || !(footprintMeters > 0.0F)) return 0.0F;
    const float texelMeters = std::max(tileMeters, 1.0e-4F) / static_cast<float>(std::max(image->width, image->height));
    return std::max(0.0F, std::log2(footprintMeters / texelMeters));
}
} // namespace

std::shared_ptr<const VoxelNormalMap> make_voxel_image(std::string name, std::uint32_t width, std::uint32_t height,
                                                       std::vector<std::uint8_t> rgba8, VoxelImageRole role, std::string* error) {
    if (width == 0U || height == 0U || width > kMaximumVoxelNormalMapDimension || height > kMaximumVoxelNormalMapDimension) {
        if (error) *error = "voxel texture size must be 1.." + std::to_string(kMaximumVoxelNormalMapDimension) + " per side";
        return nullptr;
    }
    if (rgba8.size() != static_cast<std::size_t>(width) * height * 4U) {
        if (error) *error = "voxel texture texel count does not match its size";
        return nullptr;
    }
    auto image = std::make_shared<VoxelNormalMap>();
    image->name = std::move(name);
    image->width = width;
    image->height = height;
    image->rgba8 = std::move(rgba8);
    image->role = static_cast<std::uint8_t>(role);
    std::uint64_t h = kFnvOffset;
    fnv32(h, width);
    fnv32(h, height);
    for (const std::uint8_t b : image->rgba8) fnv(h, b);
    if (role != VoxelImageRole::Normal) fnv(h, static_cast<std::uint8_t>(0xA0U + image->role));   // normal maps keep their old hash
    image->contentHash = h;
    VoxelImageLevel level{width, height, image->rgba8};
    while (level.width > 1U || level.height > 1U) {
        level = reduce(level, role);
        image->mips.push_back(level);
    }
    return image;
}

Float4 sample_voxel_image(const VoxelNormalMap& image, float u, float v, float level) noexcept {
    if (image.rgba8.empty() || !std::isfinite(u) || !std::isfinite(v)) return {0.5F, 0.5F, 1.0F, 1.0F};
    const bool color = image.role == static_cast<std::uint8_t>(VoxelImageRole::Color);
    const float top = static_cast<float>(image.mips.size());
    level = std::clamp(std::isfinite(level) ? level : 0.0F, 0.0F, top);
    const auto at = [&](std::uint32_t l) {
        if (l == 0U) return bilinear(image.rgba8.data(), image.width, image.height, u, v, color);
        const VoxelImageLevel& m = image.mips[l - 1U];
        return bilinear(m.rgba8.data(), m.width, m.height, u, v, color);
    };
    const auto l0 = static_cast<std::uint32_t>(level);
    const float t = level - static_cast<float>(l0);
    if (t <= 0.0F || l0 >= image.mips.size()) return at(l0);
    return mix4(at(l0), at(l0 + 1U), t);
}

VoxelTextureSample sample_voxel_textures(const VoxelMaterialDefinition& m, Float3 positionMeters, Float3 normal,
                                         float footprintMeters) noexcept {
    VoxelTextureSample out;
    if (!material_has_textures(m)) return out;
    const Projection pr = project(positionMeters, normal, m.textureTileMeters, m.textureSharpness);
    const auto level = [&](const std::shared_ptr<const VoxelNormalMap>& image) {
        return level_for(image.get(), m.textureTileMeters, footprintMeters);
    };
    if (m.baseColorMap) {
        const float l = level(m.baseColorMap);
        const Float4 c = triplanar(*m.baseColorMap, pr, l);
        out.baseColor = {c.x, c.y, c.z};
        out.alpha = c.w;
        out.level = l;
    }
    if (m.ormMap) {
        const Float4 c = triplanar(*m.ormMap, pr, level(m.ormMap));
        out.occlusion = c.x;
        out.roughness = c.y;
        out.metallic = c.z;
    }
    if (m.emissiveMap) {
        const Float4 c = triplanar(*m.emissiveMap, pr, level(m.emissiveMap));
        out.emissive = {c.x, c.y, c.z};
    }
    if (m.heightMap) out.height = triplanar(*m.heightMap, pr, level(m.heightMap)).x;
    return out;
}

float sample_voxel_height(const VoxelMaterialDefinition& m, Float3 positionMeters, Float3 normal, float level) noexcept {
    if (!m.heightMap) return 0.5F;
    return triplanar(*m.heightMap, project(positionMeters, normal, m.textureTileMeters, m.textureSharpness), level).x;
}

void apply_voxel_texture_sample(VoxelMaterialDefinition& m, const VoxelTextureSample& s) noexcept {
    m.baseColor = {m.baseColor.x * s.baseColor.x, m.baseColor.y * s.baseColor.y, m.baseColor.z * s.baseColor.z, m.baseColor.w * s.alpha};
    m.roughness = std::clamp(m.roughness * s.roughness, 0.0F, 1.0F);
    m.metallic = std::clamp(m.metallic * s.metallic, 0.0F, 1.0F);
    m.emissive = {m.emissive.x * s.emissive.x, m.emissive.y * s.emissive.y, m.emissive.z * s.emissive.z};
}

} // namespace dve
