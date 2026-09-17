#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/audio/audio_asset.hpp"
#include "dve/audio/audio_recorder.hpp"
#include "dve/audio/spatializer.hpp"
#include "dve/audio/synthesizer.hpp"

namespace dve::audio {

struct ChipSong;

inline constexpr std::size_t kAudioBusCount = 7;
inline constexpr std::size_t kMaxResidentSamples = 64;
inline constexpr std::size_t kMaxStreamedSamples = 8;
inline constexpr std::size_t kMaxStreamVoices = 8;
inline constexpr std::size_t kMaxLogicalSampleVoices = 128;
inline constexpr std::size_t kMaxPhysicalSampleVoices = 48;
inline constexpr std::size_t kMaxAudioCaptureSinks = 8;

enum class AudioBusId : std::uint8_t { Master, Music, Dialogue, Effects, Ambience, UserInterface, Reverb };
enum class AudioCaptureTap : std::uint8_t {
    MasterPost,
    SynthDry,
    MusicPost,
    DialoguePost,
    EffectsPost,
    AmbiencePost,
    UserInterfacePost,
    ReverbInput
};
enum class AudioPriority : std::uint8_t { Background = 16, Low = 48, Normal = 96, Important = 160, Hero = 224, Critical = 255 };

struct SampleId {
    std::uint32_t value{};
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    auto operator<=>(const SampleId&) const = default;
};

struct StreamSampleId {
    std::uint32_t value{};
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    auto operator<=>(const StreamSampleId&) const = default;
};

struct AudioSourceHandle {
    std::uint32_t value{};
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    auto operator<=>(const AudioSourceHandle&) const = default;
};

struct ResidentSampleDesc {
    std::string name;
    std::uint32_t sampleRate{kDefaultSynthSampleRate};
    std::uint8_t channels{2};
    std::vector<float> samples;
};

struct PlaySampleDesc {
    SampleId sample{};
    AudioBusId bus{AudioBusId::Effects};
    AudioPriority priority{AudioPriority::Normal};
    AudioEmitterState emitter{};
    float gain{1.0F};
    float pitch{1.0F};
    bool loop{};
    bool spatialized{true};
    std::uint64_t sampleFrame{};
};

struct PlayStreamDesc {
    StreamSampleId sample{};
    AudioBusId bus{AudioBusId::Music};
    float gain{1.0F};
    bool loop{};
    std::uint64_t sampleFrame{};
};

struct AudioBusParameters {
    float gain{1.0F};
    float lowPassHertz{20000.0F};
    float reverbSend{};
    bool mute{};
};

struct AudioBusSnapshot {
    std::array<AudioBusParameters, kAudioBusCount> buses{};
};

struct AudioBusMeter {
    float peakLeft{};
    float peakRight{};
    float rmsLeft{};
    float rmsRight{};
};

struct AudioCallbackProfile {
    double lastMilliseconds{};
    double meanMilliseconds{};
    double p95Milliseconds{};
    double p99Milliseconds{};
    double maximumMilliseconds{};
    std::uint64_t renderCalls{};
};

struct AudioMixerMeters {
    std::array<AudioBusMeter, kAudioBusCount> buses{};
    std::uint32_t logicalSampleVoices{};
    std::uint32_t physicalSampleVoices{};
    std::uint32_t virtualSampleVoices{};
    std::uint32_t streamVoices{};
    std::uint64_t streamUnderrunFrames{};
    std::uint32_t synthVoices{};
    std::uint64_t renderedFrames{};
    std::uint64_t droppedCommands{};
    std::uint64_t stolenVoices{};
    AudioSourceHandle lastStolenSource{};
    AudioPriority lastStolenPriority{AudioPriority::Background};
    AudioCallbackProfile callbackProfile{};
};

// Middleware-neutral game-audio mixer. Resident assets are registered on the control thread
// before use. The render path uses fixed-capacity storage and performs no allocation, locking,
// file I/O, or logging.
class AudioMixer {
public:
    explicit AudioMixer(std::uint32_t sampleRate = kDefaultSynthSampleRate);
    ~AudioMixer();
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    [[nodiscard]] std::uint32_t sample_rate() const noexcept;
    [[nodiscard]] std::uint64_t current_frame() const noexcept;

    [[nodiscard]] Synthesizer& synthesizer() noexcept;
    [[nodiscard]] const Synthesizer& synthesizer() const noexcept;

    // Asset registration is not a real-time operation. Samples are copied and converted during
    // registration, then remain immutable while the mixer can reference them.
    SampleId register_resident_sample(ResidentSampleDesc sample, std::string* error = nullptr);
    [[nodiscard]] std::string_view sample_name(SampleId sample) const noexcept;

    // Control-thread tracker preview. The song is compiled before publication and then rendered
    // directly by the audio callback, so repeated editor auditions do not consume resident-sample
    // slots or retain large temporary PCM buffers. Replacing/stopping a preview is thread-safe.
    bool start_chiptune_preview(const ChipSong& song, AudioBusId bus = AudioBusId::Music,
                                bool loop = false, std::string* error = nullptr);
    void stop_chiptune_preview() noexcept;
    [[nodiscard]] bool chiptune_preview_active() const noexcept;

    // Registers a cooked .dvesample stream. File I/O and ring filling occur on a worker; the
    // real-time callback only drains a fixed-capacity ring. One active playback voice is allowed
    // per registered stream to keep rewind and loop ownership deterministic.
    StreamSampleId register_streamed_sample(const std::filesystem::path& path,
                                            std::size_t ringCapacityFrames = 16384,
                                            std::string* error = nullptr);
    [[nodiscard]] std::string_view stream_name(StreamSampleId sample) const noexcept;

    AudioSourceHandle play_sample(const PlaySampleDesc& desc) noexcept;
    AudioSourceHandle play_stream(const PlayStreamDesc& desc) noexcept;
    bool stop(AudioSourceHandle source, float fadeSeconds = 0.02F) noexcept;
    bool set_source_emitter(AudioSourceHandle source, const AudioEmitterState& emitter) noexcept;
    bool set_source_gain(AudioSourceHandle source, float gain) noexcept;

    bool set_bus_parameters(AudioBusId bus, const AudioBusParameters& parameters) noexcept;
    bool apply_bus_snapshot(const AudioBusSnapshot& snapshot, float transitionSeconds = 0.25F) noexcept;
    [[nodiscard]] AudioBusSnapshot bus_snapshot() const noexcept;
    [[nodiscard]] AudioBusParameters bus_parameters(AudioBusId bus) const noexcept;
    void set_listener(const AudioListenerState& listener) noexcept;
    void set_spatializer(std::shared_ptr<const IAudioSpatializer> spatializer);

    // Installs one callback-safe recorder/analysis tap. Passing an empty sink disables capture.
    // The sink is invoked from render() and must obey IAudioCaptureSink's real-time contract.
    void set_capture_sink(std::shared_ptr<IAudioCaptureSink> sink,
                          AudioCaptureTap tap = AudioCaptureTap::MasterPost);
    // Fixed fan-out capture slots allow master, dry synth, and selected bus stems to be
    // recorded in one render pass. Slot zero is the compatibility slot used above.
    bool set_capture_sink(std::size_t slot, std::shared_ptr<IAudioCaptureSink> sink,
                          AudioCaptureTap tap = AudioCaptureTap::MasterPost) noexcept;
    void clear_capture_sinks() noexcept;

    void render(std::span<float> interleavedStereo) noexcept;
    void render(float* interleavedStereo, std::size_t frameCount) noexcept;
    [[nodiscard]] AudioMixerMeters meters() const noexcept;
    void all_sounds_off() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] constexpr std::size_t audio_bus_index(AudioBusId bus) noexcept {
    return static_cast<std::size_t>(bus);
}
[[nodiscard]] std::string_view audio_bus_name(AudioBusId bus) noexcept;

} // namespace dve::audio
