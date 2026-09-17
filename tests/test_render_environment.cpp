#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>

#include "dve/gpu_render_environment.hpp"

namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #__VA_ARGS__ "\n"; ++failures; } } while (false)
using namespace dve;

void test_default_environment_is_valid_and_gi_is_on() {
    const RenderEnvironment environment;
    std::string error;
    CHECK(environment.validate(&error));
    CHECK(environment.globalIlluminationMode == GlobalIlluminationMode::VoxelOneBounce);
    CHECK(environment.globalIlluminationSamples == 4U);
    CHECK(std::fabs(environment.globalIlluminationIntensity - 0.65F) < 1.0e-6F);
    CHECK(environment.shadowMode == ShadowMode::Soft);
    CHECK(environment.shadowSamples == 4U);
    CHECK(std::fabs(environment.shadowStrength - 0.85F) < 1.0e-6F);
    CHECK(std::fabs(environment.sunIntensity - 2.4F) < 1.0e-6F);
    if (!error.empty()) std::cerr << "  detail: " << error << "\n";
}

void test_validation_rejects_bad_values() {
    RenderEnvironment environment;
    environment.sunDirection = {0.0F, 0.0F, 0.0F}; CHECK(!environment.validate());
    environment = {}; environment.exposure = 0.0F; CHECK(!environment.validate());
    environment = {}; environment.bloomThreshold = -0.5F; CHECK(!environment.validate());
    environment = {}; environment.skyColor = {-0.1F, 0.5F, 0.5F}; CHECK(!environment.validate());
    environment = {}; environment.sunColor.x = std::numeric_limits<float>::infinity(); CHECK(!environment.validate());
    environment = {}; environment.globalIlluminationIntensity = 4.1F; CHECK(!environment.validate());
    environment = {}; environment.globalIlluminationMaxDistanceMeters = 0.0F; CHECK(!environment.validate());
    environment = {}; environment.globalIlluminationSamples = 0U; CHECK(!environment.validate());
    environment = {}; environment.globalIlluminationSamples = 17U; CHECK(!environment.validate());
    environment = {}; environment.shadowStrength = 1.1F; CHECK(!environment.validate());
    environment = {}; environment.shadowSoftnessRadians = -0.1F; CHECK(!environment.validate());
    environment = {}; environment.shadowSamples = 17U; CHECK(!environment.validate());
    environment = {}; environment.contactShadowDistanceMeters = 0.0F; CHECK(!environment.validate());
    environment = {}; environment.shadowBiasMeters = 1.1F; CHECK(!environment.validate());
}

void test_pack_field_order_and_offsets() {
    RenderEnvironment environment;
    environment.sunDirection = {0.0F, 3.0F, 4.0F};
    environment.sunIntensity = 2.5F;
    environment.sunColor = {1.0F, 0.9F, 0.8F};
    environment.subsurfaceMaxDistanceMeters = 0.25F;
    environment.skyColor = {0.1F, 0.2F, 0.3F};
    environment.exposure = 1.5F;
    environment.groundColor = {0.05F, 0.04F, 0.03F};
    environment.tonemapOperator = TonemapOperator::Reinhard;
    environment.globalTint = {0.9F, 0.95F, 1.0F};
    environment.bloomThreshold = 1.2F;
    environment.bloomIntensity = 0.3F;
    environment.bloomRadius = 6.0F;
    environment.globalIlluminationMode = GlobalIlluminationMode::AmbientHemisphere;
    environment.globalIlluminationIntensity = 0.7F;
    environment.globalIlluminationMaxDistanceMeters = 9.0F;
    environment.globalIlluminationSamples = 6U;
    environment.shadowMode = ShadowMode::Hybrid;
    environment.shadowStrength = 0.9F;
    environment.shadowSoftnessRadians = 0.02F;
    environment.shadowSamples = 8U;
    environment.shadowMaxDistanceMeters = 1500.0F;
    environment.contactShadowDistanceMeters = 1.25F;
    environment.shadowBiasMeters = 0.025F;

    const GpuRenderEnvironment gpu = pack_gpu_render_environment(environment, 1920, 1080, 0.25F);
    CHECK(std::fabs(gpu.sunDirectionY - 0.6F) < 1.0e-6F);
    CHECK(std::fabs(gpu.sunDirectionZ - 0.8F) < 1.0e-6F);
    CHECK(gpu.tonemapOperator == 1U);
    CHECK(gpu.globalIlluminationMode == 1U);
    CHECK(gpu.globalIlluminationIntensity == 0.7F);
    CHECK(gpu.globalIlluminationMaxDistanceMeters == 9.0F);
    CHECK(gpu.globalIlluminationSamples == 6U);
    CHECK(gpu.shadowMode == 4U);
    CHECK(gpu.shadowStrength == 0.9F);
    CHECK(gpu.shadowSoftnessRadians == 0.02F);
    CHECK(gpu.shadowSamples == 8U);
    CHECK(gpu.shadowMaxDistanceMeters == 1500.0F);
    CHECK(gpu.contactShadowDistanceMeters == 1.25F);
    CHECK(gpu.shadowBiasMeters == 0.025F);
    CHECK(gpu.metersPerVoxel == 0.25F);
    CHECK(gpu.imageWidth == 1920U && gpu.imageHeight == 1080U);

#define OFFSET(field, value) CHECK(offsetof(GpuRenderEnvironment, field) == value)
    OFFSET(sunDirectionX, 0); OFFSET(sunDirectionY, 4); OFFSET(sunDirectionZ, 8); OFFSET(sunIntensity, 12);
    OFFSET(sunColorR, 16); OFFSET(sunColorG, 20); OFFSET(sunColorB, 24); OFFSET(subsurfaceMaxDistanceMeters, 28);
    OFFSET(skyColorR, 32); OFFSET(skyColorG, 36); OFFSET(skyColorB, 40); OFFSET(exposure, 44);
    OFFSET(groundColorR, 48); OFFSET(groundColorG, 52); OFFSET(groundColorB, 56); OFFSET(tonemapOperator, 60);
    OFFSET(globalTintR, 64); OFFSET(globalTintG, 68); OFFSET(globalTintB, 72); OFFSET(bloomThreshold, 76);
    OFFSET(bloomIntensity, 80); OFFSET(bloomRadius, 84); OFFSET(imageWidth, 88); OFFSET(imageHeight, 92);
    OFFSET(globalIlluminationMode, 96); OFFSET(globalIlluminationIntensity, 100);
    OFFSET(globalIlluminationMaxDistanceMeters, 104); OFFSET(globalIlluminationSamples, 108);
    OFFSET(shadowMode, 112); OFFSET(shadowStrength, 116); OFFSET(shadowSoftnessRadians, 120); OFFSET(shadowSamples, 124);
    OFFSET(shadowMaxDistanceMeters, 128); OFFSET(contactShadowDistanceMeters, 132); OFFSET(shadowBiasMeters, 136);
    OFFSET(metersPerVoxel, 140);
#undef OFFSET
    CHECK(sizeof(GpuRenderEnvironment) == 144);
}

void test_zero_sun_direction_gets_safe_fallback() {
    RenderEnvironment environment;
    environment.sunDirection = {0.0F, 0.0F, 0.0F};
    const GpuRenderEnvironment gpu = pack_gpu_render_environment(environment, 1, 1, 0.10F);
    const float lengthSquared = gpu.sunDirectionX * gpu.sunDirectionX + gpu.sunDirectionY * gpu.sunDirectionY + gpu.sunDirectionZ * gpu.sunDirectionZ;
    CHECK(std::fabs(lengthSquared - 1.0F) < 1.0e-5F);
}

void test_enum_encoding() {
    RenderEnvironment environment;
    environment.globalIlluminationMode = GlobalIlluminationMode::Off;
    environment.shadowMode = ShadowMode::Hard;
    auto gpu = pack_gpu_render_environment(environment, 1, 1, 0.10F);
    CHECK(gpu.globalIlluminationMode == 0U && gpu.shadowMode == 1U);
    environment.globalIlluminationMode = GlobalIlluminationMode::VoxelOneBounce;
    environment.shadowMode = ShadowMode::Contact;
    gpu = pack_gpu_render_environment(environment, 1, 1, 0.10F);
    CHECK(gpu.globalIlluminationMode == 2U && gpu.shadowMode == 3U);
}
}

int main() {
    test_default_environment_is_valid_and_gi_is_on();
    test_validation_rejects_bad_values();
    test_pack_field_order_and_offsets();
    test_zero_sun_direction_gets_safe_fallback();
    test_enum_encoding();
    if (failures == 0) { std::cout << "dve_render_environment_tests: PASS\n"; return 0; }
    std::cerr << "dve_render_environment_tests: " << failures << " FAILURE(S)\n";
    return 1;
}
