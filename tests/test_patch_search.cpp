// Tests for the Phase 6 (SYN-016) evolutionary patch search.
// Synthetic fitness only — no audio, no Worker A/B code.
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "dve/audio/patch_search.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

// Four continuous preset params, each normalized to [0,1].
// mutate_preset() moves all four continuously (cutoff log-domain,
// resonance linear, envelope times multiplicative), so a synthetic
// quadratic bowl over them is a fair convergence probe.
struct NormParams {
    double v[4];
};

static double norm_cutoff(float hz) {
    const double lo = std::log10(18.0), hi = std::log10(24000.0);
    return (std::log10(std::max<double>(hz, 18.0)) - lo) / (hi - lo);
}
static double norm_time(float s) {
    const double lo = std::log10(5e-4), hi = std::log10(30.0);
    return (std::log10(std::max<double>(s, 5e-4)) - lo) / (hi - lo);
}
static NormParams read_params(const SynthPreset& p) {
    NormParams n;
    n.v[0] = norm_cutoff(p.filter.cutoffHertz);
    n.v[1] = static_cast<double>(p.filter.resonance);
    n.v[2] = norm_time(p.ampEnvelope.attackSeconds);
    n.v[3] = norm_time(p.ampEnvelope.decaySeconds);
    return n;
}
static double rms_distance(const NormParams& a, const NormParams& b) {
    double s = 0.0;
    for (int i = 0; i < 4; ++i) {
        const double d = a.v[i] - b.v[i];
        s += d * d;
    }
    return std::sqrt(s / 4.0);
}

// Synthetic fitness: RMS distance of the 4 params to a fixed target.
static FitnessFn bowl_fitness(const NormParams& target) {
    return [target](const SynthPreset& p) { return rms_distance(read_params(p), target); };
}

static SearchConfig test_config() {
    SearchConfig cfg;
    cfg.populationSize = 32;
    cfg.maxGenerations = 60;
    cfg.seed = 0xC0FFEEULL;
    cfg.mutationRate = 0.5;
    cfg.crossoverRate = 0.8;
    cfg.eliteCount = 2;
    cfg.targetDistance = 0.05;
    return cfg;
}

int main() {
    const NormParams target{{0.35, 0.6, 0.4, 0.55}};
    const AudioFeatureVector dummyTarget{};

    // Test 1: convergence on a synthetic bowl.
    {
        SearchResult r = evolutionary_patch_search(dummyTarget, test_config(),
                                                    bowl_fitness(target));
        std::printf("converge: generations=%d best=%.6f converged=%d ranked=%zu\n",
                    r.generationsRun, r.bestDistance, (int)r.converged, r.ranked.size());
        CHECK(r.converged);
        CHECK(r.bestDistance < 0.05);
        CHECK(r.generationsRun <= 60);
        CHECK(r.ranked.size() == 32);
        // Ranked best-first.
        for (std::size_t i = 1; i < r.ranked.size(); ++i)
            CHECK(r.ranked[i - 1].distance <= r.ranked[i].distance);
    }

    // Test 2: determinism — same seed+config twice gives identical results.
    {
        const SearchConfig cfg = test_config();
        SearchResult r1 = evolutionary_patch_search(dummyTarget, cfg, bowl_fitness(target));
        SearchResult r2 = evolutionary_patch_search(dummyTarget, cfg, bowl_fitness(target));
        CHECK(r1.bestDistance == r2.bestDistance);
        CHECK(r1.generationsRun == r2.generationsRun);
        CHECK(r1.converged == r2.converged);
        CHECK(r1.ranked.size() == r2.ranked.size());
        const NormParams p1 = read_params(r1.ranked.front().preset);
        const NormParams p2 = read_params(r2.ranked.front().preset);
        for (int i = 0; i < 4; ++i) CHECK(p1.v[i] == p2.v[i]);
        std::printf("determinism: identical bestDistance=%.6f\n", r1.bestDistance);
    }

    // Test 3: different seeds diverge in trajectory but both converge.
    {
        SearchConfig cfgA = test_config();
        SearchConfig cfgB = test_config();
        cfgA.seed = 111ULL;
        cfgB.seed = 222ULL;
        SearchResult rA = evolutionary_patch_search(dummyTarget, cfgA, bowl_fitness(target));
        SearchResult rB = evolutionary_patch_search(dummyTarget, cfgB, bowl_fitness(target));
        CHECK(rA.converged && rB.converged);
        const NormParams pA = read_params(rA.ranked.front().preset);
        const NormParams pB = read_params(rB.ranked.front().preset);
        bool anyDiffer = false;
        for (int i = 0; i < 4; ++i)
            if (pA.v[i] != pB.v[i]) anyDiffer = true;
        CHECK(anyDiffer);  // overwhelmingly likely with different seeds
        std::printf("seeds: A best=%.6f (%d gen), B best=%.6f (%d gen)\n",
                    rA.bestDistance, rA.generationsRun, rB.bestDistance, rB.generationsRun);
    }

    // Test 4: config validation — invalid configs give an empty result, no crash.
    {
        SearchConfig bad = test_config();
        bad.populationSize = 0;
        SearchResult r = evolutionary_patch_search(dummyTarget, bad, bowl_fitness(target));
        CHECK(r.ranked.empty() && r.generationsRun == 0 && !r.converged);

        bad = test_config();
        bad.maxGenerations = -3;
        r = evolutionary_patch_search(dummyTarget, bad, bowl_fitness(target));
        CHECK(r.ranked.empty() && r.generationsRun == 0 && !r.converged);

        bad = test_config();
        bad.mutationRate = 1.5;
        r = evolutionary_patch_search(dummyTarget, bad, bowl_fitness(target));
        CHECK(r.ranked.empty());

        bad = test_config();
        bad.eliteCount = bad.populationSize;  // elite < population required
        r = evolutionary_patch_search(dummyTarget, bad, bowl_fitness(target));
        CHECK(r.ranked.empty());

        FitnessFn empty;
        r = evolutionary_patch_search(dummyTarget, test_config(), empty);
        CHECK(r.ranked.empty() && !r.converged);

        // search_config_valid agrees.
        CHECK(!search_config_valid(bad, bowl_fitness(target)));
        CHECK(search_config_valid(test_config(), bowl_fitness(target)));
        std::printf("validation: invalid configs -> empty results, no crash\n");
    }

    // Test 5: elitism — best distance never increases across generations.
    {
        SearchConfig cfg = test_config();
        cfg.targetDistance = 1e-12;  // effectively unreachable: run all generations
        cfg.maxGenerations = 10;
        std::vector<double> bests;
        SearchProgressFn prog = [&](int, double best) { bests.push_back(best); };
        SearchResult r = evolutionary_patch_search(dummyTarget, cfg, bowl_fitness(target), prog);
        CHECK(bests.size() == 11);  // generation 0..10
        for (std::size_t i = 1; i < bests.size(); ++i)
            CHECK(bests[i] <= bests[i - 1]);
        CHECK(!r.converged && r.generationsRun == 10);
        std::printf("elitism: best %.6f -> %.6f monotone over %zu reports\n",
                    bests.front(), bests.back(), bests.size());
    }

    // Test 6: early stop — trivially easy target converges immediately.
    {
        SearchConfig cfg = test_config();
        cfg.maxGenerations = 60;
        FitnessFn zero = [](const SynthPreset&) { return 0.0; };
        int calls = 0;
        SearchProgressFn prog = [&](int, double) { ++calls; };
        SearchResult r = evolutionary_patch_search(dummyTarget, cfg, zero, prog);
        CHECK(r.converged);
        CHECK(r.bestDistance == 0.0);
        CHECK(r.generationsRun < cfg.maxGenerations);
        CHECK(calls == r.generationsRun + 1);  // gen 0 + each run generation
        std::printf("early stop: converged in %d generations\n", r.generationsRun);
    }

    // Test 7: hostile fitness (NaN / +inf) never crashes; result stays finite-ranked.
    {
        SearchConfig cfg = test_config();
        cfg.maxGenerations = 5;
        FitnessFn hostile = [](const SynthPreset& p) {
            return (p.filter.cutoffHertz > 1000.0F)
                       ? std::numeric_limits<double>::quiet_NaN()
                       : std::numeric_limits<double>::infinity();
        };
        SearchResult r = evolutionary_patch_search(dummyTarget, cfg, hostile);
        CHECK(r.ranked.size() == 32);
        CHECK(std::isfinite(r.bestDistance));
        std::printf("hostile fitness: no crash, best=%.6f\n", r.bestDistance);
    }

    if (g_failures == 0) std::printf("ALL PATCH SEARCH TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
