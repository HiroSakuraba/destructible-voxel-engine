#include "dve/render/bitmask_soft_shadow.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace dve::render {
namespace {
std::uint64_t bits_below(int n) noexcept {
    if (n <= 0) return 0U;
    if (n >= 64) return ~std::uint64_t{0};
    return (std::uint64_t{1} << n) - 1U;
}
float depth_at(const SoftShadowDepthLayer& layer, int x, int y) noexcept {
    return layer.depth[static_cast<std::size_t>(y) * layer.width + static_cast<std::size_t>(x)];
}
bool inside(const SoftShadowDepthLayer& layer, int x, int y) noexcept {
    return x >= 0 && y >= 0 && x < static_cast<int>(layer.width) && y < static_cast<int>(layer.height) &&
           layer.depth.size() >= static_cast<std::size_t>(layer.width) * layer.height;
}
}  // namespace

std::uint64_t sun_disc_valid_mask() noexcept {
    static const std::uint64_t mask = [] {
        std::uint64_t m = 0;
        for (std::uint32_t i = 0; i < kSunDiscSampleCount; ++i) {
            const float x = sun_disc_slot_centre(i), y = sun_disc_slot_centre(sun_disc_y_slot(i));
            if (x * x + y * y <= 1.0F) m |= std::uint64_t{1} << i;
        }
        return m;
    }();
    return mask;
}

const std::array<std::uint64_t, 65>& sun_disc_y_below() noexcept {
    static const std::array<std::uint64_t, 65> table = [] {
        std::array<std::uint64_t, 65> t{};
        for (std::uint32_t k = 0; k <= 64; ++k)
            for (std::uint32_t i = 0; i < kSunDiscSampleCount; ++i)
                if (sun_disc_y_slot(i) < k) t[k] |= std::uint64_t{1} << i;
        return t;
    }();
    return table;
}

std::uint64_t sun_disc_box_mask(float x0f, float x1f, float y0f, float y1f) noexcept {
    const int x0 = std::max(static_cast<int>(std::ceil(32.0F * (x0f + 1.0F) - 0.5F)), 0);
    const int x1 = std::min(static_cast<int>(std::floor(32.0F * (x1f + 1.0F) - 0.5F)), 63);
    const int y0 = std::max(static_cast<int>(std::ceil(32.0F * (y0f + 1.0F) - 0.5F)), 0);
    const int y1 = std::min(static_cast<int>(std::floor(32.0F * (y1f + 1.0F) - 0.5F)), 63);
    if (x0 > x1 || y0 > y1) return 0U;
    const auto& below = sun_disc_y_below();
    return (bits_below(x1 + 1) & ~bits_below(x0)) & (below[static_cast<std::size_t>(y1 + 1)] & ~below[static_cast<std::size_t>(y0)]);
}

float bitmask_soft_shadow_visibility(std::span<const SoftShadowDepthLayer> layers, float texelX, float texelY,
                                     float receiverDepth, const BitmaskSoftShadowParameters& p) noexcept {
    if (layers.empty() || !(receiverDepth >= 0.0F) || receiverDepth > 1.0F) return 1.0F;
    if (!(p.tanSunAngularRadius > 0.0F) || !(p.texelMeters > 0.0F) || !(p.depthRangeMeters > 0.0F)) return 1.0F;
    const int cx = static_cast<int>(std::floor(texelX)), cy = static_cast<int>(std::floor(texelY));
    const int cap = static_cast<int>(std::min(p.searchTexels, 16U));
    // 1. Nearest blocker over the search square (every other texel).
    float nearest = receiverDepth;
    for (int sy = -cap; sy <= cap; sy += 2)
        for (int sx = -cap; sx <= cap; sx += 2)
            for (const auto& layer : layers)
                if (inside(layer, cx + sx, cy + sy)) nearest = std::min(nearest, depth_at(layer, cx + sx, cy + sy));
    if (!(nearest < receiverDepth)) return 1.0F;
    const float reach = (receiverDepth - nearest) * p.depthRangeMeters * p.tanSunAngularRadius;
    const int radius = std::min(cap, static_cast<int>(std::ceil(reach / p.texelMeters + 0.5F)));
    // 2. Back-project every texel in front of the receiver onto the sun disc.
    std::uint64_t occluded = 0U;
    const float halfTexel = 0.5F * p.texelMeters;
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x) {
            const float ox = (static_cast<float>(cx + x) + 0.5F - texelX) * p.texelMeters;
            const float oy = (static_cast<float>(cy + y) + 0.5F - texelY) * p.texelMeters;
            for (const auto& layer : layers) {
                if (!inside(layer, cx + x, cy + y)) continue;
                const float depth = depth_at(layer, cx + x, cy + y);
                if (!(depth < receiverDepth)) continue;
                const float footprint = std::max((receiverDepth - depth) * p.depthRangeMeters * p.tanSunAngularRadius, 1.0e-9F);
                occluded |= sun_disc_box_mask((ox - halfTexel) / footprint, (ox + halfTexel) / footprint,
                                              (oy - halfTexel) / footprint, (oy + halfTexel) / footprint);
            }
        }
    const std::uint64_t valid = sun_disc_valid_mask();
    return static_cast<float>(std::popcount(valid & ~occluded)) / static_cast<float>(std::popcount(valid));
}

float pcf_shadow_visibility(std::span<const SoftShadowDepthLayer> layers, float texelX, float texelY,
                            float receiverDepth, std::uint32_t radius) noexcept {
    const int cx = static_cast<int>(std::floor(texelX)), cy = static_cast<int>(std::floor(texelY));
    const int r = static_cast<int>(std::min(radius, 4U));
    float darkest = 1.0F;
    for (const auto& layer : layers) {
        float lit = 0.0F;
        int count = 0;
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x) {
                const int tx = std::clamp(cx + x, 0, static_cast<int>(layer.width) - 1);
                const int ty = std::clamp(cy + y, 0, static_cast<int>(layer.height) - 1);
                lit += receiverDepth <= depth_at(layer, tx, ty) ? 1.0F : 0.0F;
                ++count;
            }
        darkest = std::min(darkest, count ? lit / static_cast<float>(count) : 1.0F);
    }
    return darkest;
}

} // namespace dve::render
