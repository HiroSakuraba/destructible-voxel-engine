#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/audio/sample_map.hpp"

namespace dve::audio {

struct SampleMapRecipeZone {
    std::string assetPath;
    std::uint8_t keyLow{};
    std::uint8_t keyHigh{127};
    std::uint8_t velocityLow{1};
    std::uint8_t velocityHigh{127};
    std::uint8_t rootNote{60};
    float tuningCents{};
    float gain{1.0F};
    float pan{};
    SampleTrigger trigger{SampleTrigger::Attack};
    SampleLoopMode loopMode{SampleLoopMode::Disabled};
    std::uint32_t loopStartFrame{};
    std::uint32_t loopEndFrame{};
    std::uint32_t loopCrossfadeFrames{};
    std::uint8_t roundRobinGroup{}; // Recipe groups are zero-based and converted to runtime groups 1..16.
    std::uint8_t roundRobinIndex{};
    SamplePreloadPolicy preload{SamplePreloadPolicy::Resident};
    std::uint32_t preloadFrames{16384};
    std::uint8_t grainBudget{8}; // Preserved in the recipe; runtime v1.27 uses its overload-aware global budget.
};

struct SampleMapRecipe {
    std::string name{"Untitled Sample Map"};
    std::string author{"DVE"};
    std::vector<SampleMapRecipeZone> zones;
};

// Human-editable format:
// name=...
// author=...
// zone=path|keyLow|keyHigh|velLow|velHigh|root|tuneCents|gain|pan|trigger|loop|loopStart|loopEnd|crossfade|rrGroup|rrIndex|preload|preloadFrames|grainBudget
[[nodiscard]] std::optional<SampleMapRecipe> parse_sample_map_recipe(
    std::string_view text, std::string* error = nullptr);

// Resolves .dvesample dependencies on the control thread and emits the canonical callback-ready
// SynthSampleMap format used by the integrated synthesizer. Resident zones embed complete PCM;
// hybrid zones embed their attack prefix; streamed zones retain only source metadata/path.
[[nodiscard]] bool build_sample_map_from_recipe(const SampleMapRecipe& recipe,
                                                const std::filesystem::path& assetRoot,
                                                SynthSampleMap& output,
                                                std::string* error = nullptr);
[[nodiscard]] bool cook_sample_map_recipe(const std::filesystem::path& recipePath,
                                          const std::filesystem::path& outputPath,
                                          std::string* error = nullptr);

} // namespace dve::audio
