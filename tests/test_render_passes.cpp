// Render passes and distance fog (polygon_renderer.hpp):
//   - the light passes sum to the image at every pixel of a hybrid frame with sun, shadows,
//     ambient + AO, specular, clear coat, emission, a translucent polygon and the sky;
//   - image + shadow pass = the same frame rendered without shadows;
//   - depth (metres) and normals at known pixels; empty pixels hold +inf / 0;
//   - fog: the fogged image = image x T + fogColor x (1 - T) with T from the exact distance,
//     the passes still sum to it, the sky is untouched;
//   - fog height falloff: density thins exponentially above a base height (closed form,
//     checked against numeric integration on a descending ray), uniform at/below the base;
//   - the PFM files read back.
#include "dve/render/polygon_renderer.hpp"
#include "dve/render/voxel_reference_renderer.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

CookedPolygonAsset box(Float3 lo, Float3 hi, const VoxelMaterialDefinition& material, std::uint64_t id) {
    CookedPolygonAsset a;
    a.objectId = id;
    a.materials.push_back(material);
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

VoxelMaterialDefinition material(Float3 colour, float roughness, MaterialShadingModel model = MaterialShadingModel::StandardPBR) {
    VoxelMaterialDefinition m;
    m.baseColor = {colour.x, colour.y, colour.z, 1.0F};
    m.roughness = roughness;
    m.shadingModel = model;
    return m;
}

// A voxel floor (top y = 1) with a wall and an emissive block, a clear-coat polygon box and a
// translucent polygon pane; seen from above at an angle, with sky at the top of the image.
struct Scene {
    VoxelObject object{1U};
    std::array<VoxelMaterialDefinition, 4> materials{};
    CookedPolygonAsset coat, pane;
    std::vector<VoxelReferenceInstance> voxels;
    std::vector<PolygonRenderInstance> polygons;
    Scene() {
        materials[1] = material({0.7F, 0.65F, 0.6F}, 0.4F);
        materials[2] = material({0.3F, 0.4F, 0.8F}, 0.2F);
        materials[2].metallic = 1.0F;
        materials[3] = material({0.2F, 0.2F, 0.2F}, 0.5F);
        materials[3].emissive = {3.0F, 1.0F, 0.2F};
        for (int x = -30; x < 30; ++x)
            for (int z = -30; z < 30; ++z) object.set_voxel({x, 0, z}, 1);
        for (int z = -30; z < 30; ++z)
            for (int y = 1; y <= 6; ++y) object.set_voxel({-6, y, z}, 2);
        for (int y = 1; y <= 2; ++y) object.set_voxel({8, y, 4}, 3);
        voxels.push_back({1U, &object, {}, materials, true, {1, 1, 1}});
        coat = box({2, 1, -6}, {6, 4, -2}, material({0.8F, 0.1F, 0.1F}, 0.6F, MaterialShadingModel::ClearCoat), 80);
        coat.materials[0].clearCoat = 1.0F;
        VoxelMaterialDefinition glassy = material({0.2F, 0.7F, 0.3F}, 0.1F);
        glassy.baseColor.w = 0.4F;
        glassy.blendMode = MaterialBlendMode::Translucent;
        pane = box({-2, 1, 4}, {4, 5, 4.5F}, glassy, 81);
        polygons.push_back({80U, &coat, {}, {}, {1, 1, 1, 1}, true});
        polygons.push_back({81U, &pane, {}, {}, {1, 1, 1, 1}, true});
    }
};

PolygonCamera camera() {
    PolygonCamera c;
    c.position = {0.0F, 24.0F, 30.0F};
    c.target = {0.0F, 1.0F, 0.0F};
    c.up = {0.0F, 1.0F, 0.0F};
    c.verticalFieldOfViewRadians = 1.1F;
    c.nearPlane = 0.5F;
    c.farPlane = 400.0F;
    return c;
}

RenderEnvironment lit() {
    RenderEnvironment e;
    e.sunDirection = normalize(Float3{0.6F, 0.7F, 0.3F});
    e.sunIntensity = 3.0F;
    e.shadowMode = ShadowMode::Hard;
    e.shadowStrength = 0.9F;
    e.globalIlluminationMode = GlobalIlluminationMode::AmbientHemisphere;
    e.ambientOcclusionSamples = 8;
    return e;
}

PolygonRenderTarget render(const Scene& scene, const RenderEnvironment& environment, std::uint32_t w = 96, std::uint32_t h = 64) {
    PolygonRenderTarget target;
    target.resize(w, h);
    target.enable_passes();
    PolygonRenderOptions options;
    (void)render_hybrid_reference(scene.voxels, scene.polygons, camera(), environment, target, options, 0.1F, 2U);
    return target;
}

double relative(Float3 a, Float4 b) {
    const double d = std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z);
    return d / (1e-3 + std::abs(b.x) + std::abs(b.y) + std::abs(b.z));
}

void test_sum_and_shadow() {
    Scene scene;
    const RenderEnvironment environment = lit();
    const PolygonRenderTarget target = render(scene, environment);
    check(target.valid(), "the target with passes is valid");
    double worst = 0.0;
    std::size_t sky = 0, shadowed = 0, emissive = 0, specular = 0;
    const RenderPassBuffers& p = target.passes;
    for (std::size_t i = 0; i < target.hdrColor.size(); ++i) {
        worst = std::max(worst, relative(p.light_sum(i), target.hdrColor[i]));
        sky += target.depth[i] >= 1.0F ? 1U : 0U;
        shadowed += p.sunVisibility[i] < 0.5F ? 1U : 0U;
        emissive += p.emission[i].x > 1.0F ? 1U : 0U;
        specular += p.directSpecular[i].x > 0.0F ? 1U : 0U;
    }
    std::printf("passes: worst |sum - image| / image %.2e; %zu sky, %zu shadowed, %zu emissive, %zu with sun specular\n", worst, sky,
                shadowed, emissive, specular);
    check(worst < 1e-5, "the light passes sum to the image at every pixel");
    check(sky > 0 && shadowed > 50 && emissive > 0 && specular > 1000, "the frame has sky, shadow, emission and highlights");

    // The pane blends its passes over what is behind it: those pixels differ from a frame without
    // it, keep the surface behind as their object, and still sum (checked above).
    {
        Scene noPane;
        noPane.polygons.pop_back();
        const PolygonRenderTarget behind = render(noPane, environment);
        std::size_t tinted = 0;
        for (std::size_t i = 0; i < target.hdrColor.size(); ++i)
            if (target.hdrColor[i].y != behind.hdrColor[i].y && target.objectId[i] == behind.objectId[i] &&
                p.directDiffuse[i].y != behind.passes.directDiffuse[i].y)
                ++tinted;
        std::printf("translucent pane: %zu pixels blended over the surface behind\n", tinted);
        check(tinted > 20, "the translucent pane blends its passes over the surface behind it");
    }

    // Image + shadow = the frame without shadows (no caustics, same rays otherwise).
    RenderEnvironment open = environment;
    open.shadowMode = ShadowMode::Off;
    const PolygonRenderTarget unshadowed = render(scene, open);
    double worstShadow = 0.0;
    for (std::size_t i = 0; i < target.hdrColor.size(); ++i) {
        const Float3 sum{target.hdrColor[i].x + p.shadow[i].x, target.hdrColor[i].y + p.shadow[i].y,
                         target.hdrColor[i].z + p.shadow[i].z};
        worstShadow = std::max(worstShadow, relative(sum, unshadowed.hdrColor[i]));
    }
    std::printf("image + shadow pass vs shadows off: worst relative difference %.2e\n", worstShadow);
    check(worstShadow < 1e-4, "image + shadow pass = the unshadowed image");
}

void test_depth_and_normal() {
    Scene scene;
    RenderEnvironment environment = lit();
    PolygonRenderTarget target;
    target.resize(33, 33);
    target.enable_passes();
    PolygonCamera down;
    down.position = {12.5F, 31.0F, -20.5F};
    down.target = {12.5F, 1.0F, -20.5F};
    down.up = {0.0F, 0.0F, -1.0F};
    down.verticalFieldOfViewRadians = 0.5F;
    down.nearPlane = 0.5F;
    down.farPlane = 400.0F;
    (void)render_hybrid_reference(scene.voxels, scene.polygons, down, environment, target, {}, 0.1F, 2U);
    const std::size_t centre = 16U * 33U + 16U;
    std::printf("depth at the centre %.5f m (exact 3.0), normal (%.2f %.2f %.2f)\n", target.passes.depthMeters[centre],
                target.passes.normal[centre].x, target.passes.normal[centre].y, target.passes.normal[centre].z);
    check(std::abs(target.passes.depthMeters[centre] - 3.0F) < 1e-4F, "depth pass: camera Z in metres (30 voxels x 0.1 m)");
    check(target.passes.normal[centre].y == 1.0F, "normal pass: the floor faces up");

    // A polygon in front: the clear-coat box top (y = 4), 27 voxels below the camera.
    PolygonCamera atBox = down;
    atBox.position = {4.0F, 31.0F, -4.0F};
    atBox.target = {4.0F, 1.0F, -4.0F};
    PolygonRenderTarget boxTarget;
    boxTarget.resize(33, 33);
    boxTarget.enable_passes();
    (void)render_hybrid_reference(scene.voxels, scene.polygons, atBox, environment, boxTarget, {}, 0.1F, 2U);
    check(boxTarget.objectId[centre] == 80U && std::abs(boxTarget.passes.depthMeters[centre] - 2.7F) < 1e-3F,
          "depth pass on polygons: 27 units x 0.1 m");

    // Looking at the sky: empty pixels.
    PolygonCamera up = down;
    up.target = {12.5F, 100.0F, -20.4F};
    PolygonRenderTarget skyTarget;
    skyTarget.resize(9, 9);
    skyTarget.enable_passes();
    (void)render_hybrid_reference(scene.voxels, scene.polygons, up, environment, skyTarget, {}, 0.1F, 2U);
    check(std::isinf(skyTarget.passes.depthMeters[40]) && skyTarget.passes.normal[40].y == 0.0F &&
              skyTarget.passes.sunVisibility[40] == 1.0F,
          "nothing drawn: depth +inf, normal 0, visibility 1");
}

void test_fog() {
    Scene scene;
    const RenderEnvironment clear = lit();
    RenderEnvironment foggy = clear;
    foggy.fogDensityPerMeter = 0.4F;
    foggy.fogStartMeters = 1.5F;
    foggy.fogColor = {0.6F, 0.65F, 0.7F};
    const PolygonRenderTarget a = render(scene, clear), b = render(scene, foggy);
    const PolygonCamera c = camera();
    const float tanHalf = std::tan(c.verticalFieldOfViewRadians * 0.5F), aspect = 96.0F / 64.0F;
    double worst = 0.0, worstSum = 0.0;
    std::size_t fogged = 0, skyUntouched = 0, sky = 0;
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 96; ++x) {
            const std::size_t i = y * 96U + x;
            worstSum = std::max(worstSum, relative(b.passes.light_sum(i), b.hdrColor[i]));
            if (a.depth[i] >= 1.0F) {
                ++sky;
                skyUntouched += a.hdrColor[i].x == b.hdrColor[i].x ? 1U : 0U;
                continue;
            }
            // Exact distance along the pixel's ray from the depth pass.
            const float nx = (2.0F * (x + 0.5F) / 96.0F - 1.0F) * tanHalf * aspect, ny = (1.0F - 2.0F * (y + 0.5F) / 64.0F) * tanHalf;
            const double meters = a.passes.depthMeters[i] * std::sqrt(1.0 + nx * nx + ny * ny);
            const double t = std::exp(-0.4 * std::max(0.0, meters - 1.5));
            const Float3 want{static_cast<float>(a.hdrColor[i].x * t + 0.6 * (1 - t)), static_cast<float>(a.hdrColor[i].y * t + 0.65 * (1 - t)),
                              static_cast<float>(a.hdrColor[i].z * t + 0.7 * (1 - t))};
            worst = std::max(worst, relative(want, b.hdrColor[i]));
            fogged += t < 0.5 ? 1U : 0U;
        }
    std::printf("fog: worst relative error %.2e, %zu pixels more than half fog, passes sum to %.2e, %zu/%zu sky pixels untouched\n",
                worst, fogged, worstSum, skyUntouched, sky);
    check(worst < 1e-4, "fogged = image x T + fogColor x (1 - T), T from the distance along the ray");
    check(fogged > 100, "fog thickens with distance");
    check(worstSum < 1e-5, "the passes, fog included, still sum to the fogged image");
    check(sky > 0 && skyUntouched == sky, "the sky is not fogged");

    RenderEnvironment bad = foggy;
    bad.fogDensityPerMeter = -1.0F;
    check(!bad.validate(), "negative fog density is rejected");
}

void test_fog_height() {
    // Hand-built targets and cameras with known geometry: the closed form is checked
    // against the analytic value on a level ray and against numeric integration on a
    // descending ray that crosses the base height.
    const float metersPerUnit = 0.1F;
    const auto makeTarget = [](float depthValue) {
        PolygonRenderTarget target;
        target.resize(3, 3);
        for (std::size_t i = 0; i < target.hdrColor.size(); ++i) {
            target.hdrColor[i] = {1.0F, 1.0F, 1.0F, 1.0F};
            target.depth[i] = depthValue;
        }
        return target;
    };
    PolygonCamera camera;
    camera.position = {0.0F, 100.0F, 0.0F};   // 10 m up at 0.1 m per unit
    camera.target = {10.0F, 100.0F, 0.0F};     // level: the centre ray keeps its height
    camera.up = {0.0F, 1.0F, 0.0F};
    camera.verticalFieldOfViewRadians = 0.5F;
    camera.nearPlane = 1.0F;
    camera.farPlane = 101.0F;                 // depth 0.49 -> cameraZ 50 units -> a 5 m centre ray
    RenderEnvironment env;
    env.fogDensityPerMeter = 0.4F;
    env.fogStartMeters = 1.5F;
    env.fogColor = {0.6F, 0.65F, 0.7F};
    const std::size_t centre = 4;
    const auto expect = [&](const PolygonRenderTarget& target, double t, const char* what) {
        const double want = t + 0.6 * (1.0 - t);
        check(std::abs(target.hdrColor[centre].x - want) < 2e-4, what);
    };
    {   // Falloff off: the uniform formula, unchanged.
        auto target = makeTarget(0.49F);
        (void)apply_distance_fog(target, camera, env, metersPerUnit);
        expect(target, std::exp(-0.4 * 3.5), "height fog: falloff off keeps the uniform formula");
    }
    {   // Level ray at 10 m with base 2 m and falloff 0.3: density scales by exp(-0.3 x 8).
        RenderEnvironment high = env;
        high.fogHeightFalloffPerMeter = 0.3F;
        high.fogBaseHeightMeters = 2.0F;
        auto target = makeTarget(0.49F);
        (void)apply_distance_fog(target, camera, high, metersPerUnit);
        expect(target, std::exp(-0.4 * std::exp(-2.4) * 3.5), "height fog: a level ray above the base thins by exp(-falloff x height)");
    }
    {   // The same ray entirely below the base height: identical to uniform fog.
        RenderEnvironment low = env;
        low.fogHeightFalloffPerMeter = 0.3F;
        low.fogBaseHeightMeters = 20.0F;
        auto target = makeTarget(0.49F);
        (void)apply_distance_fog(target, camera, low, metersPerUnit);
        expect(target, std::exp(-0.4 * 3.5), "height fog: at/below the base the fog is uniform");
    }
    {   // Descending ray from 10 m crossing the 2 m base: numeric Simpson cross-check.
        PolygonCamera down = camera;
        down.target = {10.0F, 0.0F, 0.0F};
        RenderEnvironment env2 = env;
        env2.fogHeightFalloffPerMeter = 0.3F;
        env2.fogBaseHeightMeters = 2.0F;
        auto target = makeTarget(0.49F);
        (void)apply_distance_fog(target, down, env2, metersPerUnit);
        const double slope = -100.0 / std::sqrt(10100.0);   // unit forward Y of the centre ray
        const auto densityAt = [&](double m) {
            return 0.4 * std::exp(-0.3 * std::max(0.0, 10.0 + slope * m - 2.0));
        };
        const int n = 20000;
        double sum = 0.0;
        for (int i = 0; i <= n; ++i) {
            const double m = 1.5 + 3.5 * static_cast<double>(i) / n;
            sum += (i == 0 || i == n ? 1.0 : (i % 2 ? 4.0 : 2.0)) * densityAt(m);
        }
        expect(target, std::exp(-sum * 3.5 / n / 3.0), "height fog: a descending ray matches numeric integration");
    }
    {   // Scene level: with falloff on, the passes (fog included) still sum to the image.
        Scene scene;
        RenderEnvironment foggy = lit();
        foggy.fogDensityPerMeter = 0.4F;
        foggy.fogStartMeters = 1.5F;
        foggy.fogColor = {0.6F, 0.65F, 0.7F};
        foggy.fogHeightFalloffPerMeter = 0.5F;
        foggy.fogBaseHeightMeters = 0.5F;
        const PolygonRenderTarget b = render(scene, foggy);
        double worstSum = 0.0;
        for (std::size_t i = 0; i < b.hdrColor.size(); ++i)
            if (b.depth[i] < 1.0F) worstSum = std::max(worstSum, relative(b.passes.light_sum(i), b.hdrColor[i]));
        check(worstSum < 1e-5, "height fog: the passes, fog included, still sum to the image");
    }
    RenderEnvironment bad = env;
    bad.fogHeightFalloffPerMeter = -0.1F;
    check(!bad.validate(), "negative fog height falloff is rejected");
    bad = env;
    bad.fogBaseHeightMeters = std::numeric_limits<float>::infinity();
    check(!bad.validate(), "infinite fog base height is rejected");
}

std::vector<float> read_pfm(const std::filesystem::path& path, std::string& header) {
    std::ifstream in(path, std::ios::binary);
    std::string magic, size, scale;
    std::getline(in, magic);
    std::getline(in, size);
    std::getline(in, scale);
    header = magic + "|" + size + "|" + scale;
    const std::string bytes((std::istreambuf_iterator<char>(in)), {});
    std::vector<float> values(bytes.size() / 4U);
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::uint32_t bits = 0;
        for (int b = 0; b < 4; ++b) bits |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i * 4 + b])) << (8 * b);
        values[i] = std::bit_cast<float>(bits);
    }
    return values;
}

void test_pfm() {
    Scene scene;
    const PolygonRenderTarget target = render(scene, lit(), 24, 16);
    const auto directory = std::filesystem::temp_directory_path() / "dve_render_passes_test";
    std::string error;
    check(write_render_passes_pfm(target, directory, "frame_", 40.0F, &error), "PFM write: " + error);
    std::string header;
    const auto beauty = read_pfm(directory / "frame_beauty.pfm", header);
    check(header == "PF|24 16|-1.0" && beauty.size() == 24U * 16U * 3U, "beauty.pfm: colour, 24 x 16, little-endian");
    // PFM rows run bottom to top: file row 0 is image row 15.
    check(beauty.size() == 24U * 16U * 3U && beauty[0] == target.hdrColor[15U * 24U].x && beauty[1] == target.hdrColor[15U * 24U].y,
          "rows are stored bottom to top");
    const auto depth = read_pfm(directory / "frame_depth.pfm", header);
    check(header == "Pf|24 16|-1.0" && depth.size() == 24U * 16U, "depth.pfm: greyscale");
    for (const char* name : {"direct_diffuse", "direct_specular", "indirect_diffuse", "indirect_specular", "emission",
                             "transmission", "background", "fog", "shadow", "normal", "sun_visibility"})
        check(std::filesystem::exists(directory / (std::string("frame_") + name + ".pfm")), std::string(name) + ".pfm written");
    PolygonRenderTarget plain;
    plain.resize(4, 4);
    check(!write_render_passes_pfm(plain, directory, "x_", 1.0F, &error), "no passes: refuses");
    std::filesystem::remove_all(directory);
}

} // namespace

int main() {
    test_sum_and_shadow();
    test_depth_and_normal();
    test_fog();
    test_fog_height();
    test_pfm();
    if (g_failures == 0) std::printf("dve_render_passes_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
