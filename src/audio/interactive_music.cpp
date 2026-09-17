#include "dve/audio/interactive_music.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <unordered_set>

namespace dve::audio {
namespace {

constexpr std::array<char, 8> kMagic{'D','V','E','I','A','U','D','1'};
constexpr std::uint32_t kCurrentVersion = 2U;
constexpr std::size_t kMaximumStringBytes = 1U << 20U;
constexpr std::size_t kMaximumStates = 4096U;
constexpr std::size_t kMaximumStems = 65536U;
constexpr std::size_t kMaximumMarkers = 1U << 20U;
constexpr std::size_t kMaximumTransitions = 65536U;
constexpr std::size_t kMaximumConditions = 65536U;
constexpr std::size_t kMaximumContainers = 65536U;
constexpr std::size_t kMaximumVariants = 1U << 20U;

float parameter_value(const std::vector<InteractiveMusicParameter>& parameters,
                      std::string_view name) noexcept {
    const auto it = std::find_if(parameters.begin(), parameters.end(),
        [name](const InteractiveMusicParameter& parameter) { return parameter.name == name; });
    return it == parameters.end() ? 0.0F : it->value;
}

bool condition_passes(const InteractiveMusicCondition& condition,
                      const std::vector<InteractiveMusicParameter>& parameters) noexcept {
    const float value = parameter_value(parameters, condition.parameter);
    switch (condition.comparison) {
        case InteractiveMusicComparison::Less: return value < condition.value;
        case InteractiveMusicComparison::LessEqual: return value <= condition.value;
        case InteractiveMusicComparison::Greater: return value > condition.value;
        case InteractiveMusicComparison::GreaterEqual: return value >= condition.value;
        case InteractiveMusicComparison::Equal: return std::abs(value - condition.value) <= 1.0e-5F;
        case InteractiveMusicComparison::NotEqual: return std::abs(value - condition.value) > 1.0e-5F;
    }
    return false;
}

const InteractiveMusicState* find_state(const InteractiveMusicGraph& graph,
                                        std::string_view name) noexcept {
    const auto it = std::find_if(graph.states.begin(), graph.states.end(),
        [name](const InteractiveMusicState& state) { return state.name == name; });
    return it == graph.states.end() ? nullptr : &*it;
}

std::uint64_t quantized_frame(const InteractiveMusicGraph& graph,
                              const InteractiveMusicState* state,
                              InteractiveMusicQuantization quantization,
                              std::uint64_t currentFrame,
                              std::uint32_t sampleRate) noexcept {
    if (quantization == InteractiveMusicQuantization::Immediate || sampleRate == 0U ||
        graph.beatsPerMinute <= 0.0) return currentFrame;
    const double framesPerBeat = static_cast<double>(sampleRate) * 60.0 / graph.beatsPerMinute;
    if (quantization == InteractiveMusicQuantization::Marker && state != nullptr &&
        !state->markerFrames.empty()) {
        const auto it = std::lower_bound(state->markerFrames.begin(), state->markerFrames.end(), currentFrame);
        if (it != state->markerFrames.end()) return *it;
    }
    const double quantum = quantization == InteractiveMusicQuantization::Bar
        ? framesPerBeat * static_cast<double>(graph.beatsPerBar) : framesPerBeat;
    if (quantum <= 1.0) return currentFrame;
    const double resolved = std::ceil(static_cast<double>(currentFrame) / quantum) * quantum;
    return resolved >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(resolved);
}

template<class T>
bool write_value(std::ostream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(stream);
}

template<class T>
bool read_value(std::istream& stream, T& value) {
    stream.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(stream);
}

bool write_string(std::ostream& stream, const std::string& value) {
    if (value.size() > kMaximumStringBytes) return false;
    const auto size = static_cast<std::uint32_t>(value.size());
    return write_value(stream, size) &&
        (size == 0U || static_cast<bool>(stream.write(value.data(), static_cast<std::streamsize>(size))));
}

bool read_string(std::istream& stream, std::string& value) {
    std::uint32_t size{};
    if (!read_value(stream, size) || size > kMaximumStringBytes) return false;
    value.resize(size);
    if (size != 0U) stream.read(value.data(), static_cast<std::streamsize>(size));
    return static_cast<bool>(stream);
}

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
}

template<class T>
void hash_value(std::uint64_t& hash, const T& value) noexcept {
    hash_bytes(hash, &value, sizeof(T));
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    const auto size = static_cast<std::uint64_t>(value.size());
    hash_value(hash, size);
    hash_bytes(hash, value.data(), value.size());
}

bool valid_quantization(InteractiveMusicQuantization value) noexcept {
    return static_cast<unsigned>(value) <= static_cast<unsigned>(InteractiveMusicQuantization::Marker);
}

bool valid_comparison(InteractiveMusicComparison value) noexcept {
    return static_cast<unsigned>(value) <= static_cast<unsigned>(InteractiveMusicComparison::NotEqual);
}

bool valid_container_mode(InteractiveMusicContainerMode value) noexcept {
    return static_cast<unsigned>(value) <= static_cast<unsigned>(InteractiveMusicContainerMode::Shuffle);
}

const InteractiveMusicContainer* find_container(const InteractiveMusicGraph& graph,
                                                 std::string_view name) noexcept {
    for (const auto& state : graph.states) {
        const auto it = std::find_if(state.containers.begin(), state.containers.end(),
            [name](const InteractiveMusicContainer& container) { return container.name == name; });
        if (it != state.containers.end()) return &*it;
    }
    return nullptr;
}

} // namespace

std::uint64_t interactive_music_graph_hash(const InteractiveMusicGraph& graph) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    hash_value(hash, kCurrentVersion);
    hash_string(hash, graph.name);
    hash_value(hash, graph.beatsPerMinute);
    hash_value(hash, graph.beatsPerBar);
    for (const auto& state : graph.states) {
        hash_string(hash, state.name);
        hash_string(hash, state.entryStinger);
        hash_string(hash, state.exitStinger);
        for (const auto marker : state.markerFrames) hash_value(hash, marker);
        for (const auto& stem : state.stems) {
            hash_string(hash, stem.name); hash_string(hash, stem.assetPath); hash_string(hash, stem.parameter);
            hash_value(hash, stem.minimumParameter); hash_value(hash, stem.maximumParameter);
            hash_value(hash, stem.gain); hash_value(hash, stem.loop);
        }
        for (const auto& container : state.containers) {
            hash_string(hash, container.name); hash_value(hash, container.mode);
            hash_value(hash, container.noImmediateRepeat);
            for (const auto& variant : container.variants) {
                hash_string(hash, variant.name); hash_string(hash, variant.assetPath);
                hash_value(hash, variant.weight); hash_value(hash, variant.gain);
                hash_value(hash, variant.cooldownFrames);
            }
        }
    }
    for (const auto& transition : graph.transitions) {
        hash_string(hash, transition.from); hash_string(hash, transition.to);
        hash_value(hash, transition.quantization); hash_value(hash, transition.crossfadeBeats);
        hash_string(hash, transition.stinger);
        for (const auto& condition : transition.conditions) {
            hash_string(hash, condition.parameter); hash_value(hash, condition.comparison);
            hash_value(hash, condition.value);
        }
    }
    return hash;
}

bool validate_interactive_music_graph(const InteractiveMusicGraph& graph, std::string* error) {
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (graph.version == 0U || graph.version > kCurrentVersion ||
        !std::isfinite(graph.beatsPerMinute) || graph.beatsPerMinute < 20.0 || graph.beatsPerMinute > 400.0 ||
        graph.beatsPerBar == 0U || graph.beatsPerBar > 32U || graph.states.empty() ||
        graph.states.size() > kMaximumStates || graph.transitions.size() > kMaximumTransitions)
        return fail("interactive music tempo, meter, version, or state list is invalid");
    std::unordered_set<std::string> names;
    std::unordered_set<std::string> containerNames;
    std::size_t totalStems{};
    std::size_t totalMarkers{};
    std::size_t totalContainers{};
    std::size_t totalVariants{};
    for (const auto& state : graph.states) {
        if (state.name.empty() || !names.insert(state.name).second)
            return fail("interactive music state names must be unique");
        totalStems += state.stems.size();
        totalMarkers += state.markerFrames.size();
        totalContainers += state.containers.size();
        if (totalStems > kMaximumStems || totalMarkers > kMaximumMarkers || totalContainers > kMaximumContainers)
            return fail("interactive music graph exceeds limits");
        if (!std::is_sorted(state.markerFrames.begin(), state.markerFrames.end()) ||
            std::adjacent_find(state.markerFrames.begin(), state.markerFrames.end()) != state.markerFrames.end())
            return fail("interactive music markers must be strictly sorted");
        for (const auto& stem : state.stems) {
            if (stem.name.empty() || stem.assetPath.empty() || !std::isfinite(stem.gain) || stem.gain < 0.0F ||
                stem.gain > 8.0F || !std::isfinite(stem.minimumParameter) || !std::isfinite(stem.maximumParameter) ||
                stem.maximumParameter < stem.minimumParameter)
                return fail("interactive music stem is invalid");
        }
        for (const auto& container : state.containers) {
            if (container.name.empty() || !containerNames.insert(container.name).second ||
                !valid_container_mode(container.mode) || container.variants.empty())
                return fail("interactive music container is invalid or duplicated");
            totalVariants += container.variants.size();
            if (totalVariants > kMaximumVariants) return fail("interactive music variants exceed limits");
            double weightSum{};
            std::unordered_set<std::string> variantNames;
            for (const auto& variant : container.variants) {
                if (variant.name.empty() || variant.assetPath.empty() ||
                    !variantNames.insert(variant.name).second || !std::isfinite(variant.weight) ||
                    variant.weight < 0.0F || !std::isfinite(variant.gain) || variant.gain < 0.0F ||
                    variant.gain > 8.0F)
                    return fail("interactive music variant is invalid");
                weightSum += variant.weight;
            }
            if (container.mode == InteractiveMusicContainerMode::RandomWeighted && weightSum <= 0.0)
                return fail("weighted interactive music container has no positive weight");
        }
    }
    std::size_t totalConditions{};
    for (const auto& transition : graph.transitions) {
        if (!valid_quantization(transition.quantization) || find_state(graph, transition.from) == nullptr ||
            find_state(graph, transition.to) == nullptr || transition.crossfadeBeats > 64U)
            return fail("interactive music transition is invalid");
        totalConditions += transition.conditions.size();
        if (totalConditions > kMaximumConditions) return fail("interactive music conditions exceed limits");
        for (const auto& condition : transition.conditions) {
            if (condition.parameter.empty() || !std::isfinite(condition.value) ||
                !valid_comparison(condition.comparison))
                return fail("interactive music transition condition is invalid");
        }
    }
    return true;
}

bool write_interactive_music_graph(const std::filesystem::path& path,
                                   const InteractiveMusicGraph& input,
                                   std::string* error) {
    InteractiveMusicGraph graph = input;
    graph.version = kCurrentVersion;
    if (!validate_interactive_music_graph(graph, error)) return false;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) { if (error) *error = "unable to create interactive audio graph"; return false; }
    stream.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    const std::uint64_t contentHash = interactive_music_graph_hash(graph);
    const auto stateCount = static_cast<std::uint32_t>(graph.states.size());
    const auto transitionCount = static_cast<std::uint32_t>(graph.transitions.size());
    if (!write_value(stream, kCurrentVersion) || !write_value(stream, contentHash) ||
        !write_string(stream, graph.name) || !write_value(stream, graph.beatsPerMinute) ||
        !write_value(stream, graph.beatsPerBar) || !write_value(stream, stateCount) ||
        !write_value(stream, transitionCount)) return false;
    for (const auto& state : graph.states) {
        const auto stemCount = static_cast<std::uint32_t>(state.stems.size());
        const auto markerCount = static_cast<std::uint32_t>(state.markerFrames.size());
        const auto containerCount = static_cast<std::uint32_t>(state.containers.size());
        if (!write_string(stream, state.name) || !write_string(stream, state.entryStinger) ||
            !write_string(stream, state.exitStinger) || !write_value(stream, stemCount) ||
            !write_value(stream, markerCount) || !write_value(stream, containerCount)) return false;
        for (const auto marker : state.markerFrames) if (!write_value(stream, marker)) return false;
        for (const auto& stem : state.stems) {
            if (!write_string(stream, stem.name) || !write_string(stream, stem.assetPath) ||
                !write_string(stream, stem.parameter) || !write_value(stream, stem.minimumParameter) ||
                !write_value(stream, stem.maximumParameter) || !write_value(stream, stem.gain) ||
                !write_value(stream, stem.loop)) return false;
        }
        for (const auto& container : state.containers) {
            const auto mode = static_cast<std::uint8_t>(container.mode);
            const auto variantCount = static_cast<std::uint32_t>(container.variants.size());
            if (!write_string(stream, container.name) || !write_value(stream, mode) ||
                !write_value(stream, container.noImmediateRepeat) || !write_value(stream, variantCount)) return false;
            for (const auto& variant : container.variants) {
                if (!write_string(stream, variant.name) || !write_string(stream, variant.assetPath) ||
                    !write_value(stream, variant.weight) || !write_value(stream, variant.gain) ||
                    !write_value(stream, variant.cooldownFrames)) return false;
            }
        }
    }
    for (const auto& transition : graph.transitions) {
        const auto quantization = static_cast<std::uint8_t>(transition.quantization);
        const auto conditionCount = static_cast<std::uint32_t>(transition.conditions.size());
        if (!write_string(stream, transition.from) || !write_string(stream, transition.to) ||
            !write_value(stream, quantization) || !write_value(stream, transition.crossfadeBeats) ||
            !write_string(stream, transition.stinger) || !write_value(stream, conditionCount)) return false;
        for (const auto& condition : transition.conditions) {
            const auto comparison = static_cast<std::uint8_t>(condition.comparison);
            if (!write_string(stream, condition.parameter) || !write_value(stream, comparison) ||
                !write_value(stream, condition.value)) return false;
        }
    }
    if (!stream) { if (error) *error = "failed to write interactive audio graph"; return false; }
    return true;
}

std::optional<InteractiveMusicGraph> read_interactive_music_graph(const std::filesystem::path& path,
                                                                  std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { if (error) *error = "unable to open interactive audio graph"; return std::nullopt; }
    std::array<char, 8> magic{};
    std::uint32_t version{};
    std::uint64_t expectedHash{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!stream || magic != kMagic || !read_value(stream, version) || version < 1U || version > kCurrentVersion ||
        !read_value(stream, expectedHash)) {
        if (error) *error = "unsupported interactive audio graph format";
        return std::nullopt;
    }
    InteractiveMusicGraph graph;
    graph.version = kCurrentVersion;
    std::uint32_t stateCount{};
    std::uint32_t transitionCount{};
    if (!read_string(stream, graph.name) || !read_value(stream, graph.beatsPerMinute) ||
        !read_value(stream, graph.beatsPerBar) || !read_value(stream, stateCount) ||
        !read_value(stream, transitionCount) || stateCount == 0U || stateCount > kMaximumStates ||
        transitionCount > kMaximumTransitions) {
        if (error) *error = "invalid interactive audio graph header";
        return std::nullopt;
    }
    graph.states.resize(stateCount);
    std::size_t totalStems{};
    std::size_t totalMarkers{};
    std::size_t totalContainers{};
    std::size_t totalVariants{};
    for (auto& state : graph.states) {
        std::uint32_t stemCount{};
        std::uint32_t markerCount{};
        std::uint32_t containerCount{};
        if (!read_string(stream, state.name) || !read_string(stream, state.entryStinger) ||
            !read_string(stream, state.exitStinger) || !read_value(stream, stemCount) ||
            !read_value(stream, markerCount) || (version >= 2U && !read_value(stream, containerCount))) {
            if (error) *error = "truncated interactive audio state";
            return std::nullopt;
        }
        totalStems += stemCount; totalMarkers += markerCount; totalContainers += containerCount;
        if (totalStems > kMaximumStems || totalMarkers > kMaximumMarkers || totalContainers > kMaximumContainers) {
            if (error) *error = "interactive audio graph exceeds limits";
            return std::nullopt;
        }
        state.markerFrames.resize(markerCount);
        for (auto& marker : state.markerFrames) {
            if (!read_value(stream, marker)) {
                if (error) *error = "truncated interactive audio marker";
                return std::nullopt;
            }
        }
        state.stems.resize(stemCount);
        for (auto& stem : state.stems) {
            if (!read_string(stream, stem.name) || !read_string(stream, stem.assetPath) ||
                !read_string(stream, stem.parameter) || !read_value(stream, stem.minimumParameter) ||
                !read_value(stream, stem.maximumParameter) || !read_value(stream, stem.gain) ||
                !read_value(stream, stem.loop)) {
                if (error) *error = "truncated interactive audio stem";
                return std::nullopt;
            }
        }
        if (version >= 2U) {
            state.containers.resize(containerCount);
            for (auto& container : state.containers) {
                std::uint8_t mode{};
                std::uint32_t variantCount{};
                if (!read_string(stream, container.name) || !read_value(stream, mode) ||
                    !read_value(stream, container.noImmediateRepeat) || !read_value(stream, variantCount)) {
                    if (error) *error = "truncated interactive audio container";
                    return std::nullopt;
                }
                container.mode = static_cast<InteractiveMusicContainerMode>(mode);
                totalVariants += variantCount;
                if (totalVariants > kMaximumVariants) {
                    if (error) *error = "interactive audio variants exceed limits";
                    return std::nullopt;
                }
                container.variants.resize(variantCount);
                for (auto& variant : container.variants) {
                    if (!read_string(stream, variant.name) || !read_string(stream, variant.assetPath) ||
                        !read_value(stream, variant.weight) || !read_value(stream, variant.gain) ||
                        !read_value(stream, variant.cooldownFrames)) {
                        if (error) *error = "truncated interactive audio variant";
                        return std::nullopt;
                    }
                }
            }
        }
    }
    graph.transitions.resize(transitionCount);
    std::size_t totalConditions{};
    for (auto& transition : graph.transitions) {
        std::uint8_t quantization{};
        std::uint32_t conditionCount{};
        if (!read_string(stream, transition.from) || !read_string(stream, transition.to) ||
            !read_value(stream, quantization) || !read_value(stream, transition.crossfadeBeats) ||
            !read_string(stream, transition.stinger) || !read_value(stream, conditionCount)) {
            if (error) *error = "truncated interactive audio transition";
            return std::nullopt;
        }
        transition.quantization = static_cast<InteractiveMusicQuantization>(quantization);
        totalConditions += conditionCount;
        if (totalConditions > kMaximumConditions) {
            if (error) *error = "interactive audio conditions exceed limits";
            return std::nullopt;
        }
        transition.conditions.resize(conditionCount);
        for (auto& condition : transition.conditions) {
            std::uint8_t comparison{};
            if (!read_string(stream, condition.parameter) || !read_value(stream, comparison) ||
                !read_value(stream, condition.value)) {
                if (error) *error = "truncated interactive audio condition";
                return std::nullopt;
            }
            condition.comparison = static_cast<InteractiveMusicComparison>(comparison);
        }
    }
    if (stream.peek() != std::char_traits<char>::eof()) {
        if (error) *error = "trailing data in interactive audio graph";
        return std::nullopt;
    }
    if (!validate_interactive_music_graph(graph, error)) return std::nullopt;
    if (interactive_music_graph_hash(graph) != expectedHash) {
        if (error) *error = "interactive audio graph hash mismatch";
        return std::nullopt;
    }
    return graph;
}

std::vector<InteractiveMusicStemMix> evaluate_interactive_music_stems(
    const InteractiveMusicGraph& graph, std::string_view stateName,
    const std::vector<InteractiveMusicParameter>& parameters) {
    std::vector<InteractiveMusicStemMix> result;
    const InteractiveMusicState* state = find_state(graph, stateName);
    if (state == nullptr) return result;
    result.reserve(state->stems.size());
    for (const auto& stem : state->stems) {
        float weight = 1.0F;
        if (!stem.parameter.empty()) {
            const float value = parameter_value(parameters, stem.parameter);
            if (stem.maximumParameter <= stem.minimumParameter)
                weight = value >= stem.minimumParameter ? 1.0F : 0.0F;
            else
                weight = std::clamp((value - stem.minimumParameter) /
                                    (stem.maximumParameter - stem.minimumParameter), 0.0F, 1.0F);
        }
        result.push_back({stem.name, stem.assetPath, stem.gain * weight, stem.loop});
    }
    return result;
}

std::optional<InteractiveMusicDecision> choose_interactive_music_transition(
    const InteractiveMusicGraph& graph, std::string_view currentState,
    std::string_view requestedState, std::uint64_t currentFrame,
    std::uint32_t sampleRate, const std::vector<InteractiveMusicParameter>& parameters) {
    const InteractiveMusicState* sourceState = find_state(graph, currentState);
    if (sourceState == nullptr || find_state(graph, requestedState) == nullptr) return std::nullopt;
    const auto transitionIt = std::find_if(graph.transitions.begin(), graph.transitions.end(),
        [&](const InteractiveMusicTransition& transition) {
            return transition.from == currentState && transition.to == requestedState &&
                std::all_of(transition.conditions.begin(), transition.conditions.end(),
                    [&](const InteractiveMusicCondition& condition) {
                        return condition_passes(condition, parameters);
                    });
        });
    if (transitionIt == graph.transitions.end()) return std::nullopt;
    InteractiveMusicDecision decision;
    decision.from = transitionIt->from;
    decision.to = transitionIt->to;
    decision.transitionFrame = quantized_frame(graph, sourceState, transitionIt->quantization,
                                                currentFrame, sampleRate);
    const double framesPerBeat = sampleRate == 0U ? 0.0 :
        static_cast<double>(sampleRate) * 60.0 / graph.beatsPerMinute;
    const double crossfade = framesPerBeat * static_cast<double>(transitionIt->crossfadeBeats);
    decision.crossfadeFrames = crossfade >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(std::llround(crossfade));
    const auto* targetState = find_state(graph, requestedState);
    decision.stinger = transitionIt->stinger.empty() && targetState != nullptr
        ? targetState->entryStinger : transitionIt->stinger;
    return decision;
}

InteractiveMusicRuntime::InteractiveMusicRuntime(InteractiveMusicGraph graph, std::uint64_t seed)
    : graph_(std::move(graph)), randomState_(seed == 0U ? 0xD7E130ULL : seed) {
    if (!graph_.states.empty()) currentState_ = graph_.states.front().name;
    for (const auto& state : graph_.states) {
        for (const auto& container : state.containers) {
            ContainerRuntimeState runtime;
            runtime.name = container.name;
            runtime.nextAllowedFrame.assign(container.variants.size(), 0U);
            runtime.shuffleOrder.resize(container.variants.size());
            std::iota(runtime.shuffleOrder.begin(), runtime.shuffleOrder.end(), 0U);
            containers_.push_back(std::move(runtime));
        }
    }
}

std::uint64_t InteractiveMusicRuntime::next_random() noexcept {
    std::uint64_t x = randomState_;
    x ^= x >> 12U; x ^= x << 25U; x ^= x >> 27U;
    randomState_ = x;
    return x * 2685821657736338717ULL;
}

bool InteractiveMusicRuntime::set_state(std::string state, std::string* error) {
    if (find_state(graph_, state) == nullptr) {
        if (error) *error = "interactive music state does not exist";
        return false;
    }
    currentState_ = std::move(state);
    return true;
}

std::optional<InteractiveMusicDecision> InteractiveMusicRuntime::request_transition(
    std::string_view requestedState, std::uint64_t currentFrame, std::uint32_t sampleRate,
    const std::vector<InteractiveMusicParameter>& parameters) {
    auto decision = choose_interactive_music_transition(graph_, currentState_, requestedState,
                                                         currentFrame, sampleRate, parameters);
    if (decision) currentState_ = decision->to;
    return decision;
}

std::optional<InteractiveMusicVariantDecision> InteractiveMusicRuntime::trigger_container(
    std::string_view containerName, std::uint64_t currentFrame) {
    const InteractiveMusicContainer* container = find_container(graph_, containerName);
    if (container == nullptr || container->variants.empty()) return std::nullopt;
    const auto stateIt = std::find_if(containers_.begin(), containers_.end(),
        [containerName](const ContainerRuntimeState& state) { return state.name == containerName; });
    if (stateIt == containers_.end()) return std::nullopt;
    auto& runtime = *stateIt;
    std::vector<std::uint32_t> eligible;
    eligible.reserve(container->variants.size());
    for (std::uint32_t index = 0U; index < container->variants.size(); ++index) {
        if (currentFrame < runtime.nextAllowedFrame[index]) continue;
        if (container->noImmediateRepeat && container->variants.size() > 1U &&
            runtime.lastVariant == static_cast<std::int32_t>(index)) continue;
        eligible.push_back(index);
    }
    if (eligible.empty()) return std::nullopt;
    std::uint32_t chosen = eligible.front();
    if (container->mode == InteractiveMusicContainerMode::Sequence) {
        for (std::size_t offset = 0U; offset < container->variants.size(); ++offset) {
            const std::uint32_t candidate = (runtime.cursor + static_cast<std::uint32_t>(offset)) %
                                            static_cast<std::uint32_t>(container->variants.size());
            if (std::find(eligible.begin(), eligible.end(), candidate) != eligible.end()) {
                chosen = candidate;
                runtime.cursor = (candidate + 1U) % static_cast<std::uint32_t>(container->variants.size());
                break;
            }
        }
    } else if (container->mode == InteractiveMusicContainerMode::Shuffle) {
        if (runtime.cursor == 0U || runtime.cursor >= runtime.shuffleOrder.size()) {
            for (std::size_t i = runtime.shuffleOrder.size(); i > 1U; --i) {
                const std::size_t j = static_cast<std::size_t>(next_random() % i);
                std::swap(runtime.shuffleOrder[i - 1U], runtime.shuffleOrder[j]);
            }
            runtime.cursor = 0U;
        }
        bool found{};
        for (std::size_t attempts = 0U; attempts < runtime.shuffleOrder.size(); ++attempts) {
            const std::uint32_t candidate = runtime.shuffleOrder[runtime.cursor++ % runtime.shuffleOrder.size()];
            if (std::find(eligible.begin(), eligible.end(), candidate) != eligible.end()) {
                chosen = candidate; found = true; break;
            }
        }
        if (!found) chosen = eligible.front();
    } else {
        double totalWeight{};
        for (const auto index : eligible) totalWeight += container->variants[index].weight;
        if (totalWeight > 0.0) {
            const double unit = static_cast<double>(next_random() >> 11U) * (1.0 / 9007199254740992.0);
            double target = unit * totalWeight;
            for (const auto index : eligible) {
                target -= container->variants[index].weight;
                if (target <= 0.0) { chosen = index; break; }
            }
        }
    }
    runtime.lastVariant = static_cast<std::int32_t>(chosen);
    const auto& variant = container->variants[chosen];
    runtime.nextAllowedFrame[chosen] = currentFrame > std::numeric_limits<std::uint64_t>::max() - variant.cooldownFrames
        ? std::numeric_limits<std::uint64_t>::max() : currentFrame + variant.cooldownFrames;
    return InteractiveMusicVariantDecision{container->name, variant.name, variant.assetPath,
                                           variant.gain, currentFrame};
}

} // namespace dve::audio
