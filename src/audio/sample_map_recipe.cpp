#include "dve/audio/sample_map_recipe.hpp"

#include "dve/audio/audio_asset.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace dve::audio {
namespace {

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1U));
}

std::vector<std::string> split(std::string_view value, char separator) {
    std::vector<std::string> result;
    std::size_t begin = 0U;
    while (begin <= value.size()) {
        const std::size_t end = value.find(separator, begin);
        result.emplace_back(value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin));
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return result;
}

template <class T>
bool parse_integer(std::string_view text, T& output) {
    unsigned long long value{};
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [position, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || position != end || value > static_cast<unsigned long long>(std::numeric_limits<T>::max()))
        return false;
    output = static_cast<T>(value);
    return true;
}

bool parse_float(std::string_view text, float& output) {
    std::string copy(text);
    char* end{};
    output = std::strtof(copy.c_str(), &end);
    return end == copy.c_str() + copy.size() && std::isfinite(output);
}

std::optional<SampleTrigger> parse_trigger(std::string_view text) {
    if (text == "attack") return SampleTrigger::Attack;
    if (text == "release") return SampleTrigger::Release;
    return std::nullopt;
}

std::optional<SampleLoopMode> parse_loop(std::string_view text) {
    if (text == "one_shot" || text == "oneshot" || text == "disabled") return SampleLoopMode::Disabled;
    if (text == "forward") return SampleLoopMode::Forward;
    return std::nullopt;
}

std::optional<SamplePreloadPolicy> parse_preload(std::string_view text) {
    if (text == "resident") return SamplePreloadPolicy::Resident;
    if (text == "streamed") return SamplePreloadPolicy::Streamed;
    if (text == "hybrid") return SamplePreloadPolicy::Hybrid;
    return std::nullopt;
}

int preload_strength(SamplePreloadPolicy policy) noexcept {
    switch (policy) {
    case SamplePreloadPolicy::Resident: return 2;
    case SamplePreloadPolicy::Hybrid: return 1;
    case SamplePreloadPolicy::Streamed: return 0;
    }
    return 0;
}

bool ranges_overlap(const SampleMapRecipeZone& a, const SampleMapRecipeZone& b) noexcept {
    return a.trigger == b.trigger && a.keyLow <= b.keyHigh && b.keyLow <= a.keyHigh &&
           a.velocityLow <= b.velocityHigh && b.velocityLow <= a.velocityHigh;
}

struct SourcePlan {
    std::string path;
    SamplePreloadPolicy preload{SamplePreloadPolicy::Streamed};
    std::uint32_t preloadFrames{};
    DecodedAudioAsset asset;
};

} // namespace

std::optional<SampleMapRecipe> parse_sample_map_recipe(std::string_view text, std::string* error) {
    SampleMapRecipe result;
    std::istringstream input{std::string(text)};
    std::string line;
    std::size_t lineNumber{};
    while (std::getline(input, line)) {
        ++lineNumber;
        const std::string cleaned = trim(line);
        if (cleaned.empty() || cleaned.front() == '#') continue;
        const std::size_t equals = cleaned.find('=');
        if (equals == std::string::npos) {
            fail(error, "recipe line " + std::to_string(lineNumber) + " has no '='");
            return std::nullopt;
        }
        const std::string key = trim(std::string_view(cleaned).substr(0U, equals));
        const std::string value = trim(std::string_view(cleaned).substr(equals + 1U));
        if (key == "name") {
            result.name = value;
        } else if (key == "author") {
            result.author = value;
        } else if (key == "zone") {
            const auto fields = split(value, '|');
            if (fields.size() != 19U) {
                fail(error, "recipe line " + std::to_string(lineNumber) + " must contain 19 zone fields");
                return std::nullopt;
            }
            SampleMapRecipeZone zone;
            zone.assetPath = trim(fields[0]);
            bool parsed = !zone.assetPath.empty() && parse_integer(fields[1], zone.keyLow) &&
                parse_integer(fields[2], zone.keyHigh) && parse_integer(fields[3], zone.velocityLow) &&
                parse_integer(fields[4], zone.velocityHigh) && parse_integer(fields[5], zone.rootNote) &&
                parse_float(fields[6], zone.tuningCents) && parse_float(fields[7], zone.gain) &&
                parse_float(fields[8], zone.pan);
            const auto trigger = parse_trigger(fields[9]);
            const auto loop = parse_loop(fields[10]);
            parsed = parsed && trigger.has_value() && loop.has_value() &&
                parse_integer(fields[11], zone.loopStartFrame) && parse_integer(fields[12], zone.loopEndFrame) &&
                parse_integer(fields[13], zone.loopCrossfadeFrames) &&
                parse_integer(fields[14], zone.roundRobinGroup) && parse_integer(fields[15], zone.roundRobinIndex);
            const auto preload = parse_preload(fields[16]);
            parsed = parsed && preload.has_value() && parse_integer(fields[17], zone.preloadFrames) &&
                parse_integer(fields[18], zone.grainBudget);
            if (!parsed) {
                fail(error, "recipe line " + std::to_string(lineNumber) + " contains an invalid field");
                return std::nullopt;
            }
            zone.trigger = *trigger;
            zone.loopMode = *loop;
            zone.preload = *preload;
            if (zone.keyLow > zone.keyHigh || zone.velocityLow == 0U || zone.velocityLow > zone.velocityHigh ||
                zone.keyHigh > 127U || zone.velocityHigh > 127U || zone.rootNote > 127U ||
                zone.roundRobinGroup >= 16U || zone.gain < 0.0F || zone.gain > 8.0F ||
                zone.pan < -1.0F || zone.pan > 1.0F || zone.tuningCents < -2400.0F ||
                zone.tuningCents > 2400.0F || zone.preloadFrames == 0U || zone.grainBudget == 0U ||
                zone.grainBudget > 32U) {
                fail(error, "recipe line " + std::to_string(lineNumber) + " is outside supported ranges");
                return std::nullopt;
            }
            if (zone.loopMode == SampleLoopMode::Forward) {
                if (zone.loopStartFrame >= zone.loopEndFrame ||
                    zone.loopCrossfadeFrames * 2U > zone.loopEndFrame - zone.loopStartFrame) {
                    fail(error, "recipe line " + std::to_string(lineNumber) + " has invalid loop bounds");
                    return std::nullopt;
                }
            } else if (zone.loopCrossfadeFrames != 0U) {
                fail(error, "recipe line " + std::to_string(lineNumber) + " gives a one-shot zone a loop crossfade");
                return std::nullopt;
            }
            result.zones.push_back(std::move(zone));
        } else {
            fail(error, "recipe line " + std::to_string(lineNumber) + " has unknown key '" + key + "'");
            return std::nullopt;
        }
    }
    if (result.name.empty() || result.name.size() > 128U || result.author.size() > 128U ||
        result.zones.empty() || result.zones.size() > kSampleMapMaxZones) {
        fail(error, "sample-map recipe name, author, or zone count is invalid");
        return std::nullopt;
    }

    // The integrated v1.27 Synthesizer owns one attack and one release zone per voice. Reject
    // overlapping groups that the standalone prototype would have layered, rather than silently
    // dropping layers during the merge.
    for (std::size_t i = 0; i < result.zones.size(); ++i) {
        for (std::size_t j = i + 1U; j < result.zones.size(); ++j) {
            if (result.zones[i].roundRobinGroup != result.zones[j].roundRobinGroup &&
                ranges_overlap(result.zones[i], result.zones[j])) {
                fail(error, "overlapping zones in different round-robin groups require layered playback, "
                            "which is deferred to v1.28");
                return std::nullopt;
            }
        }
    }
    return result;
}

bool build_sample_map_from_recipe(const SampleMapRecipe& recipe, const std::filesystem::path& assetRoot,
                                  SynthSampleMap& output, std::string* error) {
    if (recipe.zones.empty() || recipe.zones.size() > kSampleMapMaxZones)
        return fail(error, "sample-map recipe zone count is out of range");

    std::vector<SourcePlan> plans;
    std::unordered_map<std::string, std::size_t> sourceByPath;
    std::vector<std::size_t> zoneSource(recipe.zones.size());
    for (std::size_t i = 0; i < recipe.zones.size(); ++i) {
        const auto& zone = recipe.zones[i];
        const auto [it, inserted] = sourceByPath.emplace(zone.assetPath, plans.size());
        if (inserted) {
            if (plans.size() >= kSampleMapMaxSources)
                return fail(error, "recipe references more sources than the fixed runtime map supports");
            SourcePlan plan;
            plan.path = zone.assetPath;
            plan.preload = zone.preload;
            plan.preloadFrames = zone.preloadFrames;
            std::string assetError;
            auto asset = read_cooked_audio_asset(assetRoot / zone.assetPath, &assetError);
            if (!asset) return fail(error, "unable to load '" + zone.assetPath + "': " + assetError);
            if (asset->metadata.channels != 1U)
                return fail(error, "integrated sample maps currently require mono .dvesample sources: " + zone.assetPath);
            if (asset->metadata.frameCount < 2U || asset->metadata.frameCount > std::numeric_limits<std::uint32_t>::max())
                return fail(error, "sample source frame count is outside the runtime map range: " + zone.assetPath);
            plan.asset = std::move(*asset);
            plans.push_back(std::move(plan));
            zoneSource[i] = plans.size() - 1U;
        } else {
            zoneSource[i] = it->second;
            auto& plan = plans[it->second];
            if (preload_strength(zone.preload) > preload_strength(plan.preload)) plan.preload = zone.preload;
            plan.preloadFrames = std::max(plan.preloadFrames, zone.preloadFrames);
        }
    }

    SynthSampleMap map;
    map.name = recipe.name;
    map.sourceCount = static_cast<std::uint32_t>(plans.size());
    map.zoneCount = static_cast<std::uint32_t>(recipe.zones.size());
    std::uint32_t residentCursor{};
    for (std::size_t i = 0; i < plans.size(); ++i) {
        auto& target = map.sources[i];
        const auto& plan = plans[i];
        const std::uint32_t totalFrames = static_cast<std::uint32_t>(plan.asset.metadata.frameCount);
        std::uint32_t residentFrames{};
        if (plan.preload == SamplePreloadPolicy::Resident) residentFrames = totalFrames;
        else if (plan.preload == SamplePreloadPolicy::Hybrid) residentFrames = std::min(plan.preloadFrames, totalFrames);
        if (residentFrames > kSampleMapMaxResidentFrames - residentCursor)
            return fail(error, "resident/hybrid prefixes exceed the fixed callback-safe PCM pool");
        target.name = plan.asset.metadata.name.empty() ? std::filesystem::path(plan.path).filename().string()
                                                       : plan.asset.metadata.name;
        target.streamPath = plan.path;
        target.preload = plan.preload;
        target.sampleRate = plan.asset.metadata.sampleRate;
        target.totalFrameCount = totalFrames;
        target.residentSourceStartFrame = 0U;
        target.residentFrameOffset = residentCursor;
        target.residentFrameCount = residentFrames;
        target.contentHash = plan.asset.metadata.contentHash;
        std::copy_n(plan.asset.samples.begin(), residentFrames, map.residentSamples.begin() + residentCursor);
        residentCursor += residentFrames;
    }
    map.residentFrameCount = residentCursor;

    std::array<std::uint8_t, 16> groupCounts{};
    std::array<std::uint8_t, 16> groupMaximumIndex{};
    for (const auto& zone : recipe.zones) {
        const std::size_t group = zone.roundRobinGroup;
        ++groupCounts[group];
        groupMaximumIndex[group] = std::max(groupMaximumIndex[group], zone.roundRobinIndex);
    }
    for (std::size_t group = 0; group < groupCounts.size(); ++group) {
        if (groupCounts[group] != 0U && groupMaximumIndex[group] + 1U != groupCounts[group])
            return fail(error, "round-robin indices must be contiguous from zero within each recipe group");
    }

    for (std::size_t i = 0; i < recipe.zones.size(); ++i) {
        const auto& sourcePlan = plans[zoneSource[i]];
        const auto& source = map.sources[zoneSource[i]];
        const auto& input = recipe.zones[i];
        auto& zone = map.zones[i];
        zone.enabled = true;
        zone.trigger = input.trigger;
        zone.sourceIndex = static_cast<std::uint8_t>(zoneSource[i]);
        zone.keyLow = input.keyLow;
        zone.keyHigh = input.keyHigh;
        zone.velocityLow = input.velocityLow;
        zone.velocityHigh = input.velocityHigh;
        zone.rootNote = input.rootNote;
        zone.roundRobinGroup = static_cast<std::uint8_t>(input.roundRobinGroup + 1U);
        zone.roundRobinIndex = input.roundRobinIndex;
        zone.roundRobinCount = groupCounts[input.roundRobinGroup];
        zone.tuningCents = input.tuningCents;
        zone.gain = input.gain;
        zone.pan = input.pan;
        zone.startFrame = 0U;
        zone.endFrame = source.totalFrameCount;
        zone.loopMode = input.loopMode;
        zone.loopStartFrame = input.loopStartFrame;
        zone.loopEndFrame = input.loopEndFrame;
        zone.loopCrossfadeFrames = input.loopCrossfadeFrames;
        zone.reverse = false;
        if (zone.loopMode == SampleLoopMode::Forward && zone.loopEndFrame > source.totalFrameCount)
            return fail(error, "loop exceeds source length for '" + sourcePlan.path + "'");
    }
    map.contentHash = map.calculate_content_hash();
    if (!map.validate(error)) return false;
    output = std::move(map);
    return true;
}

bool cook_sample_map_recipe(const std::filesystem::path& recipePath,
                            const std::filesystem::path& outputPath,
                            std::string* error) {
    std::ifstream input(recipePath);
    if (!input) return fail(error, "unable to open sample-map recipe");
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input && !input.eof()) return fail(error, "unable to read sample-map recipe");
    auto recipe = parse_sample_map_recipe(buffer.str(), error);
    if (!recipe) return false;
    SynthSampleMap map;
    if (!build_sample_map_from_recipe(*recipe, recipePath.parent_path(), map, error)) return false;
    return save_sample_map(outputPath, map, error);
}

} // namespace dve::audio
