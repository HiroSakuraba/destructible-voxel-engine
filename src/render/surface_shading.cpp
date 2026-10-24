#include "dve/render/surface_shading.hpp"
#include "dve/render/optical_scene.hpp"

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
                                Float3 baseColor, float metallic, float roughness, float specular, float iridescence,
                                float iridescenceIor, float iridescenceThicknessNm, float baseIor) noexcept {
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
    Float3 specularF = F;
    if (iridescence > 0.0F) {
        const Float3 film = thin_film_fresnel(VdotH, iridescenceThicknessNm, iridescenceIor, baseIor, 1.0F);
        specularF = mul3(F, lerp3({1, 1, 1}, film, saturate(iridescence)));
    }
    const Float3 specularTerm = multiply(specularF, D * V);
    const Float3 diffuseTerm = mul3(multiply(diffuseColor, 1.0F / kPi), one_minus(F));
    return {multiply(mul3(diffuseTerm, lightRadiance), NdotL), multiply(mul3(specularTerm, lightRadiance), NdotL)};
}

ShadingLobes shade_clear_coat_direct(Float3 normal, Float3 viewDirection, Float3 lightDirection, Float3 lightRadiance,
                                     Float3 baseColor, float metallic, float roughness, float specular, float clearCoat,
                                     float clearCoatRoughness, float iridescence, float iridescenceIor,
                                     float iridescenceThicknessNm, float baseIor) noexcept {
    const float coatAmount = saturate(clearCoat);
    ShadingLobes base = shade_direct_light(normal, viewDirection, lightDirection, lightRadiance, baseColor, metallic,
                                           roughness, specular, iridescence, iridescenceIor, iridescenceThicknessNm,
                                           baseIor);
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
    Float3 F = fresnel_schlick(VdotH, {0.04F, 0.04F, 0.04F});
    if (iridescence > 0.0F) {
        const Float3 film = thin_film_fresnel(VdotH, iridescenceThicknessNm, iridescenceIor, 1.5F, 1.0F);
        F = mul3(F, lerp3({1, 1, 1}, film, saturate(iridescence)));
    }
    const Float3 coat = multiply(mul3(F, lightRadiance), D * V * NdotL);
    const Float3 transmission = one_minus(multiply(F, coatAmount));
    const Float3 twice = mul3(transmission, transmission);
    return {mul3(base.diffuse, twice), add(mul3(base.specular, twice), multiply(coat, coatAmount))};
}

SheenEvaluation evaluate_sheen(Float3 normal, Float3 viewDirection, Float3 lightDirection, Float3 sheenColor,
                               float sheenRoughness) noexcept {
    SheenEvaluation out;
    if (sheenColor.x <= 0.0F && sheenColor.y <= 0.0F && sheenColor.z <= 0.0F) return out;
    const float NdotV = dot(normal, viewDirection), NdotL = dot(normal, lightDirection);
    if (NdotV <= 0.0F || NdotL <= 0.0F) return out;
    const Float3 halfVector = normalize(add(viewDirection, lightDirection));
    const float NdotH = dot(normal, halfVector), VdotH = dot(viewDirection, halfVector);
    if (NdotH <= 0.0F || VdotH <= 0.0F) return out;
    // Charlie distribution (nvshaders' hvd_sheen_eval): alpha = roughness^2, and the density is
    // (1/alpha + 2) sin^(1/alpha)(theta_h) / (2 pi); theta_h is the half vector's polar angle.
    const float alpha = std::max(saturate(sheenRoughness) * saturate(sheenRoughness), 1.0e-4F);
    const float invAlpha = 1.0F / alpha;
    const float sinTheta = std::sqrt(std::max(0.0F, 1.0F - NdotH * NdotH));
    const float D = (invAlpha + 2.0F) * std::pow(sinTheta, invAlpha) / (2.0F * kPi);
    // V-cavity masking (vcavities_mask): each side keeps min(1, 2 NdotH NdotK / KdotH), and
    // KdotH is VdotH for the view and LdotH == VdotH for the light in reflection.
    const auto cavity = [NdotH, VdotH](float nk) {
        return std::min(1.0F, 2.0F * NdotH * nk / std::max(VdotH, 1.0e-6F));
    };
    const float G = std::min(cavity(NdotV), cavity(NdotL));
    out.brdf = multiply(sheenColor, D * G / (4.0F * NdotV * NdotL));
    out.pdf = D * NdotH / (4.0F * VdotH);
    return out;
}

Float3 sheen_direct(Float3 normal, Float3 viewDirection, Float3 lightDirection, Float3 lightRadiance,
                    Float3 sheenColor, float sheenRoughness) noexcept {
    const auto sheen = evaluate_sheen(normal, viewDirection, lightDirection, sheenColor, sheenRoughness);
    return multiply(mul3(sheen.brdf, lightRadiance), saturate(dot(normal, lightDirection)));
}

namespace {
// Polarised Fresnel reflectances (r_s^2, r_p^2) at a dielectric interface, and — for the second
// film interface, whose "substrate" side is the base material — the phase shift of each
// polarisation on the unit circle (nvshaders' fresnel_dielectric / fresnel_conductor with an
// absorbing coefficient of zero, i.e. a dielectric base).
struct FilmInterface {
    float rs{}, rp{};
    float phaseSinS{}, phaseCosS{}, phaseSinP{}, phaseCosP{};
};
FilmInterface film_entry_interface(float na, float nb, float cosA, float cosB) noexcept {
    const float rs = (na * cosA - nb * cosB) / (na * cosA + nb * cosB);
    const float rp = (nb * cosA - na * cosB) / (nb * cosA + na * cosB);
    return {rs * rs, rp * rp, 0, 1, 0, 1};
}
FilmInterface film_base_interface(float na, float nb, float cosA, float sinASqd) noexcept {
    // fresnel_conductor with k_b = 0: u_b^2 / v_b^2 split on the sign of half_U, one vanishing.
    const float nb2 = nb * nb, na2 = na * na;
    const float halfU = 0.5F * (nb2 - na2 * sinASqd);
    const float halfV = std::sqrt(std::max(0.0F, halfU * halfU));
    const float ub = std::sqrt(std::max(0.0F, halfU + halfV));
    const float vb2 = halfV - halfU;
    const float vb = std::sqrt(std::max(0.0F, vb2));
    const float tmp5 = na * cosA;
    FilmInterface out;
    const float denomS = (tmp5 + ub) * (tmp5 + ub) + vb2;
    out.rs = denomS > 0 ? ((tmp5 - ub) * (tmp5 - ub) + vb2) / denomS : 0;
    const float tmp1 = nb2 * cosA, tmp2 = na * ub, tmp4 = na * vb;
    const float denomP = (tmp1 + tmp2) * (tmp1 + tmp2) + tmp4 * tmp4;
    out.rp = denomP > 0 ? ((tmp1 - tmp2) * (tmp1 - tmp2) + tmp4 * tmp4) / denomP : 0;
    // Phase shifts: (sin, cos) for s from (tmp6, tmp7), for p from (tmp8, tmp9) in the source.
    const float tmp6 = 2 * tmp5 * vb, tmp7 = (ub * ub + vb2) - tmp5 * tmp5;
    const float normS = std::sqrt(tmp6 * tmp6 + tmp7 * tmp7);
    out.phaseSinS = normS > 0 ? tmp6 / normS : 0;
    out.phaseCosS = normS > 0 ? tmp7 / normS : 1;
    const float tmp8 = 2 * tmp5 * (-nb2 * vb), tmp9 = (nb2 * nb2) * cosA * cosA - na2 * (ub * ub + vb2);
    const float normP = std::sqrt(tmp8 * tmp8 + tmp9 * tmp9);
    out.phaseSinP = normP > 0 ? tmp8 / normP : 0;
    out.phaseCosP = normP > 0 ? tmp9 / normP : 1;
    return out;
}
} // namespace

Float3 thin_film_fresnel(float cosTheta, float filmThicknessNm, float filmIor, float baseIor,
                         float outsideIor) noexcept {
    const float kh = std::clamp(cosTheta, 0.0F, 1.0F);
    const float thickness = std::max(0.0F, filmThicknessNm);
    // Degenerate layers reduce to one ordinary boundary. Handle grazing limits
    // analytically instead of evaluating a 0/0 interference ratio.
    if(thickness==0 || filmIor==outsideIor || filmIor==baseIor) {
        const float f=dielectric_fresnel(kh,outsideIor,baseIor);return {f,f,f};
    }
    if(kh<=1e-6F)return {1,1,1};
    const float sin0Sqr = std::max(0.0F, 1.0F - kh * kh);
    const float eta01 = outsideIor / filmIor;
    const float sin1Sqr = eta01 * eta01 * sin0Sqr;
    if (sin1Sqr > 1.0F) return {1, 1, 1};   // total internal reflection at the entry interface
    const float cos1 = std::sqrt(std::max(0.0F, 1.0F - sin1Sqr));
    const FilmInterface r01 = film_entry_interface(outsideIor, filmIor, kh, cos1);
    const FilmInterface r12 = film_base_interface(filmIor, baseIor, cos1, sin1Sqr);
    const float phase = 4.0F * kPi * filmIor * thickness * cos1;
    const float r01r12s = std::sqrt(std::max(0.0F, r01.rs * r12.rs));
    const float r01r12p = std::sqrt(std::max(0.0F, r01.rp * r12.rp));
    // Low-resolution CIE colour matching functions (the same 16-band table nvshaders uses).
    static constexpr float kCie[16][3] = {
        {0.02986F, 0.00310F, 0.13609F}, {0.20715F, 0.02304F, 0.99584F}, {0.36717F, 0.06469F, 1.89550F},
        {0.28549F, 0.13661F, 1.67236F}, {0.08233F, 0.26856F, 0.76653F}, {0.01723F, 0.48621F, 0.21889F},
        {0.14400F, 0.77341F, 0.05886F}, {0.40957F, 0.95850F, 0.01280F}, {0.74201F, 0.97967F, 0.00060F},
        {1.03325F, 0.84591F, 0.00000F}, {1.08385F, 0.62242F, 0.00000F}, {0.79203F, 0.36749F, 0.00000F},
        {0.38751F, 0.16135F, 0.00000F}, {0.13401F, 0.05298F, 0.00000F}, {0.03531F, 0.01375F, 0.00000F},
        {0.00817F, 0.00317F, 0.00000F}};
    Float3 xyz{};
    float lambda = 400.0F + 0.5F * (300.0F / 16.0F);
    for (const auto& cie : kCie) {
        const float phi = phase / lambda;
        const float phiS = std::sin(phi), phiC = std::cos(phi);
        const float cosPhiS = phiC * r12.phaseCosS - phiS * r12.phaseSinS;
        const float termS = 2 * r01r12s * cosPhiS;
        const float rs = (r01.rs + r12.rs + termS) / (1 + r01.rs * r12.rs + termS);
        const float cosPhiP = phiC * r12.phaseCosP - phiS * r12.phaseSinP;
        const float termP = 2 * r01r12p * cosPhiP;
        const float rp = (r01.rp + r12.rp + termP) / (1 + r01.rp * r12.rp + termP);
        const float r = 0.5F * (rs + rp);
        xyz = {xyz.x + cie[0] * r, xyz.y + cie[1] * r, xyz.z + cie[2] * r};
        lambda += 300.0F / 16.0F;
    }
    xyz = multiply(xyz, 1.0F / 16.0F);
    const Float3 rgbThin{xyz.x * (3.2406F / 0.433509F) + xyz.y * (-1.5372F / 0.433509F) + xyz.z * (-0.4986F / 0.433509F),
                         xyz.x * (-0.9689F / 0.341582F) + xyz.y * (1.8758F / 0.341582F) + xyz.z * (0.0415F / 0.341582F),
                         xyz.x * (0.0557F / 0.32695F) + xyz.y * (-0.204F / 0.32695F) + xyz.z * (1.057F / 0.32695F)};
    return {std::clamp(rgbThin.x, 0.0F, 1.0F), std::clamp(rgbThin.y, 0.0F, 1.0F), std::clamp(rgbThin.z, 0.0F, 1.0F)};
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
                                 float metallic, float roughness, float specular, float ambientOcclusion,
                                 float iridescence, float iridescenceIor, float iridescenceThicknessNm,
                                 float baseIor) noexcept {
    const float NdotV = saturate(dot(normal, viewDirection));
    const Float3 f0 = specular_f0(baseColor, saturate(metallic), saturate(specular));
    const EnvironmentBrdf e = environment_brdf(NdotV, roughness);
    const float bias = e.bias * saturate(50.0F * f0.y);
    const Float3 albedo{f0.x * e.scale + bias, f0.y * e.scale + bias, f0.z * e.scale + bias};
    const float ao = saturate(ambientOcclusion);
    const Float3 diffuseColor = mul3(multiply(baseColor, 1.0F - saturate(metallic)), one_minus(albedo));
    Float3 specularLobe = mul3(albedo, reflected);
    if (iridescence > 0.0F) {
        const Float3 film = thin_film_fresnel(NdotV, iridescenceThicknessNm, iridescenceIor, baseIor, 1.0F);
        specularLobe = mul3(specularLobe, lerp3({1, 1, 1}, film, saturate(iridescence)));
    }
    return {mul3(diffuseColor, irradiance), multiply(specularLobe, specular_occlusion(NdotV, ao, roughness))};
}

} // namespace dve::render
