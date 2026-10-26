// Triplanar displacement on voxels (dve/voxel_displacement.hpp):
//   - the carved cell: a constant height carves to exactly (1 - h) x depth; a ramp tilts the
//     normal by exactly depth x dh/dx; a fully carved edge lets rays through; an unexposed side
//     stays flat;
//   - the tracer: rays pass through carved-away space to what lies behind;
//   - the renderer, traced: the depth pass moves by exactly the carve, outlines shrink, and
//     grooves shadow themselves;
//   - parallax: the read point shifts by exactly lateral / cos x depth; no shadows from the
//     relief, depth and outline unchanged;
//   - beyond displacementTraceDistanceMeters traced materials fall back to parallax exactly;
//   - DVOX v1.11 carries the settings.
#include "dve/dvox.hpp"
#include "dve/material_displacement.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/render/radiance_cascades_spwi.hpp"
#include "dve/render/voxel_reference_renderer.hpp"
#include "dve/transform.hpp"
#include "dve/voxel_displacement.hpp"
#include "dve/voxel_textures.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;
namespace fs = std::filesystem;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

std::shared_ptr<const VoxelNormalMap> height_constant(std::uint8_t h) {
    std::vector<std::uint8_t> t;
    for (int i = 0; i < 16; ++i) t.insert(t.end(), {h, h, h, 255});
    return make_voxel_image("height", 4, 4, t, VoxelImageRole::Data);
}
// Bytes 0..255 across x (u for Y and Z faces), constant in v.
std::shared_ptr<const VoxelNormalMap> height_ramp() {
    std::vector<std::uint8_t> t;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 256; ++x) t.insert(t.end(), {static_cast<std::uint8_t>(x), 0, 0, 255});
    return make_voxel_image("ramp", 256, 4, t, VoxelImageRole::Data);
}
// Grooves: h = 1 for x in [0, 0.5) of each tile, 0 for [0.5, 1).
std::shared_ptr<const VoxelNormalMap> height_grooves() {
    std::vector<std::uint8_t> t;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 64; ++x) {
            const std::uint8_t v = x < 32 ? 255 : 0;
            t.insert(t.end(), {v, 0, 0, 255});
        }
    return make_voxel_image("grooves", 64, 4, t, VoxelImageRole::Data);
}

VoxelMaterialDefinition displaced(std::shared_ptr<const VoxelNormalMap> height, float depth, std::uint8_t mode = 0U) {
    VoxelMaterialDefinition m;
    m.baseColor = {0.7F, 0.7F, 0.7F, 1.0F};
    m.specular = 0.0F;
    m.heightMap = std::move(height);
    m.displacementDepthMeters = depth;
    m.displacementMode = mode;
    return m;
}

void test_cell() {
    const Float3 mpc{0.1F, 0.1F, 0.1F};
    // Constant h = 128/255 under the +y face, depth 0.04 m = 0.4 cells: the surface sits
    // (1 - h) x 0.4 below y = 1.
    const VoxelMaterialDefinition flat = displaced(height_constant(128), 0.04F);
    const auto hit = trace_displaced_voxel(flat, {0, 0, 0}, 8U, {0.3F, 2.0F, 0.6F}, {0, -1, 0}, 1.0F, 2.0F, mpc, 0.0F, 24U);
    const float carve = (1.0F - 128.0F / 255.0F) * 0.4F;
    check(hit && std::abs(hit->t - (1.0F + carve)) < 1e-4F && hit->axis == 1 && std::abs(hit->normal.y - 1.0F) < 1e-5F,
          "a constant height carves to (1 - h) x depth with the face's normal");
    // Ramp: dh/dx = (256/255) per metre at tile 1 m; normal = n - depth x grad h.
    const VoxelMaterialDefinition ramp = displaced(height_ramp(), 0.04F);
    const auto tilted = trace_displaced_voxel(ramp, {3, 0, 0}, 8U, {3.5F, 2.0F, 0.5F}, {0, -1, 0}, 1.0F, 2.0F, mpc, 0.0F, 48U);
    const Float3 want = normalize(Float3{-0.04F * 256.0F / 255.0F, 1.0F, 0.0F});
    std::printf("ramp normal (%.5f %.5f %.5f), exact (%.5f %.5f %.5f)\n", tilted ? tilted->normal.x : 0.0F, tilted ? tilted->normal.y : 0.0F,
                tilted ? tilted->normal.z : 0.0F, want.x, want.y, want.z);
    check(tilted && std::abs(tilted->normal.x - want.x) < 2e-4F && std::abs(tilted->normal.y - want.y) < 1e-5F,
          "a ramp tilts the normal by depth x dh/dx");
    // Fully carved (h = 0) under +x and +y: the 0.4-cell notch along the edge is open.
    const VoxelMaterialDefinition hollow = displaced(height_constant(0), 0.04F);
    check(!trace_displaced_voxel(hollow, {0, 0, 0}, 2U | 8U, {0.85F, 0.85F, -1.0F}, {0, 0, 1}, 1.0F, 2.0F, mpc, 0.0F, 24U),
          "a ray through the carved edge passes");
    const auto side = trace_displaced_voxel(hollow, {0, 0, 0}, 2U | 8U, {0.3F, 0.3F, -1.0F}, {0, 0, 1}, 1.0F, 2.0F, mpc, 0.0F, 24U);
    check(side && side->t == 1.0F && side->axis == -1, "an unexposed side stays where it was");
    // Depth is capped at half a voxel.
    check(voxel_displacement_depth(displaced(height_constant(0), 5.0F), 0.1F) == 0.05F, "depth capped at half a voxel");
}

void test_tracer_pass_through() {
    // A one-voxel slab (y = 0) with h = 0 on every face (depth = half a voxel: it vanishes to a
    // zero-thickness sheet at its centre) over a plain floor at y = -3.
    VoxelObject object(1U);
    for (int x = -5; x < 5; ++x)
        for (int z = -5; z < 5; ++z) {
            object.set_voxel({x, 0, z}, 1);
            object.set_voxel({x, -3, z}, 2);
        }
    std::array<VoxelMaterialDefinition, 3> materials{VoxelMaterialDefinition{}, displaced(height_constant(0), 0.05F), VoxelMaterialDefinition{}};
    const std::vector<VoxelReferenceInstance> instances{{1U, &object, {}, materials, true, {1, 1, 1}}};
    spwi::VoxelSceneTracer tracer(instances);
    tracer.set_displacement(0.1F, {}, std::numeric_limits<float>::infinity());
    // Grazing along x at y = 0.8 (inside the carved top half): passes over the slab.
    const auto grazing = tracer.trace({-20.0F, 0.8F, 0.5F}, {1, 0, 0}, 100.0F);
    check(!grazing, "a ray through carved-away space passes the slab");
    // Straight down at the slab's centre line (y = 0.5 is the remaining sheet): hits it.
    const auto down = tracer.trace({0.5F, 5.0F, 0.5F}, {0, -1, 0}, 100.0F);
    check(down && std::abs(down->objectHit.distance - 4.5F) < 1e-3F, "the remaining sheet is hit at its centre");
    // The same rays without displacement focus (radius 0): the plain cube.
    tracer.set_displacement(0.1F, {1000, 0, 0}, 0.0F);
    const auto plain = tracer.trace({-20.0F, 0.8F, 0.5F}, {1, 0, 0}, 100.0F);
    check(plain && std::abs(plain->objectHit.distance - 15.0F) < 1e-4F, "outside the focus the voxels stay cubic");
}

struct Scene {
    VoxelObject object{1U};
    std::array<VoxelMaterialDefinition, 2> materials{};
    std::vector<VoxelReferenceInstance> instances;
    Scene(const VoxelMaterialDefinition& m, bool floor = true) {
        materials[1] = m;
        if (floor) {
            for (int x = -60; x < 60; ++x)
                for (int z = -60; z < 60; ++z)
                    for (int y = -2; y <= 0; ++y) object.set_voxel({x, y, z}, 1);   // thick: only the top face carves
        } else {
            for (int x = 0; x < 3; ++x)
                for (int y = 0; y < 3; ++y)
                    for (int z = 0; z < 3; ++z) object.set_voxel({x, y, z}, 1);
        }
        instances.push_back({1U, &object, {}, materials, true, {1, 1, 1}});
    }
};
PolygonCamera down_camera(float height) {
    PolygonCamera c;
    c.position = {0.3F, 1.0F + height, 0.27F};
    c.target = {0.3F, 1.0F, 0.28F};
    c.up = {0, 0, 1};
    c.verticalFieldOfViewRadians = 0.3F;
    c.nearPlane = 0.1F;
    c.farPlane = 500.0F;
    return c;
}
RenderEnvironment sun(Float3 direction) {
    RenderEnvironment e;
    e.sunDirection = normalize(direction);
    e.sunColor = {1, 1, 1};
    e.sunIntensity = 2.0F;
    e.shadowMode = ShadowMode::Hard;
    e.shadowStrength = 1.0F;
    e.globalIlluminationMode = GlobalIlluminationMode::Off;
    e.ambientOcclusionSamples = 0;
    return e;
}
PolygonRenderTarget draw(const Scene& scene, const RenderEnvironment& e, const PolygonCamera& camera, std::uint32_t size = 32) {
    PolygonRenderTarget target;
    target.resize(size, size);
    target.enable_passes();
    ReferenceVoxelRenderer renderer;
    renderer.threadCount = 2;
    (void)renderer.render(scene.instances, camera, e, target);
    return target;
}

void test_renderer() {
    const RenderEnvironment overhead = sun({0.1F, 1.0F, 0.05F});
    const std::size_t centre = 16U * 32U + 16U;
    // Depth pass: constant h carves (1 - h) x depth below the face.
    const Scene plain(displaced(height_constant(255), 0.0F)), carved(displaced(height_constant(64), 0.04F));
    const PolygonRenderTarget a = draw(plain, overhead, down_camera(10.0F)), b = draw(carved, overhead, down_camera(10.0F));
    const float expected = (1.0F - 64.0F / 255.0F) * 0.04F;
    std::printf("traced: depth moved by %.5f m (exact %.5f)\n", b.passes.depthMeters[centre] - a.passes.depthMeters[centre], expected);
    check(std::abs(b.passes.depthMeters[centre] - a.passes.depthMeters[centre] - expected) < 2e-4F, "the depth pass follows the carved surface");

    // Outline: a 3-voxel cube fully carved by half a voxel shrinks to 2 voxels across.
    const Scene cube(displaced(height_constant(255), 0.0F), false), shrunk(displaced(height_constant(0), 0.05F), false);
    PolygonCamera side;
    side.position = {1.5F, 1.5F, 30.0F};
    side.target = {1.5F, 1.5F, 0.0F};
    side.up = {0, 1, 0};
    side.verticalFieldOfViewRadians = 0.2F;
    side.nearPlane = 0.1F;
    side.farPlane = 200.0F;
    const auto count = [](const PolygonRenderTarget& t) {
        std::size_t n = 0;
        for (const auto id : t.objectId) n += id == 1U ? 1U : 0U;
        return n;
    };
    const std::size_t full = count(draw(cube, overhead, side, 64)), carvedCount = count(draw(shrunk, overhead, side, 64));
    const double ratio = static_cast<double>(carvedCount) / static_cast<double>(full);
    std::printf("outline: %zu pixels flat, %zu carved (ratio %.3f; (2/3)^2 = 0.444)\n", full, carvedCount, ratio);
    check(std::abs(ratio - 4.0 / 9.0) < 0.06, "the outline shrinks by the carve");

    // Grooves at a low sun across them: traced grooves shadow their own floors, parallax does not.
    const RenderEnvironment low = sun({1.0F, 0.35F, 0.0F});
    VoxelMaterialDefinition grooves = displaced(height_grooves(), 0.05F);
    grooves.textureTileMeters = 0.1F;   // one voxel per period, grooves half a voxel wide and deep
    VoxelMaterialDefinition parallaxGrooves = grooves;
    parallaxGrooves.displacementMode = 1U;
    const PolygonRenderTarget traced = draw(Scene(grooves), low, down_camera(8.0F), 64);
    const PolygonRenderTarget parallax = draw(Scene(parallaxGrooves), low, down_camera(8.0F), 64);
    double tracedShadow = 0.0, parallaxShadow = 0.0;
    std::size_t shadowedPixels = 0;
    for (std::size_t i = 0; i < traced.hdrColor.size(); ++i) {
        tracedShadow += traced.passes.shadow[i].x;
        parallaxShadow += parallax.passes.shadow[i].x;
        shadowedPixels += traced.passes.sunVisibility[i] < 0.5F ? 1U : 0U;
    }
    if (std::getenv("DVE_DEBUG"))
        for (std::size_t i = 64U * 32U; i < 64U * 32U + 64U; i += 2)
            std::printf("  px %zu vis %.2f n (%.2f %.2f %.2f) depth %.4f dd %.3f shadow %.3f\n", i, traced.passes.sunVisibility[i], traced.passes.normal[i].x,
                        traced.passes.normal[i].y, traced.passes.normal[i].z, traced.passes.depthMeters[i], traced.passes.directDiffuse[i].x,
                        traced.passes.shadow[i].x);
    std::printf("grooves, sun at 19 deg: traced %zu pixels in shadow (shadow light %.2f), parallax shadow light %.2f\n", shadowedPixels,
                tracedShadow, parallaxShadow);
    check(shadowedPixels > 800U && tracedShadow > 50.0 && parallaxShadow == 0.0, "traced relief casts shadows; parallax does not");
    // Parallax leaves depth and outline alone.
    const PolygonRenderTarget flatDepth = draw(Scene(displaced(height_grooves(), 0.0F)), low, down_camera(8.0F), 64);
    check(parallax.passes.depthMeters[2080] == flatDepth.passes.depthMeters[2080], "parallax keeps the depth pass");

    // Beyond the trace distance, traced falls back to parallax: identical to Parallax mode.
    RenderEnvironment near = low;
    near.displacementTraceDistanceMeters = 0.1F;
    const PolygonRenderTarget fallback = draw(Scene(grooves), near, down_camera(8.0F), 64);
    bool same = true;
    for (std::size_t i = 0; i < fallback.hdrColor.size(); ++i)
        same = same && fallback.hdrColor[i].x == parallax.hdrColor[i].x && fallback.passes.depthMeters[i] == parallax.passes.depthMeters[i];
    check(same, "beyond the trace distance traced displacement draws exactly as parallax");
}

void test_parallax() {
    // Constant h: the read point moves by lateral / cos x (1 - h) x depth, the normal stays.
    const VoxelMaterialDefinition m = displaced(height_constant(128), 0.04F, 1U);
    const Float3 view = normalize(Float3{0.6F, 0.8F, 0.0F});
    const VoxelParallaxResult r = voxel_parallax(m, {0.2F, 0.1F, 0.3F}, {0, 1, 0}, view, 0.04F, 0.0F, 64U);
    const float depth = (1.0F - 128.0F / 255.0F) * 0.04F;
    const float shift = -0.6F / 0.8F * depth;
    std::printf("parallax: depth %.5f (exact %.5f), x shift %.5f (exact %.5f)\n", r.depthMeters, depth, r.positionMeters.x - 0.2F, shift);
    check(std::abs(r.depthMeters - depth) < 1e-5F && std::abs(r.positionMeters.x - 0.2F - shift) < 1e-5F &&
              std::abs(r.normal.y - 1.0F) < 1e-6F,
          "parallax shifts the read point exactly for a constant height");
}

void test_dvox(const fs::path& dir) {
    CookedVoxelAsset asset(41);
    asset.materials.resize(2);
    asset.object.set_voxel({0, 0, 0}, 1);
    asset.materials[1] = displaced(height_ramp(), 0.03F, 1U);
    std::string error;
    check(write_dvox(dir / "displaced.dvox", asset, {}, &error), "displaced dvox: " + error);
    std::ifstream in(dir / "displaced.dvox", std::ios::binary);
    in.seekg(6);
    unsigned char b[2]{};
    in.read(reinterpret_cast<char*>(b), 2);
    check((b[0] | (b[1] << 8U)) == 11, "displacement needs DVOX v1.11");
    const auto back = read_dvox(dir / "displaced.dvox");
    check(back.success && back.asset.materials[1].displacementDepthMeters == 0.03F && back.asset.materials[1].displacementMode == 1U &&
              back.asset.materials[1].heightMap,
          "DVOX displacement round trip: " + back.error);
}

} // namespace

// --- Polygons -------------------------------------------------------------------------------

// A 2 x 2 quad in the z = 0 plane facing +z, whose UVs equal its triplanar Z projection at
// `scale` (u = x scale, v = y scale; tangent +x, bitangent +y).
CookedPolygonAsset projected_quad(float scale) {
    CookedPolygonAsset a;
    a.objectId = 1;
    a.materials.push_back({});
    a.materialBindings.push_back({});
    a.materialBindings[0].doubleSided = true;
    const auto v = [&](float x, float y) { return PolygonVertex{{x, y, 0}, {0, 0, 1}, {1, 0, 0, 1}, {x * scale, y * scale}, {1, 1, 1, 1}}; };
    a.vertices = {v(-1, -1), v(1, -1), v(1, 1), v(-1, 1)};
    a.indices = {0, 1, 2, 0, 2, 3};
    a.submeshes.push_back({"quad", 0, 6, 0});
    a.bounds = {{-1, -1, 0}, {1, 1, 0}};
    return a;
}
std::uint32_t add_texture(CookedPolygonAsset& a, std::uint32_t w, std::uint32_t h, const std::vector<std::uint8_t>& rgba) {
    PolygonImage image;
    image.width = w;
    image.height = h;
    image.rgba8 = rgba;
    a.images.push_back(image);
    a.textures.push_back({"t", static_cast<std::uint32_t>(a.images.size() - 1), {}});
    return static_cast<std::uint32_t>(a.textures.size() - 1);
}
// Stripes across u for colour, and a bumpy height (depth) field, so that parallax visibly moves
// the stripes.
void add_parallax_maps(CookedPolygonAsset& a, MaterialMappingMode mode, float scale) {
    std::vector<std::uint8_t> stripes, bumps;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const std::uint8_t c = (x / 2) % 2 ? 230 : 20;
            stripes.insert(stripes.end(), {c, c, c, 255});
            const auto d = static_cast<std::uint8_t>(128 + 120 * std::sin(0.8 * x) * std::cos(0.6 * y));
            bumps.insert(bumps.end(), {d, d, d, 255});
        }
    auto& b = a.materialBindings[0];
    b.baseColor.texture = add_texture(a, 16, 16, stripes);
    b.height.texture = add_texture(a, 16, 16, bumps);
    b.mapping.mappingMode = mode;
    b.mapping.triplanarScale = scale;
    b.mapping.triplanarBlendSharpness = 8.0F;
    b.mapping.heightMode = HeightMappingMode::ParallaxOcclusion;
    b.mapping.heightScale = 0.08F;
    b.mapping.maximumParallaxDistanceMeters = 1000.0F;
    a.contentHash = polygon_asset_content_hash(a);
}

struct PolygonFrame {
    std::vector<Float4> color;
    PolygonRenderStats stats;
};
PolygonFrame render_polygon(const CookedPolygonAsset& asset, const RigidTransform& pose, Float3 cameraLocal) {
    PolygonCamera camera;
    camera.position = transform_point(pose, cameraLocal);
    camera.target = transform_point(pose, {0, 0, 0});
    camera.nearPlane = 0.1F;
    camera.farPlane = 20.0F;
    PolygonRenderTarget target;
    target.resize(64, 64);
    PolygonRenderOptions options;
    options.preserveExistingDepth = false;
    options.metersPerUnit = 1.0F;
    PolygonRenderInstance instance{1, &asset, pose};
    RenderEnvironment environment;
    PolygonFrame frame;
    frame.stats = ReferencePolygonRenderer{}.render(std::span(&instance, 1), camera, environment, target, options);
    frame.color = target.hdrColor;
    return frame;
}
float max_difference(const PolygonFrame& a, const PolygonFrame& b) {
    float worst = 0.0F;
    for (std::size_t i = 0; i < a.color.size(); ++i)
        worst = std::max({worst, std::abs(a.color[i].x - b.color[i].x), std::abs(a.color[i].y - b.color[i].y),
                          std::abs(a.color[i].z - b.color[i].z)});
    return worst;
}

void test_polygon_triplanar_parallax() {
    const float scale = 0.5F;
    const Float3 eye{1.2F, 0.7F, 2.4F};   // oblique, so parallax shifts the read point
    CookedPolygonAsset uvQuad = projected_quad(scale), worldQuad = projected_quad(scale), flatQuad = projected_quad(scale);
    add_parallax_maps(uvQuad, MaterialMappingMode::UV0, scale);
    add_parallax_maps(worldQuad, MaterialMappingMode::WorldTriplanar, scale);
    add_parallax_maps(flatQuad, MaterialMappingMode::WorldTriplanar, scale);
    flatQuad.materialBindings[0].mapping.heightMode = HeightMappingMode::Off;
    flatQuad.contentHash = polygon_asset_content_hash(flatQuad);
    const RigidTransform identity{};
    const PolygonFrame uv = render_polygon(uvQuad, identity, eye);
    const PolygonFrame world = render_polygon(worldQuad, identity, eye);
    const PolygonFrame flat = render_polygon(flatQuad, identity, eye);
    if (std::getenv("DVE_DEBUG"))
        std::printf("tp %llu ps %llu/%llu fb %llu/%llu frag %llu\n", (unsigned long long)world.stats.triplanarParallaxFragments,
                    (unsigned long long)world.stats.parallaxSamples, (unsigned long long)uv.stats.parallaxSamples,
                    (unsigned long long)world.stats.parallaxFallbacks, (unsigned long long)uv.stats.parallaxFallbacks,
                    (unsigned long long)world.stats.rasterizedTriangles);
    check(world.stats.triplanarParallaxFragments > 1000, "world triplanar parallax runs on the quad's fragments");
    check(max_difference(world, flat) > 0.2F, "triplanar parallax moves the stripes");
    const float worldVsUv = max_difference(world, uv);
    std::printf("polygon triplanar parallax: %llu fragments, moves stripes by up to %.3f, world vs UV %.5f\n",
                static_cast<unsigned long long>(world.stats.triplanarParallaxFragments), max_difference(world, flat), worldVsUv);
    check(worldVsUv < 0.02F, "world triplanar parallax matches UV parallax when the UVs equal the projection (" +
                                 std::to_string(worldVsUv) + ")");

    // Object triplanar on a rotated object: the projection and the march follow the object.
    CookedPolygonAsset objectQuad = projected_quad(scale);
    add_parallax_maps(objectQuad, MaterialMappingMode::ObjectTriplanar, scale);
    const RigidTransform turned{{0.3F, -0.2F, 0.1F}, quaternion_from_axis_angle(normalize(Float3{0.4F, 1.0F, 0.2F}), 0.7F)};
    const PolygonFrame objectTurned = render_polygon(objectQuad, turned, eye);
    const PolygonFrame uvTurned = render_polygon(uvQuad, turned, eye);
    const float objectVsUv = max_difference(objectTurned, uvTurned);
    std::printf("object triplanar parallax, rotated: vs UV %.5f\n", objectVsUv);
    check(objectTurned.stats.triplanarParallaxFragments > 1000 && objectVsUv < 0.02F,
          "object triplanar parallax on a rotated object matches UV parallax (" + std::to_string(objectVsUv) + ")");
}

void test_polygon_voxel_parallax() {
    // A mesh made from voxels carrying a displaced voxel material: parallax shades its faces.
    CookedPolygonAsset quad = projected_quad(1.0F);
    VoxelMaterialDefinition m = displaced(height_grooves(), 0.05F);
    std::vector<std::uint8_t> stripes;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 16; ++x) {
            const std::uint8_t c = (x / 2) % 2 ? 255 : 30;
            stripes.insert(stripes.end(), {c, c, c, 255});
        }
    m.baseColorMap = make_voxel_image("stripes", 16, 4, stripes, VoxelImageRole::Color);
    m.textureTileMeters = 0.5F;
    quad.materials[0] = m;
    quad.contentHash = polygon_asset_content_hash(quad);
    CookedPolygonAsset plain = quad;
    plain.materials[0].displacementDepthMeters = 0.0F;
    plain.contentHash = polygon_asset_content_hash(plain);
    const Float3 eye{1.4F, 0.2F, 2.0F};
    const PolygonFrame shaded = render_polygon(quad, {}, eye), flat = render_polygon(plain, {}, eye);
    check(shaded.stats.parallaxSamples > flat.stats.parallaxSamples, "voxel displacement marches on meshes made from voxels");
    check(max_difference(shaded, flat) > 0.1F, "voxel displacement shifts the maps on meshes made from voxels");
}

void test_mesh_displacement_triplanar() {
    const auto settings = [](float scale) {
        MaterialVertexDisplacementSettings s;
        s.policy = MaterialDisplacementPolicy::VisualOnly;
        s.scaleMeters = scale;
        s.referencePlane = 0.5F;
        return s;
    };
    // Height along x for the triplanar Z projection: byte = 32 x texel; the UVs are constant 0,
    // so UV sampling would read texel 0 everywhere.
    CookedPolygonAsset quad = projected_quad(1.0F);
    for (auto& v : quad.vertices) v.texcoord = {0.0F, 0.0F};
    std::vector<std::uint8_t> ramp;
    for (int x = 0; x < 8; ++x) ramp.insert(ramp.end(), {static_cast<std::uint8_t>(32 * x), 0, 0, 255});
    auto& binding = quad.materialBindings[0];
    binding.height.texture = add_texture(quad, 8, 1, ramp);
    binding.mapping.mappingMode = MaterialMappingMode::WorldTriplanar;
    binding.mapping.triplanarScale = 0.25F;   // x in [-1, 1] -> u in [-0.25, 0.25]
    quad.contentHash = polygon_asset_content_hash(quad);
    const std::array<MaterialVertexDisplacementSettings, 1> one{settings(1.0F)};
    const auto triplanar = build_displaced_polygon_asset(quad, one, 0.0F);
    CookedPolygonAsset uvQuad = quad;
    uvQuad.materialBindings[0].mapping.mappingMode = MaterialMappingMode::UV0;
    uvQuad.contentHash = polygon_asset_content_hash(uvQuad);
    const auto uv = build_displaced_polygon_asset(uvQuad, one, 0.0F);
    check(triplanar && uv, "mesh displacement builds");
    if (triplanar && uv) {
        check(triplanar->stats.triplanarHeightVertices == 4 && uv->stats.triplanarHeightVertices == 0,
              "triplanar mappings read the height triplanar");
        // Each vertex's height comes from the Z projection at (x, y) x scale, as the renderer reads it.
        bool exact = true, varies = false;
        for (std::size_t i = 0; i < quad.vertices.size(); ++i) {
            const PolygonVertex& source = quad.vertices[i];
            const TriplanarCoordinates c = make_triplanar_coordinates(source.position, source.normal, 0.25F, 4.0F);
            const float z = triplanar->visualAsset.vertices[i].position.z;
            varies = varies || std::abs(z - triplanar->visualAsset.vertices[0].position.z) > 0.05F;
            exact = exact && std::isfinite(z) && std::abs(c.weights.z - 1.0F) < 1e-6F;
        }
        check(exact && varies, "triplanar mesh heights follow position, not the (constant) UVs");
        bool flatUv = true;
        for (const auto& v : uv->visualAsset.vertices)
            flatUv = flatUv && std::abs(v.position.z - uv->visualAsset.vertices[0].position.z) < 1e-6F;
        check(flatUv, "UV mesh heights follow the constant UVs");
    }
    // A voxel material's height map is a height source: constant h = 1 lifts by (1 - 0.5) x scale.
    CookedPolygonAsset voxelQuad = projected_quad(1.0F);
    std::vector<std::uint8_t> full;
    for (int i = 0; i < 4; ++i) full.insert(full.end(), {255, 255, 255, 255});
    voxelQuad.materials[0].heightMap = make_voxel_image("h", 2, 2, full, VoxelImageRole::Data);
    voxelQuad.contentHash = polygon_asset_content_hash(voxelQuad);
    const std::array<MaterialVertexDisplacementSettings, 1> lift{settings(0.2F)};
    const auto lifted = build_displaced_polygon_asset(voxelQuad, lift, 0.0F);
    check(lifted && lifted->stats.missingHeightBindings == 0, "a voxel height map counts as a height source");
    if (lifted) {
        bool exact = true;
        for (const auto& v : lifted->visualAsset.vertices) exact = exact && std::abs(v.position.z - 0.1F) < 1e-5F;
        check(exact, "a constant voxel height map lifts by exactly (h - reference) x scale");
    }
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "dve_triplanar_displacement_test";
    fs::create_directories(dir);
    test_cell();
    test_tracer_pass_through();
    test_renderer();
    test_parallax();
    test_dvox(dir);
    test_polygon_triplanar_parallax();
    test_polygon_voxel_parallax();
    test_mesh_displacement_triplanar();
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures == 0) std::printf("dve_triplanar_displacement_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
