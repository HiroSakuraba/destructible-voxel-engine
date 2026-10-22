// Subsurface, part 3:
//   - the translucency table (subsurface_transmission.hpp) against fresh random walks, on and off
//     its grid, and the GPU copy of it;
//   - Burley diffusion of front-lit light: its sampling, its exact shadow-edge profile, the voxel
//     renderer against that profile, the random walk against both, energy kept on flat lit
//     surfaces, and polygons softening the terminator.
#include "dve/polygon_bvh.hpp"
#include "dve/render/subsurface.hpp"
#include "dve/render/subsurface_random_walk.hpp"
#include "dve/render/subsurface_transmission.hpp"
#include "dve/render/voxel_reference_renderer.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <regex>
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
constexpr double kPi = 3.14159265358979323846;

double walk_transmission(float albedo, float x, float mu, int walks, std::uint64_t seed) {
    const SubsurfaceMedium medium = subsurface_medium_from_albedo(albedo, 1.0F, 1.0F);
    const SlabSubsurfaceDomain slab(-x, 0.0F);
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    auto random = [&] { return u(rng); };
    int through = 0;
    for (int i = 0; i < walks; ++i) {
        const auto w = enter_and_walk_subsurface(slab, medium, {0, 0, 0}, {0, 1, 0}, {std::sqrt(1.0F - mu * mu), -mu, 0.0F}, random);
        if (w && w->end == RandomWalkResult::End::Exited && w->normal.y < 0.0F && w->events > 0U) ++through;
    }
    return static_cast<double>(through) / walks;
}

void test_table() {
    std::printf("translucency table vs fresh walks (60000 each):\n   A     t/d    mu     table    walk\n");
    struct Point { float a, x, mu; };
    double worst = 0.0;
    for (const Point p : {Point{0.8F, 1.0F, 1.0F}, Point{0.5F, 4.0F, 0.6F}, Point{0.73F, 2.3F, 0.87F}, Point{0.95F, 0.7F, 0.45F},
                          Point{0.33F, 5.5F, 0.95F}, Point{0.88F, 11.0F, 0.71F}, Point{0.97F, 20.0F, 1.0F}}) {
        const double table = subsurface_slab_transmission(p.a, p.x, p.mu);
        const double walk = walk_transmission(p.a, p.x, p.mu, 60000, 77);
        std::printf("  %.2f   %5.2f  %.2f   %.4f   %.4f\n", p.a, p.x, p.mu, table, walk);
        worst = std::max(worst, std::abs(table - walk));
    }
    check(worst < 0.006, "the table matches fresh walks within 0.006, on and between its samples and past its end");
    check(subsurface_slab_transmission(0.8F, 0.0F, 1.0F) == 0.0F, "no thickness: nothing scatters through");
    check(subsurface_slab_transmission(0.0F, 2.0F, 1.0F) == 0.0F, "black: nothing comes through");
    float previous = 1.0F;
    bool decreasing = true;
    for (float x = 4.0F; x < 40.0F; x += 0.5F) {
        const float t = subsurface_slab_transmission(0.9F, x, 1.0F);
        decreasing = decreasing && t <= previous && t >= 0.0F;
        previous = t;
    }
    check(decreasing, "thick slabs: transmission falls steadily, smoothly past the table's end");

    // The GPU's table is the same numbers.
    std::ifstream in(std::filesystem::path(DVE_SOURCE_DIR) / "shaders/common/subsurface_transmission_table.hlsli");
    std::stringstream text;
    text << in.rdbuf();
    const std::string body = text.str().substr(text.str().find('{'));
    const std::regex number("([0-9]+\\.[0-9]+)F");
    std::vector<float> gpu;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), number); it != std::sregex_iterator(); ++it)
        gpu.push_back(std::stof((*it)[1].str()));
    bool same = gpu.size() == kSubsurfaceTransmissionCosines * kSubsurfaceTransmissionAlbedos * kSubsurfaceTransmissionThicknesses;
    for (std::size_t m = 0; same && m < kSubsurfaceTransmissionCosines; ++m)
        for (std::size_t a = 0; a < kSubsurfaceTransmissionAlbedos; ++a)
            for (std::size_t k = 0; k < kSubsurfaceTransmissionThicknesses; ++k)
                same = same && std::abs(gpu[(m * kSubsurfaceTransmissionAlbedos + a) * kSubsurfaceTransmissionThicknesses + k] -
                                        subsurface_transmission_entry(m, a, k)) < 1e-6F;
    check(same, "GPU table = CPU table");
}

void test_profile_sampling() {
    // Radii drawn by burley_sample_radius follow r R(r) 2 pi: the share within x is
    // 1 - 1/4 e^-x/d - 3/4 e^-x/3d, and the mean radius is 1/4 d + 3/4 3d = 2.5 d.
    const int n = 100000;
    double mean = 0.0;
    std::array<double, 3> within{};
    const std::array<double, 3> xs{0.5, 2.0, 6.0};
    for (int i = 0; i < n; ++i) {
        const double r = burley_sample_radius((i + 0.5F) / n, 1.0F);
        mean += r / n;
        for (std::size_t k = 0; k < xs.size(); ++k) within[k] += (r <= xs[k] ? 1.0 : 0.0) / n;
    }
    check(std::abs(mean - 2.5) < 0.01, "mean sample radius 2.5 d");
    for (std::size_t k = 0; k < xs.size(); ++k)
        check(std::abs(within[k] - (1.0 - 0.25 * std::exp(-xs[k]) - 0.75 * std::exp(-xs[k] / 3.0))) < 1e-3, "radius CDF");
    // The half-plane share, against the same samples with uniform angles.
    for (const float x : {0.0F, 0.5F, 1.0F, 3.0F}) {
        double beyond = 0.0;
        const int angles = 64;
        for (int i = 0; i < 20000; ++i)
            for (int j = 0; j < angles; ++j)
                if (burley_sample_radius((i + 0.5F) / 20000.0F, 1.0F) * std::cos(2.0 * kPi * (j + 0.5) / angles) > x)
                    beyond += 1.0 / (20000.0 * angles);
        std::printf("half-plane share at %.1f d: %.4f (samples %.4f)\n", x, burley_half_plane_share(x, 1.0F), beyond);
        check(std::abs(burley_half_plane_share(x, 1.0F) - beyond) < 2e-3, "half-plane share");
    }
    check(std::abs(burley_half_plane_share(-1.0F, 1.0F) - (1.0F - burley_half_plane_share(1.0F, 1.0F))) < 1e-6F, "symmetric");
}

VoxelMaterialDefinition marble(float scatterMeters) {
    VoxelMaterialDefinition m;
    m.name = "Marble";
    m.baseColor = {1.0F, 1.0F, 1.0F, 1.0F};
    m.shadingModel = MaterialShadingModel::Subsurface;
    m.subsurfaceScatterDistanceMeters = scatterMeters;
    m.subsurfaceColor = {0.8F, 0.8F, 0.8F};
    return m;
}

struct Basis { Float3 forward, right, up; float tanHalf, aspect; };
Basis basis_of(const PolygonCamera& c, std::uint32_t w, std::uint32_t h) {
    Basis b;
    b.forward = normalize(subtract(c.target, c.position));
    const Float3 r{b.forward.y * c.up.z - b.forward.z * c.up.y, b.forward.z * c.up.x - b.forward.x * c.up.z,
                   b.forward.x * c.up.y - b.forward.y * c.up.x};
    b.right = normalize(r);
    b.up = normalize(Float3{b.right.y * b.forward.z - b.right.z * b.forward.y, b.right.z * b.forward.x - b.right.x * b.forward.z,
                            b.right.x * b.forward.y - b.right.y * b.forward.x});
    b.tanHalf = std::tan(c.verticalFieldOfViewRadians * 0.5F);
    b.aspect = static_cast<float>(w) / static_cast<float>(h);
    return b;
}
// World x where pixel (px, py) sees the plane y = plane.
float pixel_x(const PolygonCamera& c, const Basis& b, std::uint32_t px, std::uint32_t py, std::uint32_t w, std::uint32_t h,
              float plane) {
    const float ndcY = 1.0F - 2.0F * ((static_cast<float>(py) + 0.5F) / static_cast<float>(h));
    const float ndcX = 2.0F * ((static_cast<float>(px) + 0.5F) / static_cast<float>(w)) - 1.0F;
    Float3 ray = add(add(b.forward, multiply(b.right, ndcX * b.tanHalf * b.aspect)), multiply(b.up, ndcY * b.tanHalf));
    ray = normalize(ray);
    const float t = (plane - c.position.y) / ray.y;
    return c.position.x + ray.x * t;
}

void test_shadow_edge() {
    // A marble block (top at y = 30) under the sun, with a roof shading x < 0: a sharp shadow
    // edge. Scatter distance 0.2 m = 2 voxels.
    VoxelObject block(1U), roof(2U);
    for (int x = -48; x < 48; ++x)
        for (int z = -48; z < 48; ++z) {
            for (int y = 0; y < 30; ++y) block.set_voxel({x, y, z}, 1);
            if (x < 0) roof.set_voxel({x, 60, z}, 1);
        }
    std::array<VoxelMaterialDefinition, 2> materials{VoxelMaterialDefinition{}, marble(0.2F)};
    std::array<VoxelMaterialDefinition, 2> roofMaterials{VoxelMaterialDefinition{}, VoxelMaterialDefinition{}};
    const std::vector<VoxelReferenceInstance> voxels{{1U, &block, {}, materials, true, {1, 1, 1}},
                                                     {2U, &roof, {}, roofMaterials, true, {1, 1, 1}}};
    RenderEnvironment environment;
    environment.sunDirection = {0.0F, 1.0F, 0.0F};
    environment.shadowMode = ShadowMode::Hard;
    environment.shadowStrength = 1.0F;
    environment.globalIlluminationMode = GlobalIlluminationMode::Off;
    environment.sunColor = {1, 1, 1};
    environment.sunIntensity = 1.0F;
    PolygonCamera camera;
    camera.position = {0.0F, 55.0F, 0.0F};   // under the roof's level is fine: the roof is above it
    camera.target = {0.0F, 30.0F, 0.0F};
    camera.up = {0.0F, 0.0F, -1.0F};
    camera.verticalFieldOfViewRadians = 0.6F;
    camera.nearPlane = 0.5F;
    const std::uint32_t w = 96, h = 24;
    const auto render = [&](std::uint32_t samples) {
        ReferenceVoxelRenderer renderer;
        renderer.subsurfaceDiffusionSamples = samples;
        PolygonRenderTarget target;
        target.resize(w, h);
        (void)renderer.render(voxels, camera, environment, target);
        return target;
    };
    const PolygonRenderTarget sharp = render(0), soft = render(512);
    const Basis b = basis_of(camera, w, h);
    // Average each pixel column (one world x).
    std::printf("\nshadow edge on marble (x in scatter distances): sharp / diffused / exact Burley\n");
    double worst = 0.0;
    for (std::uint32_t px = 0; px < w; px += 6) {
        const float x = pixel_x(camera, b, px, h / 2U, w, h, 30.0F) / 2.0F;   // d = 2 voxels
        double s = 0.0, d = 0.0;
        for (std::uint32_t py = 2; py < h - 2U; ++py) {
            s += sharp.hdrColor[py * w + px].x / (h - 4U);
            d += soft.hdrColor[py * w + px].x / (h - 4U);
        }
        const double exact = 1.0 - burley_half_plane_share(x, 1.0F);
        std::printf("  x %6.2f   %.3f   %.4f   %.4f\n", x, s, d, exact);
        worst = std::max(worst, std::abs(d - exact));
        if (std::abs(x) > 0.3F) check(std::abs(s - (x > 0.0F ? 1.0 : 0.0)) < 1e-4, "without diffusion the edge stays sharp");
    }
    check(worst < 0.03, "the voxel renderer's diffused edge follows the exact Burley profile (512 samples)");

    // The same edge from the random walk: sunlight falls on x > 0 of a half-space of the marble's
    // medium (A = base colour 1.0, d = 1); exits counted by x, normalised by the far lit side.
    // A = 1 is a medium with almost no absorption: use A = 0.8, Burley's shape is the same.
    const SubsurfaceMedium medium = subsurface_medium_from_albedo(0.8F, 1.0F, 1.0F);
    const SlabSubsurfaceDomain half(-std::numeric_limits<float>::infinity(), 0.0F);
    std::mt19937_64 rng(91);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    auto random = [&] { return u(rng); };
    const double span = 30.0, bin = 0.5;
    std::vector<double> exits(static_cast<std::size_t>(2.0 * span / bin), 0.0);
    const int n = 400000;
    for (int i = 0; i < n; ++i) {
        const float x0 = static_cast<float>(random() * span);
        const auto walk = enter_and_walk_subsurface(half, medium, {x0, 0.0F, 0.0F}, {0, 1, 0}, {0, -1, 0}, random);
        if (!walk || walk->end != RandomWalkResult::End::Exited) continue;
        const double x = walk->position.x;
        // Bin k is centred on x = k bin - span.
        const double k = std::floor((x + span + 0.5 * bin) / bin);
        if (k >= 0.0 && k < static_cast<double>(exits.size())) exits[static_cast<std::size_t>(k)] += 1.0;
    }
    // Deep in the lit side (x in [10, 20]) every bin sees the full reflectance.
    double lit = 0.0;
    int litBins = 0;
    for (double x = 10.0; x < 20.0; x += bin) { lit += exits[static_cast<std::size_t>(std::lround((x + span) / bin))]; ++litBins; }
    lit /= litBins;
    std::printf("\nshadow edge, random walk (A 0.8, sun overhead) / Burley, both relative to the lit side:\n");
    double worstWalk = 0.0;
    for (const double x : {-4.0, -2.0, -1.0, -0.5, 0.0, 0.5, 1.0, 2.0, 4.0}) {
        const double walk = exits[static_cast<std::size_t>(std::lround((x + span) / bin))] / lit;
        const double burley = 1.0 - burley_half_plane_share(static_cast<float>(x), 1.0F);
        std::printf("  x %5.1f   %.4f   %.4f\n", x, walk, burley);
        worstWalk = std::max(worstWalk, std::abs(walk - burley));
    }
    check(worstWalk < 0.03, "Burley's edge follows the random walk within 0.03 of the lit level");

    // Energy: a flat, fully lit surface is unchanged by diffusion.
    const std::vector<VoxelReferenceInstance> open{voxels[0]};
    ReferenceVoxelRenderer plain, diffused;
    plain.subsurfaceDiffusionSamples = 0;
    diffused.subsurfaceDiffusionSamples = 64;
    PolygonRenderTarget a, c;
    a.resize(w, h);
    c.resize(w, h);
    (void)plain.render(open, camera, environment, a);
    (void)diffused.render(open, camera, environment, c);
    float most = 0.0F;
    for (std::size_t i = 0; i < a.hdrColor.size(); ++i) most = std::max(most, std::abs(a.hdrColor[i].x - c.hdrColor[i].x));
    check(most < 1e-5F, "flat, fully lit surface: diffusion keeps the light exactly");
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

void test_polygon_terminator() {
    // A marble ball lit from the side (+x), seen from +z: the terminator runs down the middle.
    // Diffusion carries light past it; the lit face stays as it was.
    // Scatter distance 0.03 m against a 0.4 m ball: a soft terminator, and too thick for much
    // light to scatter right through.
    const CookedPolygonAsset ball = sphere(4.0F, marble(0.03F));
    const std::vector<PolygonRenderInstance> instances{{61U, &ball, {}, {}, {1, 1, 1, 1}, true}};
    RenderEnvironment environment;
    environment.sunDirection = {1.0F, 0.0F, 0.0F};
    environment.globalIlluminationMode = GlobalIlluminationMode::Off;
    environment.sunColor = {1, 1, 1};
    environment.sunIntensity = 1.0F;
    PolygonCamera camera;
    camera.position = {0.0F, 0.0F, 30.0F};
    camera.target = {0.0F, 0.0F, 0.0F};
    camera.verticalFieldOfViewRadians = 0.3F;
    const auto render = [&](std::uint32_t samples) {
        PolygonRenderTarget target;
        target.resize(64, 64);
        PolygonRenderOptions options;
        options.preserveExistingDepth = false;
        options.subsurfaceDiffusionSamples = samples;
        (void)ReferencePolygonRenderer{}.render(instances, camera, environment, target, options);
        return target;
    };
    const PolygonRenderTarget sharp = render(0), soft = render(256);
    const auto at = [](const PolygonRenderTarget& t, std::uint32_t x) { return t.hdrColor[32U * t.width + x].x; };
    std::printf("\nmarble ball across the terminator (x = 32 is the middle), sharp / diffused:\n ");
    for (std::uint32_t x = 24; x <= 48; x += 3) std::printf(" %2u: %.3f/%.3f", x, at(sharp, x), at(soft, x));
    std::printf("\n");
    check(at(soft, 30) - at(sharp, 30) > 0.02F && at(soft, 28) - at(sharp, 28) > 0.005F,
          "light carried past the terminator into the dark side");
    // On a curved face, averaging the cosine over the surface lowers the brightest parts a
    // little (that light went past the terminator): a few percent, not more.
    check(std::abs(at(soft, 48) - at(sharp, 48)) < 0.05F * at(sharp, 48), "the lit face keeps nearly all its light");
}

} // namespace

int main() {
    test_table();
    test_profile_sampling();
    test_shadow_edge();
    test_polygon_terminator();
    if (g_failures == 0) std::printf("dve_subsurface_diffusion_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
