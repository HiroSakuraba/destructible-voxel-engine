// Iridescence (Tier 2, lobe 2): the thin-film Fresnel factor ported from NVIDIA's nvshaders
// (Apache-2.0), pinned against an independent double-precision implementation of the same
// Born & Wolf film model.
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

    // A zero-thickness film at normal incidence is exactly the base's F0 (1 -> 1.5: 0.04, grey).
    check(near3(render::thin_film_fresnel(1.0F, 0.0F, 1.3F, 1.5F, 1.0F), 0.04, 0.04, 0.04, 1e-3),
          "zero film at normal incidence is the base F0");
    // Interference colours at 400 nm (the reference soap-film regime).
    check(near3(render::thin_film_fresnel(1.0F, 400.0F, 1.3F, 1.5F, 1.0F), 0.01451, 0.03805, 0.01640, 2e-3),
          "400 nm film at normal incidence");
    check(near3(render::thin_film_fresnel(0.5F, 400.0F, 1.3F, 1.5F, 1.0F), 0.06002, 0.02582, 0.05428, 2e-3),
          "400 nm film at cos 0.5");
    check(near3(render::thin_film_fresnel(0.5F, 100.0F, 1.3F, 1.5F, 1.0F), 0.04753, 0.03694, 0.02593, 2e-3),
          "100 nm film at cos 0.5");
    // A film whose index equals the base is no film at all: the plain Fresnel of the base,
    // achromatic at every thickness.
    const auto none = render::thin_film_fresnel(0.5F, 400.0F, 1.5F, 1.5F, 1.0F);
    check(near3(none, 0.08919, 0.08919, 0.08919, 2e-3), "film index == base index is plain Fresnel");
    // Total internal reflection entering a lower-index film at a grazing angle.
    check(near3(render::thin_film_fresnel(0.2F, 300.0F, 1.0F, 1.5F, 1.5F), 1, 1, 1, 1e-6), "TIR returns one");
    // Bounded everywhere.
    for (float c : {0.0F, 0.23F, 0.77F, 1.0F})
        for (float t : {0.0F, 137.0F, 1000.0F}) {
            const auto f = render::thin_film_fresnel(c, t, 1.3F, 2.4F, 1.0F);
            check(f.x >= 0 && f.x <= 1 && f.y >= 0 && f.y <= 1 && f.z >= 0 && f.z <= 1 &&
                      std::isfinite(f.x + f.y + f.z),
                  "film factor stays in [0,1]");
        }

    // Material gates.
    VoxelMaterialDefinition m;
    check(!material_has_iridescence(m) && material_iridescence_valid(m), "default material: no film, valid");
    m.iridescence = 0.8F;
    check(material_has_iridescence(m) && material_iridescence_valid(m), "film detected and valid");
    m.iridescenceIor = 0.5F;
    check(!material_iridescence_valid(m), "film IOR below 1 invalid");
    m.iridescenceIor = 1.3F;
    m.iridescenceThicknessNm = -5.0F;
    check(!material_iridescence_valid(m), "negative thickness invalid");

    // BSDF integration: factor 1 tints the specular lobe and leaves diffuse alone; factor 0 is
    // the untouched material. Symmetric grazing geometry puts VdotH at 0.2, where the pinned
    // 525 nm film factor is (0.2195, 0.2709, 0.3433): the specular ratio per channel must equal
    // the film factor (the plain lobe is achromatic), which is the interference colour.
    const Float3 n{0, 0, 1}, v{0.9799F, 0, 0.1999F}, l{-0.9799F, 0, 0.1999F};
    VoxelMaterialDefinition plain, film;
    plain.baseColor = {0.5F, 0.5F, 0.5F, 1.0F};
    plain.roughness = 0.4F;
    film = plain;
    film.iridescence = 1.0F;
    film.iridescenceIor = 1.3F;
    film.iridescenceThicknessNm = 525.0F;
    const auto ep = render::evaluate_material_bsdf(plain, n, v, l, 1.0F, 1.5F);
    const auto ef = render::evaluate_material_bsdf(film, n, v, l, 1.0F, 1.5F);
    check(near3(ef.value.diffuse, ep.value.diffuse.x, ep.value.diffuse.y, ep.value.diffuse.z, 1e-6),
          "diffuse untouched by the film");
    check(ep.value.specular.x > 1e-5, "plain specular is measurable at this geometry");
    if (ep.value.specular.x > 1e-5) {
        check(near(ef.value.specular.x / ep.value.specular.x, 0.2195, 0.01) &&
                  near(ef.value.specular.y / ep.value.specular.y, 0.2709, 0.01) &&
                  near(ef.value.specular.z / ep.value.specular.z, 0.3433, 0.01),
              "specular ratio per channel equals the film factor");
    }
    check(near(ef.pdf, ep.pdf, 1e-6), "film does not change the sampling pdf");

    if (failures == 0) std::printf("material iridescence tests passed\n");
    return failures == 0 ? 0 : 1;
}
