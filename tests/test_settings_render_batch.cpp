// Batch coverage for the render settings wiring: render.exposure,
// render.tonemap, render.shadow_quality, render.shadow_strength,
// render.shadow_softness, render.texture_filter, render.anisotropy.
#include "dve/editor_runtime_settings.hpp"
#include "dve/render/material_resource_residency.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/rhi/null_device.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;
using namespace dve::render;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool near(float a, float b, float eps = 1.0e-5F) { return std::abs(a - b) <= eps; }

void set(EditorSettingsRegistry& settings, std::string_view id, SettingValue value) {
    std::string error;
    if (!settings.set(SettingScope::Session, id, std::move(value), &error))
        throw std::runtime_error(error);
}

void test_environment_resolver() {
    auto registry = EditorSettingsRegistry::make_default();
    const auto defaults = render_environment_settings(registry);
    require(near(defaults.exposure, 1.0F), "default exposure changed");
    require(defaults.tonemapOperator == TonemapOperator::ACES, "default tonemap changed");
    require(near(defaults.shadowStrength, 0.85F), "default shadow strength changed");
    require(near(defaults.shadowSoftnessRadians, 0.012F), "default shadow softness changed");
    require(defaults.shadowSamples == 4U, "high quality preset must keep the default sample count");

    set(registry, "render.exposure", 2.0);
    set(registry, "render.tonemap", std::string{"reinhard"});
    set(registry, "render.shadow_strength", 0.5);
    set(registry, "render.shadow_softness", 0.05);
    set(registry, "render.shadow_quality", std::string{"low"});
    const auto resolved = render_environment_settings(registry);
    require(near(resolved.exposure, 2.0F), "exposure override ignored");
    require(resolved.tonemapOperator == TonemapOperator::Reinhard, "tonemap override ignored");
    require(near(resolved.shadowStrength, 0.5F), "shadow strength override ignored");
    require(near(resolved.shadowSoftnessRadians, 0.05F), "shadow softness override ignored");
    require(resolved.shadowSamples == 1U, "low quality preset must select one shadow sample");

    set(registry, "render.shadow_quality", std::string{"medium"});
    require(render_environment_settings(registry).shadowSamples == 2U, "medium preset wrong");
    set(registry, "render.shadow_quality", std::string{"ultra"});
    require(render_environment_settings(registry).shadowSamples == 8U, "ultra preset wrong");
    set(registry, "render.tonemap", std::string{"clamp"});
    require(render_environment_settings(registry).tonemapOperator == TonemapOperator::Clamp,
            "clamp tonemap override ignored");

    // The play session config carries the same resolved environment.
    const auto config = play_session_settings(registry);
    require(near(config.environment.exposure, 2.0F) &&
            config.environment.tonemapOperator == TonemapOperator::Clamp &&
            config.environment.shadowSamples == 8U,
            "play session config did not inherit the render environment settings");

    // Fields outside the batch survive from the base environment.
    RenderEnvironment base;
    base.sunIntensity = 3.25F;
    const auto merged = render_environment_settings(registry, base);
    require(near(merged.sunIntensity, 3.25F), "resolver clobbered an unrelated field");
    require(merged.validate(nullptr), "resolved environment must validate");
}

double reference_aces(double x) {
    const double a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    const double v = (x * (a * x + b)) / (x * (c * x + d) + e);
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}
double reference_srgb(double v) {
    v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}
std::uint8_t expected_byte(double linear, double exposure, TonemapOperator op) {
    const double x = linear * exposure;
    double mapped = 0.0;
    if (op == TonemapOperator::ACES) mapped = reference_aces(x);
    else if (op == TonemapOperator::Reinhard) {
        mapped = x / (1.0 + x);
        mapped = mapped < 0.0 ? 0.0 : (mapped > 1.0 ? 1.0 : mapped);
    } else {
        mapped = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
    }
    const double encoded = reference_srgb(mapped) * 255.0 + 0.5;
    return static_cast<std::uint8_t>(encoded < 0.0 ? 0.0 : (encoded > 255.0 ? 255.0 : encoded));
}

void test_tonemap_reference() {
    PolygonRenderTarget target;
    target.resize(4U, 1U);
    target.hdrColor = {{0.0F, 0.0F, 0.0F, 1.0F},
                       {0.18F, 0.18F, 0.18F, 1.0F},
                       {1.0F, 0.5F, 0.25F, 1.0F},
                       {4.0F, 2.0F, 8.0F, 1.0F}};
    std::string error;
    for (const auto op : {TonemapOperator::ACES, TonemapOperator::Reinhard, TonemapOperator::Clamp}) {
        std::vector<std::uint8_t> rgba;
        require(resolve_polygon_render_rgba8(target, 1.5F, op, rgba, &error), error.c_str());
        require(rgba.size() == 16U, "resolve produced the wrong byte count");
        for (std::size_t pixel = 0; pixel < 4U; ++pixel) {
            const auto& c = target.hdrColor[pixel];
            const float channels[3] = {c.x, c.y, c.z};
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const auto expected = expected_byte(channels[channel], 1.5, op);
                const auto actual = rgba[pixel * 4U + channel];
                require(std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <= 1,
                        "tonemap output disagrees with the published reference curve");
            }
            require(rgba[pixel * 4U + 3U] == 255U, "alpha must be opaque");
        }
    }
    // The legacy overload stays byte-identical to the ACES operator path.
    std::vector<std::uint8_t> legacy, aces;
    require(resolve_polygon_render_rgba8(target, 1.5F, legacy, &error), error.c_str());
    require(resolve_polygon_render_rgba8(target, 1.5F, TonemapOperator::ACES, aces, &error),
            error.c_str());
    require(legacy == aces, "legacy resolve no longer matches the ACES path");

    // Exposure scales the linear input exactly: under Clamp, value v at
    // exposure 2 resolves like value 2v at exposure 1.
    PolygonRenderTarget single;
    single.resize(1U, 1U);
    single.hdrColor = {{0.2F, 0.2F, 0.2F, 1.0F}};
    std::vector<std::uint8_t> doubled, direct;
    require(resolve_polygon_render_rgba8(single, 2.0F, TonemapOperator::Clamp, doubled, &error),
            error.c_str());
    single.hdrColor = {{0.4F, 0.4F, 0.4F, 1.0F}};
    require(resolve_polygon_render_rgba8(single, 1.0F, TonemapOperator::Clamp, direct, &error),
            error.c_str());
    require(doubled == direct, "exposure did not scale the pre-tonemap value exactly");

    // A fixed HDR ramp stays finite and monotonic under every operator.
    PolygonRenderTarget ramp;
    ramp.resize(65U, 1U);
    for (std::uint32_t i = 0; i < 65U; ++i) {
        const float v = static_cast<float>(i) * 0.125F;
        ramp.hdrColor[i] = {v, v, v, 1.0F};
    }
    for (const auto op : {TonemapOperator::ACES, TonemapOperator::Reinhard, TonemapOperator::Clamp}) {
        std::vector<std::uint8_t> out;
        require(resolve_polygon_render_rgba8(ramp, 1.0F, op, out, &error), error.c_str());
        for (std::size_t i = 1; i < 65U; ++i)
            require(out[i * 4U] >= out[(i - 1U) * 4U], "tonemap ramp is not monotonic");
    }
}

CookedPolygonAsset make_sampler_asset() {
    CookedPolygonAsset asset;
    asset.objectId = 91U;
    VoxelMaterialDefinition material;
    material.name = "sampler-policy";
    asset.materials.push_back(material);
    asset.materialBindings.push_back({});
    asset.images.push_back({"albedo", "image/raw", 1U, 1U, {200U, 180U, 160U, 255U}});
    asset.samplers.push_back({});
    asset.textures.push_back({"albedo", 0U, 0U});
    asset.materialBindings[0].baseColor.texture = 0U;
    asset.vertices = {{{0, 0, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}, {1, 1, 1, 1}, {0, 0}},
                      {{1, 0, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 0}, {1, 1, 1, 1}, {0, 0}},
                      {{0, 1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0, 1}, {1, 1, 1, 1}, {0, 0}}};
    asset.indices = {0U, 1U, 2U};
    asset.submeshes = {{"triangle", 0U, 3U, 0U}};
    asset.bounds = {{0, 0, 0}, {1, 1, 0}};
    asset.contentHash = polygon_asset_content_hash(asset);
    require(static_cast<bool>(validate_polygon_asset(asset)), "sampler fixture asset invalid");
    return asset;
}

void test_sampler_policy() {
    const auto asset = make_sampler_asset();
    std::string error;

    {   // Defaults: no override, authored sampling untouched.
        rhi::NullDevice device;
        MaterialResourceResidency residency(device);
        require(residency.retain_asset(asset.contentHash, &error), error.c_str());
        rhi::SamplerHandle sampler;
        require(residency.ensure_sampler(asset, 0U, sampler, &error), error.c_str());
        const auto stats = residency.stats();
        require(stats.samplersCreated == 1U && stats.anisotropicSamplersCreated == 0U &&
                stats.mipmapFallbackSamplersCreated == 0U, "default policy altered sampling");
        require(near(residency.effective_max_anisotropy(), 1.0F),
                "default policy must not grant anisotropy");
        residency.clear();
    }
    {   // Registry defaults (trilinear): mipmap request meets single-level
        // images, so the explicit fallback engages; anisotropy stays off.
        auto registry = EditorSettingsRegistry::make_default();
        const auto policy = material_sampler_policy(registry);
        require(policy.overrideFiltering && !policy.anisotropy, "trilinear policy shape wrong");
        rhi::NullDevice device;
        MaterialResourceResidency residency(device, policy);
        require(residency.retain_asset(asset.contentHash, &error), error.c_str());
        rhi::SamplerHandle sampler;
        require(residency.ensure_sampler(asset, 0U, sampler, &error), error.c_str());
        const auto stats = residency.stats();
        require(stats.mipmapFallbackSamplersCreated == 1U,
                "single-level images did not trigger the explicit mipmap fallback");
        require(stats.anisotropicSamplersCreated == 0U, "trilinear must not enable anisotropy");
        residency.clear();
    }
    {   // Anisotropic at the registry default of 8 on a 16x-cap device.
        auto registry = EditorSettingsRegistry::make_default();
        set(registry, "render.texture_filter", std::string{"anisotropic"});
        const auto policy = material_sampler_policy(registry);
        require(policy.anisotropy && near(policy.requestedMaxAnisotropy, 8.0F),
                "anisotropic policy did not carry the registry level");
        rhi::NullDevice device;
        MaterialResourceResidency residency(device, policy);
        require(near(residency.effective_max_anisotropy(), 8.0F), "effective anisotropy wrong");
        require(residency.retain_asset(asset.contentHash, &error), error.c_str());
        rhi::SamplerHandle sampler;
        require(residency.ensure_sampler(asset, 0U, sampler, &error), error.c_str());
        require(residency.stats().anisotropicSamplersCreated == 1U,
                "anisotropic sampler was not created");
        residency.clear();
    }
    {   // Requests above the device limit clamp to the limit, visibly.
        MaterialSamplerPolicy policy;
        policy.overrideFiltering = true;
        policy.anisotropy = true;
        policy.requestedMaxAnisotropy = 32.0F;
        rhi::NullDevice device;  // NullDevice caps anisotropy at 16.
        MaterialResourceResidency residency(device, policy);
        require(near(residency.effective_max_anisotropy(), 16.0F),
                "anisotropy request was not clamped to the device limit");
        require(residency.retain_asset(asset.contentHash, &error), error.c_str());
        rhi::SamplerHandle sampler;
        require(residency.ensure_sampler(asset, 0U, sampler, &error), error.c_str());
        require(residency.stats().anisotropicSamplersCreated == 1U,
                "clamped anisotropic sampler was not created");
        residency.clear();
    }
    {   // Nearest: no mipmap request, so no fallback is recorded.
        auto registry = EditorSettingsRegistry::make_default();
        set(registry, "render.texture_filter", std::string{"nearest"});
        rhi::NullDevice device;
        MaterialResourceResidency residency(device, material_sampler_policy(registry));
        require(residency.retain_asset(asset.contentHash, &error), error.c_str());
        rhi::SamplerHandle sampler;
        require(residency.ensure_sampler(asset, 0U, sampler, &error), error.c_str());
        require(residency.stats().mipmapFallbackSamplersCreated == 0U,
                "nearest filtering must not record a mipmap fallback");
        residency.clear();
    }
}
} // namespace

int main() {
    try {
        test_environment_resolver();
        test_tonemap_reference();
        test_sampler_policy();
        std::cout << "dve_settings_render_batch_tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dve_settings_render_batch_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
