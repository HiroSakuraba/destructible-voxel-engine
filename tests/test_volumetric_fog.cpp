// Volumetric height fog with light shafts (render/volumetric_fog.hpp):
//   - the optical depth closed form (base height, exponential falloff) matches numerical
//     integration for climbing, descending and level rays, above, below and across the base;
//   - uniform unshadowed fog: in-scattering and transmittance match the closed forms exactly;
//   - a shadow boundary: only the lit part of the ray scatters sunlight, exactly when the boundary
//     falls on a step edge and on average otherwise;
//   - in a hybrid frame: a light shaft under a hole in a roof, the passes still sum, and the result
//     does not depend on the thread count; density 0 changes nothing.
#include "dve/render/volumetric_fog.hpp"
#include "dve/render/atmosphere.hpp"
#include "dve/render/voxel_reference_renderer.hpp"
#include "dve/transform.hpp"

#include <algorithm>
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

RenderEnvironment foggy() {
    RenderEnvironment e;
    e.volumetricFogDensityPerMeter = 0.05F;
    e.volumetricFogHeightFalloffPerMeter = 0.0F;
    e.volumetricFogAlbedo = {0.9F, 0.8F, 0.7F};
    e.volumetricFogAnisotropy = 0.5F;
    e.sunDirection = {0.0F, 1.0F, 0.0F};
    e.sunColor = {1, 1, 1};
    e.sunIntensity = 2.0F;
    e.globalIlluminationMode = GlobalIlluminationMode::Off;
    e.shadowMode = ShadowMode::Hard;
    e.shadowStrength = 1.0F;
    return e;
}

void test_optical_depth() {
    RenderEnvironment e = foggy();
    e.volumetricFogHeightFalloffPerMeter = 0.2F;
    e.volumetricFogBaseHeightMeters = 3.0F;
    double worst = 0.0;
    for (const float h0 : {-2.0F, 2.5F, 3.0F, 4.0F, 12.0F})
        for (const float s : {-0.9F, -0.3F, -0.01F, 0.0F, 0.02F, 0.4F, 1.0F})
            for (const float L : {0.5F, 7.0F, 40.0F}) {
                double numeric = 0.0;
                const int n = 200000;
                for (int i = 0; i < n; ++i) {
                    const double t = (i + 0.5) * L / n;
                    numeric += volumetric_fog_density(e, static_cast<float>(h0 + s * t)) * (L / n);
                }
                const double closed = volumetric_fog_optical_depth(e, h0, s, L);
                worst = std::max(worst, std::abs(closed - numeric) / std::max(1e-12, numeric));
            }
    std::printf("optical depth, closed form vs numerical: worst relative error %.2e\n", worst);
    check(worst < 1e-5, "the optical depth closed form is exact");
}

void test_uniform() {
    const RenderEnvironment e = foggy();
    const Float3 dir = normalize(Float3{1.0F, 0.0F, 0.3F});
    const float L = 30.0F;
    const VolumetricFogSegment fog = integrate_volumetric_fog(e, {0, 1, 0}, dir, L, 1.0F, {}, 16U, 0.5F);
    const double sigma = 0.05, T = std::exp(-sigma * L);
    const double light = kPi * henyey_greenstein_phase(0.5F, dot(dir, Float3{0, 1, 0})) * 2.0;
    const Float3 exact{static_cast<float>(0.9 * light * (1 - T)), static_cast<float>(0.8 * light * (1 - T)),
                       static_cast<float>(0.7 * light * (1 - T))};
    std::printf("uniform fog, 30 m: in-scattered (%.6f %.6f %.6f), exact (%.6f %.6f %.6f); T %.6f (exact %.6f)\n", fog.inscattered.x,
                fog.inscattered.y, fog.inscattered.z, exact.x, exact.y, exact.z, fog.transmittance, T);
    check(std::abs(fog.transmittance - T) < 1e-6 && std::abs(fog.inscattered.x - exact.x) < 1e-5 * exact.x &&
              std::abs(fog.inscattered.z - exact.z) < 1e-5 * exact.z,
          "uniform fog matches its closed form");
    // Ambient sky light adds albedo x mean sky irradiance x (1 - T).
    RenderEnvironment lit = e;
    lit.globalIlluminationMode = GlobalIlluminationMode::AmbientHemisphere;
    lit.globalIlluminationIntensity = 1.0F;
    lit.sunIntensity = 0.0F;
    const VolumetricFogSegment amb = integrate_volumetric_fog(lit, {0, 1, 0}, dir, L, 1.0F, {}, 16U, 0.5F);
    const float mean = 0.5F * (lit.skyColor.y + lit.groundColor.y);
    check(std::abs(amb.inscattered.y - 0.8 * mean * (1 - T)) < 1e-5, "sky light scatters as albedo x mean irradiance");
    // Under a roof (every upward ray blocked) the sky light does not reach the fog.
    const AmbientOcclusionRay roof = [](Float3, Float3 d, float) -> std::optional<float> {
        if (d.y > 0.99F) return 5.0F;
        return std::nullopt;
    };
    const VolumetricFogSegment covered = integrate_volumetric_fog(lit, {0, 1, 0}, dir, L, 1.0F, roof, 16U, 0.5F);
    check(covered.inscattered.y == 0.0F && covered.transmittance == amb.transmittance, "a roof keeps the sky light out of the fog");
    // Height falloff: a climbing ray keeps exp(-closed-form depth).
    RenderEnvironment layered = e;
    layered.volumetricFogHeightFalloffPerMeter = 0.3F;
    const Float3 up = normalize(Float3{0.5F, 0.6F, 0.0F});
    const VolumetricFogSegment climb = integrate_volumetric_fog(layered, {0, 0.5F, 0}, up, 25.0F, 1.0F, {}, 64U, 0.5F);
    check(std::abs(climb.transmittance - std::exp(-volumetric_fog_optical_depth(layered, 0.5F, up.y, 25.0F))) < 1e-6,
          "height fog transmittance is the exact optical depth");
}

void test_shadow_boundary() {
    // Sun overhead; everything at x < 0 is in shadow. A level ray from x = -20 to +20.
    const RenderEnvironment e = foggy();
    const AmbientOcclusionRay shade = [](Float3 origin, Float3, float) -> std::optional<float> {
        if (origin.x < 0.0F) return 1.0F;
        return std::nullopt;
    };
    const double sigma = 0.05;
    const double light = kPi * henyey_greenstein_phase(0.5F, 0.0F) * 2.0 * 0.9;
    const double exact = light * (std::exp(-sigma * 20.0) - std::exp(-sigma * 40.0));
    const VolumetricFogSegment aligned = integrate_volumetric_fog(e, {-20, 1, 0}, {1, 0, 0}, 40.0F, 1.0F, shade, 32U, 0.37F);
    std::printf("half-shadowed ray, boundary on a step edge: %.6f (exact %.6f), %llu shadow rays\n", aligned.inscattered.x, exact,
                static_cast<unsigned long long>(aligned.shadowRays));
    check(std::abs(aligned.inscattered.x - exact) < 1e-5 * exact && aligned.shadowRays == 32U, "only the lit half scatters sunlight");
    // Boundary inside a step (x = 0 at t = 20 with 30 steps of 4/3): averaged over the jitter, exact.
    double mean = 0.0;
    const int n = 3000;
    for (int k = 0; k < n; ++k)
        mean += integrate_volumetric_fog(e, {-20, 1, 0}, {1, 0, 0}, 40.0F, 1.0F, shade, 30U, (k + 0.5F) / n).inscattered.x / n;
    std::printf("boundary inside a step, averaged over the jitter: %.6f (exact %.6f)\n", mean, exact);
    check(std::abs(mean - exact) < 2e-3 * exact, "the jittered shadow sample is unbiased (up to the step's light falloff)");
}

void test_hybrid() {
    // Floor at y = 1, a roof at y = 40..41 over everything but a 6-wide slot at x in [-3, 3).
    VoxelObject object(1U);
    for (int x = -40; x < 40; ++x)
        for (int z = -40; z < 40; ++z) {
            object.set_voxel({x, 0, z}, 1);
            if (x < -3 || x >= 3) object.set_voxel({x, 40, z}, 1);
        }
    std::array<VoxelMaterialDefinition, 2> materials{};
    materials[1].baseColor = {0.3F, 0.3F, 0.3F, 1.0F};
    const std::vector<VoxelReferenceInstance> voxels{{1U, &object, {}, materials, true, {1, 1, 1}}};
    PolygonCamera camera;
    camera.position = {0.5F, 20.0F, -35.0F};
    camera.target = {0.5F, 20.0F, 0.0F};
    camera.up = {0, 1, 0};
    camera.verticalFieldOfViewRadians = 1.0F;
    camera.nearPlane = 0.5F;
    camera.farPlane = 400.0F;
    RenderEnvironment e = foggy();
    e.volumetricFogDensityPerMeter = 0.2F;
    e.volumetricFogMaxDistanceMeters = 8.0F;   // metres: 80 voxels at 0.1 m
    e.volumetricFogSamples = 64;
    e.sunIntensity = 3.0F;
    const auto draw = [&](const RenderEnvironment& env, std::uint32_t threads) {
        PolygonRenderTarget target;
        target.resize(48, 32);
        target.enable_passes();
        const auto stats = render_hybrid_reference(voxels, {}, camera, env, target, {}, 0.1F, threads);
        return std::pair{std::move(target), stats};
    };
    const auto [frame, stats] = draw(e, 4U);
    const auto [single, singleStats] = draw(e, 1U);
    bool same = true;
    double worstSum = 0.0;
    for (std::size_t i = 0; i < frame.hdrColor.size(); ++i) {
        same = same && frame.hdrColor[i].x == single.hdrColor[i].x && frame.hdrColor[i].z == single.hdrColor[i].z;
        const Float3 s = frame.passes.light_sum(i);
        worstSum = std::max(worstSum, static_cast<double>(std::abs(s.x - frame.hdrColor[i].x) + std::abs(s.z - frame.hdrColor[i].z)) /
                                          (1e-6 + std::abs(frame.hdrColor[i].x) + std::abs(frame.hdrColor[i].z)));
    }
    // The centre column looks through the shaft; a quarter across looks through shadowed air.
    const std::size_t row = 16U * 48U;
    const float shaft = frame.passes.fog[row + 24U].x, shadowed = frame.passes.fog[row + 4U].x;
    std::printf("hybrid: fog light through the shaft %.4f, through shadowed air %.4f; %llu shadow rays; passes sum to %.1e\n", shaft,
                shadowed, static_cast<unsigned long long>(stats.volumetricFog.shadowRays), worstSum);
    check(shaft > 3.0F * shadowed && stats.volumetricFog.shadowRays > 0U, "a light shaft under the slot");
    check(same, "the same image for 1 and 4 threads");
    check(worstSum < 1e-5, "the fog's light goes to the fog pass and the passes still sum");
    RenderEnvironment clear = e;
    clear.volumetricFogDensityPerMeter = 0.0F;
    const auto [plain, plainStats] = draw(clear, 2U);
    check(plainStats.volumetricFog.pixels == 0U, "density 0: no fog pass runs");
    RenderEnvironment bad = e;
    bad.volumetricFogAnisotropy = 0.99F;
    check(!bad.validate(), "anisotropy above 0.95 is rejected");
    (void)plain;
}

} // namespace

int main() {
    test_optical_depth();
    test_uniform();
    test_shadow_boundary();
    test_hybrid();
    if (g_failures == 0) std::printf("dve_volumetric_fog_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
