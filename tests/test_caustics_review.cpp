// Fixes from the Codex review of the caustics PRs (#180, #181):
//   - a closed glass mesh with different glass materials on its faces is left, not re-entered;
//   - surface normals come from the right triangle when submeshes are not in index order;
//   - photons follow SunLight's direction, and refuse a region built for another one;
//   - finding the glass in a sparse voxel object walks its bricks, not its bounding box;
//   - caustic light follows the shadow strength (no double counting) and is off with shadows off;
//   - masked polygons cast shadows only where they pass the alpha test;
//   - caustic light stays on the surface that caught it, except across a seam in the same plane.
#include "dve/polygon_bvh.hpp"
#include "dve/render/caustic_map.hpp"
#include "dve/render/caustic_photons.hpp"
#include "dve/render/voxel_reference_renderer.hpp"

#include <array>
#include <chrono>
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

VoxelMaterialDefinition glass(const char* name) {
    VoxelMaterialDefinition m;
    m.name = name;
    m.transmission = 1.0F;
    m.indexOfRefraction = 1.5F;
    return m;
}
VoxelMaterialDefinition floor_material() {
    VoxelMaterialDefinition m;
    m.name = "Floor";
    m.baseColor = {0.5F, 0.5F, 0.5F, 1.0F};
    return m;
}
void add_face(CookedPolygonAsset& a, std::vector<std::uint32_t>& indices, Float3 n, std::array<Float3, 4> p,
              std::array<Float2, 4> uv = {Float2{0, 0}, Float2{1, 0}, Float2{1, 1}, Float2{0, 1}}) {
    const auto base = static_cast<std::uint32_t>(a.vertices.size());
    for (int i = 0; i < 4; ++i) a.vertices.push_back({p[i], n, {1, 0, 0, 1}, uv[i], {1, 1, 1, 1}, {0, 0}});
    indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

// A glass box whose bottom face is a second glass material, listed first in the index buffer
// but second among the submeshes.
CookedPolygonAsset two_glass_box(Float3 lo, Float3 hi) {
    CookedPolygonAsset a;
    a.objectId = 70;
    a.materials = {glass("Glass top"), glass("Glass bottom")};
    a.materialBindings.resize(2);
    std::vector<std::uint32_t> body, bottom;
    add_face(a, bottom, {0, -1, 0}, {Float3{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z}});
    add_face(a, body, {0, 1, 0}, {Float3{lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}});
    add_face(a, body, {-1, 0, 0}, {Float3{lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z}});
    add_face(a, body, {1, 0, 0}, {Float3{hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z}});
    add_face(a, body, {0, 0, -1}, {Float3{lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, lo.y, lo.z}});
    add_face(a, body, {0, 0, 1}, {Float3{lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}});
    a.indices = bottom;
    a.indices.insert(a.indices.end(), body.begin(), body.end());
    a.submeshes.push_back({"body", static_cast<std::uint32_t>(bottom.size()), static_cast<std::uint32_t>(body.size()), 0});
    a.submeshes.push_back({"bottom", 0, static_cast<std::uint32_t>(bottom.size()), 1});
    a.bounds = {lo, hi};
    return a;
}

// A smooth ball whose lower half's indices come first in the buffer but second among the submeshes.
CookedPolygonAsset shuffled_ball(float r) {
    CookedPolygonAsset a;
    a.objectId = 71;
    a.materials = {glass("Ball")};
    a.materialBindings.resize(1);
    const int slices = 64, stacks = 32;
    for (int j = 0; j <= stacks; ++j)
        for (int i = 0; i <= slices; ++i) {
            const float phi = 3.14159265F * static_cast<float>(j) / stacks, theta = 6.2831853F * static_cast<float>(i) / slices;
            const Float3 n{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            a.vertices.push_back({{n.x * r, n.y * r, n.z * r}, n, {1, 0, 0, 1}, {0, 0}, {1, 1, 1, 1}, {0, 0}});
        }
    std::vector<std::uint32_t> upper, lower;
    for (int j = 0; j < stacks; ++j)
        for (int i = 0; i < slices; ++i) {
            const std::uint32_t p = static_cast<std::uint32_t>(j * (slices + 1) + i), q = p + static_cast<std::uint32_t>(slices + 1);
            (j < stacks / 2 ? upper : lower).insert((j < stacks / 2 ? upper : lower).end(), {p, q, p + 1, p + 1, q, q + 1});
        }
    a.indices = lower;
    a.indices.insert(a.indices.end(), upper.begin(), upper.end());
    a.submeshes.push_back({"upper", static_cast<std::uint32_t>(lower.size()), static_cast<std::uint32_t>(upper.size()), 0});
    a.submeshes.push_back({"lower", 0, static_cast<std::uint32_t>(lower.size()), 0});
    a.bounds = {{-r, -r, -r}, {r, r, r}};
    return a;
}

struct Floor {
    VoxelObject object{1U};
    std::array<VoxelMaterialDefinition, 2> materials{VoxelMaterialDefinition{}, floor_material()};
    Floor() {
        for (int x = -20; x < 20; ++x)
            for (int z = -20; z < 20; ++z) object.set_voxel({x, 0, z}, 1);
    }
};

void test_mixed_material_exit_and_submesh_order() {
    Floor floor;
    const std::vector<VoxelReferenceInstance> voxels{{1U, &floor.object, {}, floor.materials, true, {1, 1, 1}}};
    const CookedPolygonAsset box = two_glass_box({-4, 4, -4}, {4, 6, 4});
    const std::vector<OpticalPolygonInstance> polygons{{70U, &box, {}, true}};
    const OpticalScene scene(voxels, polygons);
    const LightPathResult path = follow_light_transmitted(scene, {0.3F, 20.0F, -0.2F}, {0, -1, 0});
    const float exact = (1.0F - dielectric_fresnel(1.0F, 1.0F, 1.5F)) * (1.0F - dielectric_fresnel(1.0F, 1.5F, 1.0F));
    std::printf("two-material glass box: landed %d, refractions %u, throughput %.4f (exact %.4f), y %.3f\n", path.landed,
                path.refractions, path.throughput.x, exact, path.position.y);
    check(path.landed && path.refractions == 2U && std::abs(path.throughput.x - exact) < 1e-4F && std::abs(path.position.y - 1.0F) < 1e-3F,
          "a glass mesh with another glass material on its exit face is left once, straight down to the floor");

    // Normals: the shuffled ball's hits carry the sphere's own normal.
    const CookedPolygonAsset ball = shuffled_ball(2.0F);
    const std::vector<OpticalPolygonInstance> balls{{71U, &ball, {}, true}};
    const OpticalScene ballScene({}, balls);
    float worst = 0.0F;
    for (const Float3 origin : {Float3{0.7F, 10.0F, 0.4F}, Float3{-0.9F, -10.0F, 0.3F}, Float3{0.2F, -10.0F, -1.1F}}) {
        const Float3 direction{0.0F, origin.y > 0.0F ? -1.0F : 1.0F, 0.0F};
        const auto hit = ballScene.trace(origin, direction, 100.0F);
        check(hit.has_value(), "ball hit");
        if (!hit) continue;
        const Float3 sphere = normalize(hit->position);
        worst = std::max(worst, 1.0F - dot(sphere, hit->shadingNormal));
    }
    std::printf("shuffled submeshes: worst 1 - cos(normal error) %.6f\n", worst);
    check(worst < 1e-3F, "normals come from the triangle that was hit, whatever the submesh order");
}

void test_sun_direction_and_sparse_bounds() {
    Floor floor;
    const std::vector<VoxelReferenceInstance> voxels{{1U, &floor.object, {}, floor.materials, true, {1, 1, 1}}};
    const CookedPolygonAsset box = two_glass_box({-2, 4, -2}, {2, 6, 2});
    const std::vector<OpticalPolygonInstance> polygons{{70U, &box, {}, true}};
    const OpticalScene scene(voxels, polygons);
    const CausticLightRegion overhead = caustic_light_region(scene, {0, 1, 0});
    CausticPhotonSettings settings;
    settings.photonCount = 1000;
    const auto mismatch = trace_caustic_photons(scene, {normalize(Float3{1, 1, 0}), 0.0F}, overhead, settings);
    check(mismatch.regionMismatch && mismatch.photons.empty() && mismatch.emitted == 0U,
          "a region built for another sun direction is refused, not silently used");
    const Float3 slant = normalize(Float3{0.5F, 1.0F, 0.0F});
    const auto slanted = trace_caustic_photons(scene, {slant, 0.0F}, caustic_light_region(scene, slant), settings);
    std::printf("slanted sun: %zu photons, emitted %llu, direct %llu, lost %llu\n", slanted.photons.size(),
                static_cast<unsigned long long>(slanted.emitted), static_cast<unsigned long long>(slanted.direct),
                static_cast<unsigned long long>(slanted.lost));
    bool along = !slanted.photons.empty();
    float meanX = 0.0F;
    for (const CausticPhoton& p : slanted.photons) meanX += p.position.x / static_cast<float>(slanted.photons.size());
    std::printf("slanted sun: mean landing x %.3f\n", meanX);
    along = along && meanX < -1.0F;   // overhead it would be about 0: the light comes in slanted
    check(!slanted.regionMismatch && along, "photons follow the sun's direction");

    // Two glass cells a million cells apart: the bounds come from the bricks, at once.
    VoxelObject sparse(9U);
    sparse.set_voxel({0, 0, 0}, 1);
    sparse.set_voxel({1000000, 3, -1000000}, 1);
    std::array<VoxelMaterialDefinition, 2> materials{VoxelMaterialDefinition{}, glass("Glass")};
    const std::vector<VoxelReferenceInstance> sparseVoxels{{9U, &sparse, {}, materials, true, {1, 1, 1}}};
    const OpticalScene sparseScene(sparseVoxels, {});
    const auto start = std::chrono::steady_clock::now();
    Float3 lo{}, hi{};
    const bool found = sparseScene.refractive_bounds(lo, hi);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("sparse glass bounds: (%.0f %.0f %.0f) - (%.0f %.0f %.0f) in %.4f s\n", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, seconds);
    check(found && lo.x == 0.0F && hi.x == 1000001.0F && lo.z == -1000000.0F && hi.y == 4.0F && seconds < 0.5,
          "sparse refractive bounds are exact and immediate");
}

PolygonCamera camera_down() {
    PolygonCamera camera;
    camera.position = {0.0F, 30.0F, 0.01F};
    camera.target = {0.0F, 0.0F, 0.0F};
    camera.verticalFieldOfViewRadians = 0.5F;
    return camera;
}

void test_caustic_shadow_share() {
    // A glass slab (polygons: the voxel pass does not draw it) over the floor, sun overhead: under
    // it the floor gets (1 - strength) direct + strength x 0.9216 transmitted; with shadows off,
    // plain sun.
    Floor floor;
    const std::vector<VoxelReferenceInstance> voxels{{1U, &floor.object, {}, floor.materials, true, {1, 1, 1}}};
    const CookedPolygonAsset slab = two_glass_box({-8, 4, -8}, {8, 6, 8});
    const std::vector<OpticalPolygonInstance> optical{{70U, &slab, {}, true}};
    const std::vector<PolygonRenderInstance> occluders{{70U, &slab, {}, {}, {1, 1, 1, 1}, true}};
    const OpticalScene scene(voxels, optical);
    CausticMapSettings settings;
    settings.resolution = 64;
    const CausticMap map = build_caustic_map(scene, {}, settings);
    const auto centre = [&](ShadowMode mode, float strength) {
        RenderEnvironment environment;
        environment.sunDirection = {0.0F, 1.0F, 0.0F};
        environment.shadowMode = mode;
        environment.shadowStrength = strength;
        environment.globalIlluminationMode = GlobalIlluminationMode::Off;
        ReferenceVoxelRenderer renderer;
        renderer.caustics = &map;
        renderer.sunOccluders = occluders;
        PolygonRenderTarget target;
        target.resize(17, 17);
        (void)renderer.render(voxels, camera_down(), environment, target);
        return std::pair{target.hdrColor[8U * 17U + 8U].x, 0.5F * environment.sunIntensity * environment.sunColor.x};
    };
    const float through = 0.9216F;
    for (const float strength : {1.0F, 0.85F, 0.5F}) {
        const auto [value, sun] = centre(ShadowMode::Hard, strength);
        const float expected = sun * ((1.0F - strength) + strength * through);
        std::printf("under the glass, shadow strength %.2f: %.4f (expected %.4f)\n", strength, value, expected);
        check(std::abs(value - expected) < 2e-3F * sun, "caustic light follows the shadow strength");
    }
    const auto [off, sun] = centre(ShadowMode::Off, 0.85F);
    check(std::abs(off - sun) < 1e-5F * sun, "shadows off: plain sun, no caustic on top");
}

void test_masked_occluder() {
    // A masked quad over the floor: its texture is a hole on the left half (alpha 0) and solid on
    // the right; only the right half casts a shadow.
    Floor floor;
    const std::vector<VoxelReferenceInstance> voxels{{1U, &floor.object, {}, floor.materials, true, {1, 1, 1}}};
    CookedPolygonAsset quad;
    quad.objectId = 72;
    VoxelMaterialDefinition leaf;
    leaf.name = "Leaf";
    leaf.blendMode = MaterialBlendMode::Masked;
    quad.materials.push_back(leaf);
    quad.materialBindings.resize(1);
    quad.materialBindings[0].alphaCutoff = 0.5F;
    quad.materialBindings[0].doubleSided = true;
    quad.materialBindings[0].baseColor.texture = 0U;
    quad.images.push_back({"mask", "image/raw", 2U, 1U, {255, 255, 255, 0, 255, 255, 255, 255}});
    PolygonSampler nearest;
    nearest.magFilter = ImportedTextureFilter::Nearest;
    nearest.wrapS = ImportedWrapMode::ClampToEdge;
    nearest.wrapT = ImportedWrapMode::ClampToEdge;
    quad.samplers.push_back(nearest);
    quad.textures.push_back({"mask", 0U, 0U});
    std::vector<std::uint32_t> indices;
    add_face(quad, indices, {0, 1, 0}, {Float3{-6, 5, -6}, {-6, 5, 6}, {6, 5, 6}, {6, 5, -6}},
             {Float2{0, 0}, Float2{0, 1}, Float2{1, 1}, Float2{1, 0}});
    quad.indices = indices;
    quad.submeshes.push_back({"quad", 0, static_cast<std::uint32_t>(indices.size()), 0});
    quad.bounds = {{-6, 5, -6}, {6, 5, 6}};
    const std::vector<PolygonRenderInstance> occluders{{72U, &quad, {}, {}, {1, 1, 1, 1}, true}};
    RenderEnvironment environment;
    environment.sunDirection = {0.0F, 1.0F, 0.0F};
    environment.shadowMode = ShadowMode::Hard;
    environment.shadowStrength = 1.0F;
    environment.globalIlluminationMode = GlobalIlluminationMode::Off;
    ReferenceVoxelRenderer renderer;
    renderer.sunOccluders = occluders;
    PolygonRenderTarget target;
    target.resize(33, 33);
    (void)renderer.render(voxels, camera_down(), environment, target);
    const float lit = 0.5F * environment.sunIntensity;
    // Camera right is +x: left half of the image is x < 0 (the hole).
    const float hole = target.hdrColor[16U * 33U + 10U].x, solid = target.hdrColor[16U * 33U + 22U].x;
    std::printf("masked quad: under the hole %.4f, under the solid half %.4f (sunlit %.4f)\n", hole, solid, lit);
    check(std::abs(hole - lit) < 1e-4F * lit, "light passes through the masked quad's hole");
    check(solid < 1e-6F, "the solid half still shadows");
}

void test_receiver() {
    // The slab's light lands on two coplanar floors meeting at x = 0 (objects 1 and 2), and on
    // nothing else. A third surface just below floor 1 (object 3) must get none of floor 1's light.
    VoxelObject left(1U), right(2U);
    for (int z = -20; z < 20; ++z) {
        for (int x = -20; x < 0; ++x) left.set_voxel({x, 0, z}, 1);
        for (int x = 0; x < 20; ++x) right.set_voxel({x, 0, z}, 1);
    }
    std::array<VoxelMaterialDefinition, 2> materials{VoxelMaterialDefinition{}, floor_material()};
    const std::vector<VoxelReferenceInstance> voxels{{1U, &left, {}, materials, true, {1, 1, 1}},
                                                     {2U, &right, {}, materials, true, {1, 1, 1}}};
    const CookedPolygonAsset slab = two_glass_box({-8, 4, -8}, {8, 6, 8});
    const std::vector<OpticalPolygonInstance> optical{{70U, &slab, {}, true}};
    const OpticalScene scene(voxels, optical);
    CausticMapSettings settings;
    settings.resolution = 64;
    const CausticMap map = build_caustic_map(scene, {{0, 1, 0}, 0.02F}, settings);
    const Float3 up{0, 1, 0};
    const float seam = map.irradiance({-0.05F, 1.0F, 0.3F}, up, 1U).x;
    const float any = map.irradiance({-0.05F, 1.0F, 0.3F}, up).x;
    std::printf("receiver: at the seam on floor 1 %.4f (any receiver %.4f)\n", seam, any);
    check(std::abs(seam - any) < 1e-5F && std::abs(seam - 0.9216F) < 2e-3F, "across a coplanar seam the light is shared");
    const float below = map.irradiance({-3.0F, 0.95F, 0.3F}, up, 3U).x;
    const float leaked = map.irradiance({-3.0F, 0.95F, 0.3F}, up).x;
    std::printf("receiver: a surface 0.05 below floor 1 gets %.4f (it used to get %.4f)\n", below, leaked);
    check(below == 0.0F && leaked > 0.5F, "light absorbed by floor 1 does not reach a surface just below it");
}

} // namespace

int main() {
    test_mixed_material_exit_and_submesh_order();
    test_sun_direction_and_sparse_bounds();
    test_caustic_shadow_share();
    test_masked_occluder();
    test_receiver();
    if (g_failures == 0) std::printf("dve_caustics_review_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
