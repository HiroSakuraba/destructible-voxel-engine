// Rule-driven layer masks and texture anti-tiling:
//   - the rule factor: disabled is 1; slope, height and occlusion bands are 0.5 exactly at their
//     thresholds; edges need edge information; base height and noise scale the band but never
//     leave it;
//   - voxel exposed-edge distances;
//   - on voxels (coverage read exactly from the emission pass: base emits blue, the layer red):
//     a uniform layer covers by its weight, snow covers what faces up and nothing vertical, a
//     height band picks the raised block, edge wear follows the analytic distance to the block's
//     top edges, cavity dirt collects at the foot of the block, and base-height fill favours the
//     cracks of a cellular base;
//   - anti-tiling: off is the plain sample; tiles differ but stay continuous; colour, ORM and
//     height stay aligned, and traced displacement reads the same anti-tiled height;
//   - DVOX 1.13 and DMESH 1.10 carry the rules and anti-tiling.
#include "dve/dvox.hpp"
#include "dve/material_layer_rules.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/render/voxel_reference_renderer.hpp"
#include "dve/transform.hpp"
#include "dve/voxel_displacement.hpp"
#include "dve/voxel_textures.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
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
bool near(float a, float b, float e) { return std::abs(a - b) <= e; }
Float3 cross3(Float3 a, Float3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

void test_factor() {
    MaterialLayerRules r;
    MaterialLayerRuleContext c;
    check(material_layer_rule_factor(r, c) == 1.0F, "disabled rules give 1");
    r.enabled = true;
    check(material_layer_rule_factor(r, c) == 1.0F, "enabled rules with open bands give 1");
    // Slope: snow above cos 0.7, 0.2 wide.
    r.slopeMinCos = 0.7F;
    r.softness = 0.2F;
    c.worldNormal = {0, 1, 0};
    check(material_layer_rule_factor(r, c) == 1.0F, "up-facing is covered");
    c.worldNormal = {1, 0, 0};
    check(material_layer_rule_factor(r, c) == 0.0F, "vertical is bare");
    c.worldNormal = {std::sqrt(1.0F - 0.49F), 0.7F, 0.0F};
    check(near(material_layer_rule_factor(r, c), 0.5F, 1e-5F), "coverage is 0.5 exactly at the slope threshold");
    // Height band [2, 5] m along up, 1 m soft.
    r = MaterialLayerRules{};
    r.enabled = true;
    r.heightMinMeters = 2.0F;
    r.heightMaxMeters = 5.0F;
    r.heightSoftnessMeters = 1.0F;
    c = MaterialLayerRuleContext{};
    c.worldPositionMeters = {0, 3.5F, 0};
    check(material_layer_rule_factor(r, c) == 1.0F, "inside the height band");
    c.worldPositionMeters = {0, 2.0F, 0};
    check(near(material_layer_rule_factor(r, c), 0.5F, 1e-5F), "0.5 at the lower height threshold");
    c.worldPositionMeters = {0, 5.0F, 0};
    check(near(material_layer_rule_factor(r, c), 0.5F, 1e-5F), "0.5 at the upper height threshold");
    c.worldPositionMeters = {0, 6.0F, 0};
    check(material_layer_rule_factor(r, c) == 0.0F, "above the band");
    r.up = {0, 0, 2};   // a Z-up world: only the up vector's direction matters
    c.worldPositionMeters = {0, 6.0F, 3.5F};
    check(material_layer_rule_factor(r, c) == 1.0F, "height follows the up vector");
    // Occlusion: dirt where AO < 0.6.
    r = MaterialLayerRules{};
    r.enabled = true;
    r.occlusionMax = 0.6F;
    r.softness = 0.1F;
    c = MaterialLayerRuleContext{};
    c.ambientOcclusion = 0.3F;
    check(material_layer_rule_factor(r, c) == 1.0F, "occluded is dirty");
    c.ambientOcclusion = 0.6F;
    check(near(material_layer_rule_factor(r, c), 0.5F, 1e-5F), "0.5 at the occlusion threshold");
    c.ambientOcclusion = 1.0F;
    check(material_layer_rule_factor(r, c) == 0.0F, "open is clean");
    // Edges.
    r = MaterialLayerRules{};
    r.enabled = true;
    r.edgeWidthMeters = 0.02F;
    c = MaterialLayerRuleContext{};
    check(material_layer_rule_factor(r, c) == 0.0F, "no edge information: no edge wear");
    c.edgeDistanceMeters = 0.0F;
    check(material_layer_rule_factor(r, c) == 1.0F, "on the edge");
    c.edgeDistanceMeters = 0.01F;
    check(near(material_layer_rule_factor(r, c), 0.5F, 1e-5F), "half way: 0.5");
    c.edgeDistanceMeters = 0.03F;
    check(material_layer_rule_factor(r, c) == 0.0F, "beyond the width");
    // Base height and noise scale the band and never reach outside it.
    r = MaterialLayerRules{};
    r.enabled = true;
    r.baseHeightInfluence = 0.5F;
    c = MaterialLayerRuleContext{};
    c.baseHeight = 1.0F;
    check(near(material_layer_rule_factor(r, c), 0.5F, 1e-6F), "high base: 1 - influence");
    c.baseHeight = 0.0F;
    check(material_layer_rule_factor(r, c) == 1.0F, "low base: full");
    r.slopeMinCos = 0.5F;
    r.softness = 0.0F;
    c.worldNormal = {1, 0, 0};
    check(material_layer_rule_factor(r, c) == 0.0F, "the shift never covers outside the band");
    r = MaterialLayerRules{};
    r.enabled = true;
    r.noiseAmount = 0.5F;
    r.noiseScaleMeters = 0.5F;
    float lo = 1.0F, hi = 0.0F;
    for (int i = 0; i < 400; ++i) {
        c = MaterialLayerRuleContext{};
        c.worldPositionMeters = {static_cast<float>(i) * 0.037F, 0.3F, static_cast<float>(i % 17) * 0.11F};
        const float f = material_layer_rule_factor(r, c);
        lo = std::min(lo, f);
        hi = std::max(hi, f);
    }
    check(lo < 0.85F && hi == 1.0F && lo >= 0.0F, "noise breaks the coverage up");
    // Validation.
    r = MaterialLayerRules{};
    r.slopeMinCos = 0.8F;
    r.slopeMaxCos = 0.2F;
    check(!material_layer_rules_valid(r), "an empty slope band is invalid");
    r = MaterialLayerRules{};
    r.up = {0, 0, 0};
    check(!material_layer_rules_valid(r), "a zero up vector is invalid");
}

void test_edges() {
    // +x and +y exposed: the edge is the line x = 1, y = 1.
    check(near(voxel_exposed_edge_distance({0.9F, 0.95F, 0.5F}, 2U | 8U), std::sqrt(0.01F + 0.0025F), 1e-6F), "distance to the +x/+y edge");
    check(near(voxel_exposed_edge_distance({0.9F, 1.0F, 0.5F}, 2U | 8U), 0.1F, 1e-6F), "on the top face: the distance to the other face");
    check(std::isinf(voxel_exposed_edge_distance({0.5F, 1.0F, 0.5F}, 8U)), "one exposed face has no edge");
    check(std::isinf(voxel_exposed_edge_distance({0.5F, 1.0F, 0.5F}, 4U | 8U)), "opposite faces make no edge");
}

// --- On voxels ---------------------------------------------------------------------------------

constexpr float kMetersPerVoxel = 0.1F;   // the reference renderer's default

VoxelMaterialDefinition emitter(Float3 colour) {
    VoxelMaterialDefinition m;
    m.baseColor = {0, 0, 0, 1};
    m.specular = 0.0F;
    m.emissive = colour;
    return m;
}
struct Scene {
    VoxelObject object{1U};
    std::array<VoxelMaterialDefinition, 3> materials{};
    std::vector<VoxelReferenceInstance> instances;
    // A floor (y <= 0) and a 6 x 4 x 6 block on it from (0, 1, 0).
    explicit Scene(const VoxelMaterialLayer& layer, const VoxelMaterialDefinition& base = emitter({0, 0, 1})) {
        materials[1] = base;
        materials[1].layers = {layer};
        materials[2] = emitter({1, 0, 0});
        for (int x = -20; x < 26; ++x)
            for (int z = -20; z < 26; ++z)
                for (int y = -2; y <= 0; ++y) object.set_voxel({x, y, z}, 1);
        for (int x = 0; x < 6; ++x)
            for (int z = 0; z < 6; ++z)
                for (int y = 1; y <= 4; ++y) object.set_voxel({x, y, z}, 1);
        instances.push_back({1U, &object, {}, materials, true, {1, 1, 1}});
    }
};
struct Frame {
    PolygonRenderTarget target;
    PolygonCamera camera;
    VoxelReferenceRenderStats stats;
    // World position (voxel units) under a pixel, from the depth pass.
    Float3 position(std::uint32_t x, std::uint32_t y) const {
        const Float3 forward = normalize(subtract(camera.target, camera.position));
        const Float3 right = normalize(cross3(forward, camera.up));
        const Float3 up = normalize(cross3(right, forward));
        const float t = std::tan(camera.verticalFieldOfViewRadians * 0.5F);
        const float aspect = static_cast<float>(target.width) / static_cast<float>(target.height);
        const float nx = (2.0F * (static_cast<float>(x) + 0.5F) / static_cast<float>(target.width) - 1.0F) * t * aspect;
        const float ny = (1.0F - 2.0F * (static_cast<float>(y) + 0.5F) / static_cast<float>(target.height)) * t;
        const Float3 ray = add(forward, add(multiply(right, nx), multiply(up, ny)));
        return add(camera.position, multiply(ray, target.passes.depthMeters[y * target.width + x] / kMetersPerVoxel));
    }
    float coverage(std::size_t i) const { return target.passes.emission[i].x; }
};
Frame draw(const Scene& scene, Float3 eye, Float3 at, std::uint32_t size = 64, std::uint32_t aoSamples = 0U) {
    Frame f;
    f.camera.position = eye;
    f.camera.target = at;
    f.camera.up = {0, 0, 1};
    if (std::abs(normalize(subtract(at, eye)).z) > 0.9F) f.camera.up = {0, 1, 0};
    f.camera.verticalFieldOfViewRadians = 0.6F;
    f.camera.nearPlane = 0.1F;
    f.camera.farPlane = 500.0F;
    RenderEnvironment e;
    e.sunIntensity = 0.0F;
    e.shadowMode = ShadowMode::Off;
    e.globalIlluminationMode = aoSamples ? GlobalIlluminationMode::AmbientHemisphere : GlobalIlluminationMode::Off;
    e.ambientOcclusionSamples = aoSamples;
    e.ambientOcclusionMaxDistanceMeters = 0.5F;
    e.skyColor = {0, 0, 0};
    e.groundColor = {0, 0, 0};
    f.target.resize(size, size);
    f.target.enable_passes();
    ReferenceVoxelRenderer renderer;
    renderer.threadCount = 2;
    f.stats = renderer.render(scene.instances, f.camera, e, f.target);
    return f;
}
VoxelMaterialLayer layer_of(float weight) {
    VoxelMaterialLayer l;
    l.sourceMaterial = 2;
    l.weight = weight;
    return l;
}

void test_voxels() {
    const Float3 eye{-9.0F, 14.0F, -7.0F}, at{3.0F, 2.0F, 3.0F};
    // A uniform layer (no rules): coverage = weight everywhere, exactly.
    {
        const Frame f = draw(Scene(layer_of(0.3F)), eye, at);
        float worst = 0.0F;
        std::size_t drawn = 0;
        for (std::size_t i = 0; i < f.target.objectId.size(); ++i) {
            if (f.target.objectId[i] != 1U) continue;
            ++drawn;
            worst = std::max({worst, std::abs(f.coverage(i) - 0.3F), std::abs(f.target.passes.emission[i].z - 0.7F)});
        }
        check(drawn > 2000 && worst < 1e-5F && f.stats.layeredSurfaces == drawn, "a uniform layer covers by its weight on voxels");
    }
    // Snow by slope.
    {
        VoxelMaterialLayer snow = layer_of(1.0F);
        snow.rules.enabled = true;
        snow.rules.slopeMinCos = 0.7F;
        const Frame f = draw(Scene(snow), eye, at);
        std::size_t tops = 0, sides = 0;
        bool exact = true;
        for (std::size_t i = 0; i < f.target.objectId.size(); ++i) {
            if (f.target.objectId[i] != 1U) continue;
            const float ny = f.target.passes.normal[i].y;
            if (ny > 0.99F) { ++tops; exact = exact && f.coverage(i) == 1.0F; }
            if (std::abs(ny) < 0.01F) { ++sides; exact = exact && f.coverage(i) == 0.0F; }
        }
        check(tops > 1000 && sides > 200 && exact, "snow covers what faces up and none of the walls");
    }
    // Height band: only the block's top (y = 5 voxels = 0.5 m) lies above 0.3 m.
    {
        VoxelMaterialLayer band = layer_of(1.0F);
        band.rules.enabled = true;
        band.rules.heightMinMeters = 0.3F;
        band.rules.heightSoftnessMeters = 0.0F;
        const Frame f = draw(Scene(band), eye, at);
        bool exact = true;
        std::size_t raised = 0;
        for (std::uint32_t y = 0; y < 64; ++y)
            for (std::uint32_t x = 0; x < 64; ++x) {
                const std::size_t i = y * 64U + x;
                if (f.target.objectId[i] != 1U) continue;
                const float h = f.position(x, y).y * kMetersPerVoxel;
                if (std::abs(h - 0.3F) < 0.01F) continue;
                exact = exact && f.coverage(i) == (h > 0.3F ? 1.0F : 0.0F);
                raised += h > 0.3F ? 1U : 0U;
            }
        check(raised > 300 && exact, "a height band picks the raised block");
    }
    // Edge wear along the block's top edges: coverage = smoothstep(1 - d / w), with d the
    // distance to the top face's edges.
    {
        VoxelMaterialLayer wear = layer_of(1.0F);
        wear.rules.enabled = true;
        wear.rules.edgeWidthMeters = 0.08F;   // within a voxel: edges are found per cell
        const Frame f = draw(Scene(wear), {3.0F, 16.0F, 3.0F}, {3.0F, 5.0F, 3.0F}, 96);
        float worst = 0.0F;
        std::size_t samples = 0, worn = 0;
        for (std::uint32_t y = 0; y < 96; ++y)
            for (std::uint32_t x = 0; x < 96; ++x) {
                const std::size_t i = y * 96U + x;
                if (f.target.objectId[i] != 1U || f.target.passes.normal[i].y < 0.99F) continue;
                const Float3 p = f.position(x, y);
                if (std::abs(p.y - 5.0F) > 1e-2F || p.x < 0.02F || p.x > 5.98F || p.z < 0.02F || p.z > 5.98F) continue;   // block top
                const float d = std::min({p.x, 6.0F - p.x, p.z, 6.0F - p.z}) * kMetersPerVoxel;
                const float s = std::clamp(1.0F - d / 0.08F, 0.0F, 1.0F);
                worst = std::max(worst, std::abs(f.coverage(i) - s * s * (3.0F - 2.0F * s)));
                ++samples;
                worn += f.coverage(i) > 0.5F ? 1U : 0U;
            }
        std::printf("edge wear: %zu block-top pixels, %zu worn, worst error %.5f\n", samples, worn, worst);
        check(samples > 1500 && worn > 100 && worst < 2e-3F, "edge wear follows the distance to the exposed top edges");
    }
    // Cavity dirt: where AO < 0.8, so at the foot of the block and not on open floor.
    {
        VoxelMaterialLayer dirt = layer_of(1.0F);
        dirt.rules.enabled = true;
        dirt.rules.occlusionMax = 0.8F;
        dirt.rules.softness = 0.1F;
        const Frame f = draw(Scene(dirt), eye, at, 64, 32U);
        double foot = 0.0, open = 0.0;
        std::size_t nFoot = 0, nOpen = 0;
        for (std::uint32_t y = 0; y < 64; ++y)
            for (std::uint32_t x = 0; x < 64; ++x) {
                const std::size_t i = y * 64U + x;
                if (f.target.objectId[i] != 1U || f.target.passes.normal[i].y < 0.99F) continue;
                const Float3 p = f.position(x, y);
                if (std::abs(p.y - 1.0F) > 1e-2F) continue;   // floor top
                const float gap = std::max({-p.x, p.x - 6.0F, -p.z, p.z - 6.0F});   // distance outside the block's footprint
                if (gap > 0.0F && gap < 0.5F) { foot += f.coverage(i); ++nFoot; }
                if (gap > 6.0F) { open += f.coverage(i); ++nOpen; }
            }
        const double footMean = nFoot ? foot / static_cast<double>(nFoot) : 0.0, openMean = nOpen ? open / static_cast<double>(nOpen) : 1.0;
        std::printf("cavity dirt: %.3f at the foot of the block (%zu px), %.3f on open floor (%zu px)\n", footMean, nFoot, openMean, nOpen);
        check(nFoot > 20 && nOpen > 50 && footMean > 0.5 && openMean < 0.05, "cavity dirt collects at the foot of the block");
    }
    // Base-height fill on a cellular base: coverage = clamp(1 + (1 - 2h)) x 0.5, so the cracks
    // (low h) fill first.
    {
        VoxelMaterialDefinition cobble = emitter({0, 0, 1});
        cobble.surfaceDetail.pattern = SurfacePattern::Cellular;
        cobble.surfaceDetail.periodMeters = 0.3F;
        cobble.surfaceDetail.grainStretch = 8.0F;
        cobble.surfaceDetail.colorVariation = 0.0F;
        cobble.surfaceDetail.roughnessVariation = 0.0F;
        cobble.surfaceDetail.bumpHeightMeters = 0.0F;
        VoxelMaterialLayer water = layer_of(0.5F);
        water.rules.enabled = true;
        water.rules.baseHeightInfluence = 1.0F;
        const Frame f = draw(Scene(water, cobble), {-10.0F, 12.0F, -10.0F}, {-8.0F, 1.0F, -8.0F});
        float worst = 0.0F;
        std::size_t samples = 0;
        for (std::uint32_t y = 0; y < 64; ++y)
            for (std::uint32_t x = 0; x < 64; ++x) {
                const std::size_t i = y * 64U + x;
                if (f.target.objectId[i] != 1U || f.target.passes.normal[i].y < 0.99F) continue;
                const Float3 p = f.position(x, y);
                if (std::abs(p.y - 1.0F) > 1e-2F || p.x > -1.0F || p.z > -1.0F) continue;
                // The renderer filters the pattern by the pixel's footprint on the surface.
                const float footprint = length(subtract(f.camera.position, p)) * kMetersPerVoxel * 2.0F *
                                        std::tan(f.camera.verticalFieldOfViewRadians * 0.5F) / 64.0F;
                const float h = surface_pattern_height(cobble.surfaceDetail, multiply(Float3{p.x, 1.0F, p.z}, kMetersPerVoxel), footprint);
                const float expected = 0.5F * std::clamp(2.0F - 2.0F * h, 0.0F, 1.0F);
                worst = std::max(worst, std::abs(f.coverage(i) - expected));
                ++samples;
            }
        std::printf("base-height fill: %zu pixels, worst error %.4f\n", samples, worst);
        check(samples > 1000 && worst < 0.03F, "base-height fill follows the base pattern's height (cracks first)");
    }
}

// --- Anti-tiling -------------------------------------------------------------------------------

std::shared_ptr<const VoxelNormalMap> noise_image(VoxelImageRole role) {
    std::vector<std::uint8_t> t;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            // Tileable: whole periods across the image.
            const double v = 0.5 + 0.25 * std::sin(6.2831853 * x / 32.0) + 0.2 * std::cos(6.2831853 * 2.0 * y / 32.0);
            const auto b = static_cast<std::uint8_t>(std::clamp(v, 0.0, 1.0) * 255.0);
            t.insert(t.end(), {b, b, b, 255});
        }
    return make_voxel_image("noise", 32, 32, t, role);
}

void test_anti_tiling() {
    const auto image = noise_image(VoxelImageRole::Data);
    bool same = true;
    for (int i = 0; i < 50; ++i) {
        const float u = 0.173F * static_cast<float>(i), v = 0.091F * static_cast<float>(i);
        same = same && sample_voxel_image_untiled(*image, u, v, 0.0F, 0.0F, 1).x == sample_voxel_image(*image, u, v, 0.0F).x;
    }
    check(same, "anti-tiling off is the plain sample");
    // Tile centres differ between tiles (the plain repeat is identical).
    int differ = 0;
    for (int t = 1; t < 9; ++t) differ += std::abs(sample_voxel_image_untiled(*image, 0.5F + static_cast<float>(t), 0.5F, 0.0F, 0.2F, 1).x -
                                                   sample_voxel_image_untiled(*image, 0.5F, 0.5F, 0.0F, 0.2F, 1).x) > 0.02F
                                              ? 1 : 0;
    check(differ >= 6, "anti-tiled tiles differ (" + std::to_string(differ) + "/8)");
    // Continuous across tile edges: small steps change it little.
    float jump = 0.0F, plainJump = 0.0F;
    const float step = 1.0F / 512.0F;
    for (int i = 0; i < 4096; ++i) {
        const float u = static_cast<float>(i) * step, v = 0.37F;
        jump = std::max(jump, std::abs(sample_voxel_image_untiled(*image, u + step, v, 0.0F, 0.2F, 1).x -
                                       sample_voxel_image_untiled(*image, u, v, 0.0F, 0.2F, 1).x));
        plainJump = std::max(plainJump, std::abs(sample_voxel_image(*image, u + step, v, 0.0F).x - sample_voxel_image(*image, u, v, 0.0F).x));
    }
    std::printf("anti-tiling: largest step %.4f (plain %.4f) over 1/512 tile\n", jump, plainJump);
    check(jump < 6.0F * plainJump + 0.01F, "anti-tiling stays continuous across tile edges");

    // Colour, ORM and height share the offsets; traced displacement reads the same height.
    VoxelMaterialDefinition m;
    m.ormMap = image;
    m.heightMap = image;
    m.textureTileMeters = 0.2F;
    m.textureAntiTiling = 0.25F;
    m.displacementDepthMeters = 0.03F;
    bool aligned = true, traced = true;
    for (int i = 0; i < 64; ++i) {
        const Float3 p{0.0137F * static_cast<float>(i), 0.3F, 0.0291F * static_cast<float>(i)};
        const VoxelTextureSample s = sample_voxel_textures(m, p, {0, 1, 0}, 0.0F);
        aligned = aligned && std::abs(s.occlusion - s.height) < 1e-6F;
        traced = traced && std::abs(voxel_face_height(m, p, 1, 0.0F) - s.height) < 1e-6F;
    }
    check(aligned, "anti-tiled maps stay aligned");
    check(traced, "traced displacement reads the anti-tiled height");
    m.textureAntiTiling = 0.6F;
    check(!material_rules_valid(m), "anti-tiling above 0.5 is invalid");
}

// --- Formats ---------------------------------------------------------------------------------

unsigned file_minor(const fs::path& path, std::streamoff offset) {
    std::ifstream in(path, std::ios::binary);
    in.seekg(offset);
    unsigned char b[2]{};
    in.read(reinterpret_cast<char*>(b), 2);
    return b[0] | (b[1] << 8U);
}
MaterialLayerRules sample_rules() {
    MaterialLayerRules r;
    r.enabled = true;
    r.seed = 9;
    r.up = {0, 0, 1};
    r.slopeMinCos = 0.6F;
    r.heightMinMeters = -3.0F;
    r.heightMaxMeters = 40.0F;
    r.occlusionMax = 0.7F;
    r.edgeWidthMeters = 0.05F;
    r.baseHeightInfluence = -0.4F;
    r.noiseScaleMeters = 2.5F;
    r.noiseAmount = 0.3F;
    r.softness = 0.15F;
    r.heightSoftnessMeters = 2.0F;
    return r;
}
bool same_rules(const MaterialLayerRules& a, const MaterialLayerRules& b) {
    return a.enabled == b.enabled && a.seed == b.seed && material_layer_rule_values(a) == material_layer_rule_values(b);
}

void test_formats(const fs::path& dir) {
    std::string error;
    CookedVoxelAsset asset(91);
    asset.materials.resize(3);
    asset.object.set_voxel({0, 0, 0}, 1);
    asset.materials[1].layers = {layer_of(0.8F), layer_of(0.4F)};
    asset.materials[1].layers[0].rules = sample_rules();
    asset.materials[1].textureAntiTiling = 0.3F;
    check(write_dvox(dir / "rules.dvox", asset, {}, &error), "rules dvox: " + error);
    check(file_minor(dir / "rules.dvox", 6) == 13, "layer rules need DVOX 1.13");
    const auto back = read_dvox(dir / "rules.dvox");
    check(back.success && back.asset.materials[1].layers.size() == 2 && same_rules(back.asset.materials[1].layers[0].rules, sample_rules()) &&
              !back.asset.materials[1].layers[1].rules.enabled && back.asset.materials[1].textureAntiTiling == 0.3F,
          "DVOX rules round trip: " + back.error);

    CookedPolygonAsset mesh;
    mesh.objectId = 4;
    mesh.materials.resize(3);
    mesh.materials[0].layers = {layer_of(1.0F)};
    mesh.materials[0].layers[0].rules = sample_rules();
    mesh.materialBindings.resize(3);
    mesh.vertices = {{{0, 0, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
                     {{1, 0, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
                     {{1, 1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 1}, {1, 1, 1, 1}}};
    mesh.indices = {0, 1, 2};
    mesh.submeshes.push_back({"tri", 0, 3, 0});
    mesh.bounds = {{0, 0, 0}, {1, 1, 0}};
    mesh.contentHash = polygon_asset_content_hash(mesh);
    check(write_dmesh(dir / "rules.dmesh", mesh, &error), "rules dmesh: " + error);
    check(file_minor(dir / "rules.dmesh", 10) == 10, "layer rules need DMESH 1.10");
    const auto meshBack = read_dmesh(dir / "rules.dmesh");
    check(static_cast<bool>(meshBack) && same_rules(meshBack.asset.materials[0].layers[0].rules, sample_rules()),
          "DMESH rules round trip: " + meshBack.error);
    CookedPolygonAsset plain = mesh;
    plain.materials[0].layers[0].rules = {};
    check(polygon_asset_content_hash(plain) != mesh.contentHash, "the rules are part of the content hash");
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "dve_layer_rules_test";
    fs::create_directories(dir);
    test_factor();
    test_edges();
    test_voxels();
    test_anti_tiling();
    test_formats(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures == 0) std::printf("dve_layer_rules_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
