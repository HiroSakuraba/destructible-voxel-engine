// Phase 6 (SYN-016): evolutionary patch search for audio-to-synth search.
//
// Evolves a population of SynthPresets toward a target AudioFeatureVector.
// The search is deliberately decoupled from how fitness is computed: it
// takes a FitnessFn callable (distance; lower is better), so Worker B's
// offline render evaluator plugs in at merge/integration time with zero
// coupling. Tests use synthetic fitness functions.
//
// Genetic operators reuse the Phase 3 (SYN-013) patch genetics machinery:
//   - mutation  -> mutate_preset()  (musical, range-safe, seeded)
//   - crossover -> breed_presets()  (gene-group uniform / single-point)
//
// All randomness comes from a single seeded std::mt19937_64: the same
// (target, config, fitness) always produces the same SearchResult.
// No wall-clock, no rand(), no thread-locals.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "dve/audio/audio_features.hpp"
#include "dve/audio/synthesizer.hpp"

namespace dve::audio {

// Fitness: distance of a candidate preset from the target. Lower is better;
// must be deterministic for a given preset. Non-finite returns are treated
// as +infinity (worst) so a misbehaving fitness can never crash the search.
using FitnessFn = std::function<double(const SynthPreset&)>;

// Optional per-generation progress hook. Called from inside
// evolutionary_patch_search() after each generation is evaluated, with the
// 0-based generation index and the current best distance. Never called after
// the search returns. May be empty.
using SearchProgressFn = std::function<void(int generation, double bestDistance)>;

struct SearchConfig {
    int populationSize = 24;
    int maxGenerations = 40;
    std::uint64_t seed = 0x1234;
    double mutationRate = 0.25;   // per-offspring mutation intensity in [0, 1]
    double crossoverRate = 0.7;   // per-offspring crossover probability in [0, 1]
    int eliteCount = 2;           // verbatim survivors per generation (< populationSize)
    double targetDistance = 0.05; // early-stop threshold (bestDistance <= target)
    FeatureWeights weights;       // recorded for Worker B/D plumbing; fitness owns the metric
};

// A candidate preset with its fitness distance (lower is better).
struct RankedPatch {
    SynthPreset preset;
    double distance = 1e9;
};

struct SearchResult {
    std::vector<RankedPatch> ranked;  // best-first; empty when the config is invalid
    int generationsRun = 0;
    double bestDistance = 1e9;
    bool converged = false;  // bestDistance <= targetDistance was reached
};

// True when the config is usable. Invalid configs make
// evolutionary_patch_search() return an empty SearchResult instead of
// crashing: populationSize >= 4, maxGenerations >= 1, both rates in [0, 1],
// 0 <= eliteCount < populationSize, and a non-empty fitness.
[[nodiscard]] bool search_config_valid(const SearchConfig& cfg, const FitnessFn& fitness) noexcept;

// Run the evolutionary search. Deterministic for a fixed (target, cfg,
// fitness): same seed always yields the same ranked population.
[[nodiscard]] SearchResult evolutionary_patch_search(const AudioFeatureVector& target,
                                                     const SearchConfig& cfg,
                                                     FitnessFn fitness,
                                                     SearchProgressFn progress = {});

}  // namespace dve::audio
