// Bitmask soft shadows for the cascaded shadow maps (shadow blister, part 2).
//
// A CPU shadow map of two floating slabs, one in the static layer and one in the dynamic layer,
// seen from the sun straight overhead. Against an exact numeric integral over the sun disc:
//   - bitmask soft shadows follow it across the whole profile and show the blister: the gap
//     between the two penumbrae is darker than either shadow alone, across the two layers;
//   - PCF (fixed kernel, darker layer wins) cannot;
//   - penumbra width grows with the blocker distance;
//   - the HLSL tables and constants match the C++ mirror.
#include "dve/render/bitmask_soft_shadow.hpp"
#include "dve/render/cascaded_shadow_map.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

// Scene in metres. Light plane at y = 40 looking straight down; ground top at y = 1; slabs
// y = 21..22 left of x = -1 (static layer) and right of x = 1 (dynamic layer).
constexpr double kLightY = 40.0, kRange = 40.0, kGroundY = 1.0, kSlabBottom = 21.0, kSlabTop = 22.0, kGapHalf = 1.0;
constexpr double kTanSun = 0.1;   // sun angular radius ~5.7 degrees: a wide penumbra
constexpr float kTexel = 0.25F;   // 128 x 128 texels over x, z in [-16, 16]
constexpr std::uint32_t kSize = 128;

float norm_depth(double y) { return static_cast<float>((kLightY - y) / kRange); }
double texel_centre(std::uint32_t i) { return -16.0 + (i + 0.5) * kTexel; }

struct Maps {
    std::vector<float> staticDepth, dynamicDepth;
    Maps(bool left, bool right) : staticDepth(kSize * kSize), dynamicDepth(kSize * kSize, 1.0F) {
        for (std::uint32_t y = 0; y < kSize; ++y)
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const double wx = texel_centre(x);
                const std::size_t i = y * kSize + x;
                staticDepth[i] = norm_depth(left && wx < -kGapHalf && wx > -30.0 ? kSlabTop : kGroundY);
                if (right && wx > kGapHalf && wx < 30.0) dynamicDepth[i] = norm_depth(kSlabTop);
            }
    }
    std::vector<SoftShadowDepthLayer> layers() const {
        return {{kSize, kSize, staticDepth}, {kSize, kSize, dynamicDepth}};
    }
};

BitmaskSoftShadowParameters parameters(std::uint32_t search = 12) {
    return {kTexel, static_cast<float>(kRange), static_cast<float>(kTanSun), search};
}
float receiver_depth() { return norm_depth(kGroundY) - 1.0e-4F; }

float bitmask_at(const Maps& maps, double x) {
    const auto layers = maps.layers();
    return bitmask_soft_shadow_visibility(layers, static_cast<float>((x + 16.0) / kTexel), 64.0F, receiver_depth(), parameters());
}
float pcf_at(const Maps& maps, double x, std::uint32_t radius) {
    const auto layers = maps.layers();
    return pcf_shadow_visibility(layers, static_cast<float>((x + 16.0) / kTexel), 64.0F, receiver_depth(), radius);
}

// Exact visible fraction of the sun disc (sun overhead), numerically integrated over 360 x 360
// cells, rays against the slab boxes.
double truth(double x, bool left, bool right) {
    const int n = 360;
    int inside = 0, visible = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const double u = (i + 0.5) / n * 2.0 - 1.0, v = (j + 0.5) / n * 2.0 - 1.0;
            if (u * u + v * v > 1.0) continue;
            ++inside;
            // The ray rises from (x, ground); at height h it is at x + u*kTanSun*(h - ground). It is
            // blocked if it is over a slab anywhere between the slab's bottom and top.
            const double a = x + u * kTanSun * (kSlabBottom - kGroundY), b = x + u * kTanSun * (kSlabTop - kGroundY);
            const double lo = std::min(a, b), hi = std::max(a, b);
            const bool blocked = (left && lo < -kGapHalf && hi > -30.0) || (right && hi > kGapHalf && lo < 30.0);
            visible += blocked ? 0 : 1;
        }
    return double(visible) / double(inside);
}

void test_tables() {
    check(std::popcount(sun_disc_valid_mask()) == 52, "52 disc samples");
    const auto& below = sun_disc_y_below();
    check(below[0] == 0U && below[64] == ~std::uint64_t{0} && std::popcount(below[32]) == 32, "prefix table");
    // Every slot is used once in x and once in y (rook pattern).
    std::uint64_t ys = 0;
    for (std::uint32_t i = 0; i < 64; ++i) ys |= std::uint64_t{1} << sun_disc_y_slot(i);
    check(ys == ~std::uint64_t{0}, "rook pattern: one sample per row and per column");
    // A box covering the whole disc covers every sample; an empty box none; the left half about half.
    check(sun_disc_box_mask(-1.0F, 1.0F, -1.0F, 1.0F) == ~std::uint64_t{0}, "full box");
    check(sun_disc_box_mask(0.2F, 0.1F, -1.0F, 1.0F) == 0U, "empty box");
    const int left = std::popcount(sun_disc_box_mask(-1.0F, 0.0F, -1.0F, 1.0F) & sun_disc_valid_mask());
    check(left == 26, "left half of the disc: 26 of 52 (" + std::to_string(left) + ")");
}

void test_blister_against_the_disc_integral() {
    const Maps both(true, true), leftOnly(true, false), rightOnly(false, true);
    double worst = 0.0, worstPcf = 0.0;
    std::printf("   x    exact  bitmask   PCF r=4\n");
    for (double x = -5.0; x <= 5.0 + 1e-9; x += 0.25) {
        const double t = truth(x, true, true), b = bitmask_at(both, x), p = pcf_at(both, x, 4);
        worst = std::max(worst, std::abs(t - b));
        worstPcf = std::max(worstPcf, std::abs(t - p));
        if (std::fmod(x + 5.0, 1.0) < 1e-9) std::printf("%5.1f   %.3f   %.3f    %.3f\n", x, t, b, p);
    }
    std::printf("worst error: bitmask %.3f, PCF %.3f\n", worst, worstPcf);
    check(worst < 0.05, "bitmask within 0.05 of the exact disc integral (" + std::to_string(worst) + ")");
    check(worstPcf > 0.25, "PCF is far off (its softness ignores the sun and the blocker distance)");

    const double t = truth(0.0, true, true), tl = truth(0.0, true, false), tr = truth(0.0, false, true);
    const double b = bitmask_at(both, 0.0), bl = bitmask_at(leftOnly, 0.0), br = bitmask_at(rightOnly, 0.0);
    std::printf("gap centre: exact %.3f (left alone %.3f, right alone %.3f); bitmask %.3f (%.3f, %.3f); PCF %.3f\n",
                t, tl, tr, b, bl, br, pcf_at(both, 0.0, 4));
    check(b < std::min(bl, br) - 0.15, "bitmask: the gap is darker than either shadow alone (blister), across two layers");
    check(std::abs(b - t) < 0.04, "bitmask: blister depth matches the exact value");
    for (std::uint32_t r = 1; r <= 4; ++r)
        check(pcf_at(both, 0.0, r) >= std::min(pcf_at(leftOnly, 0.0, r), pcf_at(rightOnly, 0.0, r)) - 1e-6F,
              "PCF never darkens the gap beyond the darker shadow (r=" + std::to_string(r) + ")");
}

// 10%..90% width of the penumbra behind a single straight edge (blocker at x < 0, height h).
double penumbra_width(double blockerHeight) {
    std::vector<float> depth(kSize * kSize);
    for (std::uint32_t y = 0; y < kSize; ++y)
        for (std::uint32_t x = 0; x < kSize; ++x)
            depth[y * kSize + x] = norm_depth(texel_centre(x) < 0.0 ? kGroundY + blockerHeight : kGroundY);
    const std::vector<SoftShadowDepthLayer> layers{{kSize, kSize, depth}};
    double first = 1e9, last = -1e9;
    for (double x = -8.0; x <= 8.0; x += 0.02) {
        const float v = bitmask_soft_shadow_visibility(layers, static_cast<float>((x + 16.0) / kTexel), 64.0F, receiver_depth(),
                                                       {kTexel, static_cast<float>(kRange), 0.05F, 16});
        if (v > 0.1F) first = std::min(first, x);
        if (v < 0.9F) last = std::max(last, x);
    }
    return last - first;
}

void test_penumbra_grows_with_distance() {
    const double near = penumbra_width(4.0), far = penumbra_width(8.0);
    std::printf("penumbra 10-90%%: blocker 4 m up %.2f m, 8 m up %.2f m\n", near, far);
    // Geometric 10-90% width of a disc edge is ~0.73 x the diameter: 0.73 * 2 * 0.05 * h.
    check(std::abs(near - 0.73 * 0.1 * 4.0) < 0.12 && std::abs(far - 0.73 * 0.1 * 8.0) < 0.15, "width ~ 0.073 x height");
    check(far > 1.7 * near, "twice as far, about twice as wide");
}

void test_search_cap_and_edges() {
    // A blocker hidden behind no texel in front: lit. Receiver above everything: lit.
    const Maps none(false, false);
    check(bitmask_at(none, 0.0) == 1.0F, "no blocker: lit");
    // Directly under a slab, far from its edge: fully dark.
    const Maps both(true, true);
    check(bitmask_at(both, -10.0) == 0.0F && bitmask_at(both, 10.0) == 0.0F, "umbra: dark in both layers");
    // Receivers at the map border do not read outside it.
    const auto layers = both.layers();
    const float v = bitmask_soft_shadow_visibility(layers, 0.5F, 0.5F, receiver_depth(), parameters());
    check(v >= 0.0F && v <= 1.0F, "border receiver stays in range");
    // Zero sun size (or missing parameters) means "no soft shadow information": lit.
    check(bitmask_soft_shadow_visibility(layers, 64.0F, 64.0F, receiver_depth(), {kTexel, 40.0F, 0.0F, 6}) == 1.0F,
          "no sun size: lit");
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void test_hlsl_matches() {
    const std::filesystem::path root = DVE_SOURCE_DIR;
    const std::string hlsl = read_text(root / "shaders/common/cascaded_shadow_map.hlsli");
    const auto parse = [](const std::string& text) {
        std::vector<std::uint64_t> out;
        const std::regex pair(R"(uint2\(0x([0-9A-Fa-f]{8})u,\s*0x([0-9A-Fa-f]{8})u\))");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), pair); it != std::sregex_iterator(); ++it)
            out.push_back(std::stoull((*it)[1], nullptr, 16) | (std::stoull((*it)[2], nullptr, 16) << 32U));
        return out;
    };
    const auto validAt = hlsl.find("kSunDiscValid =");
    const auto tableAt = hlsl.find("kSunDiscYBelow[65] = {");
    check(validAt != std::string::npos && tableAt != std::string::npos, "HLSL tables present");
    if (validAt == std::string::npos || tableAt == std::string::npos) return;
    const auto valid = parse(hlsl.substr(validAt, hlsl.find(';', validAt) - validAt));
    const auto table = parse(hlsl.substr(tableAt, hlsl.find("};", tableAt) - tableAt));
    check(valid.size() == 1 && valid[0] == sun_disc_valid_mask(), "HLSL valid mask = C++");
    check(table.size() == 65 && std::equal(table.begin(), table.end(), sun_disc_y_below().begin()), "HLSL prefix table = C++");
    check(hlsl.find("float SampleCascadedShadowBitmask(") != std::string::npos, "HLSL sampler");

    // The resolve's cbuffer fields, in order, match GpuCascadedShadowConstants.
    const std::string resolve = read_text(root / "shaders/cascaded_shadow_resolve.hlsl");
    const std::vector<std::string> fields{"gCascadeSplitFarMeters", "gCascadeBlendStartMeters", "gCascadeTexelSize",
                                          "gCascadeCount", "gCascadePcfRadius", "gCascadedShadowElementCount",
                                          "gCascadeSoftShadowFilter", "gCascadeSoftShadowSearchTexels",
                                          "gCascadeTanSunAngularRadius", "gCascadeTexelMeters", "gCascadeDepthRangeMeters"};
    std::size_t at = resolve.find("cbuffer CascadedShadowConstants");
    bool ordered = at != std::string::npos;
    for (const auto& name : fields) {
        const auto next = resolve.find(name, at);
        ordered = ordered && next != std::string::npos;
        at = next;
    }
    check(ordered, "cbuffer field order = GpuCascadedShadowConstants");
    check(resolve.find("SampleCascadedShadowBitmask(") != std::string::npos, "resolve uses the bitmask filter");
}

void test_constants() {
    camera::CameraPose camera;
    camera.position = {0.0F, 2.0F, 0.0F};
    CascadedShadowSettings settings;
    check(settings.softShadowFilter == CascadedSoftShadowFilter::Bitmask && settings.validate(), "bitmask by default");
    const CascadedShadowPlan plan = make_cascaded_shadow_plan(camera, {0.3F, 1.0F, 0.2F}, settings);
    const auto c = make_gpu_cascaded_shadow_constants(plan, 1234U, 0.012F);
    check(c.cascadeCount == plan.cascades.size() && c.cascadedShadowElementCount == 1234U &&
              c.cascadeSoftShadowFilter == 1U && c.cascadeSoftShadowSearchTexels == 6U,
          "counts and filter");
    check(std::abs(c.cascadeTanSunAngularRadius - std::tan(0.012F)) < 1e-9F, "tan of the sun radius");
    check(c.cascadeTexelMeters[0] == plan.cascades[0].texelSizeMeters &&
              std::abs(c.cascadeDepthRangeMeters[0] - (plan.cascades[0].maximumLightDepth - plan.cascades[0].minimumLightDepth)) < 1e-3F,
          "per-cascade texel size and depth range");
    check(c.cascadeTexelSize[0] == 1.0F / float(plan.atlasWidth), "atlas texel size");
    settings.softShadowSearchTexels = 0;
    check(!settings.validate(), "search 0 refused");
    settings.softShadowSearchTexels = 17;
    check(!settings.validate(), "search 17 refused");
}
}  // namespace

int main() {
    test_tables();
    test_hlsl_matches();
    test_constants();
    test_search_cap_and_edges();
    test_penumbra_grows_with_distance();
    test_blister_against_the_disc_integral();
    if (g_failures == 0) std::printf("bitmask soft shadow tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
