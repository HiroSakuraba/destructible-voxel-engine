#include "dve/render/material_bsdf.hpp"
#include "dve/render/optical_scene.hpp"
#include "dve/render/scene_lighting.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace dve;
using namespace dve::render;
namespace {
constexpr float pi = 3.14159265358979323846F;
void check(bool v, const char *m) {
    if (!v)
        throw std::runtime_error(m);
}
void near(float a, float b, float eps, const char *m) {
    if (std::abs(a - b) > eps) {
        std::cerr << a << " != " << b << '\n';
        throw std::runtime_error(m);
    }
}
struct Random {
    unsigned s = 1;
    float next() {
        s += 0x9e3779b9U;
        auto x = s;
        x = (x ^ (x >> 16)) * 0x21f0aaadU;
        x = (x ^ (x >> 15)) * 0x735a2d97U;
        x ^= x >> 15;
        return float(x >> 8) * 0x1p-24F;
    }
};
VoxelMaterialDefinition glass(float rough = 0) {
    VoxelMaterialDefinition m;
    m.transmission = 1;
    m.roughness = rough;
    m.indexOfRefraction = 1.5F;
    return m;
}
RenderEnvironment environment() {
    RenderEnvironment e;
    e.sunIntensity = 0;
    e.skyColor = {};
    e.groundColor = {};
    e.globalIlluminationIntensity = 1;
    e.pathTraceBounces = 12;
    e.pathTraceSamples = 128;
    e.localLightSamples = 1;
    e.globalIlluminationMaxDistanceMeters = 100;
    e.bloomIntensity = 0;
    e.shadowBiasMeters = .00001F;
    return e;
}
CookedPolygonAsset slab(float front, float back, VoxelMaterialDefinition material) {
    CookedPolygonAsset a;
    a.materials = {material};
    a.materialBindings.resize(1);
    auto face = [&](Float3 p, Float3 q, Float3 r, Float3 s, Float3 normal) {
        auto base = static_cast<unsigned>(a.vertices.size());
        for (auto v : {p, q, r, s}) {
            PolygonVertex vert;
            vert.position = v;
            vert.normal = normal;
            a.vertices.push_back(vert);
        }
        for (auto i : {0U, 1U, 2U, 0U, 2U, 3U})
            a.indices.push_back(base + i);
    };
    const float w = 10;
    face({-w, -w, front}, {-w, w, front}, {w, w, front}, {w, -w, front}, {0, 0, -1});
    face({-w, -w, back}, {w, -w, back}, {w, w, back}, {-w, w, back}, {0, 0, 1});
    face({-w, -w, front}, {w, -w, front}, {w, -w, back}, {-w, -w, back}, {0, -1, 0});
    face({-w, w, front}, {-w, w, back}, {w, w, back}, {w, w, front}, {0, 1, 0});
    face({-w, -w, front}, {-w, -w, back}, {-w, w, back}, {-w, w, front}, {-1, 0, 0});
    face({w, -w, front}, {w, w, front}, {w, w, back}, {w, -w, back}, {1, 0, 0});
    a.submeshes = {{"slab", 0, static_cast<unsigned>(a.indices.size()), 0}};
    a.bounds = {{-w, -w, front}, {w, w, back}};
    return a;
}
void interfaces() {
    const auto m = glass();
    auto r = sample_material_bsdf(m, {0, 0, 1}, {0, 0, 1}, 1, 1.5F, 0, .3F, .5F, 0);
    check(r.valid && r.delta && !r.transmitted, "Fresnel reflection branch");
    near(r.direction.z, 1, 1e-6F, "mirror reflection direction");
    auto t = sample_material_bsdf(m, {0, 0, 1}, {0, 0, 1}, 1, 1.5F, 0, .3F, .5F, .5F);
    check(t.valid && t.transmitted, "refraction branch");
    near(t.weight.x, 1 / 2.25F, 1e-6F, "radiance eta squared");
    near(t.direction.z, -1, 1e-6F, "normal refraction");
    auto exit = sample_material_bsdf(m, {0, 0, 1}, {.8660254F, 0, .5F}, 1.5F, 1, 0, .2F, .4F, .99F);
    check(exit.valid && !exit.transmitted, "total internal reflection");
    near(exit.weight.x, 1, 1e-6F, "TIR energy");
    auto equal = sample_material_bsdf(glass(.7F), {0, 0, 1}, {0, 0, 1}, 1, 1, 0, .2F, .4F, .99F);
    check(equal.valid && equal.transmitted && equal.delta, "equal IOR straight interface");
}
void roughness_and_energy() {
    Random rng;
    for (float rough : {.04F, .2F, .6F, 1.F}) {
        auto m = glass(rough);
        double energy = 0;
        unsigned transmitted = 0;
        for (unsigned i = 0; i < 40000; ++i) {
            auto s = sample_material_bsdf(m, {0, 0, 1}, {0, 0, 1}, 1, 1.5F, rng.next(), rng.next(),
                                          rng.next(), rng.next());
            if (!s.valid)
                continue;
            check(std::isfinite(s.weight.x) && s.weight.x >= 0, "finite rough interface");
            auto e = evaluate_material_bsdf(m, {0, 0, 1}, {0, 0, 1}, s.direction, 1, 1.5F);
            near(e.pdf, s.pdf, 1e-4F * (1 + s.pdf), "sample/eval PDF");
            energy += s.weight.x * (s.transmitted ? 2.25 : 1);
            transmitted += s.transmitted;
        }
        check(transmitted > 10000, "rough glass transmits");
        check(energy / 40000 <= 1.015 && energy / 40000 > .45, "rough dielectric white furnace bound");
    }
    auto metal = glass();
    metal.transmission = 0;
    metal.metallic = 1;
    metal.baseColor = {.8F, .6F, .2F, 1};
    auto mirror = sample_material_bsdf(metal, {0, 0, 1}, {0, 0, 1}, 1, 1.5F, .5F, .3F, .4F, .6F);
    check(mirror.valid && mirror.delta, "smooth metallic mirror");
    near(mirror.weight.x, .8F, 1e-6F, "authored metallic reflectivity");
    double narrow = 0, wide = 0;
    auto spread = [&](float rough) {
        auto m = metal;
        m.roughness = rough;
        double sum = 0;
        for (int i = 0; i < 30000; ++i) {
            auto s = sample_material_bsdf(m, {0, 0, 1}, {0, 0, 1}, 1, 1.5F, rng.next(), rng.next(),
                                          rng.next(), rng.next());
            if (s.valid)
                sum += 1 - s.direction.z;
        }
        return sum / 30000;
    };
    narrow = spread(.1F);
    wide = spread(.7F);
    check(wide > narrow * 10, "roughness broadens reflections");
}
void slab_transport() {
    auto g = glass();
    auto glassSlab = slab(0, 1, g);
    VoxelMaterialDefinition light;
    light.shadingModel = MaterialShadingModel::Emissive;
    light.baseColor = {0, 0, 0, 1};
    light.emissive = {1, 1, 1};
    auto receiver = slab(3, 3.1F, light);
    std::array<PolygonRenderInstance, 2> instances{{{1, &glassSlab}, {2, &receiver}}};
    auto e = environment();
    SceneLighting scene({}, instances, e, 1);
    double mean = 0;
    for (unsigned i = 0; i < 150; ++i) {
        const auto value = scene.camera_transport({0, 0, -2}, {0, 0, 1}, 100, i);
        mean += value.sum().x;
        near(value.sum().x, value.transmission.x + value.indirectSpecular.x + value.directDiffuse.x, 1e-5F,
             "optical AOV sum");
    }
    near(float(mean / 150), .96F * .96F / (1 - .04F * .04F), .012F, "both slab Fresnel interfaces");
    glassSlab.materials[0].attenuationColor = {.5F, .25F, .75F};
    glassSlab.materials[0].attenuationDistanceMeters = 1;
    SceneLighting tinted({}, instances, e, 1);
    Float3 sum{};
    for (unsigned i = 0; i < 150; ++i) {
        const auto v = tinted.camera_transport({0, 0, -2}, {0, 0, 1}, 100, i).sum();
        sum.x += v.x;
        sum.y += v.y;
        sum.z += v.z;
    }
    const auto expected = [](float a) { return .96F * .96F * a / (1 - .04F * .04F * a * a); };
    near(sum.x / 150, expected(.5F), .009F, "Beer red");
    near(sum.y / 150, expected(.25F), .007F, "Beer green");
    near(sum.z / 150, expected(.75F), .01F, "Beer blue");
    // Zero opacity is coverage transparency, not refractive glass.
    auto coverage = g;
    coverage.transmission = 0;
    coverage.blendMode = MaterialBlendMode::Translucent;
    coverage.baseColor.w = 0;
    glassSlab.materials[0] = coverage;
    SceneLighting alpha({}, instances, e, 1);
    near(alpha.camera_transport({0, 0, -2}, {0, 0, 1}, 100, 2).sum().x, 1, 1e-5F,
         "zero opacity sees background geometry");
}
void voxel_glass() {
    VoxelObject object;
    object.set_voxel({0, 0, 0}, 1);
    object.set_voxel({0, 0, 1}, 1);
    std::vector<VoxelMaterialDefinition> materials(2);
    materials[1] = glass();
    materials[1].indexOfRefraction = 1;
    materials[1].attenuationColor = {.5F, .5F, .5F};
    materials[1].attenuationDistanceMeters = 2;
    VoxelReferenceInstance instance{1, &object, {}, materials};
    VoxelMaterialDefinition light;
    light.shadingModel = MaterialShadingModel::Emissive;
    light.baseColor = {0, 0, 0, 1};
    light.emissive = {1, 1, 1};
    auto receiver = slab(4, 4.1F, light);
    PolygonRenderInstance p{2, &receiver};
    auto e = environment();
    SceneLighting scene(std::span(&instance, 1), std::span(&p, 1), e, 1);
    near(scene.camera_transport({.5F, .5F, -2}, {0, 0, 1}, 100, 2).sum().x, .5F, .001F,
         "contiguous voxel medium skips internal faces");
    near(scene.camera_transport({.5F, .5F, 1}, {0, 0, 1}, 100, 2).sum().x, std::sqrt(.5F), .001F,
         "camera inside voxel medium attenuation");
}
void raster_glass_without_gi() {
    auto glassMesh = slab(0, 1, glass());
    VoxelMaterialDefinition light;
    light.shadingModel = MaterialShadingModel::Emissive;
    light.baseColor = {0, 0, 0, 1};
    light.emissive = {1, 1, 1};
    auto background = slab(3, 3.1F, light);
    std::array<PolygonRenderInstance, 2> instances{{{1, &glassMesh}, {2, &background}}};
    auto e = environment();
    e.pathTraceBounces = 0;
    e.globalIlluminationMode = GlobalIlluminationMode::Off;
    e.pathTraceSamples = 64;
    PolygonCamera camera;
    camera.position = {0, 0, -2};
    camera.target = {0, 0, 1};
    camera.nearPlane = .01F;
    PolygonRenderTarget target;
    target.resize(8, 8);
    target.enable_passes();
    (void)render_hybrid_reference({}, instances, camera, e, target, {}, 1, 1);
    const auto pixel = 4U * 8 + 4;
    check(target.objectId[pixel] == 1, "glass retains primary object identity");
    check(target.hdrColor[pixel].x > .7F, "glass automatically enables camera transport with GI off");
    near(target.passes.light_sum(pixel).x, target.hdrColor[pixel].x, 1e-5F, "raster glass AOV sum");
}
void transmitted_shadow() {
    auto g = glass();
    g.attenuationColor = {.5F, .5F, .5F};
    g.attenuationDistanceMeters = 1;
    auto mesh = slab(0, 1, g);
    PolygonRenderInstance p{1, &mesh};
    auto e = environment();
    SceneLight light;
    light.positionMeters = {0, 0, 3};
    light.intensity = 1;
    e.lights = {light};
    SceneLighting scene({}, std::span(&p, 1), e, 1);
    VoxelMaterialDefinition diffuse;
    diffuse.baseColor = {1, 1, 1, 1};
    diffuse.specular = 0;
    diffuse.roughness = 1;
    near(scene.direct({0, 0, -2}, {0, 0, 1}, {0, 0, 1}, diffuse, 4).diffuse.x, .96F * .96F * .5F / (25 * pi),
         .00001F, "straight glass shadow includes entry exit and absorption");
}
void nested_media() {
    auto g = glass();
    g.indexOfRefraction = 1;
    g.attenuationColor = {.5F, .5F, .5F};
    g.attenuationDistanceMeters = 1;
    auto outer = slab(0, 3, g);
    g.attenuationColor = {.25F, .25F, .25F};
    auto inner = slab(1, 2, g);
    VoxelMaterialDefinition light;
    light.shadingModel = MaterialShadingModel::Emissive;
    light.baseColor = {0, 0, 0, 1};
    light.emissive = {1, 1, 1};
    auto background = slab(4, 4.1F, light);
    std::array<PolygonRenderInstance, 3> instances{{{1, &outer}, {2, &inner}, {3, &background}}};
    auto e = environment();
    SceneLighting scene({}, instances, e, 1);
    near(scene.camera_transport({0, 0, -2}, {0, 0, 1}, 100, 3).sum().x, .5F * .5F * .25F, .0001F,
         "nested media restore outer absorption after inner exit");
}
void distant_voxel_surface() {
    VoxelObject object;
    object.set_voxel({0, 0, 542}, 1);
    std::vector<VoxelMaterialDefinition> materials(2);
    VoxelReferenceInstance instance{1, &object, {}, materials};
    OpticalScene scene(std::span(&instance, 1), {});
    const auto direction = Float3{0, 0, 1};
    const auto hit = scene.trace({.5F, .5F, -320.123F}, direction, 1000);
    check(bool(hit), "distant optical voxel hit");
    near(hit->position.z, 542, 1e-6F, "optical hit snaps to actual cell face");
    check(!scene.trace({.5F, .5F, hit->position.z - .0001F}, {0, 0, -1}, 1000),
          "offset ray leaves wall without self occlusion");
}

// A closed glass mesh whose triangles wind inward (vertex normals still point
// outward) makes the rasterizer keep the far hemisphere. That fragment's normal
// must not override the near surface hit by the re-traced camera path; doing so
// refracted edge rays into a permanent total internal reflection loop (black ring).
CookedPolygonAsset inward_sphere(float r, VoxelMaterialDefinition material) {
    CookedPolygonAsset a;
    a.materials = {material};
    a.materialBindings.resize(1);
    const int slices = 48, stacks = 24;
    for (int j = 0; j <= stacks; ++j)
        for (int i = 0; i <= slices; ++i) {
            const float phi = pi * float(j) / float(stacks), th = 2 * pi * float(i) / float(slices);
            const Float3 n{std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th)};
            PolygonVertex v;
            v.position = {n.x * r, n.y * r, n.z * r};
            v.normal = n;
            a.vertices.push_back(v);
        }
    for (int j = 0; j < stacks; ++j)
        for (int i = 0; i < slices; ++i) {
            const auto p = static_cast<unsigned>(j * (slices + 1) + i), q = p + static_cast<unsigned>(slices + 1);
            for (auto k : {p, q, p + 1, p + 1, q, q + 1})
                a.indices.push_back(k);
        }
    a.submeshes = {{"ball", 0, static_cast<unsigned>(a.indices.size()), 0}};
    a.bounds = {{-r, -r, -r}, {r, r, r}};
    return a;
}
void raster_sphere_edge_not_black() {
    auto ball = inward_sphere(1, glass());
    VoxelMaterialDefinition light;
    light.shadingModel = MaterialShadingModel::Emissive;
    light.baseColor = {0, 0, 0, 1};
    light.emissive = {1, 1, 1};
    auto background = slab(3, 3.1F, light);
    std::array<PolygonRenderInstance, 2> instances{{{1, &ball}, {2, &background}}};
    auto e = environment();
    e.pathTraceBounces = 0;
    e.globalIlluminationMode = GlobalIlluminationMode::Off;
    e.pathTraceSamples = 4;
    PolygonCamera camera;
    camera.position = {0, 0, -4};
    camera.target = {0, 0, 0};
    camera.nearPlane = .01F;
    camera.verticalFieldOfViewRadians = .6F;
    PolygonRenderTarget target;
    target.resize(32, 32);
    (void)render_hybrid_reference({}, instances, camera, e, target, {}, 1, 1);
    unsigned glassPixels = 0, black = 0;
    for (std::size_t k = 0; k < target.hdrColor.size(); ++k)
        if (target.objectId[k] == 1) {
            ++glassPixels;
            black += target.hdrColor[k].x < .05F ? 1U : 0U;
        }
    check(glassPixels > 50, "inward wound sphere is rasterized");
    if (black > glassPixels / 20) {
        std::cerr << black << " of " << glassPixels << " glass pixels black\n";
        throw std::runtime_error("glass sphere edge does not trap camera paths in TIR");
    }
}

// A double-sided, normal-mapped transmissive sheet seen from behind: the raster
// fragment faces away from the camera but *is* the traced first hit, so its
// mapped normal must be preserved (flipped toward the ray), not replaced by the
// flat traced normal. A normal from a different surface (distance mismatch)
// must still fall back to the traced normal.
CookedPolygonAsset sheet(float z, VoxelMaterialDefinition material) {
    CookedPolygonAsset a;
    a.materials = {material};
    a.materialBindings.resize(1);
    a.materialBindings[0].doubleSided = true;
    const float w = 10;
    for (Float3 p : {Float3{-w, -w, z}, Float3{-w, w, z}, Float3{w, w, z}, Float3{w, -w, z}}) {
        PolygonVertex v;
        v.position = p;
        v.normal = {0, 0, -1};
        a.vertices.push_back(v);
    }
    a.indices = {0, 1, 2, 0, 2, 3};
    a.submeshes = {{"sheet", 0, 6, 0}};
    a.bounds = {{-w, -w, z}, {w, w, z}};
    return a;
}
void backface_sheet_keeps_mapped_normal() {
    auto glassSheet = sheet(0, glass());
    VoxelMaterialDefinition light;
    light.shadingModel = MaterialShadingModel::Emissive;
    light.baseColor = {0, 0, 0, 1};
    light.emissive = {1, 1, 1};
    auto background = slab(-3.1F, -3, light);
    std::array<PolygonRenderInstance, 2> instances{{{1, &glassSheet}, {2, &background}}};
    auto e = environment();
    e.pathTraceBounces = 0;
    e.globalIlluminationMode = GlobalIlluminationMode::Off;
    e.pathTraceSamples = 64;
    SceneLighting scene({}, instances, e, 1);
    const auto material = glass();
    const Float3 eye{0, 0, 2}, dir{0, 0, -1};
    // Mapped normal tilted 45 degrees and facing away from the camera (sheet front is -z).
    const Float3 mapped{.70710678F, 0, -.70710678F};
    const float flat = scene.camera_transport(eye, dir, 100, 7, &material, {}).sum().x;
    const float kept = scene.camera_transport(eye, dir, 100, 7, &material, mapped, 2).sum().x;
    const float other = scene.camera_transport(eye, dir, 100, 7, &material, mapped, 1.5F).sum().x;
    check(std::abs(kept - flat) > .05F, "back-facing mapped normal of the first hit is preserved");
    near(other, flat, 1e-6F, "normal from a different surface falls back to the traced normal");
}

} // namespace
int main() {
    try {
        interfaces();
        roughness_and_energy();
        slab_transport();
        voxel_glass();
        distant_voxel_surface();
        nested_media();
        transmitted_shadow();
        raster_glass_without_gi();
        raster_sphere_edge_not_black();
        backface_sheet_keeps_mapped_normal();
        std::cout << "material transport tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
