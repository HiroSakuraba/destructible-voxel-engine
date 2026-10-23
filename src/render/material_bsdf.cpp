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
    float diffuse{}, specular{}, coat{};
};
Mixture mixture(const VoxelMaterialDefinition &m) {
    float d = (std::max)(0.F, lum(rgb(m)) * (1 - m.metallic)),
          s = (std::max)(0.F, lum(specular_f0(rgb(m), m.metallic, m.specular)));
    float c =
        m.shadingModel == MaterialShadingModel::ClearCoat ? .04F * std::clamp(m.clearCoat, 0.F, 1.F) : 0;
    if (s > 0 && d > 0)
        s = (std::max)(s, .1F * d);
    const float total = d + s + c;
    return total > 0 ? Mixture{d / total, s / total, c / total} : Mixture{1, 0, 0};
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
    return q.diffuse * nl / pi +
           q.specular * half_pdf(n, v, h, alpha((std::max)(m.roughness, .02F))) / (4 * vh) +
           q.coat * half_pdf(n, v, h, alpha((std::max)(m.clearCoatRoughness, .02F))) / (4 * vh);
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
                F = dielectric_fresnel(vh, ni, nt), ph = half_pdf(n, v, h, a);
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
    out.value.specular = {value, value, value};
    return out;
}
} // namespace
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
                                          m.clearCoat, m.clearCoatRoughness)
                : shade_direct_light(n, v, l, {1, 1, 1}, rgb(m), m.metallic, m.roughness, m.specular);
        out.value.diffuse = times(opaque.diffuse, 1 - t);
        out.value.specular = times(opaque.specular, 1 - t);
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
        const float F = dielectric_fresnel(std::clamp(dp(v, h), 0.F, 1.F), ni, nt);
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
            out.weight = {gain, gain, gain};
            return out;
        }
    } else if (smooth(m) && m.metallic >= 1) {
        out.direction = reflect_direction(times(v, -1), n);
        out.weight = fresnel_schlick(dp(n, v), specular_f0(rgb(m), m.metallic, m.specular));
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
        } else {
            const float rough = r < q.diffuse + q.specular ? m.roughness : m.clearCoatRoughness;
            const auto h = visible_normal(n, v, alpha((std::max)(rough, .02F)), u1, u2);
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
