#include "dve/render/surface_shading.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "dve/transform.hpp"

namespace dve::render {
namespace {
constexpr float kPi = 3.14159265358979323846F;
float saturate(float v) noexcept { return std::clamp(v, 0.0F, 1.0F); }
Float3 lerp3(Float3 a, Float3 b, float t) noexcept { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; }
Float3 mul3(Float3 a, Float3 b) noexcept { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
Float3 one_minus(Float3 a) noexcept { return {1.0F - a.x, 1.0F - a.y, 1.0F - a.z}; }

float radical_inverse(std::uint32_t bits) noexcept {
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<float>(bits) * 2.3283064365386963e-10F;
}

constexpr int kTable = 32;

// Directional albedo of the specular lobe split as F0 x A + B, by importance-sampling the GGX
// half vector: the estimator for f cos / pdf is F x V x 4 x VdotH x NdotL / NdotH (D cancels).
EnvironmentBrdf integrate(float NdotV, float roughness, std::uint32_t samples) noexcept {
    NdotV = std::max(NdotV, 1.0e-4F);
    const Float3 view{std::sqrt(std::max(0.0F, 1.0F - NdotV * NdotV)), 0.0F, NdotV};
    const float a = std::max(roughness * roughness, 1.0e-4F);
    double scale = 0.0, bias = 0.0;
    for (std::uint32_t i = 0; i < samples; ++i) {
        const float u = (static_cast<float>(i) + 0.5F) / static_cast<float>(samples), v = radical_inverse(i);
        const float phi = 2.0F * kPi * v;
        const float cosH = std::sqrt((1.0F - u) / (1.0F + (a * a - 1.0F) * u));
        const float sinH = std::sqrt(std::max(0.0F, 1.0F - cosH * cosH));
        const Float3 h{sinH * std::cos(phi), sinH * std::sin(phi), cosH};
        const float VdotH = dot(view, h);
        const Float3 light{2.0F * VdotH * h.x - view.x, 2.0F * VdotH * h.y - view.y, 2.0F * VdotH * h.z - view.z};
        const float NdotL = light.z;
        if (NdotL <= 0.0F || VdotH <= 0.0F) continue;
        const float V = visibility_smith_ggx_correlated(NdotV, NdotL, roughness);
        const float weight = V * 4.0F * VdotH * NdotL / std::max(cosH, 1.0e-6F);
        const float m = 1.0F - VdotH, fc = m * m * m * m * m;
        scale += (1.0 - fc) * weight;
        bias += fc * weight;
    }
    return {static_cast<float>(scale / samples), static_cast<float>(bias / samples)};
}

const std::array<EnvironmentBrdf, kTable * kTable>& table() {
    static const auto values = [] {
        std::array<EnvironmentBrdf, kTable * kTable> t{};
        for (int r = 0; r < kTable; ++r)
            for (int c = 0; c < kTable; ++c)
                t[static_cast<std::size_t>(r * kTable + c)] =
                    integrate((static_cast<float>(c) + 0.5F) / kTable, (static_cast<float>(r) + 0.5F) / kTable, 1024U);
        return t;
    }();
    return values;
}
}  // namespace

float distribution_ggx(float NdotH, float roughness) noexcept {
    const float a = std::max(roughness * roughness, 1.0e-4F);
    const float a2 = a * a;
    const float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0F) + 1.0F;
    denom = kPi * denom * denom;
    return a2 / std::max(denom, 1.0e-9F);
}

float visibility_smith_ggx_correlated(float NdotV, float NdotL, float roughness) noexcept {
    float a2 = std::max(roughness * roughness, 1.0e-4F);
    a2 = a2 * a2;
    const float lambdaV = NdotL * std::sqrt(NdotV * NdotV * (1.0F - a2) + a2);
    const float lambdaL = NdotV * std::sqrt(NdotL * NdotL * (1.0F - a2) + a2);
    return 0.5F / std::max(lambdaV + lambdaL, 1.0e-9F);
}

Float3 fresnel_schlick(float cosTheta, Float3 f0) noexcept {
    const float m = saturate(1.0F - cosTheta);
    const float m2 = m * m;
    const float k = m2 * m2 * m;
    const float grazing = saturate(50.0F * f0.y);   // below F0 0.02: no specular (FresnelSchlick)
    return {f0.x + (grazing - f0.x) * k, f0.y + (grazing - f0.y) * k, f0.z + (grazing - f0.z) * k};
}

Float3 specular_f0(Float3 baseColor, float metallic, float specular) noexcept {
    const float dielectric = 0.08F * specular;
    return lerp3({dielectric, dielectric, dielectric}, baseColor, metallic);
}

ShadingLobes shade_direct_light(Float3 normal, Float3 viewDirection, Float3 lightDirection, Float3 lightRadiance,
                                Float3 baseColor, float metallic, float roughness, float specular) noexcept {
    const Float3 halfVector = normalize(add(viewDirection, lightDirection));
    const float NdotL = saturate(dot(normal, lightDirection));
    const float NdotV = saturate(dot(normal, viewDirection)) + 1.0e-4F;
    const float NdotH = saturate(dot(normal, halfVector));
    const float VdotH = saturate(dot(viewDirection, halfVector));
    if (NdotL <= 0.0F) return {};
    const Float3 f0 = specular_f0(baseColor, metallic, specular);
    const Float3 diffuseColor = multiply(baseColor, 1.0F - metallic);
    const float D = distribution_ggx(NdotH, roughness);
    const float V = visibility_smith_ggx_correlated(NdotV, NdotL, roughness);
    const Float3 F = fresnel_schlick(VdotH, f0);
    const Float3 specularTerm = multiply(F, D * V);
    const Float3 diffuseTerm = mul3(multiply(diffuseColor, 1.0F / kPi), one_minus(F));
    return {multiply(mul3(diffuseTerm, lightRadiance), NdotL), multiply(mul3(specularTerm, lightRadiance), NdotL)};
}

ShadingLobes shade_clear_coat_direct(Float3 normal, Float3 viewDirection, Float3 lightDirection, Float3 lightRadiance,
                                     Float3 baseColor, float metallic, float roughness, float specular, float clearCoat,
                                     float clearCoatRoughness) noexcept {
    const float coatAmount = saturate(clearCoat);
    ShadingLobes base = shade_direct_light(normal, viewDirection, lightDirection, lightRadiance, baseColor, metallic,
                                           roughness, specular);
    if (coatAmount <= 0.0F) return base;
    const Float3 halfVector = normalize(add(viewDirection, lightDirection));
    const float NdotL = saturate(dot(normal, lightDirection));
    const float NdotV = saturate(dot(normal, viewDirection)) + 1.0e-4F;
    const float NdotH = saturate(dot(normal, halfVector));
    const float VdotH = saturate(dot(viewDirection, halfVector));
    if (NdotL <= 0.0F) return base;
    const float coatRoughness = std::clamp(clearCoatRoughness, 0.02F, 1.0F);
    const float D = distribution_ggx(NdotH, coatRoughness);
    const float V = visibility_smith_ggx_correlated(NdotV, NdotL, coatRoughness);
    const Float3 F = fresnel_schlick(VdotH, {0.04F, 0.04F, 0.04F});
    const Float3 coat = multiply(mul3(F, lightRadiance), D * V * NdotL);
    const Float3 transmission = one_minus(multiply(F, coatAmount));
    const Float3 twice = mul3(transmission, transmission);
    return {mul3(base.diffuse, twice), add(mul3(base.specular, twice), multiply(coat, coatAmount))};
}

EnvironmentBrdf environment_brdf(float NdotV, float roughness) noexcept {
    const auto& t = table();
    const float x = std::clamp(saturate(NdotV) * kTable - 0.5F, 0.0F, kTable - 1.0F);
    const float y = std::clamp(saturate(roughness) * kTable - 0.5F, 0.0F, kTable - 1.0F);
    const int x0 = std::min(static_cast<int>(x), kTable - 2), y0 = std::min(static_cast<int>(y), kTable - 2);
    const float tx = x - static_cast<float>(x0), ty = y - static_cast<float>(y0);
    const auto at = [&](int c, int r) { return t[static_cast<std::size_t>(r * kTable + c)]; };
    const auto mix = [](EnvironmentBrdf a, EnvironmentBrdf b, float s) {
        return EnvironmentBrdf{a.scale + (b.scale - a.scale) * s, a.bias + (b.bias - a.bias) * s};
    };
    return mix(mix(at(x0, y0), at(x0 + 1, y0), tx), mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx), ty);
}

Float3 specular_directional_albedo(float NdotV, float roughness, Float3 f0, std::uint32_t samples) noexcept {
    const EnvironmentBrdf e = integrate(NdotV, roughness, samples);
    const float bias = e.bias * saturate(50.0F * f0.y);
    return {f0.x * e.scale + bias, f0.y * e.scale + bias, f0.z * e.scale + bias};
}

float specular_occlusion(float NdotV, float ambientOcclusion, float roughness) noexcept {
    const float ao = saturate(ambientOcclusion);
    return saturate(std::pow(NdotV + ao, std::exp2(-16.0F * saturate(roughness) - 1.0F)) - 1.0F + ao);
}

ShadingLobes shade_ambient_light(Float3 normal, Float3 viewDirection, Float3 irradiance, Float3 reflected, Float3 baseColor,
                                 float metallic, float roughness, float specular, float ambientOcclusion) noexcept {
    const float NdotV = saturate(dot(normal, viewDirection));
    const Float3 f0 = specular_f0(baseColor, saturate(metallic), saturate(specular));
    const EnvironmentBrdf e = environment_brdf(NdotV, roughness);
    const float bias = e.bias * saturate(50.0F * f0.y);
    const Float3 albedo{f0.x * e.scale + bias, f0.y * e.scale + bias, f0.z * e.scale + bias};
    const float ao = saturate(ambientOcclusion);
    const Float3 diffuseColor = mul3(multiply(baseColor, 1.0F - saturate(metallic)), one_minus(albedo));
    return {mul3(diffuseColor, irradiance), multiply(mul3(albedo, reflected), specular_occlusion(NdotV, ao, roughness))};
}

} // namespace dve::render
