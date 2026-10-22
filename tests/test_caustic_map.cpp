// The caustic map (light-grid area ratio) against the photon ground truth and closed forms.
//   - glass slab and pool: exact values (the map has no noise);
//   - glass ball: the energy sent through, and the irradiance pattern on the floor at and around
//     the focus, against photons, for a sun of finite size;
//   - a point sun's fold is clamped, not infinite.
#include "dve/render/caustic_map.hpp"
#include "dve/render/voxel_reference_renderer.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
constexpr double kPi = 3.14159265358979323846;

VoxelMaterialDefinition opaque() { VoxelMaterialDefinition m; m.name = "Floor"; return m; }
VoxelMaterialDefinition dielectric(float ior) {
    VoxelMaterialDefinition m;
    m.name = "Glass";
    m.transmission = 1.0F;
    m.indexOfRefraction = ior;
    return m;
}
void fill_floor(VoxelObject& object, MaterialId id) {
    for (int x = -20; x < 20; ++x)
        for (int z = -20; z < 20; ++z) object.set_voxel({x, 0, z}, id);
}

void test_slab_and_pool() {
    VoxelObject floor(1U), slab(2U);
    fill_floor(floor, 1);
    for (int x = -8; x < 8; ++x)
        for (int z = -8; z < 8; ++z)
            for (int y = 5; y < 10; ++y) slab.set_voxel({x, y, z}, 1);
    std::array<VoxelMaterialDefinition, 2> floorMaterials{VoxelMaterialDefinition{}, opaque()};
    std::array<VoxelMaterialDefinition, 2> glassMaterials{VoxelMaterialDefinition{}, dielectric(1.5F)};
    const std::vector<VoxelReferenceInstance> voxels{{1U, &floor, {}, floorMaterials, true, {1, 1, 1}},
                                                     {2U, &slab, {}, glassMaterials, true, {1, 1, 1}}};
    const OpticalScene scene(voxels, {});
    CausticMapSettings settings;
    settings.resolution = 64;
    const CausticMap map = build_caustic_map(scene, {}, settings);
    const float e = map.irradiance({0.3F, 1.0F, -0.7F}, {0, 1, 0}).x;
    std::printf("slab: %.5f (exact 0.9216), %llu triangles\n", e, static_cast<unsigned long long>(map.stats().triangles));
    check(std::abs(e - 0.9216F) < 1e-3F, "slab: exact transmitted irradiance, no noise");
    check(map.irradiance({15.0F, 1.0F, 0.0F}, {0, 1, 0}).x == 0.0F, "no caustic light outside the slab's shadow");
    check(map.irradiance({0.3F, 1.0F, -0.7F}, {1, 0, 0}).x == 0.0F, "wrong-facing surfaces get none");

    VoxelObject pool(3U);
    fill_floor(pool, 1);
    for (int x = -10; x < 10; ++x)
        for (int z = -10; z < 10; ++z)
            for (int y = 1; y < 5; ++y) pool.set_voxel({x, y, z}, 2);
    std::array<VoxelMaterialDefinition, 3> materials{VoxelMaterialDefinition{}, opaque(), dielectric(1.33F)};
    const std::vector<VoxelReferenceInstance> poolVoxels{{3U, &pool, {}, materials, true, {1, 1, 1}}};
    const OpticalScene poolScene(poolVoxels, {});
    const float angle = 40.0F * 3.14159265F / 180.0F;
    const CausticMap poolMap = build_caustic_map(poolScene, {{std::sin(angle), std::cos(angle), 0.0F}, 0.0F}, settings);
    const double expected = std::cos(angle) * (1.0 - dielectric_fresnel(std::cos(angle), 1.0F, 1.33F));
    const float p = poolMap.irradiance({0.0F, 1.0F, 0.0F}, {0, 1, 0}).x;
    std::printf("pool: %.5f (exact %.5f)\n", p, expected);
    check(std::abs(p - expected) < 1e-3, "pool: exact oblique transmission");
}

CookedPolygonAsset sphere(float r, const VoxelMaterialDefinition& material) {
    CookedPolygonAsset a;
    a.objectId = 50;
    a.materials.push_back(material);
    a.materialBindings.resize(1);
    const int slices = 128, stacks = 64;
    for (int j = 0; j <= stacks; ++j)
        for (int i = 0; i <= slices; ++i) {
            const float phi = 3.14159265F * static_cast<float>(j) / stacks, theta = 6.2831853F * static_cast<float>(i) / slices;
            const Float3 n{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            a.vertices.push_back({{n.x * r, n.y * r, n.z * r}, n, {1, 0, 0, 1}, {0, 0}, {1, 1, 1, 1}, {0, 0}});
        }
    for (int j = 0; j < stacks; ++j)
        for (int i = 0; i < slices; ++i) {
            const std::uint32_t p = static_cast<std::uint32_t>(j * (slices + 1) + i), q = p + static_cast<std::uint32_t>(slices + 1);
            a.indices.insert(a.indices.end(), {p, q, p + 1, p + 1, q, q + 1});
        }
    a.submeshes.push_back({"ball", 0, static_cast<std::uint32_t>(a.indices.size()), 0});
    a.bounds = {{-r, -r, -r}, {r, r, r}};
    return a;
}

void test_ball_against_photons() {
    // Ball of radius 2 centred 5 units above the floor top: past its paraxial focus (3 units), so
    // the floor shows a bright disc with a caustic ring, not a single point.
    VoxelObject floor(1U);
    fill_floor(floor, 1);
    std::array<VoxelMaterialDefinition, 2> floorMaterials{VoxelMaterialDefinition{}, opaque()};
    const std::vector<VoxelReferenceInstance> voxels{{1U, &floor, {}, floorMaterials, true, {1, 1, 1}}};
    const CookedPolygonAsset ball = sphere(2.0F, dielectric(1.5F));
    const std::vector<OpticalPolygonInstance> polygons{{50U, &ball, {{0.0F, 6.0F, 0.0F}, {}}, true}};
    const OpticalScene scene(voxels, polygons);
    const SunLight sun{{0, 1, 0}, 0.03F};
    CausticMapSettings settings;
    settings.resolution = 384;
    const CausticMap map = build_caustic_map(scene, sun, settings);
    double expected = 0.0;
    for (int k = 0; k < 4000; ++k) {
        const double r = (k + 0.5) / 4000.0 * 2.0;
        const double t = 1.0 - dielectric_fresnel(static_cast<float>(std::sqrt(1.0 - r * r / 4.0)), 1.0F, 1.5F);
        expected += t * t * 2.0 * kPi * r * (2.0 / 4000.0);
    }
    std::printf("ball: map power %.4f (Fresnel integral %.4f), %llu triangles, %llu split\n", map.total_power().x, expected,
                static_cast<unsigned long long>(map.stats().triangles), static_cast<unsigned long long>(map.stats().splitTriangles));
    check(std::abs(map.total_power().x - expected) / expected < 0.02, "map power within 2% (rim triangles straddle the silhouette)");

    CausticPhotonSettings photons;
    photons.photonCount = 2000000;
    photons.includeReflections = false;
    const auto traced = trace_caustic_photons(scene, sun, caustic_light_region(scene, sun.towardSun), photons);
    const CausticPhotonMap reference(traced.photons, 0.25F);
    double worst = 0.0;
    std::printf("   r   map     photons\n");
    for (float r = 0.0F; r <= 2.5F; r += 0.25F) {
        const double m = map.irradiance({r, 1.0F, 0.0F}, {0, 1, 0}).x;
        const double p = reference.irradiance({r, 1.0F, 0.0F}, {0, 1, 0}, 0.06F).x;
        std::printf("%5.2f  %6.3f  %6.3f\n", r, m, p);
        if (p > 0.2) worst = std::max(worst, std::abs(m - p) / p);
    }
    std::printf("worst relative difference where lit: %.3f\n", worst);
    check(worst < 0.15, "map matches photons within 15% where lit");
}

void test_fold_clamp() {
    VoxelObject floor(1U);
    fill_floor(floor, 1);
    std::array<VoxelMaterialDefinition, 2> floorMaterials{VoxelMaterialDefinition{}, opaque()};
    const std::vector<VoxelReferenceInstance> voxels{{1U, &floor, {}, floorMaterials, true, {1, 1, 1}}};
    const CookedPolygonAsset ball = sphere(2.0F, dielectric(1.5F));
    const std::vector<OpticalPolygonInstance> polygons{{50U, &ball, {{0.0F, 4.0F, 0.0F}, {}}, true}};   // focus on the floor
    const OpticalScene scene(voxels, polygons);
    CausticMapSettings settings;
    settings.resolution = 128;
    const CausticMap map = build_caustic_map(scene, {}, settings);
    const float peak = map.irradiance({0.001F, 1.0F, 0.001F}, {0, 1, 0}).x;
    std::printf("point-sun focus: %.1f x the sun\n", peak);
    check(std::isfinite(peak) && peak > 20.0F, "a bright, finite focus");
}
// In the reference renderer: the glass ball blocks the direct sun (polygon sun occluders) and the
// caustic map brings the light back, focused, under it.
void test_renderer() {
    VoxelObject floor(1U);
    fill_floor(floor, 1);
    std::array<VoxelMaterialDefinition, 2> floorMaterials{VoxelMaterialDefinition{}, opaque()};
    floorMaterials[1].baseColor = {0.5F, 0.5F, 0.5F, 1.0F};
    const std::vector<VoxelReferenceInstance> voxels{{1U, &floor, {}, floorMaterials, true, {1, 1, 1}}};
    const CookedPolygonAsset ball = sphere(2.0F, dielectric(1.5F));
    const RigidTransform pose{{0.0F, 4.0F, 0.0F}, {}};
    const std::vector<PolygonRenderInstance> occluders{{50U, &ball, pose, {}, {1, 1, 1, 1}, true}};
    const std::vector<OpticalPolygonInstance> optical{{50U, &ball, pose, true}};
    const OpticalScene scene(voxels, optical);
    const CausticMap map = build_caustic_map(scene, {{0, 1, 0}, 0.012F}, {});
    RenderEnvironment environment;
    environment.sunDirection = {0.0F, 1.0F, 0.0F};
    environment.shadowMode = ShadowMode::Hard;
    environment.shadowStrength = 1.0F;
    environment.globalIlluminationMode = GlobalIlluminationMode::Off;
    PolygonCamera camera;
    camera.position = {0.0F, 40.0F, 0.01F};
    camera.target = {0.0F, 0.0F, 0.0F};
    camera.verticalFieldOfViewRadians = 0.4F;
    const auto render = [&](const CausticMap* caustics, const std::vector<PolygonRenderInstance>& blockers) {
        ReferenceVoxelRenderer renderer;
        renderer.caustics = caustics;
        renderer.sunOccluders = blockers;
        PolygonRenderTarget target;
        target.resize(65, 65);
        (void)renderer.render(voxels, camera, environment, target);
        return target;
    };
    const auto lum = [](const PolygonRenderTarget& t, std::uint32_t x, std::uint32_t y) {
        const Float4 c = t.hdrColor[y * t.width + x];
        return (c.x + c.y + c.z) / 3.0F;
    };
    const PolygonRenderTarget open = render(nullptr, {});
    const PolygonRenderTarget shadowed = render(nullptr, occluders);
    const PolygonRenderTarget focused = render(&map, occluders);
    const float sunlit = lum(open, 5, 5), underOpen = lum(open, 32, 32);
    const float underShadow = lum(shadowed, 32, 32), underFocus = lum(focused, 32, 32);
    // Halfway between the focus and the edge of the ball's shadow: dark with or without caustics.
    const float rim = lum(focused, 32 + 5, 32);
    std::printf("renderer: open floor %.3f, under the ball: no occluders %.3f, shadow %.3f, with caustics %.3f (rim %.3f)\n",
                sunlit, underOpen, underShadow, underFocus, rim);
    check(std::abs(underOpen - sunlit) < 0.05F * sunlit, "without occluders the floor under the ball is plain sunlit");
    check(underShadow < 0.1F * sunlit, "the ball's polygon blocks the direct sun");
    check(underFocus > 5.0F * sunlit, "the caustic map focuses that light back, much brighter than plain sun");
}
}  // namespace

int main() {
    test_renderer();
    test_slab_and_pool();
    test_fold_clamp();
    test_ball_against_photons();
    if (g_failures == 0) std::printf("caustic map tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
