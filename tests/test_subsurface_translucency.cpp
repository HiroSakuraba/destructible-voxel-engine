// Subsurface translucency in the CPU reference (subsurface.hpp):
//   - Burley's transmittance is the diffusion profile integrated over a plane at depth d;
//   - the march toward the sun finds the sunlit face and the slab's true thickness at any angle;
//   - the voxel and polygon renderers give the exact expected light on the shaded side of a slab,
//     the same number for both, and nothing when the far side is shadowed, blocked by another
//     material, too thick, switched off, or not a Subsurface material;
//   - the GPU shader follows the same model.
#include "dve/polygon_bvh.hpp"
#include "dve/render/subsurface.hpp"
#include "dve/render/voxel_reference_renderer.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
bool near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }
constexpr double kPi = 3.14159265358979323846;
constexpr float kDegrees = 3.14159265F / 180.0F;

VoxelMaterialDefinition wax() {
    VoxelMaterialDefinition m;
    m.name = "Wax";
    m.baseColor = {0.8F, 0.8F, 0.8F, 1.0F};
    m.shadingModel = MaterialShadingModel::Subsurface;
    m.subsurfaceScatterDistanceMeters = 0.2F;
    m.subsurfaceColor = {1.0F, 0.5F, 0.25F};
    return m;
}
VoxelMaterialDefinition stone() { VoxelMaterialDefinition m; m.name = "Stone"; return m; }

void test_burley() {
    check(near(burley_transmittance(0.0F, 0.2F), 1.0, 1e-6), "T(0) = 1");
    check(near(burley_transmittance(0.2F, 0.2F), 0.25 * std::exp(-1.0) + 0.75 * std::exp(-1.0 / 3.0), 1e-6), "T(s)");
    check(burley_transmittance(1.0F, 0.0F) == 0.0F, "no scatter distance, no transmission");
    // The profile R(r) = (e^(-r/s) + e^(-r/3s)) / (8 pi s r) integrated over the plane at depth d.
    const double s = 0.2;
    for (const double d : {0.0, 0.1, 0.3, 0.8}) {
        double sum = 0.0;
        const int n = 400000;
        const double rMax = 60.0 * s, dr = rMax / n;
        for (int i = 0; i < n; ++i) {
            const double r = (i + 0.5) * dr, rho = std::sqrt(r * r + d * d);
            sum += (std::exp(-rho / s) + std::exp(-rho / (3.0 * s))) / (8.0 * kPi * s * rho) * 2.0 * kPi * r * dr;
        }
        std::printf("Burley: d = %.1f  plane integral %.5f  closed form %.5f\n", d, sum,
                    burley_transmittance(static_cast<float>(d), static_cast<float>(s)));
        check(near(sum, burley_transmittance(static_cast<float>(d), static_cast<float>(s)), 2e-3),
              "closed form = profile over the plane");
    }
}

// Slab of wax, y in [5, 8), with an optional roof (y = 12) and an optional stone cap on top.
struct SlabScene {
    VoxelObject slab{2U}, roof{3U};
    std::array<VoxelMaterialDefinition, 3> materials{VoxelMaterialDefinition{}, wax(), stone()};
    std::array<VoxelMaterialDefinition, 3> roofMaterials{VoxelMaterialDefinition{}, stone(), stone()};
    std::vector<VoxelReferenceInstance> voxels;
    SlabScene(bool withRoof, bool stoneCap) {
        for (int x = -12; x < 12; ++x)
            for (int z = -12; z < 12; ++z) {
                for (int y = 5; y < 8; ++y) slab.set_voxel({x, y, z}, 1);
                if (stoneCap) slab.set_voxel({x, 8, z}, 2);
                if (withRoof) roof.set_voxel({x, 12, z}, 1);
            }
        voxels.push_back({2U, &slab, {}, materials, true, {1, 1, 1}});
        if (withRoof) voxels.push_back({3U, &roof, {}, roofMaterials, true, {1, 1, 1}});
    }
};

void test_voxel_march() {
    SlabScene scene(false, false);
    const VoxelReferenceInstance& slab = scene.voxels[0];
    const Float3 start{0.3F, 5.001F, -0.7F};
    const SubsurfaceExit up = march_voxel_subsurface(slab, 1, start, {0, 1, 0}, 10.0F);
    check(up.found && up.open, "overhead: leaves the slab into air");
    check(near(up.chord, 2.999, 1e-3) && near(up.thickness, 2.999, 1e-3), "overhead: chord = thickness = 3");
    check(near(up.normal.y, 1.0, 1e-6), "overhead: sunlit face is the top");
    const float a = 50.0F * kDegrees;
    const SubsurfaceExit slant = march_voxel_subsurface(slab, 1, start, {std::sin(a), std::cos(a), 0.0F}, 10.0F);
    std::printf("march at 50 deg: chord %.4f (3/cos = %.4f), thickness %.4f\n", slant.chord, 2.999 / std::cos(a), slant.thickness);
    check(slant.found && near(slant.chord, 2.999 / std::cos(a), 1e-3), "slanted: the chord is longer");
    check(near(slant.thickness, 2.999, 1e-3), "slanted: the thickness is still the slab's");
    check(!march_voxel_subsurface(slab, 1, start, {std::sin(a), std::cos(a), 0.0F}, 4.0F).found,
          "past the maximum distance: nothing");
    check(!march_voxel_subsurface(slab, 2, start, {0, 1, 0}, 10.0F).found, "not inside that material: nothing");
    SlabScene capped(false, true);
    const SubsurfaceExit cap = march_voxel_subsurface(capped.voxels[0], 1, start, {0, 1, 0}, 10.0F);
    check(cap.found && !cap.open, "a stone cap: leaves the wax, but not into the open");
}

RenderEnvironment environment_for(float sunDegrees) {
    RenderEnvironment environment;
    const float a = sunDegrees * kDegrees;
    environment.sunDirection = {std::sin(a), std::cos(a), 0.0F};
    environment.shadowMode = ShadowMode::Hard;
    environment.shadowStrength = 1.0F;
    environment.globalIlluminationMode = GlobalIlluminationMode::Off;
    environment.subsurfaceMaxDistanceMeters = 1.0F;
    return environment;
}

PolygonCamera camera_below() {
    PolygonCamera camera;
    camera.position = {0.0F, -10.0F, 0.01F};
    camera.target = {0.0F, 5.0F, 0.0F};
    camera.up = {0.0F, 0.0F, 1.0F};
    camera.verticalFieldOfViewRadians = 0.5F;
    return camera;
}

Float4 centre(const PolygonRenderTarget& t) { return t.hdrColor[(t.height / 2U) * t.width + t.width / 2U]; }

// Expected light on the underside: subsurface colour x sun x cos(sun at the top) x T(0.3 m).
Float3 expected_underside(const RenderEnvironment& environment, float shadow = 1.0F) {
    const float cosTop = environment.sunDirection.y / length(environment.sunDirection);
    const float k = environment.sunIntensity * cosTop * shadow * burley_transmittance(0.3F, 0.2F);
    const Float3 c = wax().subsurfaceColor;
    return {c.x * environment.sunColor.x * k, c.y * environment.sunColor.y * k, c.z * environment.sunColor.z * k};
}

Float4 render_voxels(const SlabScene& scene, const RenderEnvironment& environment, VoxelReferenceRenderStats* stats = nullptr) {
    ReferenceVoxelRenderer renderer;
    renderer.threadCount = 1;
    PolygonRenderTarget target;
    target.resize(33, 33);
    const auto s = renderer.render(scene.voxels, camera_below(), environment, target);
    if (stats) *stats = s;
    return centre(target);
}

void test_voxel_renderer() {
    for (const float degrees : {0.0F, 50.0F}) {
        const RenderEnvironment environment = environment_for(degrees);
        VoxelReferenceRenderStats stats;
        const Float4 c = render_voxels(SlabScene(false, false), environment, &stats);
        const Float3 e = expected_underside(environment);
        std::printf("voxel slab underside, sun %2.0f deg: (%.5f %.5f %.5f)  expected (%.5f %.5f %.5f)  marches %llu\n",
                    degrees, c.x, c.y, c.z, e.x, e.y, e.z, static_cast<unsigned long long>(stats.subsurfaceMarches));
        check(near(c.x, e.x, 2e-3 * e.x) && near(c.y, e.y, 2e-3 * e.y) && near(c.z, e.z, 2e-3 * e.z),
              "voxels: the exact transmitted light on the shaded side");
        check(stats.subsurfaceExits > 0U, "voxels: marches counted");
    }
    const RenderEnvironment overhead = environment_for(0.0F);
    check(render_voxels(SlabScene(true, false), overhead).x == 0.0F, "a roof shadowing the sunlit side: no light through");
    RenderEnvironment partial = overhead;
    partial.shadowStrength = 0.85F;
    check(near(render_voxels(SlabScene(true, false), partial).x, expected_underside(partial, 0.15F).x, 1e-3),
          "shadow strength applies to the far side's shadow");
    check(render_voxels(SlabScene(false, true), overhead).x == 0.0F, "a stone cap: no light through");
    RenderEnvironment thin = overhead;
    thin.subsurfaceMaxDistanceMeters = 0.2F;
    check(render_voxels(SlabScene(false, false), thin).x == 0.0F, "thicker than the subsurface max distance: nothing");
    RenderEnvironment off = overhead;
    off.subsurfaceMaxDistanceMeters = 0.0F;
    check(render_voxels(SlabScene(false, false), off).x == 0.0F, "subsurface switched off: nothing");
    SlabScene standard(false, false);
    standard.materials[1].shadingModel = MaterialShadingModel::StandardPBR;
    check(render_voxels(standard, overhead).x == 0.0F, "not a Subsurface material: the underside stays dark");

    // Radiance cascades shade through a separate path; the translucency must be the same there.
    RenderEnvironment cascades = overhead;
    cascades.globalIlluminationMode = GlobalIlluminationMode::RadianceCascades;
    const Float4 with = render_voxels(SlabScene(false, false), cascades);
    const Float4 without = render_voxels(standard, cascades);
    std::printf("radiance cascades: subsurface - standard = %.5f (expected %.5f)\n", with.x - without.x,
                expected_underside(cascades).x);
    check(near(with.x - without.x, expected_underside(cascades).x, 2e-3), "radiance-cascades path adds the same light");
}

// A closed box mesh with flat faces (24 vertices).
CookedPolygonAsset box(Float3 lo, Float3 hi, const VoxelMaterialDefinition& material) {
    CookedPolygonAsset a;
    a.objectId = 60;
    a.materials.push_back(material);
    a.materialBindings.resize(1);
    a.materialBindings[0].doubleSided = true;
    const auto face = [&](Float3 n, Float3 p0, Float3 p1, Float3 p2, Float3 p3) {
        const auto base = static_cast<std::uint32_t>(a.vertices.size());
        for (const Float3 p : {p0, p1, p2, p3}) a.vertices.push_back({p, n, {1, 0, 0, 1}, {0, 0}, {1, 1, 1, 1}, {0, 0}});
        a.indices.insert(a.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    face({0, -1, 0}, {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z});
    face({0, 1, 0}, {lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z});
    face({-1, 0, 0}, {lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z});
    face({1, 0, 0}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z});
    face({0, 0, -1}, {lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, lo.y, lo.z});
    face({0, 0, 1}, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z});
    a.submeshes.push_back({"box", 0, static_cast<std::uint32_t>(a.indices.size()), 0});
    a.bounds = {lo, hi};
    return a;
}

CookedPolygonAsset sphere(float r, const VoxelMaterialDefinition& material) {
    CookedPolygonAsset a;
    a.objectId = 61;
    a.materials.push_back(material);
    a.materialBindings.resize(1);
    const int slices = 96, stacks = 48;
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

void test_polygons() {
    // The same slab as a mesh: the same number as the voxels.
    for (const float degrees : {0.0F, 50.0F}) {
        const RenderEnvironment environment = environment_for(degrees);
        const CookedPolygonAsset slab = box({-12, 5, -12}, {12, 8, 12}, wax());
        const std::vector<PolygonRenderInstance> instances{{60U, &slab, {}, {}, {1, 1, 1, 1}, true}};
        PolygonRenderTarget target;
        target.resize(33, 33);
        PolygonRenderOptions options;
        options.preserveExistingDepth = false;
        const auto stats = ReferencePolygonRenderer{}.render(instances, camera_below(), environment, target, options);
        const Float4 c = centre(target);
        const Float3 e = expected_underside(environment);
        std::printf("polygon slab underside, sun %2.0f deg: (%.5f %.5f %.5f)  expected (%.5f %.5f %.5f)  marches %llu\n",
                    degrees, c.x, c.y, c.z, e.x, e.y, e.z, static_cast<unsigned long long>(stats.subsurfaceMarches));
        check(near(c.x, e.x, 2e-3 * e.x) && near(c.y, e.y, 2e-3 * e.y) && near(c.z, e.z, 2e-3 * e.z),
              "polygons: the exact transmitted light, the same as voxels");
    }
    // A smooth ball: from the bottom pole straight through to the top pole, whose smooth normal
    // faces the sun; near the terminator the light has much less to cross.
    const CookedPolygonAsset ball = sphere(2.0F, wax());
    PolygonBvh bvh;
    check(bvh.build(ball), "ball BVH");
    const SubsurfaceExit pole = march_polygon_subsurface(bvh, ball, {}, {0.0F, -1.999F, 0.0F}, {0, 1, 0}, 10.0F);
    std::printf("ball from the bottom pole: chord %.4f, thickness %.4f, exit normal y %.4f\n", pole.chord, pole.thickness,
                pole.normal.y);
    check(pole.found && near(pole.chord, 3.998, 2e-2) && near(pole.thickness, 3.998, 2e-2) && pole.normal.y > 0.999F,
          "ball: through the middle, out at the top pole");
    const float a = 100.0F * kDegrees;   // just past the terminator
    const Float3 side{1.999F * std::sin(a), 1.999F * std::cos(a), 0.0F};
    const SubsurfaceExit rim = march_polygon_subsurface(bvh, ball, {}, side, {0, 1, 0}, 10.0F);
    std::printf("ball just past the terminator: chord %.4f, thickness %.4f\n", rim.chord, rim.thickness);
    check(rim.found && rim.thickness < 0.25F * pole.thickness, "ball: much thinner near the terminator");
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void test_gpu_shader_follows() {
    const std::filesystem::path root = DVE_SOURCE_DIR;
    const std::string subsurface = read_text(root / "shaders/common/subsurface.hlsli");
    const std::string primary = read_text(root / "shaders/shade_primary.hlsl");
    check(subsurface.find("0.25F * exp(-x) + 0.75F * exp(-x / 3.0F)") != std::string::npos, "GPU: Burley transmittance");
    check(subsurface.find("march.exited = material == 0u") != std::string::npos, "GPU: only empty space lets the sun in");
    check(subsurface.find("dot(march.exitPoint - startPoint, march.exitNormal)") != std::string::npos,
          "GPU: thickness to the sunlit face's plane");
    check(primary.find("MarchSubsurfaceThicknessVoxels(\n            hitPoint, gSunDirection") != std::string::npos,
          "GPU: marches toward the sun");
    check(primary.find("dot(geometricNormal, gSunDirection) < 0.0F") != std::string::npos, "GPU: shaded side only");
    check(primary.find("TraceVoxelRay(farRay)") != std::string::npos, "GPU: the far side's own sun visibility");
    check(primary.find("gSunColor * gSunIntensity * farVisibility") != std::string::npos,
          "GPU: not the near side's (self-)shadow");
}

} // namespace

int main() {
    test_burley();
    test_voxel_march();
    test_voxel_renderer();
    test_polygons();
    test_gpu_shader_follows();
    if (g_failures == 0) std::printf("dve_subsurface_translucency_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
