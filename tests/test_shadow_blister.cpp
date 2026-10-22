// The shadow blister: two penumbrae that overlap darken the gap between them more than either
// shadow does alone, before their dark cores meet. Physically the light lost is the union of the
// parts of the sun disc each occluder hides.
//
// 1. The CPU reference path (the same sun-disc sampling as generate_shadow_rays.hlsl) matches an
//    exact numeric integral over the sun disc for two floating slabs, and shows the blister.
// 2. The ways a renderer might combine two shadows (darker of the two, product) do not.
// 3. The penumbra-aware shadow denoiser (spatial_lighting_denoise.hlsl, mirrored on the CPU) keeps a
//    narrow blister that the old fixed 5x5 blur flattens, and leaves wide penumbrae as before.
// 4. Concentric sun-disc samples are equal-area and spread out (mirror of the HLSL).
#include "dve/render/ray_lighting_reference.hpp"
#include "dve/render/voxel_reference_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <random>
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

// Scene (reference world units = voxels): ground cells y = 0 (top surface y = 1), two slabs one
// voxel thick at y = 21 (bottom 20 above the ground), with a gap of 2 cells between x = -1 and 1.
constexpr int kGroundHalfX = 40, kGroundHalfZ = 20;
constexpr int kSlabY = 21;
constexpr int kGapHalf = 1;
constexpr float kSunAngularRadius = 0.1F;   // large, so the penumbra spans a few cells

struct Box { double x0, x1, y0, y1, z0, z1; };
bool ray_hits(const Box& b, double ox, double oy, double oz, double dx, double dy, double dz) {
    double t0 = 1e-9, t1 = 1e30;
    const double o[3] = {ox, oy, oz}, d[3] = {dx, dy, dz}, lo[3] = {b.x0, b.y0, b.z0}, hi[3] = {b.x1, b.y1, b.z1};
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-15) { if (o[i] < lo[i] || o[i] > hi[i]) return false; continue; }
        double a = (lo[i] - o[i]) / d[i], c = (hi[i] - o[i]) / d[i];
        if (a > c) std::swap(a, c);
        t0 = std::max(t0, a); t1 = std::min(t1, c);
        if (t0 > t1) return false;
    }
    return true;
}
// Exact (numerically integrated, 360x360 cells over the disc) fraction of the sun disc visible
// from a ground point, sun straight overhead.
double truth_visibility(double x, double z, const std::vector<Box>& boxes) {
    const int n = 360;
    const double t = std::tan(double(kSunAngularRadius));
    int inside = 0, visible = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const double u = (i + 0.5) / n * 2.0 - 1.0, v = (j + 0.5) / n * 2.0 - 1.0;
            if (u * u + v * v > 1.0) continue;
            ++inside;
            bool blocked = false;
            for (const Box& b : boxes) blocked = blocked || ray_hits(b, x, 1.0, z, u * t, 1.0, v * t);
            visible += blocked ? 0 : 1;
        }
    return double(visible) / double(inside);
}

struct Scene {
    VoxelObject ground{1U}, left{2U}, right{3U};
    std::array<VoxelMaterialDefinition, 2> materials{};
    Scene() {
        materials[1].name = "grey";
        materials[1].baseColor = {0.5F, 0.5F, 0.5F, 1.0F};
        for (int x = -kGroundHalfX; x < kGroundHalfX; ++x)
            for (int z = -kGroundHalfZ; z < kGroundHalfZ; ++z) {
                ground.set_voxel({x, 0, z}, 1U);
                if (x < -kGapHalf && x >= -30) left.set_voxel({x, kSlabY, z}, 1U);
                if (x >= kGapHalf && x < 30) right.set_voxel({x, kSlabY, z}, 1U);
            }
    }
    std::vector<VoxelReferenceInstance> instances(bool withLeft, bool withRight) const {
        std::vector<VoxelReferenceInstance> out;
        out.push_back({1U, &ground, {}, materials, true, {1, 1, 1}});
        if (withLeft) out.push_back({2U, &left, {}, materials, true, {1, 1, 1}});
        if (withRight) out.push_back({3U, &right, {}, materials, true, {1, 1, 1}});
        return out;
    }
    static std::vector<Box> boxes(bool withLeft, bool withRight) {
        std::vector<Box> out;
        if (withLeft) out.push_back({-30.0, double(-kGapHalf), double(kSlabY), kSlabY + 1.0, -kGroundHalfZ * 1.0, kGroundHalfZ * 1.0});
        if (withRight) out.push_back({double(kGapHalf), 30.0, double(kSlabY), kSlabY + 1.0, -kGroundHalfZ * 1.0, kGroundHalfZ * 1.0});
        return out;
    }
};

RenderEnvironment soft_sun() {
    RenderEnvironment e;
    e.sunDirection = {0.0F, 1.0F, 0.0F};
    e.shadowMode = ShadowMode::Soft;
    e.shadowStrength = 1.0F;
    e.shadowSoftnessRadians = kSunAngularRadius;
    e.shadowSamples = 16;
    e.shadowMaxDistanceMeters = 100.0F;
    return e;
}

// 64 rotations x 16 rays = 1024 sun-disc samples per point.
double reference(const std::vector<VoxelReferenceInstance>& instances, double x) {
    double sum = 0.0;
    for (std::uint32_t seed = 0; seed < 64; ++seed)
        sum += reference_sun_visibility(instances, {float(x), 1.0F, 0.5F}, {0, 1, 0}, soft_sun(), kDefaultMetersPerVoxel,
                                        seed * 2654435761U + 1U);
    return sum / 64.0;
}

void test_reference_matches_the_disc_integral() {
    Scene scene;
    const auto both = scene.instances(true, true), leftOnly = scene.instances(true, false), rightOnly = scene.instances(false, true);
    const auto boxesBoth = Scene::boxes(true, true), boxesLeft = Scene::boxes(true, false), boxesRight = Scene::boxes(false, true);
    double worst = 0.0;
    std::printf("   x   truth(both) reference  |  left  right   min(L,R)  L*R\n");
    for (double x = -5.0; x <= 5.0 + 1e-9; x += 0.5) {
        const double t = truth_visibility(x, 0.5, boxesBoth);
        const double r = reference(both, x);
        const double l = truth_visibility(x, 0.5, boxesLeft), rr = truth_visibility(x, 0.5, boxesRight);
        worst = std::max(worst, std::abs(t - r));
        std::printf("%5.1f   %.3f      %.3f     |  %.3f  %.3f   %.3f     %.3f\n", x, t, r, l, rr, std::min(l, rr), l * rr);
    }
    check(worst < 0.03, "reference within 0.03 of the disc integral (worst " + std::to_string(worst) + ")");

    // The blister, in the middle of the gap.
    const double tBoth = truth_visibility(0.0, 0.5, boxesBoth);
    const double tLeft = truth_visibility(0.0, 0.5, boxesLeft), tRight = truth_visibility(0.0, 0.5, boxesRight);
    const double rBoth = reference(both, 0.0), rLeft = reference(leftOnly, 0.0), rRight = reference(rightOnly, 0.0);
    check(tBoth < std::min(tLeft, tRight) - 0.15, "truth: the gap is darker than either shadow alone");
    check(std::abs(tBoth - (tLeft + tRight - 1.0)) < 0.01, "truth: the two hidden parts of the disc add up (union)");
    check(rBoth < std::min(rLeft, rRight) - 0.15, "reference: blister reproduced (" + std::to_string(rBoth) + " vs " +
                                                       std::to_string(std::min(rLeft, rRight)) + ")");
    check(std::abs(rBoth - tBoth) < 0.03, "reference: blister depth matches the integral");
    // Combining two separate shadows gets it wrong: the darker of the two misses it entirely, the
    // product only partly.
    check(std::min(tLeft, tRight) - tBoth > 0.15, "darker-of-two composition misses the blister");
    check(tLeft * tRight - tBoth > 0.02, "product composition underestimates the blister");
    std::printf("gap centre: union %.3f, reference %.3f, darker-of-two %.3f, product %.3f\n", tBoth, rBoth,
                std::min(tLeft, tRight), tLeft * tRight);
}

// Fraction of a disc (centre c, radius R) left of the line x = e.
double left_of(double c, double R, double e) {
    const double a = std::clamp((e - c) / R, -1.0, 1.0);
    return (kPi - std::acos(a) + a * std::sqrt(1.0 - a * a)) / kPi;
}
// Two half-plane occluders (x < -gap/2 and x > gap/2) at height d over a receiver at x.
double union_visibility(double x, double d, double gap, double tanTheta) {
    const double R = d * tanTheta;
    return 1.0 - left_of(x, R, -gap / 2) - (1.0 - left_of(x, R, gap / 2));
}

struct Profile { double rms; double centre; };
Profile denoise_error(double heightMeters, bool penumbraAware, std::uint32_t seed) {
    // 1 mm pixels on the ground seen from 1 m: 0.001 rad per pixel. Sun angular radius 0.05; the
    // slabs' edges are 1.2 mm apart.
    const std::uint32_t width = 96, height = 9;
    const double pixel = 0.001, depth = 1.0, tanTheta = std::tan(0.05), gap = 0.0012;
    std::vector<float> noisy(width * height), penumbra(width * height), depthMeters(width * height, float(depth)),
        truth(width * height);
    std::vector<Float3> normals(width * height, Float3{0, 1, 0});
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            const double wx = (double(x) - width / 2.0 + 0.5) * pixel;
            const std::size_t i = y * width + x;
            truth[i] = float(union_visibility(wx, heightMeters, gap, tanTheta));
            // Temporally accumulated estimate: 128 sun-disc samples (32 frames x 4 rays).
            int visible = 0, blocked = 0;
            for (int s = 0; s < 128; ++s) {
                const render::Float2 disk = concentric_square_to_disk({float(uniform(rng)), float(uniform(rng))});
                const double hitX = wx + disk.x * heightMeters * tanTheta;
                if (hitX < -gap / 2 || hitX > gap / 2) ++blocked; else ++visible;
            }
            noisy[i] = float(visible) / 128.0F;
            penumbra[i] = blocked > 0 ? shadow_penumbra_width_meters(0.05F, float(heightMeters)) : 0.0F;
        }
    ShadowDenoiseSettings settings;
    settings.radiansPerPixel = penumbraAware ? float(pixel / depth) : 0.0F;
    const auto filtered = denoise_shadow_visibility(width, height, noisy, penumbra, depthMeters, normals, settings);
    double sum = 0.0;
    const std::uint32_t row = height / 2;
    for (std::uint32_t x = 8; x + 8 < width; ++x) {
        const double e = filtered[row * width + x] - truth[row * width + x];
        sum += e * e;
    }
    const std::size_t mid = row * width + width / 2;
    return {std::sqrt(sum / double(width - 16)), std::abs(double(filtered[mid]) - double(truth[mid]))};
}

void test_penumbra_aware_denoise() {
    // Narrow: occluders 2 cm up give a 2 mm (two pixel) penumbra, and the blister is a few pixels
    // wide. The old 5x5 Gaussian (sigma 1.5 px) flattens it; the penumbra-aware one keeps it.
    double oldRms = 0, newRms = 0, oldCentre = 0, newCentre = 0;
    for (std::uint32_t s = 1; s <= 8; ++s) {
        const Profile o = denoise_error(0.02, false, s), n = denoise_error(0.02, true, s);
        oldRms += o.rms; newRms += n.rms; oldCentre += o.centre; newCentre += n.centre;
    }
    std::printf("narrow penumbra: old filter rms %.4f (gap centre off by %.4f), penumbra-aware rms %.4f (%.4f)\n",
                oldRms / 8, oldCentre / 8, newRms / 8, newCentre / 8);
    check(newCentre < 0.5 * oldCentre, "narrow blister: the gap centre stays close to the truth");
    check(newRms < oldRms, "narrow blister: smaller error overall");
    // Wide: 3 m up, a 30 cm penumbra, far wider than the filter: same as before (the sigma clamps to
    // the full 1.5 px).
    double oldWide = 0, newWide = 0;
    for (std::uint32_t s = 1; s <= 8; ++s) { oldWide += denoise_error(3.0, false, s).rms; newWide += denoise_error(3.0, true, s).rms; }
    std::printf("wide penumbra: old rms %.4f, penumbra-aware rms %.4f\n", oldWide / 8, newWide / 8);
    check(std::abs(oldWide - newWide) < 1e-6, "wide penumbrae are filtered exactly as before");
    // Fully lit everywhere: unchanged values.
    const std::vector<float> ones(16, 1.0F), zeros(16, 0.0F), depth(16, 5.0F);
    const std::vector<Float3> up(16, Float3{0, 1, 0});
    ShadowDenoiseSettings settings;
    settings.radiansPerPixel = 0.001F;
    const auto lit = denoise_shadow_visibility(4, 4, ones, zeros, depth, up, settings);
    check(std::all_of(lit.begin(), lit.end(), [](float v) { return std::abs(v - 1.0F) < 1e-6F; }), "lit stays lit");
    check(shadow_penumbra_width_meters(0.05F, 2.0F) > 0.19F && shadow_penumbra_width_meters(0.05F, 2.0F) < 0.21F &&
              shadow_penumbra_width_meters(0.05F, 0.0F) == 0.0F,
          "penumbra width = 2 tan(radius) x blocker distance");
    const float perPixel = denoise_radians_per_pixel(1.0471976F, 1080U);   // 60 degrees, 1080 rows
    check(std::abs(perPixel - 2.0F * std::tan(0.5235988F) / 1080.0F) < 1e-9F && denoise_radians_per_pixel(1.0F, 0U) == 0.0F,
          "radians per pixel for the host constant");
}

void test_concentric_disc() {
    // Equal-area: the four quadrants of the square map to equal quarters of the disc, the centre
    // to the centre, corners to the rim; 64 stratified samples cover the disc evenly.
    const render::Float2 c = concentric_square_to_disk({0.5F, 0.5F});
    check(std::abs(c.x) < 1e-7F && std::abs(c.y) < 1e-7F, "centre -> centre");
    const render::Float2 corner = concentric_square_to_disk({1.0F, 1.0F});
    check(std::abs(std::hypot(corner.x, corner.y) - 1.0F) < 1e-6F, "corner -> rim");
    int inner = 0;
    double meanX = 0, meanY = 0;
    const int n = 256;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const render::Float2 p = concentric_square_to_disk({(i + 0.5F) / n, (j + 0.5F) / n});
            check(p.x * p.x + p.y * p.y <= 1.0F + 1e-6F, "inside the disc");
            inner += p.x * p.x + p.y * p.y < 0.5F ? 1 : 0;   // the inner disc of half the area
            meanX += p.x; meanY += p.y;
        }
    check(std::abs(double(inner) / (n * n) - 0.5) < 0.01, "equal-area: half the samples in the inner half-area disc (" +
                                                              std::to_string(double(inner) / (n * n)) + ")");
    check(std::abs(meanX) < 1e-5 && std::abs(meanY) < 1e-5, "balanced around the centre");
}
std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

// The GPU shaders carry the same rules as the CPU mirrors tested above.
void test_shader_sources() {
    const std::filesystem::path root = DVE_SOURCE_DIR;
    const std::string sampling = read_text(root / "shaders/common/random_sampling.hlsli");
    const std::string generate = read_text(root / "shaders/generate_shadow_rays.hlsl");
    const std::string resolve = read_text(root / "shaders/resolve_shadows.hlsl");
    const std::string denoise = read_text(root / "shaders/spatial_lighting_denoise.hlsl");
    check(sampling.find("float2 ConcentricSquareToDisk(float2 u)") != std::string::npos, "HLSL concentric map");
    check(generate.find("ConcentricSquareToDisk(randomPair) * tan(gShadowSoftnessRadians)") != std::string::npos,
          "sun-disc rays use the concentric map");
    check(resolve.find("RWStructuredBuffer<float> gShadowPenumbraMeters : register(u3)") != std::string::npos &&
              resolve.find("2.0F * tan(gShadowSoftnessRadians)") != std::string::npos,
          "resolve writes the penumbra width");
    check(denoise.find("gDenoiseShadowPenumbraMeters") != std::string::npos &&
              denoise.find("clamp(0.25F * penumbraMeters / pixelMeters, 0.25F, spatialSigma)") != std::string::npos &&
              denoise.find("gDenoiseRadiansPerPixel") != std::string::npos,
          "denoiser sizes the shadow blur to the penumbra");
}
}  // namespace

int main() {
    test_shader_sources();
    test_concentric_disc();
    test_penumbra_aware_denoise();
    test_reference_matches_the_disc_integral();
    if (g_failures == 0) std::printf("shadow blister tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
