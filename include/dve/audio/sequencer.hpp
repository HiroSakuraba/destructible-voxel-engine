// Phase 3 (SYN-012): multi-lane generative step sequencer.
//
// Seven independent lanes (pitch, velocity, gate, timbre, probability,
// morph, pan), each with its own step length (polymeters), direction,
// per-step ratchets / microtiming / glide / accent / skip / conditionals,
// scale quantization on the pitch lane, seeded per-cycle step mutation, and
// 16th-note tempo sync. The sequencer is sample-count driven (process()),
// emits note events through a callback, and publishes the timbre/morph/pan
// lane currents as modulation sources (ModulationSource::SeqTimbre etc.).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace dve::audio {

// Lanes of the generative sequencer.
enum class SequencerLane : std::uint8_t {
    Pitch,
    Velocity,
    Gate,
    Timbre,
    Probability,
    Morph,
    Pan,
    Count
};

inline constexpr std::size_t kSequencerLaneCount =
    static_cast<std::size_t>(SequencerLane::Count);

// Per-lane playback direction.
enum class SequencerDirection : std::uint8_t {
    Forward,
    Reverse,
    PingPong,
    Random
};

// Pitch-lane scale quantization tables.
enum class SequencerScale : std::uint8_t {
    Chromatic,
    Major,
    Minor,
    PentatonicMajor,
    PentatonicMinor,
    Dorian
};

// Per-step play condition, evaluated against the pitch lane's cycle counter
// (a cycle = one full pass over that lane's steps).
enum class SequencerStepCondition : std::uint8_t {
    EveryCycle,      // always plays
    EveryNth,        // plays when cycle % conditionN == 0
    FirstCycleOnly,  // plays only on cycle 0
    LastCycleOnly    // plays only on the last cycle of the pattern
};

inline constexpr std::size_t kSequencerMaxSteps = 64;

// One step of one lane.
struct SequencerStep {
    float value{0.0F};            // lane-dependent: pitch = semitone offset from
                                  // root, velocity = 0..1, gate = 0..1 fraction
                                  // of step, timbre = -1..1, probability =
                                  // 0..1 multiplier, morph = 0..1, pan = -1..1
    float probability{1.0F};      // per-step trigger gate probability 0..1
    std::uint8_t ratchets{1};      // 1..8 sub-divisions of the step
    float microtiming{0.0F};       // +/- fraction of a step added to the onset
    bool glide{false};             // legato: previous note releases at this onset
    float accent{1.0F};            // velocity multiplier for this step
    bool skip{false};              // step never triggers a note
    SequencerStepCondition condition{SequencerStepCondition::EveryCycle};
    std::uint8_t conditionN{2};    // cycle divisor for EveryNth
};

// Per-lane configuration plus live playback state.
struct SequencerLaneConfig {
    std::array<SequencerStep, kSequencerMaxSteps> steps{};
    std::uint8_t stepCount{16};                    // 1..64
    SequencerDirection direction{SequencerDirection::Forward};
    float mutationAmount{0.0F};                    // 0..1 seeded per-cycle drift
    std::uint8_t patternCycles{4};                 // cycle count for LastCycleOnly
    // --- live state (advanced by process()) ---
    std::uint8_t position{0};                      // current step index
    std::uint64_t cycle{0};                        // completed passes
    bool pingPongForward{true};                    // ping-pong travel direction
};

class Sequencer {
public:
    // One emitted note event. frameOffset is relative to the start of the
    // process() block that produced it.
    struct Event {
        bool noteOn{true};
        std::uint8_t note{60};
        float velocity{1.0F};          // 0..1
        std::uint8_t channel{0};
        std::uint32_t frameOffset{0};
    };
    using EventCallback = std::function<void(const Event&)>;

    Sequencer();
    ~Sequencer();

    Sequencer(const Sequencer&) = delete;
    Sequencer& operator=(const Sequencer&) = delete;
    Sequencer(Sequencer&&) noexcept;
    Sequencer& operator=(Sequencer&&) noexcept;

    // --- transport ---
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    void set_channel(std::uint8_t channel) noexcept { channel_ = channel; }
    [[nodiscard]] std::uint8_t channel() const noexcept { return channel_; }
    // Resets phase and lane positions; values are left as-authored.
    void start() noexcept;
    void stop() noexcept;

    // --- pitch lane musical setup ---
    void set_scale(SequencerScale scale) noexcept { scale_ = scale; }
    [[nodiscard]] SequencerScale scale() const noexcept { return scale_; }
    void set_root_note(std::uint8_t midiNote) noexcept { rootNote_ = midiNote; }
    [[nodiscard]] std::uint8_t root_note() const noexcept { return rootNote_; }
    // 0 = no wrap; otherwise quantized notes wrap into
    // [rootNote, rootNote + octaveRange*12].
    void set_octave_range(std::uint8_t octaves) noexcept { octaveRange_ = octaves; }
    [[nodiscard]] std::uint8_t octave_range() const noexcept { return octaveRange_; }
    void set_random_seed(std::uint32_t seed) noexcept;

    // --- lane editing ---
    [[nodiscard]] SequencerLaneConfig& lane(SequencerLane which) noexcept {
        return lanes_[static_cast<std::size_t>(which)];
    }
    [[nodiscard]] const SequencerLaneConfig& lane(SequencerLane which) const noexcept {
        return lanes_[static_cast<std::size_t>(which)];
    }
    void set_lane_step_count(SequencerLane which, std::uint8_t count) noexcept;
    void set_lane_step(SequencerLane which, std::uint8_t index, const SequencerStep& step) noexcept;

    // --- clock ---
    // Advances the sequencer by frameCount samples at the given tempo.
    // Steps are 16th notes: stepSeconds = 60 / tempoBpm / 4. Emits note events
    // via onEvent (must not throw). All lanes advance on the shared step
    // clock; only the pitch lane triggers notes.
    void process(std::uint32_t frameCount, float sampleRate, float tempoBpm,
                 const EventCallback& onEvent) noexcept;

    // --- live readouts (modulation sources read these) ---
    [[nodiscard]] float timbre_value() const noexcept { return timbreValue_; }
    [[nodiscard]] float morph_value() const noexcept { return morphValue_; }
    [[nodiscard]] float pan_value() const noexcept { return panValue_; }
    [[nodiscard]] std::uint8_t current_step(SequencerLane which) const noexcept {
        return lanes_[static_cast<std::size_t>(which)].position;
    }
    [[nodiscard]] std::uint64_t cycle_count(SequencerLane which) const noexcept {
        return lanes_[static_cast<std::size_t>(which)].cycle;
    }
    [[nodiscard]] std::uint8_t last_triggered_note() const noexcept { return lastNote_; }
    [[nodiscard]] bool note_active() const noexcept { return seqNoteActive_; }

    // --- static helpers ---
    // 16th-note step duration in seconds.
    [[nodiscard]] static double step_duration_seconds(double tempoBpm) noexcept;
    // Snaps a semitone offset (relative to the root) to the nearest scale
    // degree (ties round up). Returns the quantized offset in semitones.
    [[nodiscard]] static int quantize_offset(SequencerScale scale, int semitoneOffset) noexcept;
    // Full pipeline: offset -> scale quantization -> root + octave wrap ->
    // clamped MIDI note.
    [[nodiscard]] static std::uint8_t note_for_offset(SequencerScale scale, std::uint8_t rootNote,
                                                      std::uint8_t octaveRange,
                                                      int semitoneOffset) noexcept;

private:
    void fire_step(std::uint32_t frameOffset, double stepFrames, const EventCallback& onEvent) noexcept;
    void advance_lane(SequencerLane which) noexcept;
    [[nodiscard]] float random_unit() noexcept;
    void mutate_lane(SequencerLane which) noexcept;

    bool enabled_{false};
    std::uint8_t channel_{0};
    SequencerScale scale_{SequencerScale::Chromatic};
    std::uint8_t rootNote_{60};
    std::uint8_t octaveRange_{2};
    std::array<SequencerLaneConfig, kSequencerLaneCount> lanes_{};
    std::uint64_t rngState_{0x9E3779B97F4A7C15ULL};

    double samplesUntilNextStep_{0.0};
    std::array<std::uint64_t, kSequencerLaneCount> randomTicks_{};
    float timbreValue_{0.0F};
    float morphValue_{0.0F};
    float panValue_{0.0F};
    // Pending legato note: its note-off is deferred to the next trigger.
    bool seqNoteActive_{false};
    bool seqNoteGlide_{false};
    std::uint8_t seqNote_{60};
    std::uint8_t lastNote_{60};
    bool running_{false};
};

}  // namespace dve::audio
