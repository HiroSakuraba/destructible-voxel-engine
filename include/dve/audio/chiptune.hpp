#pragma once

// Deterministic programmable-sound-generator voices, tracker sequencing, and compact retro sound
// effects. Authoring objects use vectors; playback compiles each instrument into fixed-capacity
// arrays before it reaches the sample loop. ChiptuneVoice::next_sample and ChiptunePlayer::render
// therefore perform no allocation, locking, file I/O, or logging.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/audio/audio_asset.hpp"

namespace dve::audio {

inline constexpr std::uint32_t kChiptuneDefaultSampleRate = 48000U;
inline constexpr std::size_t kChiptuneMaxChannels = 8U;
inline constexpr std::size_t kChiptuneEnvelopeMax = 64U;
inline constexpr std::size_t kChiptuneArpeggioMax = 32U;
inline constexpr std::size_t kChiptuneWavetableMax = 64U;
inline constexpr std::uint8_t kChipNoteOff = 0xFFU;
inline constexpr std::uint8_t kChipNoteNone = 0x00U;
inline constexpr std::uint8_t kChipVolumeNone = 0xFFU;

enum class ChipWave : std::uint8_t { Pulse, Triangle, Sawtooth, Noise, Sine, Wavetable };
enum class ChipNoiseMode : std::uint8_t { Long, Short };

struct ChipEnvelope {
    std::vector<float> values;
    std::vector<float> releaseValues;
    std::size_t loopIndex{0};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct ChipPitchEnvelope {
    std::vector<float> semitones;
    std::size_t loopIndex{0};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct ChipArpeggio {
    std::vector<std::int8_t> semitones;
    std::size_t loopIndex{0};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct ChipInstrument {
    std::string name;
    ChipWave wave{ChipWave::Pulse};
    ChipNoiseMode noiseMode{ChipNoiseMode::Long};
    float duty{0.5F};
    bool bandLimited{true};
    float gain{1.0F};
    float pan{0.0F};                    // -1 left, 0 center, +1 right
    std::int8_t transposeSemitones{0};
    float fineTuneCents{0.0F};
    ChipEnvelope volume;
    ChipPitchEnvelope pitch;
    ChipEnvelope dutyEnvelope;          // maps 0..1 to pulse duty 1%..99%
    ChipArpeggio arpeggio;
    std::int8_t pitchSlidePerTick{0};   // semitone/16 per tick
    float vibratoDepthSemitones{0.0F};
    float vibratoRateHz{0.0F};
    float lowPassHz{20000.0F};
    float highPassHz{0.0F};
    std::uint8_t bitDepth{16};          // 1..16; 16 is effectively transparent
    std::uint8_t sampleHold{1};         // 1..64 output samples per held value
    std::vector<float> wavetable;       // 2..64 samples in [-1,1] for Wavetable

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

enum class ChipEffect : std::uint8_t {
    NoEffect = 0,
    Arpeggio,
    PortamentoUp,
    PortamentoDown,
    TonePortamento,
    Vibrato,
    VolumeSlide,
    SetSpeed,
    PatternBreak,
    PositionJump,
    SetPan,
    SetDuty,
    NoteCut,
    Retrigger,
    SetWave,
};

struct ChipCell {
    std::uint8_t note{kChipNoteNone};
    std::uint8_t instrument{0};
    std::uint8_t volume{kChipVolumeNone};
    ChipEffect effect{ChipEffect::NoEffect};
    std::uint8_t effectParam{0};
};

struct ChipPattern {
    std::uint32_t rowCount{64};
    std::vector<ChipCell> cells;

    [[nodiscard]] const ChipCell& at(std::uint32_t row, std::uint32_t channel,
                                     std::uint32_t channelCount) const;
    [[nodiscard]] ChipCell& at(std::uint32_t row, std::uint32_t channel,
                               std::uint32_t channelCount);
};

struct ChipSong {
    std::string name;
    std::uint32_t sampleRate{kChiptuneDefaultSampleRate};
    std::uint32_t channelCount{4};
    std::uint32_t ticksPerRow{6};
    std::uint32_t ticksPerSecond{60};
    float masterGain{0.8F};
    bool loop{true};
    std::vector<ChipInstrument> instruments;
    std::vector<ChipPattern> patterns;
    std::vector<std::uint32_t> order;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static bool parse(std::string_view text, ChipSong& out,
                                    std::string* error = nullptr);
    [[nodiscard]] std::uint64_t content_hash() const;
};

[[nodiscard]] int chip_note_to_midi(std::uint8_t note) noexcept;
[[nodiscard]] float midi_note_to_frequency(int midiNote) noexcept;
[[nodiscard]] std::string_view chip_wave_name(ChipWave wave) noexcept;
[[nodiscard]] std::string_view chip_effect_name(ChipEffect effect) noexcept;

class ChiptuneVoice {
public:
    explicit ChiptuneVoice(std::uint32_t sampleRate = kChiptuneDefaultSampleRate) noexcept;

    void set_sample_rate(std::uint32_t sampleRate) noexcept;
    void set_instrument(const ChipInstrument& instrument) noexcept;
    void note_on(int midiNote, float velocity = 1.0F) noexcept;
    void note_off() noexcept;
    void hard_stop() noexcept;
    void retrigger() noexcept;
    [[nodiscard]] bool active() const noexcept { return active_; }

    void tick() noexcept;
    void set_pitch_slide(float semitonesPerTick) noexcept { pitchSlidePerTick_ = semitonesPerTick; }
    void set_tone_portamento(int targetMidi, float rate) noexcept;
    void set_vibrato(float depthSemitones, float rateHz) noexcept;
    void set_column_volume(float volume01) noexcept;
    void slide_column_volume(float delta) noexcept;
    void set_arpeggio_overlay(int semitoneA, int semitoneB) noexcept;
    void set_pan(float pan) noexcept;
    [[nodiscard]] float pan() const noexcept { return pan_; }
    void set_duty(float duty) noexcept;
    void set_wave(ChipWave wave) noexcept;

    [[nodiscard]] float next_sample() noexcept;

private:
    struct RuntimeInstrument {
        ChipWave wave{ChipWave::Pulse};
        ChipNoiseMode noiseMode{ChipNoiseMode::Long};
        float duty{0.5F};
        bool bandLimited{true};
        float gain{1.0F};
        float pan{0.0F};
        std::int8_t transposeSemitones{0};
        float fineTuneCents{0.0F};
        std::int8_t pitchSlidePerTick{0};
        float vibratoDepthSemitones{0.0F};
        float vibratoRateHz{0.0F};
        float lowPassHz{20000.0F};
        float highPassHz{0.0F};
        std::uint8_t bitDepth{16};
        std::uint8_t sampleHold{1};
        std::array<float, kChiptuneEnvelopeMax> volume{};
        std::array<float, kChiptuneEnvelopeMax> release{};
        std::array<float, kChiptuneEnvelopeMax> pitch{};
        std::array<float, kChiptuneEnvelopeMax> dutyEnvelope{};
        std::array<std::int8_t, kChiptuneArpeggioMax> arpeggio{};
        std::array<float, kChiptuneWavetableMax> wavetable{};
        std::uint8_t volumeCount{};
        std::uint8_t releaseCount{};
        std::uint8_t pitchCount{};
        std::uint8_t dutyCount{};
        std::uint8_t arpeggioCount{};
        std::uint8_t wavetableCount{};
        std::uint8_t volumeLoop{};
        std::uint8_t pitchLoop{};
        std::uint8_t dutyLoop{};
        std::uint8_t arpeggioLoop{};
    };

    [[nodiscard]] float current_frequency() const noexcept;
    [[nodiscard]] float oscillator_sample(float phase, float phaseIncrement) noexcept;
    [[nodiscard]] float apply_tone_shaping(float sample) noexcept;
    void retrigger_lfsr() noexcept;
    void update_filter_coefficients() noexcept;

    std::uint32_t sampleRate_;
    RuntimeInstrument instrument_{};
    bool active_{false};
    bool released_{false};

    int baseMidi_{-1};
    float glideMidi_{0.0F};
    int portaTargetMidi_{-1};
    float portaRate_{0.0F};
    float pitchSlidePerTick_{0.0F};

    int arpOverlayA_{0};
    int arpOverlayB_{0};
    std::size_t arpCursor_{0};
    std::size_t volCursor_{0};
    std::size_t relCursor_{0};
    std::size_t pitchCursor_{0};
    std::size_t dutyCursor_{0};
    std::uint32_t tickCounter_{0};

    float velocity_{1.0F};
    float columnVolume_{1.0F};
    float envAmplitude_{1.0F};
    float pitchEnvelopeSemitones_{0.0F};
    float duty_{0.5F};
    float pan_{0.0F};

    float vibratoDepth_{0.0F};
    float vibratoRate_{0.0F};
    float vibratoPhase_{0.0F};

    float phase_{0.0F};
    std::uint16_t lfsr_{1};
    float noiseAccum_{0.0F};
    float noiseValue_{0.0F};

    float lowPassCoefficient_{1.0F};
    float highPassCoefficient_{0.0F};
    float lowPassState_{0.0F};
    float highPassState_{0.0F};
    float highPassPreviousInput_{0.0F};
    float heldSample_{0.0F};
    std::uint8_t holdCounter_{0};
};

class ChiptunePlayer {
public:
    ChiptunePlayer() = default;

    [[nodiscard]] bool load(const ChipSong& song, std::string* error = nullptr);
    void restart() noexcept;
    void set_playing(bool playing) noexcept { playing_ = playing; }
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return song_.sampleRate; }
    [[nodiscard]] std::uint32_t current_order() const noexcept { return orderIndex_; }
    [[nodiscard]] std::uint32_t current_row() const noexcept { return row_; }
    [[nodiscard]] std::uint32_t current_tick() const noexcept { return tickInRow_; }

    void render(std::span<float> interleavedStereo) noexcept;
    void render(float* interleavedStereo, std::size_t frameCount) noexcept;

private:
    struct ChannelEffectState {
        float volumeSlidePerTick{};
        std::uint8_t noteCutTick{0xFFU};
        std::uint8_t retriggerTicks{};
    };

    void begin_tick() noexcept;
    void finish_tick() noexcept;
    void process_row() noexcept;
    void apply_tick_effects() noexcept;

    ChipSong song_{};
    std::vector<ChiptuneVoice> voices_;
    std::vector<ChannelEffectState> channelEffects_;
    bool loaded_{false};
    bool playing_{false};
    bool finished_{false};
    bool firstTickPending_{true};

    std::uint32_t orderIndex_{0};
    std::uint32_t row_{0};
    std::uint32_t tickInRow_{0};
    std::uint32_t ticksPerRow_{6};
    std::uint64_t tickAccumulator_{0};

    bool pendingBreak_{false};
    std::uint32_t pendingBreakRow_{0};
    bool pendingJump_{false};
    std::uint32_t pendingJumpOrder_{0};
};

[[nodiscard]] std::vector<float> render_chiptune_song(const ChipSong& song,
                                                      float maxSeconds = 600.0F,
                                                      std::string* error = nullptr);
[[nodiscard]] DecodedAudioAsset render_chiptune_audio_asset(const ChipSong& song,
                                                            float maxSeconds = 600.0F,
                                                            std::string* error = nullptr);

enum class ChipSfxPreset : std::uint8_t {
    Coin,
    Jump,
    Laser,
    Explosion,
    Hit,
    PowerUp,
    UiConfirm,
    UiCancel,
};

struct ChipSfxRequest {
    ChipSfxPreset preset{ChipSfxPreset::Coin};
    std::uint32_t sampleRate{kChiptuneDefaultSampleRate};
    float durationSeconds{0.35F};
    int baseMidi{72};
    float gain{0.8F};
    float pan{0.0F};
};

[[nodiscard]] ChipSong make_chiptune_sfx_song(const ChipSfxRequest& request);
[[nodiscard]] DecodedAudioAsset render_chiptune_sfx(const ChipSfxRequest& request,
                                                    std::string* error = nullptr);
[[nodiscard]] ChipSong make_demo_chiptune_song();

} // namespace dve::audio
