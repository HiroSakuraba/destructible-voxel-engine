// The subsurface random walk (subsurface_random_walk.hpp) as ground truth:
//   1. against exact transport results: ballistic transmission e^-tau, energy conservation,
//      Chandrasekhar's semi-infinite albedo 1 - H(1) sqrt(1 - alpha), and conservation with a
//      refractive boundary;
//   2. the same walk inside a voxel slab and a polygon box as inside the analytic slab;
//   3. then, as measurements: how well Chiang et al.'s mapping reproduces the artist's albedo,
//      and how far part 1's Burley transmittance is from the random walk through a slab.
#include "dve/polygon_bvh.hpp"
#include "dve/render/optical_scene.hpp"
#include "dve/render/subsurface_random_walk.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

struct Tally {
    std::uint64_t walks{}, up{}, down{}, absorbed{}, blocked{}, dropped{}, specular{};
    double R() const { return static_cast<double>(up) / static_cast<double>(walks); }
    double T() const { return static_cast<double>(down) / static_cast<double>(walks); }
    // One standard error of a fraction p over the walks.
    double sigma(double p) const { return std::sqrt(std::max(p * (1.0 - p), 1e-12) / static_cast<double>(walks)); }
};

// Light arriving on the top face (normal +y) of `domain` at `entry`, along `incoming`.
Tally run(const SubsurfaceDomain& domain, const SubsurfaceMedium& medium, Float3 entry, Float3 incoming, std::uint64_t walks,
          std::uint64_t seed, bool diffuseIncidence = false) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    auto random = [&] { return u(rng); };
    Tally tally;
    for (std::uint64_t i = 0; i < walks; ++i) {
        ++tally.walks;
        Float3 in = incoming;
        if (diffuseIncidence) {   // cosine-weighted from the upper hemisphere
            const double r = std::sqrt(random()), phi = 6.283185307179586 * random();
            in = {static_cast<float>(r * std::cos(phi)), -static_cast<float>(std::sqrt(1.0 - r * r)),
                  static_cast<float>(r * std::sin(phi))};
        }
        const auto walk = enter_and_walk_subsurface(domain, medium, entry, {0, 1, 0}, in, random);
        if (!walk) { ++tally.specular; continue; }
        switch (walk->end) {
            case RandomWalkResult::End::Exited: (walk->normal.y > 0.5F ? tally.up : tally.down) += 1; break;
            case RandomWalkResult::End::Absorbed: ++tally.absorbed; break;
            case RandomWalkResult::End::Blocked: ++tally.blocked; break;
            case RandomWalkResult::End::Dropped: ++tally.dropped; break;
        }
    }
    return tally;
}

// Chandrasekhar's H function for isotropic scattering with albedo w, by iterating
// 1/H(mu) = sqrt(1 - w) + (w/2) Int_0^1 mu' H(mu') / (mu + mu') dmu' on a fine midpoint grid.
double chandrasekhar_h1(double w) {
    const int n = 4000;
    std::vector<double> mu(n), h(n, 1.0);
    for (int i = 0; i < n; ++i) mu[i] = (i + 0.5) / n;
    for (int iteration = 0; iteration < 400; ++iteration) {
        std::vector<double> next(n);
        for (int i = 0; i < n; ++i) {
            double integral = 0.0;
            for (int j = 0; j < n; ++j) integral += mu[j] * h[j] / (mu[i] + mu[j]);
            next[i] = 1.0 / (std::sqrt(1.0 - w) + 0.5 * w * integral / n);
        }
        h.swap(next);
    }
    double integral = 0.0;
    for (int j = 0; j < n; ++j) integral += mu[j] * h[j] / (1.0 + mu[j]);
    return 1.0 / (std::sqrt(1.0 - w) + 0.5 * w * integral / n);
}

void test_exact() {
    const Float3 down{0, -1, 0};
    {   // Absorption only: what comes out the bottom never scattered, e^-tau.
        SubsurfaceMedium m; m.extinction = 1.0F; m.albedo = 0.0F;
        const Tally t = run(SlabSubsurfaceDomain(-1.0F, 0.0F), m, {0, 0, 0}, down, 400000, 1);
        std::printf("ballistic: T %.4f  exact e^-1 = %.4f  (+-%.4f)\n", t.T(), std::exp(-1.0), t.sigma(t.T()));
        check(std::abs(t.T() - std::exp(-1.0)) < 4.0 * t.sigma(t.T()), "ballistic transmission is e^-tau");
        check(t.up == 0U, "nothing reflected without scattering");
    }
    {   // No absorption: everything comes out one side or the other.
        SubsurfaceMedium m; m.extinction = 1.0F; m.albedo = 1.0F;
        const Tally t = run(SlabSubsurfaceDomain(-2.0F, 0.0F), m, {0, 0, 0}, down, 200000, 2);
        std::printf("conservative slab tau 2: R %.4f + T %.4f = %.6f\n", t.R(), t.T(), t.R() + t.T());
        check(t.up + t.down == t.walks, "no absorption: R + T = 1");
    }
    for (const float w : {0.5F, 0.9F, 0.99F}) {   // Semi-infinite, normal incidence: 1 - H(1) sqrt(1 - w).
        SubsurfaceMedium m; m.extinction = 1.0F; m.albedo = w;
        const Tally t = run(SlabSubsurfaceDomain(-std::numeric_limits<float>::infinity(), 0.0F), m, {0, 0, 0}, down, 400000,
                            3 + static_cast<std::uint64_t>(w * 100));
        const double exact = 1.0 - chandrasekhar_h1(w) * std::sqrt(1.0 - w);
        std::printf("semi-infinite albedo %.2f: R %.4f  exact %.4f  (+-%.4f)\n", w, t.R(), exact, t.sigma(t.R()));
        check(std::abs(t.R() - exact) < 4.0 * t.sigma(t.R()), "semi-infinite reflectance matches Chandrasekhar");
    }
    {   // A refractive boundary (eta 1.4) keeps light in by internal reflection; still no loss.
        SubsurfaceMedium m; m.extinction = 1.0F; m.albedo = 1.0F; m.boundaryIndex = 1.4F;
        const Tally t = run(SlabSubsurfaceDomain(-2.0F, 0.0F), m, {0, 0, 0}, down, 100000, 7);
        const double fresnel = dielectric_fresnel(1.0F, 1.0F, 1.4F);
        std::printf("eta 1.4 slab: specular %.4f (Fresnel %.4f)  R %.4f  T %.4f  lost %llu\n",
                    static_cast<double>(t.specular) / t.walks, fresnel, t.R(), t.T(),
                    static_cast<unsigned long long>(t.absorbed + t.blocked + t.dropped));
        check(std::abs(static_cast<double>(t.specular) / t.walks - fresnel) < 4.0 * t.sigma(fresnel), "entry Fresnel");
        check(t.specular + t.up + t.down == t.walks, "refractive boundary: nothing lost");
    }
}

CookedPolygonAsset box(Float3 lo, Float3 hi) {
    CookedPolygonAsset a;
    a.objectId = 60;
    VoxelMaterialDefinition m;
    a.materials.push_back(m);
    a.materialBindings.resize(1);
    const auto face = [&](Float3 n, Float3 p0, Float3 p1, Float3 p2, Float3 p3) {
        const auto base = static_cast<std::uint32_t>(a.vertices.size());
        for (const Float3 p : {p0, p1, p2, p3}) a.vertices.push_back({p, n, {1, 0, 0, 1}, {0, 0}, {1, 1, 1, 1}, {0, 0}});
        a.indices.insert(a.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    face({0, -1, 0}, {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z});
    face({0, 1, 0}, {lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z});
    face({-1, 0, 0}, {lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z});
    face({1, 0, 0}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z});
    face({0, 0, -1}, {lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, lo.y, lo.z});
    face({0, 0, 1}, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z});
    a.submeshes.push_back({"box", 0, static_cast<std::uint32_t>(a.indices.size()), 0});
    a.bounds = {lo, hi};
    return a;
}

void test_domains() {
    // A 4-unit slab, 0 <= y <= 4, as an analytic slab, voxels and a mesh; light enters the top.
    SubsurfaceMedium m; m.extinction = 1.0F; m.albedo = 0.95F; m.boundaryIndex = 1.3F;
    const Float3 entry{0.37F, 4.0F, -0.21F}, incoming{0.5F, -0.866F, 0.0F};
    const Tally analytic = run(SlabSubsurfaceDomain(0.0F, 4.0F), m, entry, incoming, 200000, 11);
    VoxelObject slab(1U);
    for (int x = -40; x < 40; ++x)
        for (int z = -40; z < 40; ++z)
            for (int y = 0; y < 4; ++y) slab.set_voxel({x, y, z}, 1);
    std::array<VoxelMaterialDefinition, 2> materials{};
    const VoxelReferenceInstance instance{1U, &slab, {}, materials, true, {1, 1, 1}};
    const Tally voxels = run(VoxelSubsurfaceDomain(instance, 1), m, entry, incoming, 200000, 12);
    const CookedPolygonAsset mesh = box({-40, 0, -40}, {40, 4, 40});
    PolygonBvh bvh;
    check(bvh.build(mesh), "box BVH");
    const Tally polygons = run(PolygonSubsurfaceDomain(bvh, mesh, {}), m, entry, incoming, 200000, 13);
    std::printf("domains, R / T: slab %.4f / %.4f   voxels %.4f / %.4f   mesh %.4f / %.4f  (+-%.4f)\n", analytic.R(),
                analytic.T(), voxels.R(), voxels.T(), polygons.R(), polygons.T(), analytic.sigma(analytic.R()));
    const auto same = [&](double a, double b, const Tally& t) { return std::abs(a - b) < 5.0 * std::sqrt(2.0) * t.sigma(a); };
    check(same(analytic.R(), voxels.R(), voxels) && same(analytic.T(), voxels.T(), voxels), "voxel slab = analytic slab");
    check(same(analytic.R(), polygons.R(), polygons) && same(analytic.T(), polygons.T(), polygons), "box mesh = analytic slab");
    check(voxels.blocked == 0U && polygons.dropped == 0U, "no walks lost to the domains");

    // Leaving a voxel medium into another solid absorbs (it is not a boundary light crosses).
    for (int x = -40; x < 40; ++x)
        for (int z = -40; z < 40; ++z) slab.set_voxel({x, -1, z}, 2);
    const Tally capped = run(VoxelSubsurfaceDomain(instance, 1), m, entry, incoming, 20000, 14);
    check(capped.blocked > 0U && capped.down == 0U, "a stone floor under the voxel slab: blocked, not transmitted");
}

double burley_slab_prediction(float albedo, float thicknessOverD) { return albedo * burley_transmittance(thicknessOverD, 1.0F); }

void test_measurements() {
    // Chiang's mapping: does the medium for albedo A reflect A from a half-space?
    std::printf("\nChiang mapping, half-space reflectance for artist albedo A (index-matched):\n");
    std::printf("   A     alpha    normal incidence   diffuse incidence\n");
    for (const float a : {0.2F, 0.5F, 0.8F, 0.95F}) {
        const SubsurfaceMedium m = subsurface_medium_from_albedo(a, 0.1F, 0.1F);   // d = 1 unit
        const SlabSubsurfaceDomain half(-std::numeric_limits<float>::infinity(), 0.0F);
        const Tally normal = run(half, m, {0, 0, 0}, {0, -1, 0}, 200000, 21);
        const Tally diffuse = run(half, m, {0, 0, 0}, {0, -1, 0}, 200000, 22, true);
        std::printf("  %.2f   %.4f   %.4f             %.4f\n", a, m.albedo, normal.R(), diffuse.R());
        check(std::abs(diffuse.R() - a) < 0.1, "Chiang's mapping gives roughly the artist's albedo");
    }

    // Burley's reflectance profile (what part 2 will blur front-lit light with) against the
    // walk: light arriving diffusely at one point of a half-space, where it comes out.
    std::printf("\nreflectance profile, share of the light coming out between r0 and r1 (r in units of d):\n");
    for (const float a : {0.3F, 0.8F, 0.95F}) {
        const SubsurfaceMedium m = subsurface_medium_from_albedo(a, 0.1F, 0.1F);
        const SlabSubsurfaceDomain half(-std::numeric_limits<float>::infinity(), 0.0F);
        const double edges[] = {0, 0.25, 0.5, 1, 2, 4, 8, 16, 32};
        std::array<double, 8> bins{};
        std::mt19937_64 rng(40);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        auto random = [&] { return u(rng); };
        const int n = 200000;
        for (int i = 0; i < n; ++i) {
            const double r0 = std::sqrt(random()), phi = 6.283185307179586 * random();
            const Float3 in{static_cast<float>(r0 * std::cos(phi)), -static_cast<float>(std::sqrt(1.0 - r0 * r0)),
                            static_cast<float>(r0 * std::sin(phi))};
            const auto walk = enter_and_walk_subsurface(half, m, {0, 0, 0}, {0, 1, 0}, in, random);
            if (!walk || walk->end != RandomWalkResult::End::Exited) continue;
            const double r = std::hypot(walk->position.x, walk->position.z);
            for (std::size_t b = 0; b < bins.size(); ++b)
                if (r >= edges[b] && r < edges[b + 1]) bins[b] += 1.0 / n;
        }
        const auto burley = [&](double r) { return a * (1.0 - 0.25 * std::exp(-r) - 0.75 * std::exp(-r / 3.0)); };
        std::printf("  A %.2f   r0-r1:  ", a);
        for (std::size_t b = 0; b < bins.size(); ++b) std::printf("%5.2f-%-5.2f ", edges[b], edges[b + 1]);
        std::printf("\n           walk:   ");
        for (const double v : bins) std::printf("%.4f      ", v);
        std::printf("\n           Burley: ");
        double worst = 0.0;
        for (std::size_t b = 0; b < bins.size(); ++b) {
            const double v = burley(edges[b + 1]) - burley(edges[b]);
            std::printf("%.4f      ", v);
            if (edges[b + 1] <= 8.0) worst = std::max(worst, std::abs(v - bins[b]));
        }
        std::printf("\n");
        check(worst < 0.03, "Burley's profile follows the walk within 0.03 of the light per ring, out to 8 d");
    }

    // Part 1's transmittance against the walk through a slab. The walk's light that crossed
    // without scattering is counted apart: it is a straight beam (the sun seen through the
    // slab), not a glow, and part 1 draws Subsurface materials opaque.
    std::printf("\nslab transmission, sun overhead: part 1 (Burley, A T(t/d)) vs the walk's scattered light (index-matched):\n");
    std::printf("   A     t/d    Burley    walk      (+ straight through)\n");
    for (const float a : {0.5F, 0.8F, 0.95F})
        for (const float t : {0.5F, 1.0F, 2.0F, 4.0F, 8.0F}) {
            const SubsurfaceMedium m = subsurface_medium_from_albedo(a, 0.1F, 0.1F);   // d = 1 unit
            const Tally overhead = run(SlabSubsurfaceDomain(-t, 0.0F), m, {0, 0, 0}, {0, -1, 0}, 60000, 31);
            const double straight = std::exp(-static_cast<double>(m.extinction) * t);
            std::printf("  %.2f   %.1f    %.4f    %.4f    %.4f\n", a, t, burley_slab_prediction(a, t), overhead.T() - straight,
                        straight);
        }
}

} // namespace

int main() {
    test_exact();
    test_domains();
    test_measurements();
    if (g_failures == 0) std::printf("dve_subsurface_random_walk_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
