// Phase 3 (SYN-012): multi-lane generative step sequencer implementation.
#include "dve/audio/sequencer.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dve::audio {
namespace {

constexpr float clamp01(float v) noexcept { return v < 0.0F ? 0.0F : (v > 1.0F ? 1.0F : v); }
constexpr float clampf(float v, float lo, float hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Scale interval tables: semitone offsets of each degree within the octave.
constexpr int kChromatic[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr int kMajor[7] = {0, 2, 4, 5, 7, 9, 11};
constexpr int kMinor[7] = {0, 2, 3, 5, 7, 8, 10};
constexpr int kPentMajor[5] = {0, 2, 4, 7, 9};
constexpr int kPentMinor[5] = {0, 3, 5, 7, 10};
constexpr int kDorian[7] = {0, 2, 3, 5, 7, 9, 10};

struct ScaleTable {
    const int* degrees;
    std::size_t count;
};

constexpr ScaleTable scale_table(SequencerScale scale) noexcept {
    switch (scale) {
        case SequencerScale::Major: return {kMajor, 7};
        case SequencerScale::Minor: return {kMinor, 7};
        case SequencerScale::PentatonicMajor: return {kPentMajor, 5};
        case SequencerScale::PentatonicMinor: return {kPentMinor, 5};
        case SequencerScale::Dorian: return {kDorian, 7};
        case SequencerScale::Chromatic: break;
    }
    return {kChromatic, 12};
}

// Sensible defaults so a fresh Sequencer plays musically without any
// lane programming: full probability, near-full velocity, slightly
// detached gate, centered timbre/morph/pan.
constexpr float default_step_value(SequencerLane lane) noexcept {
    switch (lane) {
        case SequencerLane::Pitch: return 0.0F;
        case SequencerLane::Velocity: return 0.9F;
        case SequencerLane::Gate: return 0.8F;
        case SequencerLane::Timbre: return 0.0F;
        case SequencerLane::Probability: return 1.0F;
        case SequencerLane::Morph: return 0.0F;
        case SequencerLane::Pan: return 0.0F;
        case SequencerLane::Count: break;
    }
    return 0.0F;
}
// Value clamp ranges per lane (applied after mutation / at read time).
constexpr float lane_min(SequencerLane lane) noexcept {
    switch (lane) {
        case SequencerLane::Pitch: return -48.0F;
        case SequencerLane::Velocity: return 0.0F;
        case SequencerLane::Gate: return 0.0F;
        case SequencerLane::Timbre: return -1.0F;
        case SequencerLane::Probability: return 0.0F;
        case SequencerLane::Morph: return 0.0F;
        case SequencerLane::Pan: return -1.0F;
        case SequencerLane::Count: break;
    }
    return 0.0F;
}
constexpr float lane_max(SequencerLane lane) noexcept {
    switch (lane) {
        case SequencerLane::Pitch: return 48.0F;
        case SequencerLane::Velocity: return 1.0F;
        case SequencerLane::Gate: return 1.0F;
        case SequencerLane::Timbre: return 1.0F;
        case SequencerLane::Probability: return 1.0F;
        case SequencerLane::Morph: return 1.0F;
        case SequencerLane::Pan: return 1.0F;
        case SequencerLane::Count: break;
    }
    return 1.0F;
}
// Full-scale drift applied when mutationAmount == 1.
constexpr float lane_mutation_span(SequencerLane lane) noexcept {
    switch (lane) {
        case SequencerLane::Pitch: return 2.0F;   // +/-1 semitone at full amount
        case SequencerLane::Velocity: return 0.4F;
        case SequencerLane::Gate: return 0.4F;
        case SequencerLane::Timbre: return 0.8F;
        case SequencerLane::Probability: return 0.4F;
        case SequencerLane::Morph: return 0.8F;
        case SequencerLane::Pan: return 0.8F;
        case SequencerLane::Count: break;
    }
    return 0.0F;
}

}  // namespace

Sequencer::Sequencer() {
    for (std::size_t li = 0; li < kSequencerLaneCount; ++li) {
        const SequencerLane which = static_cast<SequencerLane>(li);
        for (SequencerStep& step : lanes_[li].steps) step.value = default_step_value(which);
    }
}
Sequencer::~Sequencer() = default;
Sequencer::Sequencer(Sequencer&&) noexcept = default;
Sequencer& Sequencer::operator=(Sequencer&&) noexcept = default;

void Sequencer::set_random_seed(std::uint32_t seed) noexcept {
    // SplitMix-style seed expansion so seed 0 is still usable.
    rngState_ = static_cast<std::uint64_t>(seed) + 0x9E3779B97F4A7C15ULL;
    rngState_ = (rngState_ ^ (rngState_ >> 30)) * 0xBF58476D1CE4E5B9ULL;
    rngState_ = (rngState_ ^ (rngState_ >> 27)) * 0x94D049BB133111EBULL;
    rngState_ ^= rngState_ >> 31;
    if (rngState_ == 0) rngState_ = 0x9E3779B97F4A7C15ULL;
}

void Sequencer::set_lane_step_count(SequencerLane which, std::uint8_t count) noexcept {
    SequencerLaneConfig& laneConfig = lanes_[static_cast<std::size_t>(which)];
    laneConfig.stepCount = static_cast<std::uint8_t>(std::clamp<int>(count, 1, 64));
    if (laneConfig.position >= laneConfig.stepCount) laneConfig.position = 0;
}

void Sequencer::set_lane_step(SequencerLane which, std::uint8_t index,
                              const SequencerStep& step) noexcept {
    if (index >= kSequencerMaxSteps) return;
    SequencerStep clamped = step;
    clamped.probability = clamp01(clamped.probability);
    clamped.ratchets = static_cast<std::uint8_t>(std::clamp<int>(clamped.ratchets, 1, 8));
    clamped.microtiming = clampf(clamped.microtiming, -1.0F, 1.0F);
    clamped.conditionN = static_cast<std::uint8_t>(std::max<int>(clamped.conditionN, 1));
    lanes_[static_cast<std::size_t>(which)].steps[index] = clamped;
}

void Sequencer::start() noexcept {
    samplesUntilNextStep_ = 0.0;
    seqNoteActive_ = false;
    seqNoteGlide_ = false;
    running_ = true;
    for (SequencerLaneConfig& laneConfig : lanes_) {
        laneConfig.position = 0;
        laneConfig.cycle = 0;
        laneConfig.pingPongForward = true;
    }
    randomTicks_.fill(0);
    timbreValue_ = clampf(lanes_[static_cast<std::size_t>(SequencerLane::Timbre)].steps[0].value, -1.0F, 1.0F);
    morphValue_ = clamp01(lanes_[static_cast<std::size_t>(SequencerLane::Morph)].steps[0].value);
    panValue_ = clampf(lanes_[static_cast<std::size_t>(SequencerLane::Pan)].steps[0].value, -1.0F, 1.0F);
}

void Sequencer::stop() noexcept {
    running_ = false;
    seqNoteActive_ = false;
    seqNoteGlide_ = false;
}

double Sequencer::step_duration_seconds(double tempoBpm) noexcept {
    if (!(tempoBpm > 0.0)) return 0.125;
    return 60.0 / tempoBpm / 4.0;  // 16th notes
}

int Sequencer::quantize_offset(SequencerScale scale, int semitoneOffset) noexcept {
    const ScaleTable table = scale_table(scale);
    const int octave = semitoneOffset >= 0 ? semitoneOffset / 12
                                           : -((-semitoneOffset + 11) / 12);
    const int pc = semitoneOffset - octave * 12;  // 0..11
    int best = table.degrees[0];
    int bestDist = 12;
    for (std::size_t i = 0; i < table.count; ++i) {
        const int dist = std::abs(table.degrees[i] - pc);
        // Ties round up (toward the higher degree).
        if (dist < bestDist || (dist == bestDist && table.degrees[i] > best)) {
            bestDist = dist;
            best = table.degrees[i];
        }
    }
    return octave * 12 + best;
}

std::uint8_t Sequencer::note_for_offset(SequencerScale scale, std::uint8_t rootNote,
                                        std::uint8_t octaveRange, int semitoneOffset) noexcept {
    int note = static_cast<int>(rootNote) + quantize_offset(scale, semitoneOffset);
    if (octaveRange > 0) {
        const int lo = static_cast<int>(rootNote);
        const int hi = lo + static_cast<int>(octaveRange) * 12;
        int guard = 0;
        while (note > hi && guard++ < 24) note -= 12;
        guard = 0;
        while (note < lo && guard++ < 24) note += 12;
    }
    return static_cast<std::uint8_t>(std::clamp(note, 0, 127));
}

float Sequencer::random_unit() noexcept {
    // xorshift64*.
    std::uint64_t x = rngState_;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rngState_ = x;
    return static_cast<float>((x * 0x2545F4914F6CDD1DULL) >> 11) * (1.0F / 9007199254740992.0F);
}

void Sequencer::advance_lane(SequencerLane which) noexcept {
    SequencerLaneConfig& laneConfig = lanes_[static_cast<std::size_t>(which)];
    const std::uint8_t count = laneConfig.stepCount;
    switch (laneConfig.direction) {
        case SequencerDirection::Forward:
            if (laneConfig.position + 1U >= count) {
                laneConfig.position = 0;
                ++laneConfig.cycle;
            } else {
                ++laneConfig.position;
            }
            break;
        case SequencerDirection::Reverse:
            if (laneConfig.position == 0) {
                laneConfig.position = static_cast<std::uint8_t>(count - 1U);
                ++laneConfig.cycle;
            } else {
                --laneConfig.position;
            }
            break;
        case SequencerDirection::PingPong:
            if (count <= 1) {
                ++laneConfig.cycle;
                break;
            }
            if (laneConfig.pingPongForward) {
                if (laneConfig.position + 1U >= count) {
                    laneConfig.pingPongForward = false;
                    --laneConfig.position;
                    ++laneConfig.cycle;
                } else {
                    ++laneConfig.position;
                }
            } else {
                if (laneConfig.position == 0) {
                    laneConfig.pingPongForward = true;
                    ++laneConfig.position;
                    ++laneConfig.cycle;
                } else {
                    --laneConfig.position;
                }
            }
            break;
        case SequencerDirection::Random: {
            // Random has no wrap; count a cycle every stepCount draws so
            // mutation still applies on a deterministic schedule.
            laneConfig.position = static_cast<std::uint8_t>(
                static_cast<unsigned>(random_unit() * 256.0F) % count);
            std::uint64_t& ticks = randomTicks_[static_cast<std::size_t>(which)];
            if (++ticks % count == 0U) ++laneConfig.cycle;
            break;
        }
    }
}

void Sequencer::mutate_lane(SequencerLane which) noexcept {
    SequencerLaneConfig& laneConfig = lanes_[static_cast<std::size_t>(which)];
    const float amount = clamp01(laneConfig.mutationAmount);
    if (amount <= 0.0F) return;
    const float span = lane_mutation_span(which);
    const float lo = lane_min(which);
    const float hi = lane_max(which);
    for (std::uint8_t i = 0; i < laneConfig.stepCount; ++i) {
        SequencerStep& step = laneConfig.steps[i];
        const float drift = (random_unit() * 2.0F - 1.0F) * amount * span;
        step.value = clampf(step.value + drift, lo, hi);
    }
}

void Sequencer::fire_step(std::uint32_t frameOffset, double stepFrames,
                          const EventCallback& onEvent) noexcept {
    // Publish the non-pitch lane currents (modulation sources read these).
    timbreValue_ = clampf(lanes_[static_cast<std::size_t>(SequencerLane::Timbre)]
                              .steps[lanes_[static_cast<std::size_t>(SequencerLane::Timbre)].position]
                              .value,
                          -1.0F, 1.0F);
    morphValue_ = clamp01(lanes_[static_cast<std::size_t>(SequencerLane::Morph)]
                              .steps[lanes_[static_cast<std::size_t>(SequencerLane::Morph)].position]
                              .value);
    panValue_ = clampf(lanes_[static_cast<std::size_t>(SequencerLane::Pan)]
                           .steps[lanes_[static_cast<std::size_t>(SequencerLane::Pan)].position]
                           .value,
                       -1.0F, 1.0F);

    const SequencerLaneConfig& pitchLane = lanes_[static_cast<std::size_t>(SequencerLane::Pitch)];
    const SequencerStep& pitchStep = pitchLane.steps[pitchLane.position];

    // Close a pending legato (glide) note at this onset.
    if (seqNoteActive_ && seqNoteGlide_) {
        onEvent(Event{false, seqNote_, 0.0F, channel_, frameOffset});
        seqNoteActive_ = false;
        seqNoteGlide_ = false;
    }

    // --- trigger gating ---
    bool fire = !pitchStep.skip;
    if (fire) {
        const float probLane = clamp01(lanes_[static_cast<std::size_t>(SequencerLane::Probability)]
                                           .steps[lanes_[static_cast<std::size_t>(SequencerLane::Probability)]
                                                      .position]
                                           .value);
        const float gate = clamp01(pitchStep.probability) * probLane;
        if (random_unit() > gate) fire = false;
    }
    if (fire) {
        const std::uint64_t cycle = pitchLane.cycle;
        switch (pitchStep.condition) {
            case SequencerStepCondition::EveryCycle: break;
            case SequencerStepCondition::EveryNth:
                fire = (cycle % pitchStep.conditionN) == 0U;
                break;
            case SequencerStepCondition::FirstCycleOnly:
                fire = cycle == 0U;
                break;
            case SequencerStepCondition::LastCycleOnly: {
                const std::uint8_t total = pitchLane.patternCycles == 0 ? 1 : pitchLane.patternCycles;
                fire = (cycle % total) == static_cast<std::uint64_t>(total - 1U);
                break;
            }
        }
    }

    if (fire) {
        const int offsetSemitones =
            static_cast<int>(std::lround(pitchStep.value));
        const std::uint8_t note =
            note_for_offset(scale_, rootNote_, octaveRange_, offsetSemitones);
        const float velLane = clamp01(lanes_[static_cast<std::size_t>(SequencerLane::Velocity)]
                                          .steps[lanes_[static_cast<std::size_t>(SequencerLane::Velocity)]
                                                     .position]
                                          .value);
        const float velocity = clamp01(velLane * pitchStep.accent);
        const float gateLane = clamp01(lanes_[static_cast<std::size_t>(SequencerLane::Gate)]
                                           .steps[lanes_[static_cast<std::size_t>(SequencerLane::Gate)]
                                                      .position]
                                           .value);
        const std::uint8_t ratchets = pitchStep.ratchets;
        const double ratchetFrames = stepFrames / static_cast<double>(ratchets);
        const double microFrames = static_cast<double>(pitchStep.microtiming) * stepFrames;

        // Cut any still-sounding non-legato note before retriggering.
        if (seqNoteActive_) {
            onEvent(Event{false, seqNote_, 0.0F, channel_, frameOffset});
            seqNoteActive_ = false;
            seqNoteGlide_ = false;
        }

        for (std::uint8_t r = 0; r < ratchets; ++r) {
            const std::int64_t onset =
                static_cast<std::int64_t>(frameOffset) +
                static_cast<std::int64_t>(std::llround(r * ratchetFrames + microFrames));
            const std::uint32_t onsetClamped =
                static_cast<std::uint32_t>(std::max<std::int64_t>(onset, 0));
            onEvent(Event{true, note, velocity, channel_, onsetClamped});
            lastNote_ = note;
            // Each ratchet's note-off: gate fraction of its own subdivision,
            // except the last ratchet of a glide step, whose off is deferred
            // to the next step's onset (legato).
            const bool deferOff = pitchStep.glide && r + 1U == ratchets;
            if (!deferOff) {
                const double offIn = std::max(1.0, gateLane * ratchetFrames);
                const std::int64_t offAt = onset + static_cast<std::int64_t>(std::llround(offIn));
                onEvent(Event{false, note, 0.0F, channel_,
                              static_cast<std::uint32_t>(std::max<std::int64_t>(offAt, 0))});
            }
        }
        if (pitchStep.glide) {
            seqNoteActive_ = true;
            seqNoteGlide_ = true;
            seqNote_ = note;
        }
    } else if (seqNoteActive_ && seqNoteGlide_) {
        // A glide note with no following trigger still has to end.
        onEvent(Event{false, seqNote_, 0.0F, channel_, frameOffset});
        seqNoteActive_ = false;
        seqNoteGlide_ = false;
    }

    // Advance every lane on the shared step clock (polymeters), mutating on wrap.
    for (std::size_t li = 0; li < kSequencerLaneCount; ++li) {
        const SequencerLane which = static_cast<SequencerLane>(li);
        SequencerLaneConfig& laneConfig = lanes_[li];
        const std::uint64_t cycleBefore = laneConfig.cycle;
        advance_lane(which);
        if (laneConfig.cycle != cycleBefore) mutate_lane(which);
    }
}

void Sequencer::process(std::uint32_t frameCount, float sampleRate, float tempoBpm,
                        const EventCallback& onEvent) noexcept {
    if (!enabled_ || !running_ || frameCount == 0 || sampleRate <= 0.0F) return;
    const double stepFrames = step_duration_seconds(static_cast<double>(tempoBpm)) *
                              static_cast<double>(sampleRate);
    if (!(stepFrames >= 1.0)) return;
    double remaining = static_cast<double>(frameCount);
    std::uint32_t offset = 0;
    // Guard against pathological tempo/frame combos looping forever. A step
    // landing exactly on the block end belongs to the next block (strict <),
    // so an onset offset never equals frameCount.
    for (int guard = 0; guard < 4096 && samplesUntilNextStep_ < remaining; ++guard) {
        const auto stepIn = static_cast<std::uint32_t>(samplesUntilNextStep_);
        offset += stepIn;
        remaining -= samplesUntilNextStep_;
        fire_step(offset, stepFrames, onEvent);
        samplesUntilNextStep_ = stepFrames;
    }
    samplesUntilNextStep_ -= remaining;
}

}  // namespace dve::audio
