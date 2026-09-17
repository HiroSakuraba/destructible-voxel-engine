#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dve::audio {

enum class InteractiveMusicQuantization : std::uint8_t { Immediate, Beat, Bar, Marker };
enum class InteractiveMusicComparison : std::uint8_t { Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual };
enum class InteractiveMusicContainerMode : std::uint8_t { Sequence, RandomWeighted, Shuffle };

struct InteractiveMusicStem {
    std::string name;
    std::string assetPath;
    std::string parameter;
    float minimumParameter{};
    float maximumParameter{1.0F};
    float gain{1.0F};
    bool loop{true};
};

struct InteractiveMusicVariant {
    std::string name;
    std::string assetPath;
    float weight{1.0F};
    float gain{1.0F};
    std::uint64_t cooldownFrames{};
};

struct InteractiveMusicContainer {
    std::string name;
    InteractiveMusicContainerMode mode{InteractiveMusicContainerMode::RandomWeighted};
    bool noImmediateRepeat{true};
    std::vector<InteractiveMusicVariant> variants;
};

struct InteractiveMusicState {
    std::string name;
    std::vector<InteractiveMusicStem> stems;
    std::vector<std::uint64_t> markerFrames;
    std::string entryStinger;
    std::string exitStinger;
    std::vector<InteractiveMusicContainer> containers;
};

struct InteractiveMusicCondition {
    std::string parameter;
    InteractiveMusicComparison comparison{InteractiveMusicComparison::GreaterEqual};
    float value{};
};

struct InteractiveMusicTransition {
    std::string from;
    std::string to;
    InteractiveMusicQuantization quantization{InteractiveMusicQuantization::Bar};
    std::uint32_t crossfadeBeats{1};
    std::string stinger;
    std::vector<InteractiveMusicCondition> conditions;
};

struct InteractiveMusicGraph {
    std::uint32_t version{2};
    std::string name{"Interactive music"};
    double beatsPerMinute{120.0};
    std::uint16_t beatsPerBar{4};
    std::vector<InteractiveMusicState> states;
    std::vector<InteractiveMusicTransition> transitions;
};

struct InteractiveMusicParameter {
    std::string name;
    float value{};
};

struct InteractiveMusicStemMix {
    std::string name;
    std::string assetPath;
    float gain{};
    bool loop{};
};

struct InteractiveMusicDecision {
    std::string from;
    std::string to;
    std::uint64_t transitionFrame{};
    std::uint64_t crossfadeFrames{};
    std::string stinger;
};

struct InteractiveMusicVariantDecision {
    std::string container;
    std::string variant;
    std::string assetPath;
    float gain{1.0F};
    std::uint64_t triggerFrame{};
};

[[nodiscard]] std::uint64_t interactive_music_graph_hash(const InteractiveMusicGraph& graph) noexcept;
[[nodiscard]] bool write_interactive_music_graph(const std::filesystem::path& path,
                                                  const InteractiveMusicGraph& graph,
                                                  std::string* error = nullptr);
[[nodiscard]] std::optional<InteractiveMusicGraph> read_interactive_music_graph(
    const std::filesystem::path& path, std::string* error = nullptr);

class InteractiveMusicRuntime {
public:
    explicit InteractiveMusicRuntime(InteractiveMusicGraph graph = {}, std::uint64_t seed = 0xD7E130ULL);
    [[nodiscard]] const InteractiveMusicGraph& graph() const noexcept { return graph_; }
    [[nodiscard]] std::string_view current_state() const noexcept { return currentState_; }
    bool set_state(std::string state, std::string* error = nullptr);
    [[nodiscard]] std::optional<InteractiveMusicDecision> request_transition(
        std::string_view requestedState, std::uint64_t currentFrame, std::uint32_t sampleRate,
        const std::vector<InteractiveMusicParameter>& parameters);
    [[nodiscard]] std::optional<InteractiveMusicVariantDecision> trigger_container(
        std::string_view container, std::uint64_t currentFrame);
private:
    struct ContainerRuntimeState {
        std::string name;
        std::uint32_t cursor{};
        std::int32_t lastVariant{-1};
        std::vector<std::uint32_t> shuffleOrder;
        std::vector<std::uint64_t> nextAllowedFrame;
    };
    [[nodiscard]] std::uint64_t next_random() noexcept;
    InteractiveMusicGraph graph_;
    std::string currentState_;
    std::uint64_t randomState_{};
    std::vector<ContainerRuntimeState> containers_;
};

[[nodiscard]] bool validate_interactive_music_graph(const InteractiveMusicGraph& graph,
                                                     std::string* error = nullptr);
[[nodiscard]] std::vector<InteractiveMusicStemMix> evaluate_interactive_music_stems(
    const InteractiveMusicGraph& graph, std::string_view state,
    const std::vector<InteractiveMusicParameter>& parameters);
[[nodiscard]] std::optional<InteractiveMusicDecision> choose_interactive_music_transition(
    const InteractiveMusicGraph& graph, std::string_view currentState,
    std::string_view requestedState, std::uint64_t currentFrame,
    std::uint32_t sampleRate, const std::vector<InteractiveMusicParameter>& parameters);

} // namespace dve::audio
