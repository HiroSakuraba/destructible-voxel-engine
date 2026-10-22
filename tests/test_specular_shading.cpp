// Specular shading in the CPU reference renderers (surface_shading.hpp):
//   - the lobes match the GPU model's closed forms (pbr_lighting.hlsli) at known angles;
//   - energy: a white surface reflects no more light than it receives, from the sun (direct lobes
//     integrated over the hemisphere) or from a uniform sky (ambient diffuse + specular = 1);
//   - the tabulated environment BRDF matches the numerical directional albedo;
//   - metals have no diffuse; specular occlusion follows AO;
//   - in both renderers the highlight follows the camera: the pixel equals pi x (diffuse +
//     specular) of the direct model for the camera's view, mirror and retro views alike,
//     including the clear coat (whose polygon highlight used a fixed view before).
#include "dve/render/surface_shading.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/render/voxel_reference_renderer.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
constexpr double kPi = 3.14159265358979323846;

Float3 unit(Float3 v) { return normalize(v); }
float sum3(Float3 v) { return v.x + v.y + v.z; }

void test_closed_forms() {
    // Normal incidence: N = V = L. D = 1 / (pi a^2), V = 1/4, F = F0.
    for (const float r : {0.2F, 0.5F, 1.0F}) {
        const ShadingLobes l = shade_direct_light({0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {1, 1, 1}, {1, 1, 1}, 0.0F, r, 0.5F);
        const double a = static_cast<double>(r) * r;
        const double spec = 0.04 / (kPi * a * a) * 0.25;
        check(std::abs(l.specular.x - spec) < 1e-3 * spec, "normal-incidence specular = F0 D V (roughness " + std::to_string(r) + ")");
        check(std::abs(l.diffuse.x - 0.96 / kPi) < 1e-5, "normal-incidence diffuse = (1 - F0) / pi");
    }
    // Metals: no diffuse, coloured F0.
    const ShadingLobes metal = shade_direct_light({0, 1, 0}, unit({1, 1, 0}), unit({-1, 1, 0}), {1, 1, 1}, {0.9F, 0.6F, 0.2F}, 1.0F, 0.4F, 0.5F);
    check(sum3(metal.diffuse) == 0.0F, "metals have no diffuse lobe");
    check(metal.specular.x > metal.specular.y && metal.specular.y > metal.specular.z, "metal highlights take the base colour");
    // No light from below.
    const ShadingLobes below = shade_direct_light({0, 1, 0}, {0, 1, 0}, {0, -1, 0}, {1, 1, 1}, {1, 1, 1}, 0.0F, 0.5F, 0.5F);
    check(sum3(below.diffuse) + sum3(below.specular) == 0.0F, "nothing from below the surface");
    // Clear coat adds a sharp lobe and dims the base.
    const ShadingLobes plain = shade_direct_light({0, 1, 0}, unit({1, 1, 0}), unit({-1, 1, 0}), {1, 1, 1}, {0.5F, 0.1F, 0.1F}, 0.0F, 0.8F, 0.5F);
    const ShadingLobes coat = shade_clear_coat_direct({0, 1, 0}, unit({1, 1, 0}), unit({-1, 1, 0}), {1, 1, 1}, {0.5F, 0.1F, 0.1F}, 0.0F, 0.8F, 0.5F, 1.0F, 0.05F);
    check(coat.specular.x > 10.0F * plain.specular.x && coat.diffuse.x < plain.diffuse.x, "clear coat: a sharp highlight over a dimmed base");
}

// Directional albedo of the direct lobes (diffuse + specular) by uniform hemisphere sampling.
double direct_albedo(Float3 base, float metallic, float roughness, float ndotv) {
    const Float3 n{0, 1, 0};
    const Float3 v{std::sqrt(1.0F - ndotv * ndotv), ndotv, 0.0F};
    const int k = 400;
    double sum = 0.0;
    for (int i = 0; i < k; ++i)
        for (int j = 0; j < k; ++j) {
            const double cosT = (i + 0.5) / k, phi = 2.0 * kPi * (j + 0.5) / k, sinT = std::sqrt(1.0 - cosT * cosT);
            const Float3 l{static_cast<float>(sinT * std::cos(phi)), static_cast<float>(cosT), static_cast<float>(sinT * std::sin(phi))};
            const ShadingLobes s = shade_direct_light(n, v, l, {1, 1, 1}, base, metallic, roughness, 0.5F);
            sum += s.diffuse.x + s.specular.x;   // the lobes already include cos
        }
    return sum * 2.0 * kPi / (static_cast<double>(k) * k);
}

void test_energy() {
    // The GPU model (kept for parity) weights diffuse by 1 - F at the half vector, which lets a
    // white dielectric seen at grazing angles reflect a little more than it receives (about 1.2 at
    // NdotV 0.2, roughness 0.3); facing views stay within 1%. Single-scattering GGX loses energy
    // at high roughness (rough metals come out darker), as on the GPU.
    double worstDirect = 0.0, worstFacing = 0.0;
    for (const float metallic : {0.0F, 1.0F})
        for (const float r : {0.3F, 0.6F, 1.0F})
            for (const float c : {0.2F, 0.6F, 1.0F}) {
                const double v = direct_albedo({1, 1, 1}, metallic, r, c);
                if (std::getenv("DVE_VERBOSE")) std::printf("  metallic %.0f roughness %.1f NdotV %.1f: %.4f\n", metallic, r, c, v);
                worstDirect = std::max(worstDirect, v);
                if (c >= 0.6F) worstFacing = std::max(worstFacing, v);
            }
    std::printf("white furnace, sun: largest reflected share %.4f (NdotV >= 0.6: %.4f)\n", worstDirect, worstFacing);
    check(worstFacing <= 1.01, "a white surface seen face-on reflects no more of the sun than it receives");
    check(worstDirect <= 1.25, "the grazing overshoot of the half-vector Fresnel stays bounded");

    double worstAmbient = 0.0, lowestDielectric = 2.0;
    for (const float metallic : {0.0F, 1.0F})
        for (float r = 0.0F; r <= 1.0F; r += 0.125F)
            for (float c = 0.05F; c <= 1.0F; c += 0.15F) {
                const Float3 v{std::sqrt(1.0F - c * c), c, 0.0F};
                const ShadingLobes s = shade_ambient_light({0, 1, 0}, v, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, metallic, r, 0.5F, 1.0F);
                const double total = s.diffuse.x + s.specular.x;
                worstAmbient = std::max(worstAmbient, total);
                if (metallic == 0.0F) lowestDielectric = std::min(lowestDielectric, total);
            }
    std::printf("white furnace, uniform sky: reflected share %.4f..%.4f (white dielectric)\n", lowestDielectric, worstAmbient);
    check(worstAmbient <= 1.0 + 1e-4, "ambient diffuse + specular never exceed the sky");
    check(lowestDielectric >= 1.0 - 1e-4, "a white dielectric under a uniform sky reflects all of it (diffuse takes 1 - Es)");

    // Specular occlusion: none when unoccluded, full when fully occluded.
    check(std::abs(specular_occlusion(0.7F, 1.0F, 0.5F) - 1.0F) < 1e-6F && specular_occlusion(0.7F, 0.0F, 0.5F) == 0.0F,
          "specular occlusion runs from 1 (open) to 0 (closed)");
}

void test_table() {
    double worst = 0.0;
    for (const float r : {0.1F, 0.35F, 0.6F, 0.9F})
        for (const float c : {0.1F, 0.4F, 0.75F, 0.95F})
            for (const float f : {0.04F, 1.0F}) {
                const EnvironmentBrdf e = environment_brdf(c, r);
                const Float3 exact = specular_directional_albedo(c, r, {f, f, f}, 8192U);
                worst = std::max(worst, std::abs(static_cast<double>(f * e.scale + e.bias - exact.x)));
            }
    std::printf("environment BRDF table vs numerical albedo: worst %.4f\n", worst);
    check(worst < 0.02, "the table matches the integral (32 x 32, bilinear)");
}

PolygonCamera camera_at(Float3 position, Float3 target) {
    PolygonCamera camera;
    camera.position = position;
    camera.target = target;
    camera.up = {0.0F, 1.0F, 0.0F};
    camera.verticalFieldOfViewRadians = 0.2F;
    camera.nearPlane = 0.5F;
    camera.farPlane = 2000.0F;
    return camera;
}

VoxelMaterialDefinition glossy(MaterialShadingModel model) {
    VoxelMaterialDefinition m;
    m.baseColor = {0.6F, 0.3F, 0.2F, 1.0F};
    m.roughness = 0.3F;
    m.specular = 0.5F;
    m.shadingModel = model;
    m.clearCoat = model == MaterialShadingModel::ClearCoat ? 1.0F : 0.0F;
    m.clearCoatRoughness = 0.1F;
    return m;
}

CookedPolygonAsset slab(const VoxelMaterialDefinition& material) {
    CookedPolygonAsset a;
    a.objectId = 90;
    a.materials.push_back(material);
    a.materialBindings.resize(1);
    for (const Float3 q : {Float3{-8, 1, -8}, Float3{-8, 1, 8}, Float3{8, 1, 8}, Float3{8, 1, -8}})
        a.vertices.push_back({q, {0, 1, 0}, {1, 0, 0, 1}, {0, 0}, {1, 1, 1, 1}, {0, 0}});
    a.indices = {0, 1, 2, 0, 2, 3};
    a.submeshes.push_back({"slab", 0, 6, 0});
    a.bounds = {{-8, 0.99F, -8}, {8, 1.01F, 8}};   // small: every vertex in front of the camera
    return a;
}

void test_highlight_follows_camera() {
    RenderEnvironment environment;
    environment.sunDirection = unit({1, 1, 0});
    environment.sunColor = {1, 1, 1};
    environment.sunIntensity = 3.0F;
    environment.globalIlluminationMode = GlobalIlluminationMode::Off;
    environment.ambientOcclusionSamples = 0;
    environment.shadowMode = ShadowMode::Off;
    const Float3 point{0.3F, 1.0F, 0.2F};
    const Float3 n{0, 1, 0}, l = unit({1, 1, 0});
    for (const MaterialShadingModel model : {MaterialShadingModel::StandardPBR, MaterialShadingModel::ClearCoat}) {
        const VoxelMaterialDefinition m = glossy(model);
        const Float3 rgb{m.baseColor.x, m.baseColor.y, m.baseColor.z};
        const char* name = model == MaterialShadingModel::ClearCoat ? "clear coat" : "standard";
        const auto expected = [&](Float3 view) {
            const ShadingLobes s = model == MaterialShadingModel::ClearCoat
                ? shade_clear_coat_direct(n, view, l, {3, 3, 3}, rgb, 0.0F, m.roughness, m.specular, 1.0F, 0.1F)
                : shade_direct_light(n, view, l, {3, 3, 3}, rgb, 0.0F, m.roughness, m.specular);
            return kPi * (s.diffuse.x + s.specular.x);
        };
        // Polygon reference.
        const CookedPolygonAsset floor = slab(m);
        // Voxel reference: a voxel floor, top face at y = 1.
        VoxelObject object(1U);
        for (int x = -20; x < 20; ++x)
            for (int z = -20; z < 20; ++z) object.set_voxel({x, 0, z}, 1);
        std::array<VoxelMaterialDefinition, 2> materials{VoxelMaterialDefinition{}, m};
        const std::vector<VoxelReferenceInstance> voxels{{1U, &object, {}, materials, true, {1, 1, 1}}};
        double mirror[2]{}, retro[2]{};
        for (const bool atMirror : {true, false}) {
            const Float3 view = atMirror ? unit({-1, 1, 0}) : unit({1, 1, 0});
            const PolygonCamera camera = camera_at(add(point, multiply(view, 30.0F)), point);
            PolygonRenderTarget polygon;
            polygon.resize(3, 3);
            PolygonRenderOptions options;
            options.preserveExistingDepth = false;
            const std::vector<PolygonRenderInstance> instances{{90U, &floor, {}, {}, {1, 1, 1, 1}, true}};
            (void)ReferencePolygonRenderer{}.render(instances, camera, environment, polygon, options);
            PolygonRenderTarget voxel;
            voxel.resize(3, 3);
            (void)ReferenceVoxelRenderer{}.render(voxels, camera, environment, voxel);
            const double want = expected(view);
            const double got[2]{polygon.hdrColor[4].x, voxel.hdrColor[4].x};
            for (int r = 0; r < 2; ++r) {
                (atMirror ? mirror : retro)[r] = got[r];
                check(std::abs(got[r] - want) < 2e-3 * want + 1e-4,
                      std::string(name) + (r == 0 ? " polygon" : " voxel") + (atMirror ? " mirror" : " retro") +
                          " view: pixel " + std::to_string(got[r]) + " = pi (diffuse + specular) " + std::to_string(want));
            }
        }
        std::printf("%s: polygon mirror %.4f / retro %.4f, voxel mirror %.4f / retro %.4f\n", name, mirror[0], retro[0], mirror[1], retro[1]);
        check(mirror[0] > 1.5 * retro[0] && mirror[1] > 1.5 * retro[1], std::string(name) + ": the highlight sits at the mirror view");
    }
}

} // namespace

int main() {
    test_closed_forms();
    test_energy();
    test_table();
    test_highlight_follows_camera();
    if (g_failures == 0) std::printf("dve_specular_shading_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
