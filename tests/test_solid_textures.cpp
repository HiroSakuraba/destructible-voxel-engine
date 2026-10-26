// Procedural solid textures (Marble, Strata, Cellular; dve/material_surface_detail.hpp):
//   - each pattern spans about two units and is centred on its mean (neutral on average);
//   - far away the pattern filters to that neutral 0, with no gradient left;
//   - the analytic gradient matches the value's change per metre (rotation and period included);
//   - character: marble veins thin as grainStretch grows, strata are layers of constant local y,
//     cellular stones have cracks between them, everything varies in 3D (cuts show it);
//   - as a height source: a cellular voxel carves cracks into its face (traced and parallax),
//     with no height map;
//   - formats: DVOX 1.12 and DMESH 1.9 carry them; DVOX 1.11 refuses them (the material library,
//     v12, is checked in test_material_optics).
#include "dve/dvox.hpp"
#include "dve/material_surface_detail.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/render/voxel_reference_renderer.hpp"
#include "dve/transform.hpp"
#include "dve/voxel_displacement.hpp"

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

MaterialSurfaceDetail pattern(SurfacePattern p, float period = 0.1F, float stretch = 20.0F) {
    MaterialSurfaceDetail d;
    d.pattern = p;
    d.periodMeters = period;
    d.grainStretch = stretch;
    d.seed = 5;
    d.bumpHeightMeters = 0.0F;
    return d;
}
// A deterministic spread of points over many periods.
std::vector<Float3> points(std::size_t n, float extent) {
    std::vector<Float3> out;
    std::uint32_t s = 12345U;
    const auto next = [&] { s = s * 1664525U + 1013904223U; return static_cast<float>(s >> 8) / 16777216.0F; };
    for (std::size_t i = 0; i < n; ++i) out.push_back({(next() - 0.5F) * extent, (next() - 0.5F) * extent, (next() - 0.5F) * extent});
    return out;
}

void test_statistics() {
    check(material_surface_detail_valid(pattern(SurfacePattern::Cellular)), "cellular is a valid pattern");
    MaterialSurfaceDetail bad = pattern(SurfacePattern::Cellular);
    bad.pattern = static_cast<SurfacePattern>(9);
    check(!material_surface_detail_valid(bad), "patterns past Cellular are invalid");
    const std::vector<Float3> where = points(20000, 5.0F);
    for (const SurfacePattern p : {SurfacePattern::Marble, SurfacePattern::Strata, SurfacePattern::Cellular}) {
        const MaterialSurfaceDetail d = pattern(p);
        double sum = 0.0;
        float lo = 1e9F, hi = -1e9F;
        for (const Float3& q : where) {
            const float v = sample_surface_pattern(d, q).value;
            sum += v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        const float mean = static_cast<float>(sum / static_cast<double>(where.size()));
        // Far away (a footprint of 100 periods) the pattern is its filtered mean.
        const SurfacePatternSample far = sample_surface_pattern(d, {0.31F, 0.17F, -0.05F}, 10.0F);
        std::printf("pattern %u: range [%.3f, %.3f], mean %.3f, far value %.3f\n", static_cast<unsigned>(p), lo, hi, mean, far.value);
        check(lo >= -2.0F && hi <= 2.0F && hi - lo > 1.5F, "pattern " + std::to_string(unsigned(p)) + " spans about two units");
        check(std::abs(mean) < 0.05F, "pattern " + std::to_string(unsigned(p)) + " is neutral on average (mean " + std::to_string(mean) + ")");
        check(far.value == 0.0F, "pattern " + std::to_string(unsigned(p)) + " filters to neutral far away");
        check(length(far.gradientPerMeter) < 1e-3F, "pattern " + std::to_string(unsigned(p)) + " has no gradient far away");
    }
}

void test_gradient() {
    for (const SurfacePattern p : {SurfacePattern::Marble, SurfacePattern::Strata, SurfacePattern::Cellular}) {
        MaterialSurfaceDetail d = pattern(p, 0.2F);
        d.rotationRadians = {0.3F, -0.6F, 0.2F};
        int good = 0, total = 0;
        for (const Float3& q : points(200, 1.0F)) {
            const SurfacePatternSample s = sample_surface_pattern(d, q);
            const float e = 2e-4F;   // metres: a thousandth of the period
            const Float3 numeric{(sample_surface_pattern(d, add(q, {e, 0, 0})).value - sample_surface_pattern(d, add(q, {-e, 0, 0})).value) / (2 * e),
                                 (sample_surface_pattern(d, add(q, {0, e, 0})).value - sample_surface_pattern(d, add(q, {0, -e, 0})).value) / (2 * e),
                                 (sample_surface_pattern(d, add(q, {0, 0, e})).value - sample_surface_pattern(d, add(q, {0, 0, -e})).value) / (2 * e)};
            ++total;
            // Kinks (vein crests, cell borders) make a few samples disagree; most must match.
            good += length(subtract(s.gradientPerMeter, numeric)) < 0.05F * (1.0F + length(numeric)) ? 1 : 0;
        }
        check(good >= total * 9 / 10, "pattern " + std::to_string(unsigned(p)) + " gradient matches its value (" + std::to_string(good) + "/" +
                                           std::to_string(total) + ")");
    }
}

void test_character() {
    const std::vector<Float3> where = points(20000, 5.0F);
    // Marble: the share of dark vein (value < 0) falls as grainStretch (vein sharpness) rises.
    const auto dark = [&](float stretch) {
        const MaterialSurfaceDetail d = pattern(SurfacePattern::Marble, 0.1F, stretch);
        // Centred values shift with the vein share, so count against the maximum (vein-free stone).
        float top = -1e9F;
        for (const Float3& q : where) top = std::max(top, sample_surface_pattern(d, q).value);
        std::size_t n = 0;
        for (const Float3& q : where) n += sample_surface_pattern(d, q).value < top - 1.0F ? 1U : 0U;
        return static_cast<double>(n) / static_cast<double>(where.size());
    };
    const double soft = dark(4.0F), sharp = dark(80.0F);
    std::printf("marble: vein share %.3f at stretch 4, %.3f at stretch 80\n", soft, sharp);
    check(soft > sharp * 2.0 && sharp > 0.005, "marble veins thin as grainStretch grows");

    // Strata: along local x the value changes far less than along local y (layers).
    const MaterialSurfaceDetail strata = pattern(SurfacePattern::Strata, 0.1F, 40.0F);
    double alongX = 0.0, alongY = 0.0;
    for (const Float3& q : points(4000, 5.0F)) {
        const float v = sample_surface_pattern(strata, q).value;
        alongX += std::abs(sample_surface_pattern(strata, add(q, {0.02F, 0, 0})).value - v);
        alongY += std::abs(sample_surface_pattern(strata, add(q, {0, 0.02F, 0})).value - v);
    }
    std::printf("strata: mean change over 2 cm along x %.4f, along y %.4f\n", alongX / 4000.0, alongY / 4000.0);
    check(alongY > 2.5 * alongX, "strata are layers of constant local y");

    // Cellular: cracks (value near -1) cover a share that grows with the crack width, and every
    // axis varies (a cut through the volume shows stones).
    const auto cracks = [&](float stretch) {
        const MaterialSurfaceDetail d = pattern(SurfacePattern::Cellular, 0.1F, stretch);
        // Cracks are the flat minimum (-1 before centring).
        float bottom = 1e9F;
        for (const Float3& q : where) bottom = std::min(bottom, sample_surface_pattern(d, q).value);
        std::size_t n = 0;
        for (const Float3& q : where) n += sample_surface_pattern(d, q).value < bottom + 0.1F ? 1U : 0U;
        return static_cast<double>(n) / static_cast<double>(where.size());
    };
    const double wide = cracks(8.0F), narrow = cracks(40.0F);
    std::printf("cellular: crack share %.3f wide, %.3f narrow\n", wide, narrow);
    check(wide > narrow * 2.0 && narrow > 0.002, "cellular cracks widen with 1.6 / grainStretch");
    const MaterialSurfaceDetail cells = pattern(SurfacePattern::Cellular);
    double ax[3] = {};
    for (const Float3& q : points(4000, 5.0F)) {
        const float v = sample_surface_pattern(cells, q).value;
        ax[0] += std::abs(sample_surface_pattern(cells, add(q, {0.05F, 0, 0})).value - v);
        ax[1] += std::abs(sample_surface_pattern(cells, add(q, {0, 0.05F, 0})).value - v);
        ax[2] += std::abs(sample_surface_pattern(cells, add(q, {0, 0, 0.05F})).value - v);
    }
    const double lo = std::min({ax[0], ax[1], ax[2]}), hi = std::max({ax[0], ax[1], ax[2]});
    check(lo > 0.7 * hi, "cellular stone varies alike along every axis (a solid texture)");
}

// --- As a height source ------------------------------------------------------------------------

VoxelMaterialDefinition cobble(float depth, std::uint8_t mode = 0U, float period = 0.25F) {
    VoxelMaterialDefinition m;
    m.baseColor = {0.6F, 0.6F, 0.6F, 1.0F};
    m.specular = 0.0F;
    m.surfaceDetail = pattern(SurfacePattern::Cellular, period, 10.0F);
    m.surfaceDetail.colorVariation = 0.0F;
    m.surfaceDetail.roughnessVariation = 0.0F;
    m.displacementDepthMeters = depth;
    m.displacementMode = mode;
    return m;
}

void test_height_source() {
    const VoxelMaterialDefinition m = cobble(0.04F, 0U, 0.04F);   // stones 4 cm across: a few per 10 cm face
    check(!m.heightMap && material_has_displacement(m), "a surface pattern is a height source without a height map");
    // Down the +y face of voxel (0,0,0) at many points: the carve is (1 - h) x depth, h from the
    // pattern on the face plane.
    const Float3 mpc{0.1F, 0.1F, 0.1F};
    float worst = 0.0F, shallowest = 1.0F, deepest = 0.0F;
    for (int i = 0; i < 10; ++i)
        for (int k = 0; k < 10; ++k) {
            const float x = (static_cast<float>(i) + 0.5F) * 0.1F, z = (static_cast<float>(k) + 0.5F) * 0.1F;
            const auto hit = trace_displaced_voxel(m, {0, 0, 0}, 8U, {x, 2.0F, z}, {0, -1, 0}, 1.0F, 2.0F, mpc, 0.0F, 48U);
            const float h = surface_pattern_height(m.surfaceDetail, {x * 0.1F, 0.1F, z * 0.1F});
            const float expected = 1.0F + (1.0F - h) * 0.4F;   // cells: depth 0.04 m = 0.4 cells
            if (hit) {
                worst = std::max(worst, std::abs(hit->t - expected));
                shallowest = std::min(shallowest, hit->t - 1.0F);
                deepest = std::max(deepest, hit->t - 1.0F);
            } else {
                worst = 1.0F;
            }
        }
    std::printf("cellular height source: carve %.3f .. %.3f cells, worst error %.5f\n", shallowest, deepest, worst);
    check(worst < 2e-3F, "the traced carve follows the pattern's height exactly");
    check(deepest - shallowest > 0.15F, "cracks carve deeper than stones");

    // Parallax reads the same height: a view straight down meets the surface at (1 - h) x depth.
    const Float3 point{0.033F, 0.1F, 0.071F};
    const VoxelParallaxResult straight = voxel_parallax(m, point, {0, 1, 0}, {0, 1, 0}, 0.04F, 0.0F, 64U);
    const float h = surface_pattern_height(m.surfaceDetail, point);
    check(std::abs(straight.depthMeters - (1.0F - h) * 0.04F) < 1e-3F, "parallax on a pattern height meets (1 - h) x depth");
    const VoxelParallaxResult oblique = voxel_parallax(m, point, {0, 1, 0}, normalize(Float3{0.6F, 0.8F, 0.0F}), 0.04F, 0.0F, 64U);
    check(oblique.positionMeters.x < point.x - 1e-4F, "oblique parallax shifts the read point away from the viewer");
}

struct Scene {
    VoxelObject object{1U};
    std::array<VoxelMaterialDefinition, 2> materials{};
    std::vector<VoxelReferenceInstance> instances;
    explicit Scene(const VoxelMaterialDefinition& m) {
        materials[1] = m;
        for (int x = -40; x < 40; ++x)
            for (int z = -40; z < 40; ++z)
                for (int y = -2; y <= 0; ++y) object.set_voxel({x, y, z}, 1);
        instances.push_back({1U, &object, {}, materials, true, {1, 1, 1}});
    }
};
PolygonRenderTarget draw(const Scene& scene, std::uint32_t size = 48) {
    PolygonCamera c;
    c.position = {0.3F, 9.0F, 0.27F};
    c.target = {0.3F, 1.0F, 0.28F};
    c.up = {0, 0, 1};
    c.verticalFieldOfViewRadians = 0.5F;
    c.nearPlane = 0.1F;
    c.farPlane = 500.0F;
    RenderEnvironment e;
    e.sunDirection = normalize(Float3{1.0F, 0.6F, 0.2F});
    e.sunIntensity = 2.0F;
    e.shadowMode = ShadowMode::Hard;
    e.shadowStrength = 1.0F;
    e.globalIlluminationMode = GlobalIlluminationMode::Off;
    e.ambientOcclusionSamples = 0;
    PolygonRenderTarget target;
    target.resize(size, size);
    target.enable_passes();
    ReferenceVoxelRenderer renderer;
    renderer.threadCount = 2;
    (void)renderer.render(scene.instances, c, e, target);
    return target;
}

void test_render() {
    // A floor of cellular stone carved 5 cm (half a voxel): cracks show in the depth pass and in
    // the shadows; without depth the floor is flat.
    const PolygonRenderTarget flat = draw(Scene(cobble(0.0F))), carved = draw(Scene(cobble(0.05F)));
    float lo = 1e9F, hi = -1e9F, flatLo = 1e9F, flatHi = -1e9F;
    std::size_t shadowed = 0;
    for (std::size_t i = 0; i < carved.passes.depthMeters.size(); ++i) {
        if (carved.objectId[i] != 1U) continue;
        lo = std::min(lo, carved.passes.depthMeters[i]);
        hi = std::max(hi, carved.passes.depthMeters[i]);
        flatLo = std::min(flatLo, flat.passes.depthMeters[i]);
        flatHi = std::max(flatHi, flat.passes.depthMeters[i]);
        shadowed += carved.passes.sunVisibility[i] < 0.5F ? 1U : 0U;
    }
    std::printf("cobble floor: depth spread %.4f m carved, %.4f m flat; %zu pixels in crack shadow\n", hi - lo, flatHi - flatLo, shadowed);
    check(hi - lo > (flatHi - flatLo) + 0.02F, "carved cobbles move the depth pass");
    check(shadowed > 20U, "cracks between cobbles shadow themselves");
}

// --- Formats ---------------------------------------------------------------------------------------

unsigned file_minor(const fs::path& path, std::streamoff offset) {
    std::ifstream in(path, std::ios::binary);
    in.seekg(offset);
    unsigned char b[2]{};
    in.read(reinterpret_cast<char*>(b), 2);
    return b[0] | (b[1] << 8U);
}

void test_formats(const fs::path& dir) {
    std::string error;
    CookedVoxelAsset asset(77);
    asset.materials.resize(2);
    asset.object.set_voxel({0, 0, 0}, 1);
    asset.materials[1].surfaceDetail = pattern(SurfacePattern::Marble, 0.2F, 24.0F);
    check(write_dvox(dir / "marble.dvox", asset, {}, &error), "marble dvox: " + error);
    check(file_minor(dir / "marble.dvox", 6) == 12, "solid textures need DVOX 1.12");
    const auto back = read_dvox(dir / "marble.dvox");
    check(back.success && back.asset.materials[1].surfaceDetail.pattern == SurfacePattern::Marble &&
              back.asset.materials[1].surfaceDetail.grainStretch == 24.0F,
          "DVOX solid-texture round trip: " + back.error);
    // A 1.11 file claiming a solid texture is refused: patch the minor version down.
    {
        std::fstream f(dir / "marble.dvox", std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(6);
        const char eleven[2] = {11, 0};
        f.write(eleven, 2);
    }
    check(!read_dvox(dir / "marble.dvox").success, "DVOX 1.11 refuses a solid-texture pattern");
    asset.materials[1].surfaceDetail = pattern(SurfacePattern::Mineral);
    check(write_dvox(dir / "mineral.dvox", asset, {}, &error) && file_minor(dir / "mineral.dvox", 6) == 7,
          "older patterns keep DVOX 1.7");

    // DMESH: a quad with strata.
    CookedPolygonAsset mesh;
    mesh.objectId = 3;
    mesh.materials.push_back({});
    mesh.materials[0].surfaceDetail = pattern(SurfacePattern::Strata);
    mesh.materialBindings.push_back({});
    mesh.vertices = {{{0, 0, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
                     {{1, 0, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
                     {{1, 1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 1}, {1, 1, 1, 1}}};
    mesh.indices = {0, 1, 2};
    mesh.submeshes.push_back({"tri", 0, 3, 0});
    mesh.bounds = {{0, 0, 0}, {1, 1, 0}};
    mesh.contentHash = polygon_asset_content_hash(mesh);
    check(write_dmesh(dir / "strata.dmesh", mesh, &error), "DMESH with strata: " + error);
    check(file_minor(dir / "strata.dmesh", 10) == 9, "solid textures need DMESH 1.9");
    const auto meshBack = read_dmesh(dir / "strata.dmesh");
    check(static_cast<bool>(meshBack) && meshBack.asset.materials[0].surfaceDetail.pattern == SurfacePattern::Strata, "DMESH solid-texture round trip");
    // The editor material library (v12) is checked in test_material_optics.
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "dve_solid_textures_test";
    fs::create_directories(dir);
    test_statistics();
    test_gradient();
    test_character();
    test_height_source();
    test_render();
    test_formats(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures == 0) std::printf("dve_solid_textures_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
