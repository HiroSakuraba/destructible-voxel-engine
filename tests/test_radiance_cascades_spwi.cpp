// Radiance Cascades Phase 2: CPU SPWI (screen probes, world-space intervals) on the reference voxel
// renderer, plus the one-bounce GI sun-term fix. See docs/RADIANCE_CASCADES.md.

#include "dve/gpu_render_environment.hpp"
#include "dve/query.hpp"
#include "dve/render/radiance_cascades_spwi.hpp"
#include "dve/render/ray_lighting_reference.hpp"
#include "dve/render/voxel_lighting_plan.hpp"
#include "dve/render/voxel_reference_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x + " at " + std::to_string(__LINE__)); } while(false)

using namespace dve;
using namespace dve::render;
namespace sp = dve::render::spwi;

// Two worker threads: exercises the threaded path while leaving cores for the timing-sensitive
// audio tests that ctest runs in parallel (PROCESSORS 2 in CMake).
constexpr std::uint32_t kThreads = 2;
constexpr std::uint32_t kWidth = 128;
constexpr std::uint32_t kHeight = 72;
constexpr std::uint32_t kReferenceSamples = 512;
// Accuracy and leak tests run at the bench resolution: SPWI error depends on the probe spacing in
// world units, and at 128x72 a 2 px probe covers ~2.5x the area it does at 320x180.
constexpr std::uint32_t kAccuracyWidth = 320;
constexpr std::uint32_t kAccuracyHeight = 180;

bool same_bits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }
bool same_bits(Float3 a, Float3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

RadianceCascadeSettings settings_for(RadianceCascadeMerge merge, float overlap) {
    RadianceCascadeSettings settings;
    settings.merge = merge;
    settings.intervalOverlap = overlap;
    settings.threadCount = kThreads;
    return settings;
}

struct Fixture {
    sp::SyntheticSceneSetup setup;
    VoxelReferenceInstance instance{};
    sp::VoxelGBuffer gbuffer;

    explicit Fixture(sp::SyntheticScene scene, std::uint32_t width = kWidth, std::uint32_t height = kHeight)
        : setup(sp::make_synthetic_scene(scene)) {
        instance = setup.instance();
        const sp::VoxelSceneTracer tracer(instances());
        gbuffer = sp::build_voxel_gbuffer(tracer, setup.camera, width, height, {}, kThreads);
    }
    [[nodiscard]] std::span<const VoxelReferenceInstance> instances() const { return {&instance, 1}; }
};

// Nearest hit over instances exactly as the renderer's original trace_scene did.
std::optional<TransformedRayHit> reference_trace(std::span<const VoxelReferenceInstance> instances,
                                                 Float3 origin, Float3 direction, float maximum,
                                                 std::size_t* which) {
    std::optional<TransformedRayHit> nearest;
    float nearestDistance = maximum;
    for (std::size_t k = 0; k < instances.size(); ++k) {
        const auto hit = raycast_voxels_transformed(*instances[k].object, instances[k].transform,
                                                     origin, direction, nearestDistance);
        if (!hit || hit->objectHit.distance >= nearestDistance) continue;
        nearestDistance = hit->objectHit.distance;
        nearest = hit;
        *which = k;
    }
    return nearest;
}

// ---------------------------------------------------------------------------------------------

void test_tracer_matches_raycast_voxels() {
    sp::SyntheticSceneSetup courtyard = sp::make_synthetic_scene(sp::SyntheticScene::Courtyard);
    sp::SyntheticSceneSetup bunker = sp::make_synthetic_scene(sp::SyntheticScene::Bunker);
    std::array<VoxelReferenceInstance, 2> instances{courtyard.instance(), bunker.instance()};
    instances[1].transform.position = {7.25F, -3.5F, 41.0F};
    instances[1].transform.rotation = normalize(Quaternion{0.1F, 0.35F, -0.2F, 0.9F});
    const sp::VoxelSceneTracer dense(instances);
    const sp::VoxelSceneTracer sparse(instances, 0U); // material_at path, no snapshot
    std::uint32_t state = 12345U;
    auto next = [&] { state = state * 1664525U + 1013904223U; return static_cast<float>(state >> 8U) / 16777216.0F; };
    std::uint32_t hits = 0;
    for (int i = 0; i < 4000; ++i) {
        const Float3 origin{next() * 70.0F - 15.0F, next() * 30.0F - 5.0F, next() * 90.0F - 15.0F};
        const Float3 direction{next() * 2.0F - 1.0F, next() * 2.0F - 1.0F, next() * 2.0F - 1.0F};
        const float maximum = 5.0F + next() * 120.0F;
        std::size_t which = 0;
        const auto expected = reference_trace(instances, origin, direction, maximum, &which);
        for (const sp::VoxelSceneTracer* tracer : {&dense, &sparse}) {
            const auto actual = tracer->trace(origin, direction, maximum);
            CHECK(actual.has_value() == expected.has_value());
            if (!expected) continue;
            CHECK(actual->instance == which);
            CHECK(actual->objectHit.voxel.x == expected->objectHit.voxel.x &&
                  actual->objectHit.voxel.y == expected->objectHit.voxel.y &&
                  actual->objectHit.voxel.z == expected->objectHit.voxel.z);
            CHECK(actual->objectHit.normal.x == expected->objectHit.normal.x &&
                  actual->objectHit.normal.y == expected->objectHit.normal.y &&
                  actual->objectHit.normal.z == expected->objectHit.normal.z);
            CHECK(actual->objectHit.material == expected->objectHit.material);
            CHECK(same_bits(actual->objectHit.distance, expected->objectHit.distance));
            CHECK(same_bits(actual->worldPosition, expected->worldPosition));
            CHECK(same_bits(actual->worldNormal, expected->worldNormal));
        }
        if (expected) ++hits;
    }
    CHECK(hits > 500U); // the random rays genuinely exercise hits and misses
    CHECK(dense.dense_bytes() > 0U && sparse.dense_bytes() == 0U);
    std::cout << "  tracer: 4000 rays, " << hits << " hits, bit-identical to raycast_voxels_transformed\n";
}

void test_gbuffer_matches_renderer() {
    Fixture fixture(sp::SyntheticScene::Courtyard, 96, 54);
    RenderEnvironment environment = fixture.setup.environment;
    environment.globalIlluminationMode = GlobalIlluminationMode::Off;
    PolygonRenderTarget target;
    target.resize(96, 54);
    const auto stats = ReferenceVoxelRenderer{}.render(fixture.instances(), fixture.setup.camera, environment, target);
    std::uint64_t valid = 0;
    for (std::size_t i = 0; i < target.depth.size(); ++i) {
        const auto& texel = fixture.gbuffer.texels[i];
        CHECK(texel.valid == (target.depth[i] < 1.0F));
        if (!texel.valid) continue;
        ++valid;
        CHECK(same_bits(texel.normalizedDepth, target.depth[i]));
        CHECK(texel.material == target.materialIndex[i]);
    }
    std::cout << "  gbuffer: " << valid << " hits (renderer " << stats.hitRays << ")\n";
    CHECK(valid == stats.hitRays && valid > 1000U);
}

void test_octahedral_maps_and_layout() {
    for (const std::uint32_t resolution : {2U, 4U, 8U, 32U}) {
        const auto solidAngles = sp::octahedral_texel_solid_angles(resolution);
        double sum = 0.0;
        for (const float value : solidAngles) {
            CHECK(value > 0.0F);
            sum += value;
        }
        CHECK(std::abs(sum - 4.0 * 3.14159265358979) < 4.0e-3 * 4.0 * 3.14159265358979);
    }
    // Children (2x2 at double resolution) stay within their parent's cone.
    for (std::uint32_t ty = 0; ty < 8U; ++ty)
        for (std::uint32_t tx = 0; tx < 8U; ++tx) {
            const Float3 parent = sp::octahedral_texel_direction(tx, ty, 8U);
            CHECK(std::abs(length(parent) - 1.0F) < 1.0e-5F);
            for (std::uint32_t c = 0; c < 4U; ++c) {
                const Float3 child = sp::octahedral_texel_direction(2U * tx + (c & 1U), 2U * ty + (c >> 1U), 16U);
                CHECK(dot(parent, child) > 0.85F);
            }
        }

    RenderEnvironment environment;
    // 4.8 m at the default 0.1 m/voxel = 48 voxels; interval bounds are in voxels.
    environment.globalIlluminationMaxDistanceMeters = 4.8F;
    RadianceCascadeSettings settings;
    const auto levels = sp::describe_cascades(settings, environment, 320U, 180U);
    CHECK(levels.size() == 4U); // [0,1) [1,5) [5,21) [21,48]
    const std::array<float, 5> bounds{0.0F, 1.0F, 5.0F, 21.0F, 48.0F};
    for (std::size_t i = 0; i < levels.size(); ++i) {
        CHECK(levels[i].probeSpacingPixels == 2U << i);
        CHECK(levels[i].directionResolution == 8U << i);
        CHECK(levels[i].intervalStart == bounds[i] && levels[i].intervalEnd == bounds[i + 1U]);
        // Constant memory per level (up to ceil() at the screen edge).
        const double ratio = static_cast<double>(levels[i].texels) / static_cast<double>(levels[0].texels);
        CHECK(ratio >= 1.0 && ratio < 1.12);
    }
    // The paper's x2 intervals need more levels; the screen caps them (>= 2 probes per axis).
    settings.intervalGrowth = 2.0F;
    const auto paper = sp::describe_cascades(settings, environment, 320U, 180U);
    CHECK(paper.size() == 6U && paper.back().probeSpacingPixels == 64U && paper.back().intervalEnd == 48.0F);

    std::string error;
    RadianceCascadeSettings bad;
    bad.baseDirectionResolution = 3U;
    CHECK(!bad.validate(&error) && !error.empty());
    bad = {};
    bad.intervalGrowth = 0.5F;
    CHECK(!bad.validate());
    bad = {};
    bad.baseProbeSpacingPixels = 0U;
    CHECK(!bad.validate());
    CHECK(RadianceCascadeSettings{}.validate());
    // Decision (docs/RADIANCE_CASCADES.md §6): the bilinear fix stays the default merge.
    CHECK(RadianceCascadeSettings{}.merge == RadianceCascadeMerge::BilinearFix);
    CHECK(ReferenceVoxelRenderer{}.radianceCascades.merge == RadianceCascadeMerge::BilinearFix);
}

// The CPU one-bounce GI used only the hemisphere ambient at bounce hits; resolve_gi.hlsl also adds
// sun light with an explicit visibility ray. A wall facing away from the sun sees a sunlit floor.
void test_bounce_includes_sun_like_gpu() {
    VoxelObject object(501U);
    for (int z = -30; z <= 30; ++z)
        for (int x = -30; x <= 30; ++x) (void)object.set_voxel({x, 0, z}, 1U);
    for (int z = -6; z <= 6; ++z)
        for (int y = 1; y <= 8; ++y) (void)object.set_voxel({0, y, z}, 1U);
    std::array<VoxelMaterialDefinition, 2> materials{};
    materials[1].baseColor = {0.8F, 0.8F, 0.8F, 1.0F};
    const VoxelReferenceInstance instance{501U, &object, {}, materials, true};
    PolygonCamera camera;
    camera.position = {8.0F, 4.5F, 0.5F};
    camera.target = {1.0F, 4.5F, 0.5F};
    camera.nearPlane = 0.1F;
    camera.farPlane = 100.0F;
    RenderEnvironment environment;
    environment.sunDirection = {0.0F, 1.0F, 0.0F}; // straight down: the wall gets no direct sun
    environment.sunIntensity = 3.0F;
    environment.skyColor = {0.0F, 0.0F, 0.0F};
    environment.groundColor = {0.0F, 0.0F, 0.0F};
    environment.shadowMode = ShadowMode::Hard;
    environment.globalIlluminationSamples = 16U;
    environment.globalIlluminationIntensity = 1.0F;
    environment.globalIlluminationMaxDistanceMeters = 4.0F; // 40 voxels at 0.1 m/voxel
    PolygonRenderTarget target;
    target.resize(9, 9);
    const auto stats = ReferenceVoxelRenderer{}.render({&instance, 1}, camera, environment, target);
    const std::size_t centre = 4U * 9U + 4U;
    CHECK(target.objectId[centre] == 501U);
    // Expected: floor hits carry 0.8·3/π; roughly half of the cosine lobe of a wall sees floor.
    const float floorRadiance = 0.8F * 3.0F / 3.14159265F;
    const float wallIndirect = target.hdrColor[centre].x / 0.8F;
    CHECK(wallIndirect > 0.25F * floorRadiance && wallIndirect < 0.75F * floorRadiance);
    CHECK(stats.globalIlluminationSunRays > 0U && stats.globalIlluminationSunRays <= stats.globalIlluminationHits);

    // bounce_hit_radiance is one_bounce_diffuse_radiance with a real visibility ray.
    const sp::VoxelSceneTracer tracer({&instance, 1});
    const auto hit = tracer.trace({5.0F, 3.0F, 20.0F}, {0.0F, -1.0F, 0.0F}, 10.0F);
    CHECK(hit.has_value());
    const Float3 lit = sp::bounce_hit_radiance(tracer, *hit, environment);
    const Float3 expected = one_bounce_diffuse_radiance({0.8F, 0.8F, 0.8F}, 0.0F, {}, {}, environment.sunColor,
                                                        environment.sunIntensity, hit->worldNormal,
                                                        environment.sunDirection, 1.0F);
    CHECK(same_bits(lit, expected));

    // A roof over everything removes the sun from every bounce: indirect goes to exactly zero.
    for (int z = -30; z <= 30; ++z)
        for (int x = -30; x <= 30; ++x) (void)object.set_voxel({x, 20, z}, 1U);
    PolygonRenderTarget roofed;
    roofed.resize(9, 9);
    (void)ReferenceVoxelRenderer{}.render({&instance, 1}, camera, environment, roofed);
    CHECK(roofed.hdrColor[centre].x == 0.0F);
    std::cout << "  bounce: wall indirect " << wallIndirect << " (floor radiance " << floorRadiance
              << "), roofed 0\n";
}

// Energy / empty-scene sanity: under a constant environment every direction carries the same
// radiance, so the cosine-weighted, renormalised gather must return it exactly; with the default
// sky/ground gradient a bare floor must match the analytic cosine average (t̄ = 5/6).
void test_open_floor_energy() {
    VoxelObject object(502U);
    for (int z = -40; z <= 40; ++z)
        for (int x = -40; x <= 40; ++x) (void)object.set_voxel({x, 0, z}, 1U);
    std::array<VoxelMaterialDefinition, 2> materials{};
    materials[1].baseColor = {0.5F, 0.5F, 0.5F, 1.0F};
    const VoxelReferenceInstance instance{502U, &object, {}, materials, true};
    PolygonCamera camera;
    camera.position = {0.0F, 14.0F, 12.0F};
    camera.target = {0.0F, 0.0F, 0.0F};
    camera.nearPlane = 0.1F;
    camera.farPlane = 100.0F;
    RenderEnvironment environment;
    environment.sunIntensity = 0.0F;
    environment.globalIlluminationIntensity = 1.0F;
    environment.globalIlluminationMaxDistanceMeters = 4.8F; // 48 voxels at 0.1 m/voxel
    const sp::VoxelSceneTracer tracer({&instance, 1});
    const auto gbuffer = sp::build_voxel_gbuffer(tracer, camera, 64, 36, {}, kThreads);

    environment.skyColor = {0.3F, 0.3F, 0.3F};
    environment.groundColor = {0.3F, 0.3F, 0.3F};
    for (const auto merge : {RadianceCascadeMerge::Vanilla, RadianceCascadeMerge::BilinearFix}) {
        const auto rc = sp::solve_radiance_cascades(tracer, gbuffer, environment, settings_for(merge, 1.0F));
        for (std::size_t i = 0; i < gbuffer.texels.size(); ++i) {
            if (!gbuffer.texels[i].valid) continue;
            CHECK(std::abs(rc.indirect[i].x - 0.3F) < 1.0e-4F);
        }
    }

    environment.skyColor = {0.40F, 0.50F, 0.60F};
    environment.groundColor = {0.10F, 0.10F, 0.10F};
    const Float3 analytic{0.10F + (0.40F - 0.10F) * 5.0F / 6.0F, 0.10F + (0.50F - 0.10F) * 5.0F / 6.0F,
                          0.10F + (0.60F - 0.10F) * 5.0F / 6.0F};
    const auto bf = sp::solve_brute_force_indirect(tracer, gbuffer, environment, 1024U, kThreads);
    const auto rc = sp::solve_radiance_cascades(tracer, gbuffer, environment, settings_for(RadianceCascadeMerge::BilinearFix, 1.0F));
    double worstBf = 0.0, worstRc = 0.0;
    std::uint64_t floorPixels = 0;
    for (std::size_t i = 0; i < gbuffer.texels.size(); ++i) {
        const auto& texel = gbuffer.texels[i];
        // Stay away from the floor's outer edge, where the ground plane ends.
        if (!texel.valid || std::abs(texel.position.x) > 30.0F || std::abs(texel.position.z) > 30.0F) continue;
        ++floorPixels;
        worstBf = std::max(worstBf, static_cast<double>(std::abs(sp::luminance(bf.indirect[i]) - sp::luminance(analytic))));
        worstRc = std::max(worstRc, static_cast<double>(std::abs(sp::luminance(rc.indirect[i]) - sp::luminance(analytic))));
    }
    const double scale = sp::luminance(analytic);
    CHECK(floorPixels > 1000U);
    CHECK(worstBf < 0.01 * scale);
    CHECK(worstRc < 0.02 * scale);
    std::cout << "  open floor: max |bf-analytic| " << worstBf / scale << ", max |rc-analytic| " << worstRc / scale
              << " (relative)\n";
}

void test_accuracy_against_brute_force() {
    struct Case { sp::SyntheticScene scene; const char* name; };
    struct Threshold { RadianceCascadeMerge merge; float overlap; const char* name; double relRmse; };
    // Calibrated at 320x180 against a 512-spp reference with ~30% headroom; MC16 is the existing VoxelOneBounce budget at its 16-sample cap.
    const std::array<Case, 2> cases{{{sp::SyntheticScene::Courtyard, "courtyard"}, {sp::SyntheticScene::Bunker, "bunker"}}};
    const std::array<std::array<Threshold, 4>, 2> thresholds{{
        {{{RadianceCascadeMerge::Vanilla, 0.0F, "vanilla", 0.10},
          {RadianceCascadeMerge::Vanilla, 1.0F, "vanilla+overlap", 0.06},
          {RadianceCascadeMerge::BilinearFix, 0.0F, "bilinear", 0.09},
          {RadianceCascadeMerge::BilinearFix, 1.0F, "bilinear+overlap", 0.07}}},
        {{{RadianceCascadeMerge::Vanilla, 0.0F, "vanilla", 0.025},
          {RadianceCascadeMerge::Vanilla, 1.0F, "vanilla+overlap", 0.025},
          {RadianceCascadeMerge::BilinearFix, 0.0F, "bilinear", 0.025},
          {RadianceCascadeMerge::BilinearFix, 1.0F, "bilinear+overlap", 0.025}}}}};
    for (std::size_t c = 0; c < cases.size(); ++c) {
        Fixture fixture(cases[c].scene, kAccuracyWidth, kAccuracyHeight);
        const sp::VoxelSceneTracer tracer(fixture.instances());
        const auto& environment = fixture.setup.environment;
        const auto reference = sp::solve_brute_force_indirect(tracer, fixture.gbuffer, environment, kReferenceSamples, kThreads);
        const auto mc16 = sp::solve_brute_force_indirect(tracer, fixture.gbuffer, environment, 16U, kThreads);
        const auto mcError = sp::compare_indirect(mc16, reference, fixture.gbuffer);
        std::cout << "  " << cases[c].name << ": MC16 rel RMSE " << mcError.relativeRmse;
        for (const Threshold& t : thresholds[c]) {
            const auto rc = sp::solve_radiance_cascades(tracer, fixture.gbuffer, environment, settings_for(t.merge, t.overlap));
            const auto error = sp::compare_indirect(rc, reference, fixture.gbuffer);
            std::cout << ", " << t.name << " " << error.relativeRmse << " (max " << error.relativeMax << ")";
            CHECK(error.pixels > 1500U);
            CHECK(error.relativeRmse < t.relRmse);
            if (t.merge == RadianceCascadeMerge::BilinearFix && t.overlap > 0.0F)
                CHECK(error.relativeRmse < mcError.relativeRmse); // the default beats the 16-spp GI cap
            CHECK(error.relativeMax < 1.2);
            CHECK(std::abs(error.meanBias) < 0.05 * error.meanReference);
        }
        std::cout << '\n';
    }
}

double thin_wall_leak(const Fixture& fixture, const sp::IndirectResult& test, const sp::IndirectResult& reference) {
    double left = 0.0, right = 0.0;
    std::uint64_t nl = 0, nr = 0;
    const float wall = static_cast<float>(fixture.setup.wallX);
    for (std::size_t i = 0; i < fixture.gbuffer.texels.size(); ++i) {
        const auto& t = fixture.gbuffer.texels[i];
        if (!t.valid || t.normal.y < 0.5F || t.position.y > 1.5F) continue; // floor
        if (t.position.x < wall) { left += sp::luminance(reference.indirect[i]); ++nl; }
        else if (t.position.x > wall + 1.0F) {
            right += sp::luminance(test.indirect[i]);
            ++nr;
            CHECK(sp::luminance(reference.indirect[i]) == 0.0F); // nothing on the right sees the lamp
        }
    }
    CHECK(nl > 500U && nr > 500U && left > 0.0);
    return (right / static_cast<double>(nr)) / (left / static_cast<double>(nl));
}

void test_thin_wall_leak() {
    // A 1-voxel wall (0.1 m at the engine's 0.1 m/voxel) seen from above: screen neighbours on the
    // two floors are coplanar, so bilateral weights alone cannot separate them.
    Fixture fixture(sp::SyntheticScene::ThinWall, kAccuracyWidth, kAccuracyHeight);
    const sp::VoxelSceneTracer tracer(fixture.instances());
    const auto& environment = fixture.setup.environment;
    const auto reference = sp::solve_brute_force_indirect(tracer, fixture.gbuffer, environment, 256U, kThreads);
    auto leak = [&](RadianceCascadeMerge merge, float overlap, float growth) {
        RadianceCascadeSettings settings = settings_for(merge, overlap);
        settings.intervalGrowth = growth;
        return thin_wall_leak(fixture, sp::solve_radiance_cascades(tracer, fixture.gbuffer, environment, settings), reference);
    };
    const double vanilla = leak(RadianceCascadeMerge::Vanilla, 0.0F, 2.0F);
    const double vanillaDefault = leak(RadianceCascadeMerge::Vanilla, 1.0F, 4.0F);
    const double bilinearPaper = leak(RadianceCascadeMerge::BilinearFix, 0.0F, 2.0F);
    const double bilinearDefault = leak(RadianceCascadeMerge::BilinearFix, 1.0F, 4.0F);
    std::cout << "  thin wall leak (right/left): vanilla x2 " << vanilla << ", vanilla+overlap " << vanillaDefault
              << ", bilinear x2 " << bilinearPaper << ", bilinear+overlap " << bilinearDefault << '\n';
    CHECK(vanilla > 0.01);           // the scene does provoke the classic SPWI leak...
    CHECK(vanillaDefault < 0.002);   // ...overlap + x4 intervals nearly close it...
    CHECK(bilinearPaper < 5.0e-4);   // ...and rays to the upper probes see the wall.
    CHECK(bilinearDefault < 1.0e-4);
}

void test_determinism() {
    Fixture fixture(sp::SyntheticScene::Courtyard, 64, 36);
    const sp::VoxelSceneTracer tracer(fixture.instances());
    for (const auto merge : {RadianceCascadeMerge::Vanilla, RadianceCascadeMerge::BilinearFix}) {
        RadianceCascadeSettings settings = settings_for(merge, 1.0F);
        settings.threadCount = 1U;
        const auto one = sp::solve_radiance_cascades(tracer, fixture.gbuffer, fixture.setup.environment, settings);
        for (const std::uint32_t threads : {2U, 3U, 2U}) {
            settings.threadCount = threads;
            const auto other = sp::solve_radiance_cascades(tracer, fixture.gbuffer, fixture.setup.environment, settings);
            CHECK(other.stats.intervalRays == one.stats.intervalRays);
            for (std::size_t i = 0; i < one.indirect.size(); ++i) CHECK(same_bits(one.indirect[i], other.indirect[i]));
        }
    }
    const auto a = sp::solve_brute_force_indirect(tracer, fixture.gbuffer, fixture.setup.environment, 64U, 1U);
    const auto b = sp::solve_brute_force_indirect(tracer, fixture.gbuffer, fixture.setup.environment, 64U, 3U);
    for (std::size_t i = 0; i < a.indirect.size(); ++i) CHECK(same_bits(a.indirect[i], b.indirect[i]));
}

void test_destruction_updates_immediately() {
    sp::SyntheticSceneSetup setup = sp::make_synthetic_scene(sp::SyntheticScene::Bunker);
    const VoxelReferenceInstance instance = setup.instance();
    RenderEnvironment environment = setup.environment;
    environment.globalIlluminationMode = GlobalIlluminationMode::RadianceCascades;
    ReferenceVoxelRenderer renderer;
    renderer.radianceCascades = settings_for(RadianceCascadeMerge::BilinearFix, 1.0F);
    std::vector<sp::IndirectError> roomErrors;
    auto frame = [&](double* receiverRc, double* receiverBf, sp::IndirectError* error) {
        const sp::VoxelSceneTracer tracer({&instance, 1});
        const auto gbuffer = sp::build_voxel_gbuffer(tracer, setup.camera, kAccuracyWidth, kAccuracyHeight, {}, kThreads);
        const auto rc = sp::solve_radiance_cascades(tracer, gbuffer, setup.environment, renderer.radianceCascades);
        const auto bf = sp::solve_brute_force_indirect(tracer, gbuffer, setup.environment, 256U, kThreads);
        double sumRc = 0.0, sumBf = 0.0;
        std::uint64_t count = 0;
        for (std::size_t i = 0; i < gbuffer.texels.size(); ++i) {
            const auto& t = gbuffer.texels[i];
            if (!t.valid || t.normal.y < 0.5F || t.position.y > 1.5F || t.position.x < 25.0F || t.position.x > 31.0F) continue;
            sumRc += sp::luminance(rc.indirect[i]);
            sumBf += sp::luminance(bf.indirect[i]);
            ++count;
        }
        CHECK(count > 200U);
        *receiverRc = sumRc / static_cast<double>(count);
        *receiverBf = sumBf / static_cast<double>(count);
        *error = sp::compare_indirect(rc, bf, gbuffer);
        // Room interior only (excludes the yard seen through the opening). After the edit the
        // residual error concentrates on the 1-voxel jamb faces and the wall base beside the new
        // sunlit patch: short-range light on geometry narrower than a level-0 probe footprint.
        std::vector<std::uint8_t> room(gbuffer.texels.size(), 0U);
        for (std::size_t i = 0; i < room.size(); ++i)
            room[i] = gbuffer.texels[i].valid && gbuffer.texels[i].position.x > 24.0F ? 1U : 0U;
        roomErrors.push_back(sp::compare_indirect(rc, bf, gbuffer, room));
        PolygonRenderTarget target;
        target.resize(kAccuracyWidth, kAccuracyHeight);
        (void)renderer.render({&instance, 1}, setup.camera, environment, target);
        return target;
    };
    double beforeRc = 0.0, beforeBf = 0.0, afterRc = 0.0, afterBf = 0.0;
    sp::IndirectError beforeError, afterError;
    const PolygonRenderTarget before = frame(&beforeRc, &beforeBf, &beforeError);
    const std::uint32_t removed = sp::remove_box(*setup.object, setup.removableMinimum, setup.removableMaximum);
    CHECK(removed == 54U);
    const PolygonRenderTarget after = frame(&afterRc, &afterBf, &afterError);
    std::cout << "  destruction: receiver RC " << beforeRc << " -> " << afterRc << " (brute force " << beforeBf
              << " -> " << afterBf << "), rel RMSE " << beforeError.relativeRmse << " -> " << afterError.relativeRmse << '\n';
    CHECK(afterBf > 1.1 * beforeBf);                       // the new opening really matters
    CHECK(std::abs(beforeRc - beforeBf) < 0.05 * beforeBf); // and RC tracks it on the very next solve
    CHECK(std::abs(afterRc - afterBf) < 0.05 * afterBf);
    std::cout << "  destruction: room-only rel RMSE " << roomErrors[0].relativeRmse << " -> " << roomErrors[1].relativeRmse << '\n';
    CHECK(beforeError.relativeRmse < 0.03 && afterError.relativeRmse < 0.16);
    CHECK(roomErrors[0].relativeRmse < 0.03 && roomErrors[1].relativeRmse < 0.15);
    std::uint64_t changed = 0;
    for (std::size_t i = 0; i < before.hdrColor.size(); ++i)
        if (std::abs(before.hdrColor[i].x - after.hdrColor[i].x) > 1.0e-3F) ++changed;
    CHECK(changed > 2000U);
}

void test_mode_switch() {
    RenderEnvironment environment;
    environment.globalIlluminationMode = GlobalIlluminationMode::RadianceCascades;
    std::string error;
    CHECK(environment.validate(&error));
    // GPU backends do not execute it yet: packing and the default plan retain one bounce.
    CHECK(pack_gpu_render_environment(environment, 64U, 64U, 0.1F).globalIlluminationMode ==
          static_cast<std::uint32_t>(GlobalIlluminationMode::VoxelOneBounce));
    RenderEnvironment oneBounce = environment;
    oneBounce.globalIlluminationMode = GlobalIlluminationMode::VoxelOneBounce;
    const auto rcPlan = make_voxel_lighting_frame_plan(64U, 32U, environment);
    const auto obPlan = make_voxel_lighting_frame_plan(64U, 32U, oneBounce);
    CHECK(rcPlan.validate(&error) && rcPlan.dispatches.size() == obPlan.dispatches.size());
    CHECK(rcPlan.radianceCascadeLevels.empty() &&
          rcPlan.activeGlobalIlluminationRayCount == obPlan.activeGlobalIlluminationRayCount);

    Fixture fixture(sp::SyntheticScene::Courtyard, 64, 36);
    RenderEnvironment rcEnvironment = fixture.setup.environment;
    rcEnvironment.globalIlluminationMode = GlobalIlluminationMode::RadianceCascades;
    RenderEnvironment obEnvironment = fixture.setup.environment;
    obEnvironment.globalIlluminationMode = GlobalIlluminationMode::VoxelOneBounce;
    RenderEnvironment offEnvironment = fixture.setup.environment;
    offEnvironment.globalIlluminationMode = GlobalIlluminationMode::Off;
    ReferenceVoxelRenderer renderer;
    renderer.radianceCascades = settings_for(RadianceCascadeMerge::BilinearFix, 1.0F);
    PolygonRenderTarget rcTarget, obTarget, offTarget;
    rcTarget.resize(64, 36);
    obTarget.resize(64, 36);
    offTarget.resize(64, 36);
    const auto rcStats = renderer.render(fixture.instances(), fixture.setup.camera, rcEnvironment, rcTarget);
    const auto obStats = renderer.render(fixture.instances(), fixture.setup.camera, obEnvironment, obTarget);
    const auto offStats = renderer.render(fixture.instances(), fixture.setup.camera, offEnvironment, offTarget);
    CHECK(rcStats.radianceCascadeIntervalRays > 0U && rcStats.globalIlluminationRays == 0U);
    CHECK(obStats.radianceCascadeIntervalRays == 0U && obStats.globalIlluminationRays > 0U);
    CHECK(rcStats.hitRays == offStats.hitRays && rcStats.shadowRays == offStats.shadowRays);
    // RC mode shades exactly like the other modes, only with the SPWI indirect term.
    const sp::VoxelSceneTracer tracer(fixture.instances());
    const auto rc = sp::solve_radiance_cascades(tracer, fixture.gbuffer, fixture.setup.environment, renderer.radianceCascades);
    for (std::size_t i = 0; i < rcTarget.hdrColor.size(); ++i) {
        CHECK(rcTarget.depth[i] == offTarget.depth[i] && rcTarget.objectId[i] == offTarget.objectId[i]);
        if (!fixture.gbuffer.texels[i].valid) continue;
        const Float4 base = fixture.setup.materials[fixture.gbuffer.texels[i].material].baseColor;
        CHECK(std::abs(rcTarget.hdrColor[i].x - (offTarget.hdrColor[i].x + base.x * rc.indirect[i].x)) < 1.0e-5F);
        CHECK(std::abs(rcTarget.hdrColor[i].z - (offTarget.hdrColor[i].z + base.z * rc.indirect[i].z)) < 1.0e-5F);
    }
}

// Every RenderEnvironment `*Meters` distance is metres on the CPU too: the reference renderer and
// the SPWI solvers divide by metersPerVoxel (default 0.1 m), matching the GPU's MetersToVoxelUnits.
void test_meters_are_converted_with_voxel_size() {
    const RenderEnvironment defaults;
    const VoxelLightingDistances d = voxel_lighting_distances(defaults);
    CHECK(d.metersPerVoxel == kDefaultMetersPerVoxel && kDefaultMetersPerVoxel == 0.10F);
    CHECK(std::abs(d.globalIlluminationMaxDistance - 120.0F) < 1.0e-3F); // 12 m
    CHECK(std::abs(d.shadowMaxDistance - 25000.0F) < 1.0e-1F);           // 2500 m
    CHECK(std::abs(d.contactShadowDistance - 20.0F) < 1.0e-4F);          // 2 m
    CHECK(std::abs(d.shadowBias - 0.15F) < 1.0e-6F);                     // 0.015 m
    CHECK(std::abs(d.subsurfaceMaxDistance - 5.0F) < 1.0e-5F);           // 0.5 m
    // Same conversion as the GPU packer's metersPerVoxel and the shader-side helper.
    CHECK(pack_gpu_render_environment(defaults, 1U, 1U, 0.25F).metersPerVoxel == 0.25F);
    CHECK(voxel_lighting_distances(defaults, 0.25F).globalIlluminationMaxDistance ==
          meters_to_voxel_units(defaults.globalIlluminationMaxDistanceMeters, 0.25F));
    for (const float invalid : {0.0F, -1.0F, std::nanf(""), INFINITY}) {
        CHECK(voxel_lighting_distances(defaults, invalid).metersPerVoxel == kDefaultMetersPerVoxel);
        CHECK(pack_gpu_render_environment(defaults, 1U, 1U, invalid).metersPerVoxel == kDefaultMetersPerVoxel);
    }

    // Cascade layout: the top interval ends at the GI max distance in voxels.
    RenderEnvironment environment;
    environment.globalIlluminationMaxDistanceMeters = 4.8F;
    const RadianceCascadeSettings settings;
    const auto coarse = sp::describe_cascades(settings, environment, 320U, 180U, 1.0F, 0.4F);
    CHECK(coarse.size() == 3U && std::abs(coarse.back().intervalEnd - 12.0F) < 1.0e-4F); // [0,1) [1,5) [5,12]

    // Renderer: floor at y = 0 and a roof whose underside is 9 voxels above the floor top, sun
    // straight up. The same 0.5 m contact distance / GI reach is 5 voxels at 0.1 m/voxel (roof
    // out of reach) and 20 voxels at 0.025 m/voxel (roof in reach).
    VoxelObject object(503U);
    for (int z = -20; z <= 20; ++z)
        for (int x = -20; x <= 20; ++x) {
            (void)object.set_voxel({x, 0, z}, 1U);
            (void)object.set_voxel({x, 10, z}, 1U);
        }
    std::array<VoxelMaterialDefinition, 2> materials{};
    materials[1].baseColor = {0.5F, 0.5F, 0.5F, 1.0F};
    const VoxelReferenceInstance instance{503U, &object, {}, materials, true};
    PolygonCamera camera;
    camera.position = {0.5F, 8.0F, 0.5F};
    camera.target = {0.5F, 0.0F, 0.5F};
    camera.up = {0.0F, 0.0F, -1.0F};
    camera.nearPlane = 0.1F;
    camera.farPlane = 50.0F;
    environment = {};
    environment.sunDirection = {0.0F, 1.0F, 0.0F};
    environment.sunIntensity = 2.0F;
    environment.sunColor = {1.0F, 1.0F, 1.0F};
    environment.shadowMode = ShadowMode::Contact;
    environment.shadowStrength = 1.0F;
    environment.contactShadowDistanceMeters = 0.5F;
    environment.globalIlluminationMode = GlobalIlluminationMode::Off;
    auto centre_red = [&](float metersPerVoxel, const RenderEnvironment& env, VoxelReferenceRenderStats* stats) {
        ReferenceVoxelRenderer renderer;
        renderer.metersPerVoxel = metersPerVoxel;
        PolygonRenderTarget target;
        target.resize(9, 9);
        const auto s = renderer.render({&instance, 1}, camera, env, target);
        if (stats) *stats = s;
        CHECK(target.objectId[4U * 9U + 4U] == 503U);
        return target.hdrColor[4U * 9U + 4U].x;
    };
    CHECK(std::abs(centre_red(0.10F, environment, nullptr) - 1.0F) < 1.0e-6F); // 5 voxels: lit
    CHECK(centre_red(0.025F, environment, nullptr) == 0.0F);                   // 20 voxels: shadowed
    CHECK(centre_red(0.0F, environment, nullptr) == centre_red(0.10F, environment, nullptr)); // fallback

    environment.shadowMode = ShadowMode::Off;
    environment.globalIlluminationMode = GlobalIlluminationMode::VoxelOneBounce;
    environment.globalIlluminationMaxDistanceMeters = 0.5F;
    environment.globalIlluminationSamples = 16U;
    VoxelReferenceRenderStats nearStats, farStats;
    (void)centre_red(0.10F, environment, &nearStats);
    (void)centre_red(0.025F, environment, &farStats);
    CHECK(nearStats.globalIlluminationRays > 0U && nearStats.globalIlluminationHits == 0U);
    CHECK(farStats.globalIlluminationHits > 0U);
    std::cout << "  units: 0.5 m contact/GI = 5 voxels at 0.1 m (roof missed), 20 voxels at 0.025 m ("
              << farStats.globalIlluminationHits << " GI hits)\n";
}

} // namespace

int main() {
    try {
        test_tracer_matches_raycast_voxels();
        test_gbuffer_matches_renderer();
        test_octahedral_maps_and_layout();
        test_bounce_includes_sun_like_gpu();
        test_open_floor_energy();
        test_accuracy_against_brute_force();
        test_thin_wall_leak();
        test_determinism();
        test_destruction_updates_immediately();
        test_mode_switch();
        test_meters_are_converted_with_voxel_size();
        std::cout << "dve_rc_spwi_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dve_rc_spwi_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}
