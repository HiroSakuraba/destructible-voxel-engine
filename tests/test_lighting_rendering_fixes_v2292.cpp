#include "dve/environment_lighting.hpp"
#include "dve/render/cascaded_shadow_map.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x + " at " + std::to_string(__LINE__)); } while(false)

constexpr float kPi = 3.14159265358979323846F;

bool close(float a, float b, float tolerance = 1.0e-4F) {
    return std::abs(a - b) <= tolerance;
}

dve::Float3 normalize(dve::Float3 value) {
    const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    return length > 1.0e-8F ? dve::Float3{value.x / length, value.y / length, value.z / length}
                            : dve::Float3{0.0F, 0.0F, 1.0F};
}

dve::EnvironmentCube constant_cube(std::uint32_t resolution, dve::Float3 value) {
    return {resolution, std::vector<dve::Float3>(6ULL * resolution * resolution, value)};
}

dve::EnvironmentCube directional_cube(std::uint32_t resolution) {
    dve::EnvironmentCube cube{resolution, std::vector<dve::Float3>(6ULL * resolution * resolution)};
    for (std::uint32_t face = 0; face < 6U; ++face) {
        for (std::uint32_t y = 0; y < resolution; ++y) {
            for (std::uint32_t x = 0; x < resolution; ++x) {
                const dve::Float3 direction = dve::cube_lookup_to_direction(
                    static_cast<dve::CubeFace>(face),
                    {(static_cast<float>(x) + 0.5F) / static_cast<float>(resolution),
                     (static_cast<float>(y) + 0.5F) / static_cast<float>(resolution)});
                cube.texels[cube.face_offset(static_cast<dve::CubeFace>(face)) +
                            static_cast<std::size_t>(y) * resolution + x] =
                    {0.5F * (direction.x + 1.0F), 0.5F * (direction.y + 1.0F),
                     0.5F * (direction.z + 1.0F)};
            }
        }
    }
    return cube;
}

dve::EnvironmentCube checker_cube(std::uint32_t resolution) {
    dve::EnvironmentCube cube{resolution, std::vector<dve::Float3>(6ULL * resolution * resolution)};
    for (std::uint32_t face = 0; face < 6U; ++face) {
        for (std::uint32_t y = 0; y < resolution; ++y) {
            for (std::uint32_t x = 0; x < resolution; ++x) {
                const float value = ((x + y + face) & 1U) == 0U ? 0.0F : 1.0F;
                cube.texels[cube.face_offset(static_cast<dve::CubeFace>(face)) +
                            static_cast<std::size_t>(y) * resolution + x] = {value, value, value};
            }
        }
    }
    return cube;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    CHECK(stream.good());
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

void test_split_sum_brdf_lut() {
    dve::IblBakeSettings settings;
    settings.irradianceResolution = 1U;
    settings.specularResolution = 1U;
    settings.specularMipLevels = 1U;
    settings.sampleCount = 512U;
    settings.brdfResolution = 11U;
    const auto sample = dve::integrate_environment_brdf(0.7F, 0.4F, settings.sampleCount);
    CHECK(close(sample.scale, 0.8895F, 0.006F));
    CHECK(sample.scale > 0.87F);
    const auto baked = dve::bake_image_based_lighting(
        constant_cube(1U, {1.0F, 1.0F, 1.0F}), settings);
    CHECK(baked.validate());
}

void test_seamless_cube_filtering() {
    const auto cube = directional_cube(8U);
    const dve::Float3 leftDirection = normalize({1.0F, 0.12F, 0.999F});
    const dve::Float3 rightDirection = normalize({0.999F, 0.12F, 1.0F});
    const auto left = dve::sample_environment_cube(cube, leftDirection);
    const auto right = dve::sample_environment_cube(cube, rightDirection);
    CHECK(std::abs(left.x - right.x) < 0.025F);
    CHECK(std::abs(left.y - right.y) < 0.025F);
    CHECK(std::abs(left.z - right.z) < 0.025F);
    const auto expected = normalize({1.0F, 0.12F, 1.0F});
    CHECK(std::abs(left.x - 0.5F * (expected.x + 1.0F)) < 0.06F);
    CHECK(std::abs(left.z - 0.5F * (expected.z + 1.0F)) < 0.06F);
}

void test_source_mips_and_rough_prefilter() {
    const auto source = checker_cube(32U);
    const auto mips = dve::build_environment_cube_mip_chain(source);
    CHECK(mips.validate());
    CHECK(mips.levels.size() == 6U);
    CHECK(mips.levels.back().resolution == 1U);
    for (const auto texel : mips.levels.back().texels) {
        CHECK(std::abs(texel.x - 0.5F) < 0.12F);
    }

    dve::IblBakeSettings settings;
    settings.irradianceResolution = 1U;
    settings.specularResolution = 8U;
    settings.specularMipLevels = 4U;
    settings.sampleCount = 64U;
    settings.brdfResolution = 2U;
    const auto baked = dve::bake_image_based_lighting(source, settings);
    const auto& roughest = baked.specularPrefilter.levels.back();
    float minimum = 1.0F;
    float maximum = 0.0F;
    for (const auto texel : roughest.texels) {
        minimum = std::min(minimum, texel.x);
        maximum = std::max(maximum, texel.x);
    }
    CHECK(maximum - minimum < 0.30F);
}

void test_specular_occlusion() {
    dve::IblBakeResult ibl;
    ibl.diffuseIrradiance = constant_cube(1U, {0.0F, 0.0F, 0.0F});
    ibl.specularPrefilter.levels.push_back(constant_cube(1U, {1.0F, 1.0F, 1.0F}));
    ibl.brdfResolution = 1U;
    ibl.brdfLut.push_back({1.0F, 0.0F});
    CHECK(ibl.validate());

    dve::IblMaterialInput material;
    material.normal = {0.0F, 0.0F, 1.0F};
    material.viewDirection = {0.0F, 0.0F, 1.0F};
    material.baseColor = {1.0F, 1.0F, 1.0F};
    material.metallic = 0.0F;
    material.roughness = 0.1F;
    material.specular = 0.5F;
    material.ambientOcclusion = 0.1F;
    const auto reflected = dve::evaluate_image_based_lighting(ibl, material);
    const float expectedOcclusion = std::clamp(
        std::pow(1.0F + material.ambientOcclusion,
                 std::exp2(-16.0F * material.roughness - 1.0F)) - 1.0F +
            material.ambientOcclusion,
        0.0F, 1.0F);
    CHECK(close(reflected.x, 0.04F * expectedOcclusion, 2.0e-5F));
    CHECK(reflected.x > 0.04F * material.ambientOcclusion);
}

void test_shadow_bias_and_bounds() {
    dve::camera::CameraPose camera;
    camera.position = {0.0F, 3.0F, 8.0F};
    camera.target = {0.0F, 1.0F, 0.0F};
    camera.lens.nearPlaneMeters = 0.1F;
    camera.lens.farPlaneMeters = 500.0F;
    camera.lens.aspectRatio = 16.0F / 9.0F;

    dve::render::CascadedShadowSettings unbiasedSettings;
    unbiasedSettings.cascadeResolution = 256U;
    unbiasedSettings.maximumDistanceMeters = 100.0F;
    unbiasedSettings.receiverBiasMeters = 0.0F;
    unbiasedSettings.normalBiasMeters = 0.0F;
    auto biasedSettings = unbiasedSettings;
    biasedSettings.receiverBiasMeters = 0.04F;
    biasedSettings.normalBiasMeters = 0.08F;

    const auto unbiasedPlan = dve::render::make_cascaded_shadow_plan(
        camera, {0.4F, 0.8F, 0.2F}, unbiasedSettings);
    const auto biasedPlan = dve::render::make_cascaded_shadow_plan(
        camera, {0.4F, 0.8F, 0.2F}, biasedSettings);
    const auto point = unbiasedPlan.cascades.front().snappedCenter;
    const auto unbiased = dve::render::cascaded_shadow_atlas_coordinate(unbiasedPlan, 0U, point);
    const auto receiverBiased = dve::render::cascaded_shadow_atlas_coordinate(biasedPlan, 0U, point);
    const auto normalBiased = dve::render::cascaded_shadow_atlas_coordinate(
        biasedPlan, 0U, point, {-biasedPlan.cascades.front().lightForward.x,
                                -biasedPlan.cascades.front().lightForward.y,
                                -biasedPlan.cascades.front().lightForward.z});
    CHECK(receiverBiased.z < unbiased.z);
    CHECK(normalBiased.z < receiverBiased.z);

    const auto bounds = dve::render::cascaded_shadow_atlas_uv_bounds(biasedPlan, 0U);
    CHECK(receiverBiased.x >= bounds.minimumU && receiverBiased.x <= bounds.maximumU);
    CHECK(receiverBiased.y >= bounds.minimumV && receiverBiased.y <= bounds.maximumV);
    const auto& cascade = biasedPlan.cascades.front();
    const dve::Float3 outside{point.x + cascade.lightRight.x * cascade.radiusMeters * 1.1F,
                             point.y + cascade.lightRight.y * cascade.radiusMeters * 1.1F,
                             point.z + cascade.lightRight.z * cascade.radiusMeters * 1.1F};
    const auto invalid = dve::render::cascaded_shadow_atlas_coordinate(biasedPlan, 0U, outside);
    CHECK(invalid.x < 0.0F && invalid.y < 0.0F);
}

void test_shader_equations() {
    const std::filesystem::path sourceRoot = DVE_SOURCE_DIR;
    const std::string pbr = read_text(sourceRoot / "shaders/common/pbr_lighting.hlsli");
    const std::string ibl = read_text(sourceRoot / "shaders/common/environment_ibl.hlsli");
    const std::string primary = read_text(sourceRoot / "shaders/shade_primary.hlsl");
    const std::string shadow = read_text(sourceRoot / "shaders/common/cascaded_shadow_map.hlsli");

    CHECK(pbr.find("dot(-shadingNormal, lightDirection)") != std::string::npos);
    CHECK(pbr.find("max(foliageColor, 0.0F.xxx) * (1.0F / kPi)") != std::string::npos);
    CHECK(pbr.find("base * baseTransmission * baseTransmission") != std::string::npos);
    CHECK(ibl.find("diffuse * ao + specularLighting * specularOcclusion") != std::string::npos);
    CHECK(ibl.find("exp2(-16.0F * saturate(roughness) - 1.0F)") != std::string::npos);
    CHECK(primary.find("gEnvironmentLightingEnabled == 0u") != std::string::npos);
    CHECK(shadow.find("uint cascadeCount") != std::string::npos);
    CHECK(shadow.find("interiorMinimum") != std::string::npos);
    CHECK(shadow.find("tapUv = clamp") != std::string::npos);

    const float frontLambert = 1.0F / kPi;
    const float transmittedLambert = 1.0F / kPi;
    CHECK(close(frontLambert, transmittedLambert));
    const float fresnel = 0.04F;
    CHECK(close((1.0F - fresnel) * (1.0F - fresnel), 0.9216F, 1.0e-5F));
}
}

int main() {
    try {
        test_split_sum_brdf_lut();
        test_seamless_cube_filtering();
        test_source_mips_and_rough_prefilter();
        test_specular_occlusion();
        test_shadow_bias_and_bounds();
        test_shader_equations();
        std::cout << "dve_v2292_lighting_rendering_fixes_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dve_v2292_lighting_rendering_fixes_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}
