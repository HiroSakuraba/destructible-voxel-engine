// Ambient occlusion (dve/render/ambient_occlusion.hpp) against exact answers.
//
// With a max distance far beyond the scene, a ray that hits anything occludes ~fully, so the AO
// is the share of the cosine-weighted sky a point sees: the view factor to the open sky. That has
// closed forms for simple shapes:
//   - open ground: 1;
//   - ground at distance x from a wide wall of height H: 1 - 1/2 (1 - x / sqrt(x^2 + H^2));
//   - the bottom centre of a long slot of width w and depth H: sin(atan((w/2) / H)).
// With the real max distance, the (1 - d / max)^2 weighting is integrated exactly by quadrature.
// Rays leave from the shadow bias above the surface (0.015 m = 0.15 voxels here), so the walls
// they see are that much lower: the exact values use H - bias.
// Then the renderers: the voxel reference's AmbientHemisphere ambient, traced one-bounce GI with
// black walls (which must agree with the open-sky share), polygons, and a hybrid image where
// voxels and polygons occlude each other.
#include "dve/render/ambient_occlusion.hpp"
#include "dve/render/radiance_cascades_spwi.hpp"
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

VoxelMaterialDefinition colour(float r, float g, float b) {
    VoxelMaterialDefinition m;
    m.baseColor = {r, g, b, 1.0F};
    return m;
}

// Average AO at one point over many pixel rotations: the integral, not one pixel's 64 rays.
double integrated_ao(const AmbientOcclusionRay& nearest, Float3 point, Float3 normal, RenderEnvironment environment,
                     float metersPerUnit = 0.1F, int pixels = 2048) {
    environment.ambientOcclusionSamples = 64;
    double sum = 0.0;
    for (int p = 0; p < pixels; ++p)
        sum += reference_ambient_occlusion(point, normal, static_cast<std::uint32_t>(p), 0U, environment, metersPerUnit, nearest);
    return sum / pixels;
}

AmbientOcclusionRay voxel_rays(const spwi::VoxelSceneTracer& tracer) {
    return [&tracer](Float3 origin, Float3 direction, float maximum) -> std::optional<float> {
        if (const auto hit = tracer.trace(origin, direction, maximum)) return hit->objectHit.distance;
        return std::nullopt;
    };
}

// Floor (top at y = 1) with a wall of height H on x < 0.
struct WallScene {
    VoxelObject object{1U};
    std::array<VoxelMaterialDefinition, 3> materials{VoxelMaterialDefinition{}, colour(1, 1, 1), colour(0, 0, 0)};
    std::vector<VoxelReferenceInstance> instances;
    explicit WallScene(int height) {
        for (int z = -400; z < 400; ++z) {
            for (int x = -40; x < 200; ++x) object.set_voxel({x, 0, z}, 1);
            for (int x = -40; x < 0; ++x)
                for (int y = 1; y <= height; ++y) object.set_voxel({x, y, z}, 2);
        }
        instances.push_back({1U, &object, {}, materials, true, {1, 1, 1}});
    }
};

// Floor at y = 1 between two walls x < -w/2 and x >= w/2, depth H.
struct SlotScene {
    VoxelObject object{1U};
    std::array<VoxelMaterialDefinition, 3> materials{VoxelMaterialDefinition{}, colour(1, 1, 1), colour(0, 0, 0)};
    std::vector<VoxelReferenceInstance> instances;
    SlotScene(int width, int depth) {
        for (int z = -400; z < 400; ++z)
            for (int x = -60; x < 60; ++x) {
                object.set_voxel({x, 0, z}, 1);
                if (x < -width / 2 || x >= width / 2)
                    for (int y = 1; y <= depth; ++y) object.set_voxel({x, y, z}, 2);
            }
        instances.push_back({1U, &object, {}, materials, true, {1, 1, 1}});
    }
};

void test_open_sky_shares() {
    RenderEnvironment environment;
    environment.ambientOcclusionMaxDistanceMeters = 1.0e4F;   // every hit occludes ~fully
    const Float3 up{0, 1, 0};
    const double bias = ambient_occlusion_origin_bias(environment, 0.1F);
    {
        WallScene open(0);
        const spwi::VoxelSceneTracer tracer(open.instances);
        const double ao = integrated_ao(voxel_rays(tracer), {50.3F, 1.0F, 0.2F}, up, environment);
        std::printf("open ground: %.5f (exact 1)\n", ao);
        check(ao == 1.0, "open ground sees the whole sky");
    }
    for (const auto [height, x] : {std::pair{20, 10.0F}, std::pair{20, 3.0F}, std::pair{6, 15.0F}}) {
        WallScene scene(height);
        const spwi::VoxelSceneTracer tracer(scene.instances);
        const double ao = integrated_ao(voxel_rays(tracer), {x, 1.0F, 0.37F}, up, environment);
        const double h = height - bias;
        const double exact = 1.0 - 0.5 * (1.0 - x / std::sqrt(static_cast<double>(x) * x + h * h));
        std::printf("ground %4.1f from a wall %2d high: %.4f (exact %.4f)\n", x, height, ao, exact);
        check(std::abs(ao - exact) < 1.5e-3, "beside a wall: 1 - 1/2 (1 - x / sqrt(x^2 + H^2))");
    }
    for (const auto [width, depth] : {std::pair{10, 10}, std::pair{4, 12}, std::pair{20, 5}}) {
        SlotScene scene(width, depth);
        const spwi::VoxelSceneTracer tracer(scene.instances);
        const double ao = integrated_ao(voxel_rays(tracer), {0.0F, 1.0F, 0.41F}, up, environment);
        const double half = width / 2.0;
        const double h = depth - bias;
        const double exact = half / std::sqrt(half * half + h * h);
        std::printf("bottom of a slot %2d wide, %2d deep: %.4f (exact %.4f)\n", width, depth, ao, exact);
        check(std::abs(ao - exact) < 1.5e-3, "bottom of a slot: sin(atan((w/2) / H))");
    }
}

// Exact AO for ground at distance x from an infinitely tall, wide wall, with the real distance
// weighting: 1 - (1/pi) Int (1 - d/max)^2 cos(theta) dOmega over the directions that reach the
// wall within max (d = x / (sin(theta) cos(phi)) for cos(phi) > 0).
double exact_weighted_wall(double x, double maximum) {
    double sum = 0.0;
    const int nt = 2000, np = 2000;
    for (int i = 0; i < nt; ++i) {
        const double theta = (i + 0.5) / nt * kPi / 2.0;
        for (int j = 0; j < np; ++j) {
            const double phi = (j + 0.5) / np * kPi - kPi / 2.0;   // toward the wall: cos(phi) > 0
            const double d = x / (std::sin(theta) * std::cos(phi));
            if (d >= maximum) continue;
            const double proximity = 1.0 - d / maximum;
            sum += proximity * proximity * std::cos(theta) * std::sin(theta) * (kPi / 2.0 / nt) * (kPi / np);
        }
    }
    return 1.0 - sum / kPi;
}

void test_distance_weighting() {
    RenderEnvironment environment;
    environment.ambientOcclusionMaxDistanceMeters = 1.0F;   // 10 voxels
    WallScene scene(300);
    const spwi::VoxelSceneTracer tracer(scene.instances);
    for (const float x : {0.5F, 2.0F, 5.0F, 9.0F, 12.0F}) {
        const double ao = integrated_ao(voxel_rays(tracer), {x, 1.0F, 0.37F}, {0, 1, 0}, environment);
        const double exact = exact_weighted_wall(x, 10.0);
        std::printf("weighted (max 1 m), ground %4.1f voxels from a tall wall: %.4f (exact %.4f)\n", x, ao, exact);
        check(std::abs(ao - exact) < 1.5e-3, "distance weighting integrates to the exact value");
    }
}

PolygonCamera camera_down(Float3 at, float height, float fov) {
    PolygonCamera camera;
    camera.position = {at.x, at.y + height, at.z + 0.001F};
    camera.target = at;
    camera.up = {0.0F, 0.0F, -1.0F};
    camera.verticalFieldOfViewRadians = fov;
    camera.nearPlane = 0.5F;
    camera.farPlane = 2000.0F;
    return camera;
}

double strip_mean(const PolygonRenderTarget& t, std::uint32_t column) {
    double sum = 0.0;
    for (std::uint32_t y = 0; y < t.height; ++y) sum += t.hdrColor[y * t.width + column].x;
    return sum / t.height;
}

void test_renderers_agree() {
    // The bottom of a slot 10 wide and 10 deep, seen from above: white floor, black walls, a
    // uniform grey sky, no sun. Open-sky share 0.4472.
    SlotScene scene(10, 10);
    RenderEnvironment environment;
    environment.sunIntensity = 0.0F;
    environment.skyColor = environment.groundColor = {0.5F, 0.5F, 0.5F};
    environment.globalIlluminationIntensity = 1.0F;
    environment.globalIlluminationMaxDistanceMeters = 1.0e4F;
    environment.ambientOcclusionMaxDistanceMeters = 1.0e4F;
    environment.ambientOcclusionSamples = 64;
    environment.shadowMode = ShadowMode::Off;
    const double h = 10.0 - ambient_occlusion_origin_bias(environment, 0.1F);
    const double exact = 0.5 * 5.0 / std::sqrt(25.0 + h * h);
    // A thin strip down the slot's centre line (image columns are x, rows run along the slot).
    const PolygonCamera camera = camera_down({0.0F, 1.0F, 0.0F}, 30.0F, 0.9F);
    const auto render = [&](GlobalIlluminationMode mode, std::uint32_t giSamples) {
        RenderEnvironment e = environment;
        e.globalIlluminationMode = mode;
        e.globalIlluminationSamples = giSamples;
        ReferenceVoxelRenderer renderer;
        PolygonRenderTarget target;
        target.resize(1, 400);
        (void)renderer.render(scene.instances, camera, e, target);
        return strip_mean(target, 0);
    };
    const double ambient = render(GlobalIlluminationMode::AmbientHemisphere, 1);
    const double traced = render(GlobalIlluminationMode::VoxelOneBounce, 16);
    std::printf("slot floor, voxel reference: AO'd ambient %.4f, traced GI (black walls) %.4f, exact %.4f\n", ambient, traced,
                exact);
    check(std::abs(ambient - exact) < 0.004, "voxel AmbientHemisphere ambient x AO = sky x open share");
    check(std::abs(traced - exact) < 0.004, "traced one-bounce GI with black walls agrees: the occlusion is real");
    RenderEnvironment noAo = environment;
    noAo.ambientOcclusionSamples = 0;
    noAo.globalIlluminationMode = GlobalIlluminationMode::AmbientHemisphere;
    ReferenceVoxelRenderer renderer;
    PolygonRenderTarget open;
    open.resize(1, 50);
    (void)renderer.render(scene.instances, camera, noAo, open);
    check(std::abs(strip_mean(open, 0) - 0.5) < 1e-5, "AO off: the full ambient");
}

CookedPolygonAsset box(Float3 lo, Float3 hi, Float4 colourValue) {
    CookedPolygonAsset a;
    a.objectId = 80;
    VoxelMaterialDefinition m;
    m.baseColor = colourValue;
    a.materials.push_back(m);
    a.materialBindings.resize(1);
    const auto face = [&](Float3 n, std::array<Float3, 4> p) {
        const auto base = static_cast<std::uint32_t>(a.vertices.size());
        for (const Float3 q : p) a.vertices.push_back({q, n, {1, 0, 0, 1}, {0, 0}, {1, 1, 1, 1}, {0, 0}});
        a.indices.insert(a.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    face({0, -1, 0}, {Float3{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z}});
    face({0, 1, 0}, {Float3{lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}});
    face({-1, 0, 0}, {Float3{lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z}});
    face({1, 0, 0}, {Float3{hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z}});
    face({0, 0, -1}, {Float3{lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, lo.y, lo.z}});
    face({0, 0, 1}, {Float3{lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}});
    a.submeshes.push_back({"box", 0, static_cast<std::uint32_t>(a.indices.size()), 0});
    a.bounds = {lo, hi};
    return a;
}

void test_polygons_and_hybrid() {
    // A polygon slab floor (top y = 1) and a polygon wall 20 high on x < 0; ground 10 from it:
    // exact AO 1 - 1/2 (1 - 10 / sqrt(500)) = 0.7236.
    RenderEnvironment environment;
    environment.sunIntensity = 0.0F;
    environment.skyColor = environment.groundColor = {0.5F, 0.5F, 0.5F};
    environment.globalIlluminationIntensity = 1.0F;
    environment.ambientOcclusionMaxDistanceMeters = 1.0e4F;
    environment.ambientOcclusionSamples = 64;
    environment.shadowMode = ShadowMode::Off;
    const double h = 20.0 - ambient_occlusion_origin_bias(environment, 0.1F);
    const double exact = 1.0 - 0.5 * (1.0 - 10.0 / std::sqrt(100.0 + h * h));
    const CookedPolygonAsset floor = box({-40, 0, -400}, {200, 1, 400}, {1, 1, 1, 1});
    const CookedPolygonAsset wall = box({-40, 1, -400}, {0, 21, 400}, {0, 0, 0, 1});
    const PolygonCamera camera = camera_down({10.0F, 1.0F, 0.0F}, 30.0F, 0.9F);
    {
        const std::vector<PolygonRenderInstance> instances{{80U, &floor, {}, {}, {1, 1, 1, 1}, true},
                                                           {81U, &wall, {}, {}, {1, 1, 1, 1}, true}};
        PolygonRenderTarget target;
        target.resize(1, 400);
        PolygonRenderOptions options;
        options.preserveExistingDepth = false;
        const auto stats = ReferencePolygonRenderer{}.render(instances, camera, environment, target, options);
        const double value = strip_mean(target, 0) / 0.5;
        std::printf("polygons, ground 10 from a 20-high wall: AO %.4f (exact %.4f), %llu AO rays\n", value, exact,
                    static_cast<unsigned long long>(stats.ambientOcclusionRays));
        check(std::abs(value - exact) < 0.004, "polygon ambient x AO matches the open-sky share");
    }
    {
        // Hybrid: the wall is voxels now, the floor still polygons; and the reverse.
        WallScene voxelWall(20);
        const std::vector<PolygonRenderInstance> floorOnly{{80U, &floor, {}, {}, {1, 1, 1, 1}, true}};
        std::vector<VoxelReferenceInstance> wallOnly = voxelWall.instances;
        VoxelObject wallObject(2U);
        for (int z = -400; z < 400; ++z)
            for (int x = -40; x < 0; ++x)
                for (int y = 1; y <= 20; ++y) wallObject.set_voxel({x, y, z}, 2);
        wallOnly[0].object = &wallObject;
        PolygonRenderTarget target;
        target.resize(1, 400);
        (void)render_hybrid_reference(wallOnly, floorOnly, camera, environment, target);
        const double value = strip_mean(target, 0) / 0.5;
        std::printf("hybrid, polygon ground beside a voxel wall: AO %.4f (exact %.4f)\n", value, exact);
        check(std::abs(value - exact) < 0.004, "polygon AO rays see the voxels in a hybrid image");

        const std::vector<PolygonRenderInstance> wallPolygon{{81U, &wall, {}, {}, {1, 1, 1, 1}, true}};
        VoxelObject floorObject(3U);
        for (int z = -400; z < 400; ++z)
            for (int x = -40; x < 200; ++x) floorObject.set_voxel({x, 0, z}, 1);
        std::vector<VoxelReferenceInstance> floorVoxels = voxelWall.instances;
        floorVoxels[0].object = &floorObject;
        RenderEnvironment ambient = environment;
        ambient.globalIlluminationMode = GlobalIlluminationMode::AmbientHemisphere;
        PolygonRenderTarget reverse;
        reverse.resize(1, 400);
        (void)render_hybrid_reference(floorVoxels, wallPolygon, camera, ambient, reverse);
        const double voxelValue = strip_mean(reverse, 0) / 0.5;
        std::printf("hybrid, voxel ground beside a polygon wall: AO %.4f (exact %.4f)\n", voxelValue, exact);
        check(std::abs(voxelValue - exact) < 0.004, "voxel AO rays see the polygons in a hybrid image");
    }
}

void test_shared_rays() {
    // The CPU mirror of the GPU sampling: same directions for the same pixel and sample.
    const Float3 n = normalize(Float3{0.3F, 0.8F, -0.5F});
    float minimumCos = 1.0F;
    double meanCos = 0.0;
    const int count = 4096;
    for (int i = 0; i < count; ++i) {
        const Float3 d = ambient_occlusion_ray_direction(n, static_cast<std::uint32_t>(i % 64), 64U,
                                                         static_cast<std::uint32_t>(i / 64), 7U);
        minimumCos = std::min(minimumCos, dot(d, n));
        meanCos += dot(d, n) / count;
    }
    std::printf("AO ray directions: min cos %.4f, mean cos %.4f (cosine-weighted: 2/3)\n", minimumCos, meanCos);
    check(minimumCos >= 0.0F && std::abs(meanCos - 2.0 / 3.0) < 5e-3, "rays cover the hemisphere, cosine-weighted");
}

} // namespace

int main() {
    test_shared_rays();
    test_open_sky_shares();
    test_distance_weighting();
    test_renderers_agree();
    test_polygons_and_hybrid();
    if (g_failures == 0) std::printf("dve_ambient_occlusion_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
