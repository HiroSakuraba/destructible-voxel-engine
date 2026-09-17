#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "dve/audio/mixer.hpp"

namespace dve::audio {

inline constexpr std::size_t kMaxAudioEventActions = 64;
inline constexpr std::uint32_t kAudioEventAssetVersion = 4;

enum class AudioEventNodeType : std::uint8_t {
    Sample,
    SynthNote,
    RandomNoRepeat,
    Sequence,
    Layer,
    Delay,
    Gain,
    Bus,
    Switch,
    Scatter,
    Cooldown,
    Blend,
    Loop,
    Stream,
};

enum class AudioBlendCurve : std::uint8_t { Linear, SmoothStep, EqualPower };

struct AudioEventNode {
    AudioEventNodeType type{AudioEventNodeType::Layer};
    std::vector<std::uint32_t> children;
    SampleId sample{};
    StreamSampleId stream{};
    bool loop{};
    AudioBusId bus{AudioBusId::Master};
    AudioPriority priority{AudioPriority::Normal};
    std::uint8_t note{60};
    float velocity{0.8F};
    float durationSeconds{0.25F};
    float value{1.0F};
    float value2{};
    std::uint32_t count{1};
    std::string parameter;
    float threshold{};
    AudioBlendCurve curve{AudioBlendCurve::Linear};
};

struct AudioEventGraph {
    std::string name;
    std::uint32_t root{};
    std::vector<AudioEventNode> nodes;
};

struct CompiledAudioEventNode {
    AudioEventNodeType type{};
    std::uint32_t childOffset{};
    std::uint16_t childCount{};
    SampleId sample{};
    StreamSampleId stream{};
    bool loop{};
    AudioBusId bus{AudioBusId::Master};
    AudioPriority priority{AudioPriority::Normal};
    std::uint8_t note{60};
    float velocity{0.8F};
    float durationSeconds{0.25F};
    float value{1.0F};
    float value2{};
    std::uint32_t count{1};
    std::uint32_t parameterHash{};
    float threshold{};
    AudioBlendCurve curve{AudioBlendCurve::Linear};
};

struct CompiledAudioEvent {
    std::string name;
    std::uint32_t root{};
    std::uint32_t deterministicSeed{0x9e3779b9U};
    std::uint32_t noRepeatHistory{1};
    std::vector<CompiledAudioEventNode> nodes;
    std::vector<std::uint32_t> children;
};

struct AudioEventEditorNode {
    // Canvas-local coordinates in authoring units. They are deliberately independent of pixels
    // and UI scale so the same graph layout is stable on Windows, Linux, and macOS.
    float x{};
    float y{};
};

struct AudioEventAsset {
    std::uint32_t version{kAudioEventAssetVersion};
    std::uint32_t deterministicSeed{0x9e3779b9U};
    std::uint32_t noRepeatHistory{1};
    AudioEventGraph graph;
    std::vector<AudioEventEditorNode> editorNodes;
};

enum class AudioEventDiagnosticSeverity : std::uint8_t { Info, Warning, Error };
struct AudioEventDiagnostic {
    AudioEventDiagnosticSeverity severity{AudioEventDiagnosticSeverity::Error};
    std::uint32_t node{UINT32_MAX};
    std::uint32_t edge{UINT32_MAX};
    std::string message;
};
struct AudioEventCompileReport {
    std::optional<CompiledAudioEvent> event;
    std::vector<AudioEventDiagnostic> diagnostics;
    [[nodiscard]] bool succeeded() const noexcept { return event.has_value(); }
};

struct AudioEventParameters {
    std::unordered_map<std::string, float> values;
    [[nodiscard]] float get(std::string_view name, float fallback = 0.0F) const noexcept;
};

enum class AudioEventActionType : std::uint8_t { PlaySample, PlayStream, SynthNoteOn, SynthNoteOff };
struct AudioEventAction {
    AudioEventActionType type{};
    std::uint64_t frameOffset{};
    SampleId sample{};
    AudioBusId bus{AudioBusId::Effects};
    AudioPriority priority{AudioPriority::Normal};
    std::uint8_t note{};
    float velocity{};
    float gain{1.0F};
    StreamSampleId stream{};
    bool loop{};
};

struct AudioEventExecution {
    std::array<AudioEventAction, kMaxAudioEventActions> actions{};
    std::size_t count{};
    bool truncated{};
    std::uint32_t suppressedByCooldown{};
};

struct AudioEventInstanceState {
    std::array<std::uint32_t, 64> sequenceCursor{};
    std::array<std::uint32_t, 64> lastRandomChoice{};
    std::array<std::array<std::uint32_t, 16>, 64> randomHistory{};
    std::array<std::uint8_t, 64> randomHistoryCount{};
    std::array<std::uint8_t, 64> randomHistoryCursor{};
    std::array<std::uint64_t, 64> lastTriggerFrame{};
    std::uint32_t randomState{0x9e3779b9U};
    bool initialized{};
};

[[nodiscard]] AudioEventCompileReport compile_audio_event_report(const AudioEventAsset& asset);
[[nodiscard]] std::optional<CompiledAudioEvent> compile_audio_event(const AudioEventGraph& graph,
                                                                     std::string* error = nullptr);
[[nodiscard]] std::optional<CompiledAudioEvent> compile_audio_event(const AudioEventAsset& asset,
                                                                     std::string* error = nullptr);
AudioEventExecution execute_audio_event(const CompiledAudioEvent& event,
                                        const AudioEventParameters& parameters,
                                        AudioEventInstanceState& state,
                                        std::uint32_t sampleRate = kDefaultSynthSampleRate,
                                        std::uint64_t absoluteStartFrame = 0U) noexcept;
void dispatch_audio_event(const AudioEventExecution& execution, AudioMixer& mixer,
                          const AudioEmitterState& emitter = {}) noexcept;

// Canonical, deterministic, line-oriented .dveaudio serialization. The format is intentionally
// diff-friendly: one node per line, stable numeric fields, quoted parameter/name strings, and
// explicit child lists. Unknown versions are rejected rather than guessed.
[[nodiscard]] bool write_audio_event_asset(const std::filesystem::path& path,
                                            const AudioEventAsset& asset,
                                            std::string* error = nullptr);
[[nodiscard]] std::optional<AudioEventAsset> read_audio_event_asset(
    const std::filesystem::path& path, std::string* error = nullptr);

[[nodiscard]] std::uint32_t audio_parameter_hash(std::string_view value) noexcept;
[[nodiscard]] std::string_view audio_event_node_type_name(AudioEventNodeType type) noexcept;

} // namespace dve::audio
