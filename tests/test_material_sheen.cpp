// Sheen lobe (Tier 2, lobe 1): Charlie distribution + V-cavity masking after NVIDIA's
// nvshaders (Apache-2.0), pinned against double-precision reference values computed
// independently of the implementation.
#include "dve/asset_cooker.hpp"
#include "dve/render/material_bsdf.hpp"
#include "dve/render/surface_shading.hpp"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
}
bool near(float a, double b, double tol) { return std::abs(static_cast<double>(a) - b) <= tol; }
bool near3(dve::Float3 a, double x, double y, double z, double tol) {
    return near(a.x, x, tol) && near(a.y, y, tol) && near(a.z, z, tol);
}
} // namespace

int main() {
    using namespace dve;
    const Float3 n{0, 0, 1};
    const Float3 v{0.5F, 0, 0.8660254F};        // polar 30 deg
    const Float3 l{0.9396926F, 0, 0.3420201F};  // polar 70 deg
    const Float3 color{0.8F, 0.4F, 0.2F};

    // Reference (float64): h=(0.766044,0,0.642788), D=0.328842, G=0.467911,
    // f = color * 0.129870, pdf = 0.056235 at sheenRoughness 0.5 (alpha 0.25).
    const auto e = render::evaluate_sheen(n, v, l, color, 0.5F);
    check(near3(e.brdf, 0.103896, 0.051948, 0.025974, 2e-4), "sheen brdf matches reference");
    check(near(e.pdf, 0.056235, 2e-4), "sheen pdf matches reference");

    // sheen_direct is brdf x radiance x NdotL.
    const auto d = render::sheen_direct(n, v, l, {2, 2, 2}, color, 0.5F);
    check(near3(d, 0.103896 * 2 * 0.3420201, 0.051948 * 2 * 0.3420201, 0.025974 * 2 * 0.3420201, 4e-4),
          "sheen_direct scales brdf by radiance and cosine");

    // Head-on (h == n) the Charlie density is exactly zero: sheen lives at grazing half-angles.
    const auto head = render::evaluate_sheen(n, n, n, color, 0.5F);
    check(head.brdf.x == 0 && head.brdf.y == 0 && head.brdf.z == 0 && head.pdf == 0, "head-on sheen is zero");

    // Coincident grazing view/light (v == l at 80 deg): the velvet peak. f = 0.449... x color.
    const Float3 g{0.9848078F, 0, 0.1736482F};
    const auto graz = render::evaluate_sheen(n, g, g, color, 0.5F);
    check(near(graz.brdf.x, 0.8 * 0.449104, 2e-3), "grazing coincident sheen peaks");
    check(graz.pdf > 0 && std::isfinite(graz.pdf), "grazing pdf positive and finite");

    // Back-facing light or view, and black sheen, are exactly zero.
    check(render::evaluate_sheen(n, v, {0, 0, -1}, color, 0.5F).pdf == 0, "back light: no sheen");
    check(render::evaluate_sheen(n, {0, 0, -1}, l, color, 0.5F).pdf == 0, "back view: no sheen");
    check(render::evaluate_sheen(n, v, l, {0, 0, 0}, 0.5F).pdf == 0, "black sheen: none");

    // Roughness extremes stay finite (0 = tightest Charlie peak, 1 = broadest).
    for (float r : {0.0F, 0.05F, 1.0F}) {
        const auto x = render::evaluate_sheen(n, v, l, color, r);
        check(std::isfinite(x.brdf.x) && std::isfinite(x.pdf) && x.pdf >= 0, "roughness extremes finite");
    }

    // Material gates.
    VoxelMaterialDefinition m;
    check(!material_has_sheen(m) && material_sheen_valid(m), "default material: no sheen, valid");
    m.sheenColor = {0.6F, 0.3F, 0.1F};
    m.sheenRoughness = 0.6F;
    check(material_has_sheen(m) && material_sheen_valid(m), "sheen detected and valid");
    m.sheenRoughness = 1.5F;
    check(!material_sheen_valid(m), "roughness above 1 invalid");
    m.sheenRoughness = 0.6F;
    m.sheenColor.x = -0.1F;
    check(!material_sheen_valid(m), "negative sheen colour invalid");

    // BSDF integration: a sheen material's eval exceeds the same material without sheen,
    // and the no-sheen material is untouched by the new code paths.
    VoxelMaterialDefinition plain, velvet;
    plain.baseColor = {0.5F, 0.5F, 0.5F, 1.0F};
    plain.roughness = 0.8F;
    velvet = plain;
    velvet.sheenColor = color;
    velvet.sheenRoughness = 0.5F;
    const auto ep = render::evaluate_material_bsdf(plain, n, v, l, 1.0F, 1.5F);
    const auto ev = render::evaluate_material_bsdf(velvet, n, v, l, 1.0F, 1.5F);
    check(ev.value.specular.x > ep.value.specular.x, "eval adds the sheen lobe");
    check(near(ev.value.specular.x - ep.value.specular.x, 0.103896, 2e-4), "eval sheen amount matches reference brdf");
    // A black base with sheen only: the mixture is pure sheen, so the eval pdf is the Charlie
    // pdf itself and the specular value is the sheen brdf (adding a lobe dilutes the other
    // lobes' mixture shares, so a plain pdf comparison would be meaningless).
    VoxelMaterialDefinition pure;
    pure.baseColor = {0, 0, 0, 1.0F};
    pure.specular = 0.0F;
    pure.sheenColor = color;
    pure.sheenRoughness = 0.5F;
    const auto es = render::evaluate_material_bsdf(pure, n, v, l, 1.0F, 1.5F);
    check(near(es.pdf, 0.056235, 2e-4), "pure-sheen eval pdf is the Charlie pdf");
    check(near3(es.value.specular, 0.103896, 0.051948, 0.025974, 2e-4), "pure-sheen eval value is the sheen brdf");
    check(near3(ev.value.diffuse, ep.value.diffuse.x, ep.value.diffuse.y, ep.value.diffuse.z, 1e-6),
          "diffuse untouched by sheen");

    // Sampling: force the sheen band (choose above diffuse+specular+coat shares) and confirm a
    // valid above-surface sample with finite weight whose pdf matches the eval's mixture pdf.
    bool sampled = false;
    for (float choose : {0.97F, 0.99F, 0.999F}) {
        const auto s = render::sample_material_bsdf(velvet, n, v, 1.0F, 1.5F, choose, 0.05F, 0.9F, 0.5F);
        if (!s.valid) continue;
        sampled = true;
        check(s.direction.z > 0, "sheen sample stays above the surface");
        check(std::isfinite(s.weight.x) && std::isfinite(s.weight.y) && std::isfinite(s.weight.z),
              "sheen sample weight finite");
        const auto at = render::evaluate_material_bsdf(velvet, n, v, s.direction, 1.0F, 1.5F);
        check(near(s.pdf, at.pdf, 1e-4), "sample pdf matches eval mixture pdf");
    }
    check(sampled, "sheen band sampling produced a valid sample");

    if (failures == 0) std::printf("material sheen tests passed\n");
    return failures == 0 ? 0 : 1;
}
