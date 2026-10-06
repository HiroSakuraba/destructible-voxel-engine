// Radiance Cascades Phase 1: CPU 2D reference tests (docs/RADIANCE_CASCADES.md).

#include "dve/packed_brickmap.hpp"
#include "dve/render/radiance_cascades_2d.hpp"
#include "dve/voxel_object.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x + " at " + std::to_string(__LINE__)); } while(false)

namespace rc = dve::render::rc2d;

constexpr std::array<rc::MergeMode, 3> kModes{rc::MergeMode::Vanilla, rc::MergeMode::BilinearFix,
                                              rc::MergeMode::ParallaxFix};

bool close(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

rc::Grid2D scene(rc::SyntheticScene kind, std::uint32_t size, float radius = 4.0F) {
    rc::SyntheticSceneOptions options;
    options.width = size;
    options.height = size;
    options.lightRadius = radius;
    return rc::make_synthetic_scene(kind, options);
}

// Two worker threads by default: enough to exercise the threaded path while leaving cores for
// the timing-sensitive audio tests that ctest runs in parallel (see PROCESSORS in CMake).
constexpr std::uint32_t kTestThreads = 2;

rc::FluenceImage solve(const rc::Grid2D& grid, rc::MergeMode mode, float overlap = 0.0F,
                       std::uint32_t threads = kTestThreads, std::uint32_t cascades = 0, rc::Rgb sky = {}) {
    rc::CascadeConfig config;
    config.mergeMode = mode;
    config.intervalOverlap = overlap;
    config.threads = threads;
    config.cascadeCount = cascades;
    config.sky = sky;
    return rc::solve_radiance_cascades(grid, config).fluence;
}

rc::FluenceImage brute(const rc::Grid2D& grid, std::uint32_t rays, std::uint32_t threads = kTestThreads,
                       rc::Rgb sky = {}) {
    rc::BruteForceConfig config;
    config.raysPerPixel = rays;
    config.threads = threads;
    config.sky = sky;
    return rc::solve_brute_force(grid, config).fluence;
}

float at(const rc::Grid2D& grid, const rc::FluenceImage& image, std::uint32_t x, std::uint32_t y) {
    return rc::luminance(image.pixels[grid.index(x, y)]);
}

bool bitwise_equal(const rc::FluenceImage& a, const rc::FluenceImage& b) {
    return a.pixels.size() == b.pixels.size() &&
           std::memcmp(a.pixels.data(), b.pixels.data(), a.pixels.size() * sizeof(rc::Rgb)) == 0;
}

double max_difference(const rc::FluenceImage& a, const rc::FluenceImage& b) {
    double worst = 0.0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        worst = std::max({worst, static_cast<double>(std::abs(a.pixels[i].r - b.pixels[i].r)),
                          static_cast<double>(std::abs(a.pixels[i].g - b.pixels[i].g)),
                          static_cast<double>(std::abs(a.pixels[i].b - b.pixels[i].b))});
    }
    return worst;
}

void test_penumbra_calculator_port() {
    // Reference values produced by running the calculator's own Cascades class from
    // https://kornel.ski/radiance (2024.08, default mode) under Node.
    rc::PenumbraCalculatorInput input;  // 1920x1080, spacing 1, light 1, 90 degrees
    const rc::PenumbraCascade c0 = rc::penumbra_cascade(input, 0);
    CHECK(c0.probeSpacing == 1U && close(c0.intervalStart, 0.0, 1e-6) && close(c0.intervalEnd, 1.5, 1e-5));
    CHECK(c0.minRays == 10U && c0.maxRays == 16U && c0.probes == 2073600U && c0.intervalSteps == 2U);
    CHECK(close(c0.penumbraAngleDegrees, 90.0, 1e-3) && !c0.outsideBounds);
    const rc::PenumbraCascade c4 = rc::penumbra_cascade(input, 4);
    CHECK(c4.probeSpacing == 16U && close(c4.intervalStart, 8.5, 1e-5) && close(c4.intervalEnd, 16.5, 1e-5));
    CHECK(c4.minRays == 104U && c4.maxRays == 205U && c4.probes == 8160U && c4.intervalSteps == 9U);
    const rc::PenumbraCascade c8 = rc::penumbra_cascade(input, 8);
    CHECK(close(c8.intervalStart, 128.5, 1e-4) && close(c8.intervalEnd, 256.5, 1e-4));
    CHECK(c8.minRays == 1612U && c8.maxRays == 3221U && c8.probes == 40U && c8.intervalSteps == 129U);

    input.width = 256;
    input.height = 256;
    input.initialSpacing = 2;
    input.minLightWidth = 2.0F;
    input.penumbraAngleDegrees = 60.0F;
    const rc::PenumbraCascade d0 = rc::penumbra_cascade(input, 0);
    CHECK(close(d0.intervalEnd, 5.1962, 1e-3) && d0.minRays == 17U && d0.maxRays == 23U);
    CHECK(d0.probes == 16384U && d0.intervalSteps == 3U && close(d0.penumbraAngleDegrees, 60.0, 1e-3));
    const rc::PenumbraCascade d3 = rc::penumbra_cascade(input, 3);
    CHECK(close(d3.intervalStart, 15.5885, 1e-3) && close(d3.intervalEnd, 29.4449, 1e-3));
    CHECK(d3.minRays == 93U && d3.maxRays == 143U && d3.probes == 256U && d3.intervalSteps == 7U);
    CHECK(!rc::penumbra_cascade(input, 7).outsideBounds && rc::penumbra_cascade(input, 8).outsideBounds);

    // The engine's default 4-ray/4x layout does not meet the calculator's angular condition for
    // 1-cell lights; the check must say so rather than silently pass.
    const auto levels = rc::build_cascade_levels(rc::CascadeConfig{}, 128, 128);
    const auto checks = rc::check_penumbra_condition(levels, 1.0F, 90.0F);
    CHECK(checks.size() == levels.size() && !checks[0].angularOk && checks[0].requiredRays == 7U);
    const auto relaxed = rc::check_penumbra_condition(levels, 8.0F, 90.0F);
    CHECK(relaxed[0].angularOk && relaxed[1].angularOk);
}

void test_cascade_levels_and_validation() {
    rc::CascadeConfig config;
    CHECK(config.validate());
    CHECK(rc::auto_cascade_count(config, 128, 128) == 5U);
    const auto levels = rc::build_cascade_levels(config, 128, 128);
    CHECK(levels.size() == 5U);
    const std::array<float, 5> starts{0.0F, 1.0F, 5.0F, 21.0F, 85.0F};
    for (std::size_t i = 0; i < levels.size(); ++i) {
        CHECK(levels[i].probeSpacing == (1U << i));
        CHECK(levels[i].rayCount == (4U << (2U * i)));
        CHECK(close(levels[i].intervalStart, starts[i], 1e-5));
        // 4x rays / 2x spacing keeps every cascade the same size (GM Shaders part 1).
        CHECK(static_cast<std::uint64_t>(levels[i].probesX) * levels[i].probesY * levels[i].rayCount == 65536U);
        if (i + 1U < levels.size()) CHECK(close(levels[i].intervalEnd, starts[i + 1], 1e-5));
    }
    CHECK(levels.back().intervalEnd >= std::sqrt(2.0F) * 128.0F);

    config.intervalOverlap = 1.0F;
    const auto overlapped = rc::build_cascade_levels(config, 128, 128);
    CHECK(close(overlapped[0].intervalEnd, 1.0 + std::sqrt(2.0) * 2.0, 1e-5));
    CHECK(close(overlapped[2].intervalEnd, 21.0 + std::sqrt(2.0) * 8.0, 1e-4));

    rc::CascadeConfig bad;
    bad.baseProbeSpacing = 3;
    CHECK(!bad.validate());
    bad = {};
    bad.baseIntervalLength = 0.0F;
    CHECK(!bad.validate());
    bad = {};
    bad.intervalGrowth = 0.5F;
    CHECK(!bad.validate());
    bad = {};
    bad.sky = {-1.0F, 0.0F, 0.0F};
    CHECK(!bad.validate());
    CHECK(rc::solve_radiance_cascades(scene(rc::SyntheticScene::Empty, 16), bad).fluence.pixels.empty());
}

void test_dda_segment() {
    rc::Grid2D grid = rc::Grid2D::empty(8, 8);
    grid.fill_rect(5, 3, 6, 4, true, {1.0F, 2.0F, 3.0F});
    const rc::Rgb sky{0.25F, 0.25F, 0.25F};
    rc::SegmentResult hit = rc::trace_segment(grid, 0.5F, 3.5F, 1.0F, 0.0F, 10.0F, sky);
    CHECK(hit.transmittance == 0.0F && hit.radiance.g == 2.0F && hit.steps == 5U);
    // The cell boundary at x = 5 is reached at t = 4.5: excluded at length 4.4, included at 4.6.
    CHECK(rc::trace_segment(grid, 0.5F, 3.5F, 1.0F, 0.0F, 4.4F, sky).transmittance == 1.0F);
    CHECK(rc::trace_segment(grid, 0.5F, 3.5F, 1.0F, 0.0F, 4.6F, sky).transmittance == 0.0F);
    const rc::SegmentResult escape = rc::trace_segment(grid, 0.5F, 0.5F, -1.0F, 0.0F, 10.0F, sky);
    CHECK(escape.transmittance == 0.0F && escape.radiance.r == 0.25F);
    const rc::SegmentResult inside = rc::trace_segment(grid, 5.5F, 3.5F, 0.0F, 1.0F, 1.0F, sky);
    CHECK(inside.transmittance == 0.0F && inside.steps == 0U && inside.radiance.b == 3.0F);
    const float d = std::sqrt(0.5F);
    CHECK(rc::trace_segment(grid, 2.5F, 0.5F, d, d, 10.0F, sky).transmittance == 0.0F);
    CHECK(rc::trace_segment(grid, 2.5F, 0.5F, d, d, 10.0F, sky).radiance.r == 1.0F);
}

void test_empty_scene_energy() {
    const rc::Grid2D empty = scene(rc::SyntheticScene::Empty, 48);
    const rc::Rgb sky{0.25F, 0.5F, 1.0F};
    for (const rc::MergeMode mode : kModes) {
        for (const float overlap : {0.0F, 1.0F}) {
            const rc::FluenceImage lit = solve(empty, mode, overlap, kTestThreads, 0, sky);
            for (const rc::Rgb& p : lit.pixels)
                CHECK(close(p.r, 0.25, 1e-6) && close(p.g, 0.5, 1e-6) && close(p.b, 1.0, 1e-6));
            for (const rc::Rgb& p : solve(empty, mode, overlap).pixels)
                CHECK(p.r == 0.0F && p.g == 0.0F && p.b == 0.0F);
        }
    }
    for (const rc::Rgb& p : brute(empty, 256, kTestThreads, sky).pixels)
        CHECK(close(p.r, 0.25, 1e-6) && close(p.b, 1.0, 1e-6));
    // Black occluders never create light, and fluence never exceeds the brightest radiance.
    rc::Grid2D blocks = empty;
    blocks.fill_rect(10, 10, 20, 12, true);
    blocks.fill_disc(30.0F, 30.0F, 5.0F, true);
    for (const rc::MergeMode mode : kModes) {
        for (const rc::Rgb& p : solve(blocks, mode).pixels) CHECK(p.r == 0.0F && p.g == 0.0F);
        for (const rc::Rgb& p : solve(blocks, mode, 0.0F, kTestThreads, 0, sky).pixels)
            CHECK(p.b >= 0.0F && p.b <= 1.0F + 1e-5F);
    }
}

void test_single_light_falloff() {
    constexpr std::uint32_t n = 128;
    const rc::Grid2D grid = scene(rc::SyntheticScene::SingleLight, n, 3.0F);
    const rc::FluenceImage reference = brute(grid, 2048);
    const rc::FluenceImage vanilla = solve(grid, rc::MergeMode::Vanilla);
    const rc::FluenceImage fixed = solve(grid, rc::MergeMode::BilinearFix);
    double vanillaPeak = 0.0, fixedPeak = 0.0, previous = 1e9;
    for (std::uint32_t x = 72; x < 124; ++x) {
        const double d = static_cast<double>(x) + 0.5 - 64.0;
        const double analytic = 2.0 * std::asin(std::min(1.0, 3.0 / d)) / (2.0 * 3.14159265358979);
        const double b = at(grid, reference, x, 64);
        // Brute force follows the analytic angular size of the disc; the rasterised disc is a
        // little larger than radius 3, so the reference sits slightly above.
        CHECK(b >= analytic * 0.97 && b <= analytic * 1.25);
        CHECK(b <= previous * 1.06);  // monotone up to rasterisation/stratification noise
        previous = b;
        vanillaPeak = std::max(vanillaPeak, at(grid, vanilla, x, 64) / b);
        fixedPeak = std::max(fixedPeak, at(grid, fixed, x, 64) / b);
    }
    std::cout << "  single light: worst RC/BF ratio on the +x profile: vanilla " << vanillaPeak
              << ", bilinear-fix " << fixedPeak << '\n';
    // Vanilla rings where cascades meet (paper sec. 5 / Osborne & Sannikov 2024 fig. 6); the
    // bilinear fix reduces the overshoot.
    CHECK(vanillaPeak > fixedPeak);
    CHECK(fixedPeak < 1.25);
    CHECK(vanillaPeak < 1.6);
}

void test_occluder_shadow_and_penumbra() {
    constexpr std::uint32_t n = 128;
    const rc::Grid2D grid = scene(rc::SyntheticScene::OccluderShadow, n);
    // Light at (32,64) r=4; bar at x in [51,53), y in [51,76).
    const rc::FluenceImage reference = brute(grid, 1024);
    std::vector<std::pair<const char*, rc::FluenceImage>> images{{"brute-force", reference}};
    for (const rc::MergeMode mode : kModes) images.emplace_back(rc::merge_mode_name(mode), solve(grid, mode));
    for (const auto& [name, image] : images) {
        const double umbra = at(grid, image, 80, 64);
        const double lit = at(grid, image, 32, 112);  // same distance from the light, unoccluded
        // Penumbra profile along x = 110 walking up out of the shadow.
        double minimum = 1e9, maximum = 0.0;
        bool hasPenumbra = false;
        const double litColumn = at(grid, image, 110, 124);
        for (std::uint32_t y = 64; y < 125; ++y) {
            const double v = at(grid, image, 110, y);
            minimum = std::min(minimum, v);
            maximum = std::max(maximum, v);
            if (v > 0.2 * litColumn && v < 0.8 * litColumn) hasPenumbra = true;
        }
        std::cout << "  occluder " << name << ": umbra/lit " << umbra / lit << '\n';
        CHECK(umbra < 0.15 * lit);
        CHECK(hasPenumbra);
        CHECK(minimum < 0.2 * maximum);
    }
}

void test_error_against_reference() {
    constexpr std::uint32_t n = 128;
    struct Case {
        rc::SyntheticScene kind;
        std::array<double, 3> maxRelativeRmse;
    };
    // Thresholds are ~25% above the values measured when Phase 1 landed (bench numbers in
    // docs/RADIANCE_CASCADES.md) so regressions trip them but float noise does not.
    const std::array<Case, 2> cases{{
        {rc::SyntheticScene::Room, {0.40, 0.23, 0.41}},
        {rc::SyntheticScene::OccluderShadow, {0.43, 0.24, 0.43}},
    }};
    for (const Case& c : cases) {
        const rc::Grid2D grid = scene(c.kind, n);
        const rc::FluenceImage reference = brute(grid, 1024);
        std::array<double, 3> rmse{};
        for (std::size_t m = 0; m < kModes.size(); ++m) {
            const rc::ImageError error = rc::compare_fluence(grid, solve(grid, kModes[m]), reference);
            rmse[m] = error.relativeRmse;
            std::cout << "  scene " << static_cast<int>(c.kind) << ' ' << rc::merge_mode_name(kModes[m])
                      << ": rel RMSE " << error.relativeRmse << ", rel max " << error.relativeMax << '\n';
            CHECK(error.comparedPixels > n * n / 2U);
            CHECK(error.relativeRmse < c.maxRelativeRmse[m]);
        }
        CHECK(rmse[1] < rmse[0]);  // the bilinear fix is the most accurate merge
    }
}

void test_thin_wall_leak() {
    constexpr std::uint32_t n = 128;
    const rc::Grid2D grid = scene(rc::SyntheticScene::ThinWall, n);
    auto leak = [&](const rc::FluenceImage& image) {
        double left = 0.0, right = 0.0;
        std::uint64_t nl = 0, nr = 0;
        for (std::uint32_t y = 0; y < n; ++y)
            for (std::uint32_t x = 0; x < n; ++x) {
                if (grid.is_opaque(x, y)) continue;
                const double v = at(grid, image, x, y);
                if (x < n / 2) { left += v; ++nl; } else { right += v; ++nr; }
            }
        return (right / static_cast<double>(nr)) / (left / static_cast<double>(nl));
    };
    CHECK(leak(brute(grid, 256)) == 0.0);
    const double vanilla = leak(solve(grid, rc::MergeMode::Vanilla));
    const double parallax = leak(solve(grid, rc::MergeMode::ParallaxFix));
    std::cout << "  thin wall leak ratio: vanilla " << vanilla << ", parallax-fix " << parallax << '\n';
    // Vanilla/parallax leak through a 1-voxel wall because the cascade-i interval ends before the
    // wall while the interpolated cascade-(i+1) probes sit behind it. Bounded, not zero.
    CHECK(vanilla < 0.03 && parallax < 0.03);
    // Either fix removes the leak in this scene: the bilinear fix traces to the upper probes, and
    // the interval-overlap extension makes every interval reach past the gap.
    CHECK(leak(solve(grid, rc::MergeMode::BilinearFix)) < 1e-4);
    CHECK(leak(solve(grid, rc::MergeMode::Vanilla, 1.0F)) < 1e-4);
    CHECK(leak(solve(grid, rc::MergeMode::ParallaxFix, 1.0F)) < 1e-4);
}

void test_determinism() {
    const rc::Grid2D grid = scene(rc::SyntheticScene::Room, 96);
    for (const rc::MergeMode mode : kModes) {
        const rc::FluenceImage single = solve(grid, mode, 0.0F, 1);
        CHECK(bitwise_equal(single, solve(grid, mode, 0.0F, 7)));
        CHECK(bitwise_equal(single, solve(grid, mode, 0.0F, 3)));
        CHECK(bitwise_equal(single, solve(grid, mode, 0.0F, 1)));
    }
    CHECK(bitwise_equal(brute(grid, 128, 1), brute(grid, 128, 5)));
}

void test_cascade_count_invariance() {
    const rc::Grid2D grid = scene(rc::SyntheticScene::Room, 96);
    const std::uint32_t count = rc::auto_cascade_count(rc::CascadeConfig{}, 96, 96);
    const rc::FluenceImage reference = brute(grid, 512);
    for (const rc::MergeMode mode : kModes) {
        const rc::FluenceImage automatic = solve(grid, mode);
        if (mode != rc::MergeMode::BilinearFix) {
            // Cascades whose intervals start beyond the grid diagonal add nothing.
            CHECK(max_difference(automatic, solve(grid, mode, 0.0F, kTestThreads, count + 1U)) < 1e-6);
            CHECK(max_difference(automatic, solve(grid, mode, 0.0F, kTestThreads, count + 2U)) < 1e-6);
        } else {
            // With the bilinear fix the former top level now traces towards the (off-grid) upper
            // probes instead of straight out, so rays bend slightly: nearly, not exactly, equal.
            for (const std::uint32_t extra : {1U, 2U}) {
                const double drift = rc::compare_fluence(
                    grid, solve(grid, mode, 0.0F, kTestThreads, count + extra), automatic).relativeRmse;
                std::cout << "  bilinear-fix +" << extra << " cascades: rel RMSE vs auto " << drift << '\n';
                CHECK(drift < 0.02);
            }
        }
        // One cascade fewer is still a valid (extended top) solution, just less accurate.
        const double fewer = rc::compare_fluence(grid, solve(grid, mode, 0.0F, kTestThreads, count - 1U), reference).relativeRmse;
        std::cout << "  " << rc::merge_mode_name(mode) << " with " << count - 1U << " cascades: rel RMSE " << fewer << '\n';
        CHECK(std::isfinite(fewer) && fewer < 1.0);
    }
}

void test_voxel_slicing_and_destruction() {
    dve::VoxelObject object(3);
    std::vector<rc::SliceMaterial> materials(3);
    materials[1] = {{0.5F, 0.5F, 0.5F}, {}};
    materials[2] = {{1.0F, 1.0F, 1.0F}, {1.0F, 0.5F, 0.25F}};
    constexpr std::int32_t n = 64;
    for (std::int32_t z = -1; z <= 1; ++z) {
        for (std::int32_t y = 0; y < n; ++y) object.set_voxel({n / 2, y, z}, 1U);  // 1-voxel wall
        for (std::int32_t y = 30; y < 34; ++y)
            for (std::int32_t x = 10; x < 14; ++x) object.set_voxel({x, y, z}, 2U);
    }
    object.set_voxel({5, 5, 0}, 9U);  // material without a table entry
    rc::SliceRequest request;
    request.normal = rc::SliceNormal::Z;
    request.plane = 0;
    request.width = n;
    request.height = n;
    const rc::Grid2D grid = rc::slice_voxel_object(object, request, materials);
    CHECK(grid.validate());
    CHECK(grid.is_opaque(n / 2, 17) && grid.is_opaque(11, 31) && !grid.is_opaque(20, 20));
    CHECK(grid.emission[grid.index(11, 31)].g == 0.5F && grid.emission[grid.index(n / 2, 3)].r == 0.0F);
    CHECK(grid.is_opaque(5, 5) && grid.emission[grid.index(5, 5)].r == 0.0F);

    // Axis mapping.
    rc::SliceRequest side = request;
    side.normal = rc::SliceNormal::X;
    side.plane = n / 2;
    side.originV = 0;
    side.originU = -1;
    side.width = 3;
    const rc::Grid2D wallFace = rc::slice_voxel_object(object, side, materials);
    CHECK(wallFace.is_opaque(0, 10) && wallFace.is_opaque(2, 10));
    CHECK(rc::slice_cell_to_voxel(side, 2, 7).x == n / 2 && rc::slice_cell_to_voxel(side, 2, 7).z == 1);
    rc::SliceRequest top = request;
    top.normal = rc::SliceNormal::Y;
    top.plane = 31;
    top.originV = -1;
    top.height = 3;
    CHECK(rc::slice_voxel_object(object, top, materials).is_opaque(11, 1));
    CHECK(rc::slice_cell_to_voxel(top, 4, 2).y == 31 && rc::slice_cell_to_voxel(top, 4, 2).z == 1);

    // The GPU-mirror CPU reference slices identically.
    dve::PackedBrickmapScene packed;
    packed.rebuild(object);
    const rc::Grid2D packedGrid = rc::slice_packed_brickmap(packed, request, materials);
    CHECK(packedGrid.opaque == grid.opaque);

    // Destruction: no precompute, so an edit is visible on the very next solve.
    rc::CascadeConfig config;
    config.mergeMode = rc::MergeMode::BilinearFix;
    config.threads = kTestThreads;
    const double before = at(grid, rc::solve_radiance_cascades(grid, config).fluence, 48, 32);
    for (std::int32_t z = -1; z <= 1; ++z)
        for (std::int32_t y = 26; y < 38; ++y) object.set_voxel({n / 2, y, z}, dve::kAirMaterial);
    const rc::Grid2D opened = rc::slice_voxel_object(object, request, materials);
    const rc::FluenceImage after = rc::solve_radiance_cascades(opened, config).fluence;
    const rc::FluenceImage afterReference = brute(opened, 1024);
    std::cout << "  destruction receiver: before " << before << ", after " << at(opened, after, 48, 32)
              << " (brute force " << at(opened, afterReference, 48, 32) << ")\n";
    CHECK(before < 1e-6);
    CHECK(at(opened, after, 48, 32) > 0.01);
    CHECK(close(at(opened, after, 48, 32), at(opened, afterReference, 48, 32),
                0.3 * at(opened, afterReference, 48, 32)));
}

} // namespace

int main() {
    try {
        test_penumbra_calculator_port();
        test_cascade_levels_and_validation();
        test_dda_segment();
        test_empty_scene_energy();
        test_single_light_falloff();
        test_occluder_shadow_and_penumbra();
        test_error_against_reference();
        test_thin_wall_leak();
        test_determinism();
        test_cascade_count_invariance();
        test_voxel_slicing_and_destruction();
        std::cout << "dve_rc2d_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dve_rc2d_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}
