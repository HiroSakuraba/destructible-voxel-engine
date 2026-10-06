// Phase 6 (SYN-016): evolutionary patch search. See patch_search.hpp.
//
// The EA operates on whole SynthPreset individuals and reuses the Phase 3
// (SYN-013) genetics operators: mutate_preset() for mutation and
// breed_presets() for gene-group crossover (uniform mask or single-point
// split over the 8 gene groups). Elites are copied verbatim; everything
// else is a tournament-selected, crossed-over, mutated offspring.
//
// Determinism: one std::mt19937_64 seeded from cfg.seed drives every random
// choice (tournament picks, crossover coin flips and masks, operator seeds).
// Sorting is stable_sort on distance, so ties resolve deterministically.

#include "dve/audio/patch_search.hpp"

#include <algorithm>
#include <cmath>
#include <random>

#include "dve/audio/patch_genetics.hpp"

namespace dve::audio {
namespace {

// Eight gene groups (see GeneGroup in patch_genetics.hpp).
constexpr int kGeneGroupCount = 8;
constexpr double kWorstDistance = 1e9;

// Never let a misbehaving fitness crash or poison the search.
double safe_fitness(const FitnessFn& fitness, const SynthPreset& preset) noexcept {
    double d = kWorstDistance;
    try {
        d = fitness(preset);
    } catch (...) {
        return kWorstDistance;
    }
    return std::isfinite(d) ? d : kWorstDistance;
}

// Best of `tournamentSize` random individuals (by index into ranked pop).
const RankedPatch& tournament_pick(const std::vector<RankedPatch>& pop,
                                   std::mt19937_64& rng) {
    constexpr int kTournamentSize = 3;
    std::uniform_int_distribution<std::size_t> pick(0, pop.size() - 1);
    std::size_t best = pick(rng);
    for (int i = 1; i < kTournamentSize; ++i) {
        const std::size_t cand = pick(rng);
        if (pop[cand].distance < pop[best].distance) best = cand;
    }
    return pop[best];
}

// Crossover via breed_presets(): uniform gene-group mask or a single-point
// split over the 8 gene groups, chosen 50/50 per event.
SynthPreset crossover_presets(const SynthPreset& a, const SynthPreset& b,
                              std::mt19937_64& rng) {
    std::uint8_t groupsFromB = 0;
    if (std::uniform_int_distribution<int>(0, 1)(rng) == 0) {
        // Uniform: each gene group independently from B with p = 0.5.
        groupsFromB = static_cast<std::uint8_t>(
            std::uniform_int_distribution<unsigned>(0, 0xFFU)(rng));
    } else {
        // Single-point: pick a split in [1, 7); groups at/after it come from B.
        const int split = std::uniform_int_distribution<int>(1, kGeneGroupCount - 1)(rng);
        groupsFromB = static_cast<std::uint8_t>((0xFFU << split) & 0xFFU);
    }
    return breed_presets(a, b, groupsFromB, rng());
}

// Seeded random starting population: heavily mutated builtin presets.
// Falls back to the unmutated builtin if a mutation ever fails validate()
// (defensive; mutate_preset clamps everything by contract).
std::vector<RankedPatch> init_population(const SearchConfig& cfg,
                                         const FitnessFn& fitness,
                                         std::mt19937_64& rng) {
    const std::vector<SynthPreset> base = SynthPreset::builtin_presets();
    std::vector<RankedPatch> pop;
    pop.reserve(static_cast<std::size_t>(cfg.populationSize));
    for (int i = 0; i < cfg.populationSize; ++i) {
        SynthPreset p = mutate_preset(base[static_cast<std::size_t>(i) % base.size()],
                                      1.0F, rng(), 0U);
        if (!p.validate()) p = base[static_cast<std::size_t>(i) % base.size()];
        const double d = safe_fitness(fitness, p);
        pop.push_back(RankedPatch{std::move(p), d});
    }
    return pop;
}

void sort_best_first(std::vector<RankedPatch>& pop) {
    std::stable_sort(pop.begin(), pop.end(),
                     [](const RankedPatch& a, const RankedPatch& b) {
                         return a.distance < b.distance;
                     });
}

}  // namespace

bool search_config_valid(const SearchConfig& cfg, const FitnessFn& fitness) noexcept {
    if (!fitness) return false;
    if (cfg.populationSize < 4) return false;
    if (cfg.maxGenerations < 1) return false;
    if (!std::isfinite(cfg.mutationRate) || cfg.mutationRate < 0.0 ||
        cfg.mutationRate > 1.0)
        return false;
    if (!std::isfinite(cfg.crossoverRate) || cfg.crossoverRate < 0.0 ||
        cfg.crossoverRate > 1.0)
        return false;
    if (cfg.eliteCount < 0 || cfg.eliteCount >= cfg.populationSize) return false;
    return true;
}

SearchResult evolutionary_patch_search(const AudioFeatureVector& /*target*/,
                                       const SearchConfig& cfg,
                                       FitnessFn fitness,
                                       SearchProgressFn progress) {
    SearchResult result;
    if (!search_config_valid(cfg, fitness)) return result;

    std::mt19937_64 rng(cfg.seed);
    std::vector<RankedPatch> pop = init_population(cfg, fitness, rng);
    sort_best_first(pop);

    auto report = [&](int generation) {
        if (progress) progress(generation, pop.front().distance);
    };
    report(0);
    if (pop.front().distance <= cfg.targetDistance) {
        result.ranked = std::move(pop);
        result.bestDistance = result.ranked.front().distance;
        result.converged = true;
        return result;
    }

    std::bernoulli_distribution doCrossover(
        std::clamp(cfg.crossoverRate, 0.0, 1.0));
    const auto popSize = static_cast<std::size_t>(cfg.populationSize);
    const auto elites = static_cast<std::size_t>(cfg.eliteCount);

    for (int gen = 1; gen <= cfg.maxGenerations; ++gen) {
        std::vector<RankedPatch> next;
        next.reserve(popSize);
        // Elitism: verbatim survivors with their known distances.
        for (std::size_t e = 0; e < elites; ++e) next.push_back(pop[e]);
        // Offspring: tournament-select, cross over, mutate.
        while (next.size() < popSize) {
            const RankedPatch& parentA = tournament_pick(pop, rng);
            const RankedPatch& parentB = tournament_pick(pop, rng);
            SynthPreset child = doCrossover(rng)
                                    ? crossover_presets(parentA.preset, parentB.preset, rng)
                                    : parentA.preset;
            if (cfg.mutationRate > 0.0) {
                child = mutate_preset(child, static_cast<float>(cfg.mutationRate),
                                      rng(), 0U);
                if (!child.validate()) child = parentA.preset;  // defensive
            }
            const double d = safe_fitness(fitness, child);
            next.push_back(RankedPatch{std::move(child), d});
        }
        pop = std::move(next);
        sort_best_first(pop);
        result.generationsRun = gen;
        report(gen);
        if (pop.front().distance <= cfg.targetDistance) {
            result.converged = true;
            break;
        }
    }

    result.ranked = std::move(pop);
    result.bestDistance = result.ranked.front().distance;
    return result;
}

}  // namespace dve::audio
