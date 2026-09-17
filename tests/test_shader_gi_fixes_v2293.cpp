#include "dve/gpu_render_environment.hpp"
#include "dve/render/ray_lighting_reference.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x + " at " + std::to_string(__LINE__)); } while(false)
constexpr float kPi = 3.14159265358979323846F;

bool close(float a, float b, float tolerance = 1.0e-5F) {
    return std::abs(a - b) <= tolerance;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    CHECK(stream.good());
    std::ostringstream out;
    out << stream.rdbuf();
    return out.str();
}

std::uint32_t pcg_hash(std::uint32_t input) {
    const std::uint32_t state = input * 747796405U + 2891336453U;
    const std::uint32_t word =
        ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

dve::render::Float2 hash2(std::uint32_t seed) {
    const std::uint32_t a = pcg_hash(seed);
    const std::uint32_t b = pcg_hash(a);
    constexpr float inverse = 1.0F / 4294967295.0F;
    return {static_cast<float>(a) * inverse, static_cast<float>(b) * inverse};
}

dve::Float3 cosine_sample(dve::render::Float2 random) {
    const float radius = std::sqrt(random.x);
    const float theta = 2.0F * kPi * random.y;
    return {radius * std::cos(theta), radius * std::sin(theta),
            std::sqrt(std::max(0.0F, 1.0F - random.x))};
}

float dot(dve::Float3 a, dve::Float3 b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

void test_world_scale_and_distance_falloff() {
    using namespace dve::render;
    CHECK(close(meters_to_voxel_units(12.0F, 0.10F), 120.0F));
    CHECK(close(meters_to_voxel_units(0.015F, 0.10F), 0.15F));
    CHECK(close(voxel_units_to_meters(5.0F, 0.10F), 0.50F));
    CHECK(close(distance_weighted_ao_visibility(false, 0.0F, 2.0F, 0.10F), 1.0F));
    const float nearVisibility = distance_weighted_ao_visibility(true, 1.0F, 2.0F, 0.10F);
    const float farVisibility = distance_weighted_ao_visibility(true, 19.0F, 2.0F, 0.10F);
    CHECK(nearVisibility < 0.2F);
    CHECK(farVisibility > 0.99F);
    CHECK(farVisibility > nearVisibility);
}

void test_hammersley_variance() {
    constexpr std::uint32_t samples = 8U;
    constexpr std::uint32_t trials = 20000U;
    constexpr float truth = 0.3738F;
    const float coneCosine = std::cos(40.0F * kPi / 180.0F);
    const dve::Float3 coneAxis{
        std::sin(25.0F * kPi / 180.0F), 0.0F,
        std::cos(25.0F * kPi / 180.0F)};

    double whiteSquaredError = 0.0;
    double stratifiedSquaredError = 0.0;
    for (std::uint32_t pixel = 0U; pixel < trials; ++pixel) {
        std::uint32_t whiteHits = 0U;
        std::uint32_t stratifiedHits = 0U;
        const auto rotation = hash2(pcg_hash(pixel ^ 12345U));
        for (std::uint32_t sample = 0U; sample < samples; ++sample) {
            const auto white = hash2(pcg_hash(pixel * 1315423911U + sample * 6271U));
            if (dot(cosine_sample(white), coneAxis) >= coneCosine) ++whiteHits;
            const auto stratified = dve::render::rotated_hammersley_2d(
                sample, samples, rotation);
            if (dot(cosine_sample(stratified), coneAxis) >= coneCosine) ++stratifiedHits;
        }
        const double whiteError = static_cast<double>(whiteHits) / samples - truth;
        const double stratifiedError = static_cast<double>(stratifiedHits) / samples - truth;
        whiteSquaredError += whiteError * whiteError;
        stratifiedSquaredError += stratifiedError * stratifiedError;
    }
    const double whiteRmse = std::sqrt(whiteSquaredError / trials);
    const double stratifiedRmse = std::sqrt(stratifiedSquaredError / trials);
    CHECK(whiteRmse > 0.16 && whiteRmse < 0.18);
    CHECK(stratifiedRmse > 0.09 && stratifiedRmse < 0.11);
    CHECK(whiteSquaredError / stratifiedSquaredError > 2.8);
}

void test_sun_bounce_radiance() {
    const dve::Float3 red{1.0F, 0.05F, 0.02F};
    const dve::Float3 black{};
    const dve::Float3 white{1.0F, 1.0F, 1.0F};
    const dve::Float3 up{0.0F, 1.0F, 0.0F};
    const auto withoutSun = dve::render::one_bounce_diffuse_radiance(
        red, 0.0F, black, black, white, 4.0F, up, up, 0.0F);
    const auto withSun = dve::render::one_bounce_diffuse_radiance(
        red, 0.0F, black, black, white, 4.0F, up, up, 1.0F);
    CHECK(close(withoutSun.x, 0.0F));
    CHECK(withSun.x > 1.2F);
    CHECK(withSun.x > withSun.y * 10.0F);
}

void test_temporal_accumulation_and_rejection() {
    dve::render::LightingTemporalSettings settings;
    settings.historyWeight = 0.90F;
    settings.depthToleranceMeters = 0.05F;
    settings.normalThreshold = 0.85F;
    settings.maximumHistory = 32U;
    CHECK(settings.validate());

    dve::render::LightingTemporalSample current;
    current.indirectDiffuse = {1.0F, 0.0F, 0.0F};
    current.ambientOcclusion = 0.4F;
    current.shadowVisibility = 0.6F;
    current.depthMeters = 2.0F;
    current.normal = {0.0F, 1.0F, 0.0F};

    auto history = current;
    history.indirectDiffuse = {0.0F, 0.0F, 1.0F};
    history.ambientOcclusion = 0.8F;
    history.shadowVisibility = 1.0F;
    history.historyLength = 8U;

    const auto accumulated = dve::render::accumulate_lighting_history(
        current, history, settings);
    CHECK(accumulated.historyLength == 9U);
    CHECK(accumulated.indirectDiffuse.z > 0.6F);
    CHECK(accumulated.indirectDiffuse.x < 0.4F);

    history.depthMeters = 3.0F;
    const auto rejected = dve::render::accumulate_lighting_history(
        current, history, settings);
    CHECK(rejected.historyLength == 1U);
    CHECK(close(rejected.indirectDiffuse.x, current.indirectDiffuse.x));
    CHECK(close(rejected.indirectDiffuse.z, current.indirectDiffuse.z));
}

void test_environment_abi_scale() {
    dve::RenderEnvironment environment;
    const auto gpu = dve::pack_gpu_render_environment(environment, 1280U, 720U, 0.125F);
    CHECK(close(gpu.metersPerVoxel, 0.125F));
    CHECK(sizeof(dve::GpuRenderEnvironment) == 144U);
}

void test_shader_source_contracts() {
    const std::filesystem::path root = DVE_SOURCE_DIR;
    const std::string environment = read_text(root / "shaders/common/render_environment.hlsli");
    const std::string aoGenerate = read_text(root / "shaders/generate_ao_rays.hlsl");
    const std::string aoResolve = read_text(root / "shaders/resolve_ao.hlsl");
    const std::string giGenerate = read_text(root / "shaders/generate_gi_rays.hlsl");
    const std::string giSun = read_text(root / "shaders/generate_gi_sun_rays.hlsl");
    const std::string giResolve = read_text(root / "shaders/resolve_gi.hlsl");
    const std::string shadows = read_text(root / "shaders/generate_shadow_rays.hlsl");
    const std::string primary = read_text(root / "shaders/shade_primary.hlsl");
    const std::string sampling = read_text(root / "shaders/common/random_sampling.hlsli");
    const std::string temporal = read_text(root / "shaders/temporal_lighting_accumulate.hlsl");
    const std::string denoise = read_text(root / "shaders/spatial_lighting_denoise.hlsl");

    CHECK(environment.find("float gMetersPerVoxel") != std::string::npos);
    CHECK(environment.find("MetersToVoxelUnits") != std::string::npos);
    CHECK(aoGenerate.find("MetersToVoxelUnits(gAoMaxDistanceMeters)") != std::string::npos);
    CHECK(aoResolve.find("result.distance / maximumDistanceVoxels") != std::string::npos);
    CHECK(aoResolve.find("proximity * proximity") != std::string::npos);
    CHECK(giGenerate.find("TraceHitPosition(gPrimaryRays[pixelIndex], primary)") != std::string::npos);
    CHECK(giGenerate.find("RotatedHammersley2D") != std::string::npos);
    CHECK(giSun.find("gGiSunRays") != std::string::npos);
    CHECK(giResolve.find("gGiSunResults") != std::string::npos);
    CHECK(giResolve.find("directSun") != std::string::npos);
    CHECK(shadows.find("MetersToVoxelUnits(distanceMeters)") != std::string::npos);
    CHECK(primary.find("MarchSubsurfaceThicknessVoxels") != std::string::npos);
    CHECK(primary.find("VoxelUnitsToMeters(thicknessVoxels)") != std::string::npos);
    CHECK(sampling.find("RadicalInverseBase2") != std::string::npos);
    CHECK(temporal.find("gLightingDepthToleranceMeters") != std::string::npos);
    CHECK(temporal.find("dot(currentNormal, historyNormal)") != std::string::npos);
    CHECK(denoise.find("depthWeight") != std::string::npos);
    CHECK(denoise.find("normalWeight") != std::string::npos);
}

} // namespace

int main() {
    try {
        test_world_scale_and_distance_falloff();
        test_hammersley_variance();
        test_sun_bounce_radiance();
        test_temporal_accumulation_and_rejection();
        test_environment_abi_scale();
        test_shader_source_contracts();
        std::cout << "dve_v2293_shader_gi_fixes_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dve_v2293_shader_gi_fixes_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}
