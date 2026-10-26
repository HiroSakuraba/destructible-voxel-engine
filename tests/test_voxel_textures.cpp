// Texture sets on voxel materials (dve/voxel_textures.hpp):
//   - mip chains: sizes down to 1 x 1; colour averages in linear light, data averages raw bytes;
//   - sampling: texel centres exact, constant images constant, trilinear between levels;
//   - the voxel renderer: base colour, occlusion, emission and roughness maps change exactly what
//     they should; far away a fine checker filters to its mean instead of aliasing;
//   - polygons made from voxels and the DVOX format (v1.10 only with maps) carry the same maps.
#include "dve/dvox.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/render/voxel_reference_renderer.hpp"
#include "dve/transform.hpp"
#include "dve/voxel_textures.hpp"

#include <array>
#include <cmath>
#include <cstdio>
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
float srgb(float c) { return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F); }

std::vector<std::uint8_t> constant(std::uint32_t w, std::uint32_t h, std::array<std::uint8_t, 4> c) {
    std::vector<std::uint8_t> t;
    for (std::uint32_t i = 0; i < w * h; ++i) t.insert(t.end(), c.begin(), c.end());
    return t;
}
std::vector<std::uint8_t> checker(std::uint32_t n) {
    std::vector<std::uint8_t> t;
    for (std::uint32_t y = 0; y < n; ++y)
        for (std::uint32_t x = 0; x < n; ++x) {
            const std::uint8_t v = ((x + y) & 1U) ? 255 : 0;
            t.insert(t.end(), {v, v, v, 255});
        }
    return t;
}

void test_mips_and_sampling() {
    const auto colour = make_voxel_image("checker", 16, 16, checker(16), VoxelImageRole::Color);
    const auto data = make_voxel_image("checker", 16, 16, checker(16), VoxelImageRole::Data);
    check(colour && colour->mips.size() == 4U && colour->mips.back().width == 1U, "a 16 x 16 image has 4 reduced levels down to 1 x 1");
    // Level 1 of a 1-texel checker: colour is the linear mean (0.5 -> sRGB 188), data the raw mean (128).
    std::printf("checker mip 1: colour byte %u (sRGB of linear 0.5 = 188), data byte %u (128)\n", colour->mips[0].rgba8[0],
                data->mips[0].rgba8[0]);
    check(colour->mips[0].rgba8[0] == 188U && data->mips[0].rgba8[0] == 128U, "colour mips average light, data mips average bytes");
    check(colour->contentHash != data->contentHash, "the role is part of the content hash");
    // Texel centres are exact; constant images are constant; levels blend trilinearly.
    const auto flat = make_voxel_image("grey", 8, 4, constant(8, 4, {64, 128, 192, 255}), VoxelImageRole::Data);
    const Float4 a = sample_voxel_image(*flat, 0.37F, 0.81F, 1.6F);
    check(std::abs(a.x - 64 / 255.0F) < 1e-6F && std::abs(a.z - 192 / 255.0F) < 1e-6F, "a constant image samples constant at any level");
    const Float4 centre = sample_voxel_image(*data, 2.5F / 16.0F, 5.5F / 16.0F, 0.0F);
    check(centre.x == 1.0F, "a texel centre returns that texel");   // (2 + 5) odd: white
    const Float4 l0 = sample_voxel_image(*data, 0.3F, 0.3F, 2.0F), l1 = sample_voxel_image(*data, 0.3F, 0.3F, 3.0F),
                 mid = sample_voxel_image(*data, 0.3F, 0.3F, 2.25F);
    check(std::abs(mid.x - (0.75F * l0.x + 0.25F * l1.x)) < 1e-6F, "fractional levels blend the two levels");
}

struct Floor {
    VoxelObject object{1U};
    std::array<VoxelMaterialDefinition, 2> materials{};
    explicit Floor(const VoxelMaterialDefinition& m) {
        materials[1] = m;
        for (int x = -60; x < 60; ++x)
            for (int z = -60; z < 600; ++z) object.set_voxel({x, 0, z}, 1);
    }
};
VoxelMaterialDefinition matte() {
    VoxelMaterialDefinition m;
    m.baseColor = {0.8F, 0.8F, 0.8F, 1.0F};
    m.specular = 0.0F;
    return m;
}
RenderEnvironment sun_only() {
    RenderEnvironment e;
    e.sunDirection = normalize(Float3{0.3F, 0.9F, 0.2F});
    e.sunColor = {1, 1, 1};
    e.sunIntensity = 2.0F;
    e.shadowMode = ShadowMode::Off;
    e.globalIlluminationMode = GlobalIlluminationMode::Off;
    e.ambientOcclusionSamples = 0;
    return e;
}
PolygonRenderTarget draw(const VoxelMaterialDefinition& m, const RenderEnvironment& e, const PolygonCamera& camera, std::uint32_t w = 32,
                         std::uint32_t h = 32) {
    Floor floor(m);
    const std::vector<VoxelReferenceInstance> instances{{1U, &floor.object, {}, floor.materials, true, {1, 1, 1}}};
    PolygonRenderTarget target;
    target.resize(w, h);
    target.enable_passes();
    ReferenceVoxelRenderer renderer;
    renderer.threadCount = 2;
    (void)renderer.render(instances, camera, e, target);
    return target;
}
PolygonCamera down(float height) {
    PolygonCamera c;
    c.position = {0.3F, 1.0F + height, 0.2F};
    c.target = {0.3F, 1.0F, 0.21F};
    c.up = {0, 0, 1};
    c.verticalFieldOfViewRadians = 0.5F;
    c.nearPlane = 0.1F;
    c.farPlane = 2000.0F;
    return c;
}

void test_renderer() {
    const RenderEnvironment e = sun_only();
    const float cosine = normalize(e.sunDirection).y;
    const std::size_t centre = 16U * 32U + 16U;
    // Base colour map: sRGB 128 -> linear 0.2158, times the material's 0.8.
    VoxelMaterialDefinition tinted = matte();
    tinted.baseColorMap = make_voxel_image("grey", 4, 4, constant(4, 4, {128, 128, 128, 255}), VoxelImageRole::Color);
    const PolygonRenderTarget a = draw(tinted, e, down(10.0F));
    const double want = 0.8 * srgb(128 / 255.0F) * 2.0 * cosine;
    std::printf("base colour map: pixel %.5f, expected %.5f\n", a.hdrColor[centre].x, want);
    check(std::abs(a.hdrColor[centre].x - want) < 1e-5 * want, "the base colour map multiplies the material colour (decoded from sRGB)");

    // Occlusion map (R = 0.5): halves the ambient light. No sun, uniform sky.
    RenderEnvironment ambient = e;
    ambient.sunIntensity = 0.0F;
    ambient.globalIlluminationMode = GlobalIlluminationMode::AmbientHemisphere;
    ambient.globalIlluminationIntensity = 1.0F;
    ambient.skyColor = ambient.groundColor = {0.5F, 0.5F, 0.5F};
    VoxelMaterialDefinition occluded = matte();
    const PolygonRenderTarget open = draw(occluded, ambient, down(10.0F));
    occluded.ormMap = make_voxel_image("orm", 4, 4, constant(4, 4, {128, 255, 255, 255}), VoxelImageRole::Data);
    const PolygonRenderTarget half = draw(occluded, ambient, down(10.0F));
    std::printf("occlusion map 0.502: ambient pixel %.5f of %.5f\n", half.hdrColor[centre].x, open.hdrColor[centre].x);
    check(std::abs(half.hdrColor[centre].x - open.hdrColor[centre].x * 128.0F / 255.0F) < 1e-5F, "the occlusion map scales ambient light");

    // Emission map: emissive x linear(map), in the emission pass.
    VoxelMaterialDefinition glowing = matte();
    glowing.emissive = {2.0F, 1.0F, 0.5F};
    glowing.emissiveMap = make_voxel_image("glow", 4, 4, constant(4, 4, {255, 64, 255, 255}), VoxelImageRole::Color);
    const PolygonRenderTarget g = draw(glowing, e, down(10.0F));
    const Float3 emission = g.passes.emission[centre];
    check(std::abs(emission.x - 2.0F) < 1e-5F && std::abs(emission.y - srgb(64 / 255.0F)) < 1e-5F && std::abs(emission.z - 0.5F) < 1e-5F,
          "the emission map multiplies the emissive colour");

    // Roughness map: G scales roughness; a rougher floor spreads the sun's highlight.
    VoxelMaterialDefinition glossy = matte();
    glossy.specular = 0.5F;
    glossy.roughness = 0.6F;
    VoxelMaterialDefinition rougher = glossy;
    rougher.ormMap = make_voxel_image("orm", 4, 4, constant(4, 4, {255, 64, 255, 255}), VoxelImageRole::Data);   // 0.6 x 0.25
    const PolygonRenderTarget smooth = draw(rougher, e, down(10.0F)), base = draw(glossy, e, down(10.0F));
    check(smooth.passes.directSpecular[centre].x != base.passes.directSpecular[centre].x, "the roughness map changes the highlight");

    // Mip filtering: a 1-texel checker tiled every 0.05 m (64 texels: 0.8 mm each) seen from far away
    // filters to its mean instead of aliasing to black or white.
    VoxelMaterialDefinition fine = matte();
    fine.baseColorMap = make_voxel_image("checker", 64, 64, checker(64), VoxelImageRole::Color);
    fine.textureTileMeters = 0.05F;
    const PolygonRenderTarget far = draw(fine, e, down(400.0F));
    const double mean = 0.8 * 0.5 * 2.0 * cosine;
    double worst = 0.0;
    std::size_t floorPixels = 0;
    for (std::size_t i = 0; i < far.hdrColor.size(); ++i) {
        if (far.objectId[i] != 1U) continue;
        ++floorPixels;
        worst = std::max(worst, std::abs(far.hdrColor[i].x - mean) / mean);
    }
    check(floorPixels > 200U, "the far view sees the floor");
    std::printf("fine checker from 40 m: worst pixel off the mean by %.4f\n", worst);
    check(worst < 0.02, "far away the checker filters to its mean");
    const PolygonRenderTarget near = draw(fine, e, down(0.3F));
    double low = 1e9, high = 0.0;
    for (const Float4& c : near.hdrColor) { low = std::min(low, static_cast<double>(c.x)); high = std::max(high, static_cast<double>(c.x)); }
    check(high > 1.8 * mean && low < 0.2 * mean, "close up the checker stays sharp");
}

void test_polygon_from_voxels() {
    // A quad whose material carries a voxel texture set (as meshes made from voxels do).
    VoxelMaterialDefinition m = matte();
    m.baseColorMap = make_voxel_image("grey", 4, 4, constant(4, 4, {128, 128, 128, 255}), VoxelImageRole::Color);
    CookedPolygonAsset quad;
    quad.objectId = 9;
    quad.materials.push_back(m);
    quad.materialBindings.resize(1);
    for (const Float3 q : {Float3{-5, 1, -5}, Float3{-5, 1, 5}, Float3{5, 1, 5}, Float3{5, 1, -5}})
        quad.vertices.push_back({q, {0, 1, 0}, {1, 0, 0, 1}, {0, 0}, {1, 1, 1, 1}, {0, 0}});
    quad.indices = {0, 1, 2, 0, 2, 3};
    quad.submeshes.push_back({"quad", 0, 6, 0});
    quad.bounds = {{-5, 0.99F, -5}, {5, 1.01F, 5}};
    const std::vector<PolygonRenderInstance> instances{{9U, &quad, {}, {}, {1, 1, 1, 1}, true}};
    PolygonRenderTarget target;
    target.resize(32, 32);
    PolygonRenderOptions options;
    options.preserveExistingDepth = false;
    const RenderEnvironment e = sun_only();
    (void)ReferencePolygonRenderer{}.render(instances, down(10.0F), e, target, options);
    const double want = 0.8 * srgb(128 / 255.0F) * 2.0 * normalize(e.sunDirection).y;
    check(std::abs(target.hdrColor[16U * 32U + 16U].x - want) < 1e-4 * want, "polygons carrying voxel maps sample them too");
}

void test_dvox(const fs::path& dir) {
    CookedVoxelAsset plain(31);
    plain.materials.resize(2);
    plain.object.set_voxel({0, 0, 0}, 1);
    std::string error;
    check(write_dvox(dir / "plain.dvox", plain, {}, &error), "plain dvox: " + error);
    CookedVoxelAsset mapped(32);
    mapped.materials = plain.materials;
    mapped.object.set_voxel({0, 0, 0}, 1);
    mapped.materials[1].baseColorMap = make_voxel_image("albedo", 8, 8, checker(8), VoxelImageRole::Color);
    mapped.materials[1].heightMap = make_voxel_image("height", 4, 2, constant(4, 2, {10, 20, 30, 40}), VoxelImageRole::Data);
    mapped.materials[1].textureTileMeters = 0.25F;
    mapped.materials[1].textureSharpness = 8.0F;
    check(write_dvox(dir / "mapped.dvox", mapped, {}, &error), "mapped dvox: " + error);
    const auto minor = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        in.seekg(6);
        unsigned char b[2]{};
        in.read(reinterpret_cast<char*>(b), 2);
        return b[0] | (b[1] << 8U);
    };
    check(minor(dir / "plain.dvox") < 10 && minor(dir / "mapped.dvox") == 10, "texture sets need DVOX v1.10; files without them keep their version");
    const auto back = read_dvox(dir / "mapped.dvox");
    check(back.success, "mapped read: " + back.error);
    if (back.success) {
        const VoxelMaterialDefinition& m = back.asset.materials[1];
        check(m.baseColorMap && m.heightMap && !m.ormMap && !m.emissiveMap && m.baseColorMap->rgba8 == mapped.materials[1].baseColorMap->rgba8 &&
                  m.baseColorMap->contentHash == mapped.materials[1].baseColorMap->contentHash &&
                  m.heightMap->contentHash == mapped.materials[1].heightMap->contentHash && m.textureTileMeters == 0.25F &&
                  m.textureSharpness == 8.0F && m.baseColorMap->mips.size() == 3U,
              "DVOX texture-set round trip (maps, roles, projection, mips rebuilt)");
    }
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "dve_voxel_textures_test";
    fs::create_directories(dir);
    test_mips_and_sampling();
    test_renderer();
    test_polygon_from_voxels();
    test_dvox(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures == 0) std::printf("dve_voxel_textures_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
