// Caustics ground truth: light through glass and water (OpticalScene) and forward photons.
//
// Checked against closed-form answers:
//   - Fresnel and Snell (normal incidence 4% for glass, total internal reflection past 41.8 deg);
//   - a glass slab under an overhead sun lets through (1 - 0.04)^2, times Beer-Lambert absorption;
//   - a pool (water voxels on a floor, same object) under a 40-degree sun lights the floor with
//     cos(40) x (1 - F(40 deg));
//   - a smooth glass ball focuses: total transmitted power matches the integral of the two
//     Fresnel transmissions over its cross-section, and the floor at its focus is many times
//     brighter than the sun; a sun of finite size blurs the peak.
#include "dve/render/caustic_photons.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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

VoxelMaterialDefinition opaque() {
    VoxelMaterialDefinition m;
    m.name = "Floor";
    return m;
}
VoxelMaterialDefinition dielectric(float ior, float transmission = 1.0F) {
    VoxelMaterialDefinition m;
    m.name = "Glass";
    m.transmission = transmission;
    m.indexOfRefraction = ior;
    return m;
}

void test_fresnel_and_snell() {
    check(std::abs(dielectric_fresnel(1.0F, 1.0F, 1.5F) - 0.04F) < 1e-6F, "glass at normal incidence: 4%");
    check(std::abs(dielectric_fresnel(1.0F, 1.5F, 1.0F) - 0.04F) < 1e-6F, "the same leaving the glass");
    const float critical = std::asin(1.0F / 1.5F);
    check(dielectric_fresnel(std::cos(critical + 0.01F), 1.5F, 1.0F) == 1.0F, "total internal reflection past the critical angle");
    check(dielectric_fresnel(std::cos(critical - 0.05F), 1.5F, 1.0F) < 1.0F, "partial below it");
    check(dielectric_fresnel(0.0F, 1.0F, 1.5F) > 0.999F, "grazing: all reflected");
    const float theta = 0.5F;
    const auto t = refract_direction({std::sin(theta), -std::cos(theta), 0.0F}, {0.0F, 1.0F, 0.0F}, 1.0F / 1.33F);
    check(t && std::abs(std::sin(theta) - 1.33F * t->x) < 1e-5F && t->y < 0.0F, "Snell: sin i = n sin t");
    check(!refract_direction({std::sin(1.2F), -std::cos(1.2F), 0.0F}, {0.0F, 1.0F, 0.0F}, 1.5F), "TIR has no refracted ray");
    VoxelMaterialDefinition tinted = dielectric(1.5F);
    tinted.attenuationColor = {0.5F, 1.0F, 0.25F};
    tinted.attenuationDistanceMeters = 0.2F;
    const Float3 tr = medium_transmittance(tinted, 4.0F, 0.1F);   // 4 units x 0.1 m = 0.4 m = 2 attenuation distances
    check(std::abs(tr.x - 0.25F) < 1e-6F && tr.y == 1.0F && std::abs(tr.z - 0.0625F) < 1e-6F, "Beer-Lambert");
}

// Floor cells at y = 0 (top at 1), x, z in [-20, 20).
void fill_floor(VoxelObject& object, MaterialId id) {
    for (int x = -20; x < 20; ++x)
        for (int z = -20; z < 20; ++z) object.set_voxel({x, 0, z}, id);
}

double transmitted_slab(float absorbColor, float absorbDistance) {
    VoxelObject floor(1U), slab(2U);
    fill_floor(floor, 1);
    for (int x = -8; x < 8; ++x)
        for (int z = -8; z < 8; ++z)
            for (int y = 5; y < 10; ++y) slab.set_voxel({x, y, z}, 1);   // 5 units = 0.5 m thick
    std::array<VoxelMaterialDefinition, 2> floorMaterials{VoxelMaterialDefinition{}, opaque()};
    std::array<VoxelMaterialDefinition, 2> glassMaterials{VoxelMaterialDefinition{}, dielectric(1.5F)};
    glassMaterials[1].attenuationColor = {absorbColor, absorbColor, absorbColor};
    glassMaterials[1].attenuationDistanceMeters = absorbDistance;
    const std::vector<VoxelReferenceInstance> voxels{{1U, &floor, {}, floorMaterials, true, {1, 1, 1}},
                                                     {2U, &slab, {}, glassMaterials, true, {1, 1, 1}}};
    const OpticalScene scene(voxels, {});
    const CausticLightRegion region = caustic_light_region(scene, {0, 1, 0});
    CausticPhotonSettings settings;
    settings.photonCount = 40000;
    settings.includeReflections = false;
    const auto traced = trace_caustic_photons(scene, {}, region, settings);
    const CausticPhotonMap map(traced.photons, 1.0F);
    return map.irradiance({0.0F, 1.0F, 0.0F}, {0, 1, 0}, 2.0F).x;
}

void test_slab() {
    const double clear = transmitted_slab(1.0F, 0.0F);
    std::printf("glass slab: %.4f (expected %.4f)\n", clear, 0.96 * 0.96);
    check(std::abs(clear - 0.9216) < 0.01, "slab transmits (1 - 0.04)^2");
    // 0.5 m of glass whose colour halves every 0.25 m: x 0.25.
    const double tinted = transmitted_slab(0.5F, 0.25F);
    std::printf("absorbing slab: %.4f (expected %.4f)\n", tinted, 0.9216 * 0.25);
    check(std::abs(tinted - 0.9216 * 0.25) < 0.005, "Beer-Lambert inside the slab");
}

void test_pool() {
    // One object: floor cells (opaque) at y = 0 and water cells y = 1..4 over x, z in [-10, 10).
    VoxelObject pool(3U);
    fill_floor(pool, 1);
    for (int x = -10; x < 10; ++x)
        for (int z = -10; z < 10; ++z)
            for (int y = 1; y < 5; ++y) pool.set_voxel({x, y, z}, 2);
    std::array<VoxelMaterialDefinition, 3> materials{VoxelMaterialDefinition{}, opaque(), dielectric(1.33F)};
    const std::vector<VoxelReferenceInstance> voxels{{3U, &pool, {}, materials, true, {1, 1, 1}}};
    const OpticalScene scene(voxels, {});
    const float angle = 40.0F * 3.14159265F / 180.0F;
    const Float3 toward{std::sin(angle), std::cos(angle), 0.0F};
    const CausticLightRegion region = caustic_light_region(scene, toward);
    CausticPhotonSettings settings;
    settings.photonCount = 60000;
    settings.includeReflections = false;
    const auto traced = trace_caustic_photons(scene, {toward, 0.0F}, region, settings);
    const CausticPhotonMap map(traced.photons, 1.0F);
    const double e = map.irradiance({0.0F, 1.0F, 0.0F}, {0, 1, 0}, 2.0F).x;
    const double expected = std::cos(angle) * (1.0 - dielectric_fresnel(std::cos(angle), 1.0F, 1.33F));
    std::printf("pool floor under a 40-degree sun: %.4f (expected %.4f)\n", e, expected);
    check(std::abs(e - expected) < 0.012, "flat water: floor gets cos(i) x (1 - F(i))");
}

// A UV sphere (smooth normals) of radius r: 96 x 48 segments.
CookedPolygonAsset sphere(float r, const VoxelMaterialDefinition& material) {
    CookedPolygonAsset a;
    a.objectId = 50;
    a.materials.push_back(material);
    a.materialBindings.resize(1);
    const int slices = 96, stacks = 48;
    for (int j = 0; j <= stacks; ++j)
        for (int i = 0; i <= slices; ++i) {
            const float phi = 3.14159265F * static_cast<float>(j) / stacks, theta = 6.2831853F * static_cast<float>(i) / slices;
            const Float3 n{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            a.vertices.push_back({{n.x * r, n.y * r, n.z * r}, n, {1, 0, 0, 1}, {0, 0}, {1, 1, 1, 1}, {0, 0}});
        }
    for (int j = 0; j < stacks; ++j)
        for (int i = 0; i < slices; ++i) {
            const std::uint32_t p = static_cast<std::uint32_t>(j * (slices + 1) + i), q = p + static_cast<std::uint32_t>(slices + 1);
            a.indices.insert(a.indices.end(), {p, q, p + 1, p + 1, q, q + 1});
        }
    a.submeshes.push_back({"ball", 0, static_cast<std::uint32_t>(a.indices.size()), 0});
    a.bounds = {{-r, -r, -r}, {r, r, r}};
    return a;
}

void test_lens() {
    // Glass ball, radius 2, centred 3 units above the floor top: its paraxial focus is at
    // n R / (2 (n - 1)) = 3 from the centre, on the floor.
    VoxelObject floor(1U);
    fill_floor(floor, 1);
    std::array<VoxelMaterialDefinition, 2> floorMaterials{VoxelMaterialDefinition{}, opaque()};
    const std::vector<VoxelReferenceInstance> voxels{{1U, &floor, {}, floorMaterials, true, {1, 1, 1}}};
    const CookedPolygonAsset ball = sphere(2.0F, dielectric(1.5F));
    const std::vector<OpticalPolygonInstance> polygons{{50U, &ball, {{0.0F, 4.0F, 0.0F}, {}}, true}};
    const OpticalScene scene(voxels, polygons);
    const CausticLightRegion region = caustic_light_region(scene, {0, 1, 0});
    check(std::abs(region.area() - 16.0F) < 0.5F, "light region = the ball's footprint (" + std::to_string(region.area()) + ")");
    CausticPhotonSettings settings;
    settings.photonCount = 250000;
    settings.includeReflections = false;
    const auto traced = trace_caustic_photons(scene, {}, region, settings);
    const CausticPhotonMap map(traced.photons, 0.5F);
    // Total power landing: integral over the cross-section of (1 - F(i))^2, sin i = r / R.
    double expected = 0.0;
    const int n = 4000;
    for (int k = 0; k < n; ++k) {
        const double r = (k + 0.5) / n * 2.0;
        const double cosI = std::sqrt(std::max(0.0, 1.0 - (r / 2.0) * (r / 2.0)));
        const double t = 1.0 - dielectric_fresnel(static_cast<float>(cosI), 1.0F, 1.5F);
        expected += t * t * 2.0 * kPi * r * (2.0 / n);
    }
    const double total = map.total_power().x;
    std::printf("ball: transmitted power %.4f (expected %.4f, cross-section %.4f)\n", total, expected, kPi * 4.0);
    check(std::abs(total - expected) / expected < 0.01, "energy through the ball matches the Fresnel integral");
    const double peak = map.irradiance({0.0F, 1.0F, 0.0F}, {0, 1, 0}, 0.1F).x;
    const double ring = map.irradiance({1.5F, 1.0F, 0.0F}, {0, 1, 0}, 0.1F).x;
    std::printf("ball focus: %.1f x the sun at the centre, %.3f at 1.5 units out\n", peak, ring);
    check(peak > 20.0, "focused: the floor under the ball is many times brighter than the sun");
    check(ring < 0.2, "outside the focus little light arrives");

    // A finite sun (radius 0.05 rad) blurs the focus: lower peak, same total power.
    CausticPhotonSettings blurred = settings;
    const auto tracedSun = trace_caustic_photons(scene, {{0, 1, 0}, 0.05F}, region, blurred);
    const CausticPhotonMap sunMap(tracedSun.photons, 0.5F);
    const double sunPeak = sunMap.irradiance({0.0F, 1.0F, 0.0F}, {0, 1, 0}, 0.1F).x;
    std::printf("with a 0.05 rad sun: peak %.1f, power %.4f\n", sunPeak, sunMap.total_power().x);
    check(sunPeak < 0.7 * peak, "a finite sun blurs the focus");
    check(std::abs(sunMap.total_power().x - total) / total < 0.02, "and keeps the energy");

    // Full ground truth: reflections included (stochastic Fresnel): less light reaches the floor
    // under the ball (some is reflected away), more scatters elsewhere.
    CausticPhotonSettings full = settings;
    full.includeReflections = true;
    const auto tracedFull = trace_caustic_photons(scene, {}, region, full);
    std::printf("with reflections: %zu photons stored, %llu lost\n", tracedFull.photons.size(),
                static_cast<unsigned long long>(tracedFull.lost));
    const CausticPhotonMap fullMap(tracedFull.photons, 0.5F);
    check(std::abs(fullMap.irradiance({0.0F, 1.0F, 0.0F}, {0, 1, 0}, 0.1F).x - peak) / peak < 0.15,
          "the focus is the same light either way (reflections land elsewhere)");
}
}  // namespace

int main() {
    test_fresnel_and_snell();
    test_slab();
    test_pool();
    test_lens();
    if (g_failures == 0) std::printf("caustic photon tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
