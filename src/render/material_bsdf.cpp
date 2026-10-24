#include "dve/render/material_bsdf.hpp"
#include "dve/render/optical_scene.hpp"
#include <algorithm>
#include <cmath>

namespace dve::render {
namespace {
constexpr float pi = 3.14159265358979323846F;
Float3 plus(Float3 a, Float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Float3 times(Float3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
float dp(Float3 a, Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 cp(Float3 a, Float3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Float3 norm(Float3 a) {
    float l = std::sqrt(dp(a, a));
    return l > 1e-20F ? times(a, 1 / l) : Float3{0, 1, 0};
}
Float3 rgb(const VoxelMaterialDefinition &m) { return {m.baseColor.x, m.baseColor.y, m.baseColor.z}; }
float lum(Float3 a) { return .2126F * a.x + .7152F * a.y + .0722F * a.z; }
float alpha(float rough) { return (std::max)(1e-4F, rough * rough); }
bool smooth(const VoxelMaterialDefinition &m) { return m.roughness <= .001F; }
struct Basis {
    Float3 n, t, b;
    explicit Basis(Float3 normal) : n(norm(normal)) {
        t = norm(cp(std::abs(n.y) < .9F ? Float3{0, 1, 0} : Float3{1, 0, 0}, n));
        b = cp(n, t);
    }
    Float3 local(Float3 v) const { return {dp(v, t), dp(v, b), dp(v, n)}; }
    Float3 world(Float3 v) const { return plus(plus(times(t, v.x), times(b, v.y)), times(n, v.z)); }
};
float g1(float cosine, float a) {
    cosine = std::abs(cosine);
    if (cosine <= 0)
        return 0;
    return 2 / (1 + std::sqrt(1 + a * a * (1 - cosine * cosine) / (cosine * cosine)));
}
float g2(float v, float l, float a) {
    const float gv = g1(v, a), gl = g1(l, a);
    return gv > 0 && gl > 0 ? 1 / (1 / gv + 1 / gl - 1) : 0;
}
float distribution(float nh, float a) {
    if (nh <= 0)
        return 0;
    const float q = nh * nh * (a * a - 1) + 1;
    return a * a / (pi * q * q);
}
// Isotropic GGX visible-normal sampling in a stretched view frame (Heitz's disk construction).
Float3 visible_normal(Float3 n, Float3 view, float a, float u1, float u2) {
    Basis basis(n);
    const auto v = basis.local(view);
    const auto vh = norm({a * v.x, a * v.y, (std::max)(v.z, 1e-7F)});
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const auto t1 = lensq > 0 ? times(Float3{-vh.y, vh.x, 0}, 1 / std::sqrt(lensq)) : Float3{1, 0, 0};
    const auto t2 = cp(vh, t1);
    const float radius = std::sqrt(std::clamp(u1, 0.F, .99999994F)), phi = 2 * pi * u2;
    const float x = radius * std::cos(phi);
    float y = radius * std::sin(phi);
    const float s = .5F * (1 + vh.z);
    y = (1 - s) * std::sqrt((std::max)(0.F, 1 - x * x)) + s * y;
    const auto h =
        plus(plus(times(t1, x), times(t2, y)), times(vh, std::sqrt((std::max)(0.F, 1 - x * x - y * y))));
    return norm(basis.world(norm({a * h.x, a * h.y, (std::max)(0.F, h.z)})));
}
float half_pdf(Float3 n, Float3 v, Float3 h, float a) {
    const float nv = dp(n, v);
    return nv > 0 ? distribution(dp(n, h), a) * g1(nv, a) * std::abs(dp(v, h)) / nv : 0;
}
struct Mixture {
    float diffuse{}, specular{}, coat{}, sheen{};
};
Mixture mixture(const VoxelMaterialDefinition &m) {
    float d = (std::max)(0.F, lum(rgb(m)) * (1 - m.metallic)),
          s = (std::max)(0.F, lum(specular_f0(rgb(m), m.metallic, m.specular)));
    float c =
        m.shadingModel == MaterialShadingModel::ClearCoat ? .04F * std::clamp(m.clearCoat, 0.F, 1.F) : 0;
    // Sheen's selection prior: half its strongest channel (a broad lobe; the exact directional
    // albedo varies with angle, and the mixture only chooses which sampler runs — the pdf
    // mixture in opaque_pdf keeps the estimator unbiased).
    float sh = material_has_sheen(m)
                   ? .5F * (std::max)({m.sheenColor.x, m.sheenColor.y, m.sheenColor.z})
                   : 0;
    if (s > 0 && d > 0)
        s = (std::max)(s, .1F * d);
    const float total = d + s + c + sh;
    return total > 0 ? Mixture{d / total, s / total, c / total, sh / total} : Mixture{1, 0, 0, 0};
}
float opaque_pdf(const VoxelMaterialDefinition &m, Float3 n, Float3 v, Float3 l) {
    const float nl = dp(n, l);
    if (nl <= 0)
        return 0;
    auto q = mixture(m);
    auto h = norm(plus(v, l));
    const float vh = std::abs(dp(v, h));
    if (vh <= 0)
        return 0;
    float pdf = q.diffuse * nl / pi +
                q.specular * half_pdf(n, v, h, alpha((std::max)(m.roughness, .02F))) / (4 * vh) +
                q.coat * half_pdf(n, v, h, alpha((std::max)(m.clearCoatRoughness, .02F))) / (4 * vh);
    if (q.sheen > 0)
        pdf += q.sheen * evaluate_sheen(n, v, l, m.sheenColor, m.sheenRoughness).pdf;
    return pdf;
}
MaterialBsdfEvaluation dielectric_eval(const VoxelMaterialDefinition &m, Float3 n, Float3 v, Float3 l,
                                       float ni, float nt) {
    MaterialBsdfEvaluation out;
    if (smooth(m) || ni == nt)
        return out;
    const float nv = dp(n, v), nl = dp(n, l);
    if (nv <= 0 || nl == 0)
        return out;
    const bool reflection = nl > 0;
    const float eta = nt / ni;
    auto h = norm(reflection ? plus(v, l) : plus(v, times(l, eta)));
    if (dp(n, h) < 0)
        h = times(h, -1);
    const float vh = dp(v, h), lh = dp(l, h);
    if (vh <= 0 || (reflection ? lh <= 0 : lh >= 0))
        return out;
    const float a = alpha(m.roughness), D = distribution(dp(n, h), a), G = g2(nv, nl, a),
                ph = half_pdf(n, v, h, a);
    const auto reflectance=material_interface_reflectance(m,vh,ni,nt);
    const float F=lum(reflectance);
    float value = 0;
    if (reflection) {
        value = F * D * G / (4 * nv);
        out.pdf = ph * F / (4 * vh);
    } else {
        const float den = vh + eta * lh;
        if (std::abs(den) < 1e-12F)
            return out;
        value = (1 - F) * D * G * std::abs(vh * lh) / (nv * den * den);
        out.pdf = ph * (1 - F) * std::abs(lh) * eta * eta / (den * den);
    }
    const float probability=reflection?F:1-F;
    const auto energy=reflection?reflectance:Float3{1-reflectance.x,1-reflectance.y,1-reflectance.z};
    out.value.specular = probability>0?times(energy,value/probability):Float3{};
    return out;
}
} // namespace
Float3 material_interface_reflectance(const VoxelMaterialDefinition& m,float cosine,float ni,float nt) noexcept {
    const float ordinary=dielectric_fresnel(std::clamp(cosine,0.F,1.F),ni,nt);
    if(m.iridescence<=0 || ordinary>=1)return {ordinary,ordinary,ordinary};
    const auto film=thin_film_fresnel(cosine,m.iridescenceThicknessNm,m.iridescenceIor,nt,ni);
    const float w=std::clamp(m.iridescence,0.F,1.F);
    return {ordinary+(film.x-ordinary)*w,ordinary+(film.y-ordinary)*w,ordinary+(film.z-ordinary)*w};
}
MaterialBsdfEvaluation evaluate_material_bsdf(const VoxelMaterialDefinition &m, Float3 n, Float3 v, Float3 l,
                                              float ni, float nt) noexcept {
    MaterialBsdfEvaluation out;
    n = norm(n);
    v = norm(v);
    l = norm(l);
    if (dp(n, v) <= 0 || ni <= 0 || nt <= 0)
        return out;
    const float t = std::clamp(m.transmission, 0.F, 1.F);
    if (t < 1 && !(smooth(m) && m.metallic >= 1)) {
        const auto opaque =
            m.shadingModel == MaterialShadingModel::ClearCoat
                ? shade_clear_coat_direct(n, v, l, {1, 1, 1}, rgb(m), m.metallic, m.roughness, m.specular,
                                          m.clearCoat, m.clearCoatRoughness, m.iridescence, m.iridescenceIor,
                                          m.iridescenceThicknessNm, m.indexOfRefraction)
                : shade_direct_light(n, v, l, {1, 1, 1}, rgb(m), m.metallic, m.roughness, m.specular,
                                     m.iridescence, m.iridescenceIor, m.iridescenceThicknessNm,
                                     m.indexOfRefraction);
        out.value.diffuse = times(opaque.diffuse, 1 - t);
        out.value.specular = times(opaque.specular, 1 - t);
        if (material_has_sheen(m))   // sheen_direct = brdf x NdotL: the value carries the incident cosine like the other lobes
            out.value.specular =
                plus(out.value.specular, times(sheen_direct(n, v, l, {1, 1, 1}, m.sheenColor, m.sheenRoughness), 1 - t));
        out.pdf = (1 - t) * opaque_pdf(m, n, v, l);
    }
    if (t > 0) {
        const auto glass = dielectric_eval(m, n, v, l, ni, nt);
        out.value.specular = plus(out.value.specular, times(glass.value.specular, t));
        out.pdf += t * glass.pdf;
    }
    return out;
}
MaterialBsdfSample sample_material_bsdf(const VoxelMaterialDefinition &m, Float3 n, Float3 v, float ni,
                                        float nt, float choose, float u1, float u2, float uc) noexcept {
    MaterialBsdfSample out;
    n = norm(n);
    v = norm(v);
    if (dp(n, v) <= 0 || ni <= 0 || nt <= 0)
        return out;
    const float t = std::clamp(m.transmission, 0.F, 1.F);
    const bool glass = choose < t;
    if (glass) {
        const bool delta = smooth(m) || ni == nt;
        const auto h = delta ? n : visible_normal(n, v, alpha(m.roughness), u1, u2);
        const auto reflectance=material_interface_reflectance(m,dp(v,h),ni,nt);
        const float F=lum(reflectance);
        const auto refracted = refract_direction(times(v, -1), h, ni / nt);
        const bool reflect = !refracted || uc < F;
        out.direction = reflect ? reflect_direction(times(v, -1), h) : *refracted;
        out.transmitted = !reflect;
        if ((reflect && dp(n, out.direction) <= 0) || (!reflect && dp(n, out.direction) >= 0))
            return {};
        if (delta) {
            out.delta = true;
            out.valid = true;
            out.pdf = t * (reflect ? F : 1 - F);
            const float gain = reflect ? 1 : ni * ni / (nt * nt);
            const float probability=reflect?F:1-F;
            const auto energy=reflect?reflectance:Float3{1-reflectance.x,1-reflectance.y,1-reflectance.z};
            out.weight = probability>0?times(energy,gain/probability):Float3{};
            return out;
        }
    } else if (smooth(m) && m.metallic >= 1) {
        out.direction = reflect_direction(times(v, -1), n);
        out.weight = fresnel_schlick(dp(n, v), specular_f0(rgb(m), m.metallic, m.specular));
        if (m.iridescence > 0) {
            // Mirror lobe: the film factor at NdotV == VdotH tints the reflection, as in eval.
            const auto film = thin_film_fresnel(dp(n, v), m.iridescenceThicknessNm, m.iridescenceIor,
                                                m.indexOfRefraction, 1.0F);
            const float w = std::clamp(m.iridescence, 0.F, 1.F);
            out.weight = {out.weight.x * (1 + (film.x - 1) * w), out.weight.y * (1 + (film.y - 1) * w),
                          out.weight.z * (1 + (film.z - 1) * w)};
        }
        out.pdf = 1 - t;
        out.delta = true;
        out.valid = true;
        return out;
    } else {
        const auto q = mixture(m);
        const float r = t < 1 ? (choose - t) / (1 - t) : 0;
        if (r < q.diffuse) {
            const float radius = std::sqrt(std::clamp(u1, 0.F, .99999994F)), phi = 2 * pi * u2;
            out.direction = norm(Basis(n).world(
                {radius * std::cos(phi), radius * std::sin(phi), std::sqrt((std::max)(0.F, 1 - u1))}));
        } else if (q.sheen <= 0 || r < q.diffuse + q.specular + q.coat) {   // no sheen lobe: rounding at r ~ 1 stays on the coat/specular lobes
            const float rough = r < q.diffuse + q.specular ? m.roughness : m.clearCoatRoughness;
            const auto h = visible_normal(n, v, alpha((std::max)(rough, .02F)), u1, u2);
            out.direction = reflect_direction(times(v, -1), h);
            if (dp(n, out.direction) <= 0)
                return {};
        } else {
            // Sheen: sample the Charlie distribution by inversion (nvshaders' hvd_sheen_sample):
            // phi uniform, sin(theta_h) = (1 - u2)^(1 / (1/alpha + 2)). The resulting half-vector
            // density is D x NdotH, so the directional pdf is D x NdotH / (4 VdotH) — exactly
            // what evaluate_sheen reports.
            const float rough = std::clamp(m.sheenRoughness, 0.F, 1.F);
            const float invA = 1 / (std::max)(rough * rough, 1e-4F);
            const float phi = 2 * pi * u1;
            const float sinT = std::pow(1 - std::clamp(u2, 0.F, .99999994F), 1 / (invA + 2));
            const float cosT = std::sqrt((std::max)(0.F, 1 - sinT * sinT));
            const auto h = Basis(n).world({std::cos(phi) * sinT, std::sin(phi) * sinT, cosT});
            out.direction = reflect_direction(times(v, -1), h);
            if (dp(n, out.direction) <= 0)
                return {};
        }
    }
    const auto e = evaluate_material_bsdf(m, n, v, out.direction, ni, nt);
    if (!(e.pdf > 0) || !std::isfinite(e.pdf))
        return {};
    const auto total = plus(e.value.diffuse, e.value.specular);
    out.weight = times(total, 1 / e.pdf);
    out.pdf = e.pdf;
    out.diffuseShare = {total.x > 0 ? e.value.diffuse.x / total.x : 0,
                        total.y > 0 ? e.value.diffuse.y / total.y : 0,
                        total.z > 0 ? e.value.diffuse.z / total.z : 0};
    out.valid = std::isfinite(out.weight.x) && std::isfinite(out.weight.y) && std::isfinite(out.weight.z);
    return out;
}
} // namespace dve::render
