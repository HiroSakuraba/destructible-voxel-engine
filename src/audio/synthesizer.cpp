#include "dve/audio/synthesizer.hpp"
#include "dve/audio/audio_asset.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstring>
#include <complex>
#include <fstream>
#include <limits>
#include <mutex>
#include <numbers>
#include <iomanip>
#include <set>
#include <sstream>
#include <type_traits>
#include <utility>

namespace dve::audio {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTwoPi = 2.0F * kPi;
constexpr std::size_t kMidiQueueCapacity = 2048;
constexpr std::size_t kPresetQueueCapacity = 8;
constexpr std::size_t kMidiOutQueueCapacity = 2048;

float clampf(float value, float low, float high) noexcept { return std::clamp(value, low, high); }
float db_to_gain(float db) noexcept { return std::pow(10.0F, db / 20.0F); }

float poly_blep(float phase, float increment) noexcept {
    if (increment <= 0.0F) return 0.0F;
    if (phase < increment) {
        const float x = phase / increment;
        return x + x - x * x - 1.0F;
    }
    if (phase > 1.0F - increment) {
        const float x = (phase - 1.0F) / increment;
        return x * x + x + x + 1.0F;
    }
    return 0.0F;
}

float wrap_phase(float phase) noexcept {
    phase -= std::floor(phase);
    return phase;
}

// High-accuracy range-reduced polynomial. The maximum absolute error over one
// cycle is below the noise floor of 24-bit audio while avoiding libm calls in
// the real-time oscillator, modulation, chorus and phaser paths.
float fast_sin_phase(float cycles) noexcept {
    float phase = wrap_phase(cycles);
    float x = phase * kTwoPi;
    if (x > kPi) x -= kTwoPi;
    constexpr float halfPi = 0.5F * kPi;
    if (x > halfPi) x = kPi - x;
    else if (x < -halfPi) x = -kPi - x;
    const float x2 = x * x;
    return x * (1.0F + x2 * (-1.0F / 6.0F + x2 * (1.0F / 120.0F +
           x2 * (-1.0F / 5040.0F + x2 * (1.0F / 362880.0F +
           x2 * (-1.0F / 39916800.0F))))));
}

std::size_t wavetable_mip(float cyclesPerTable) noexcept {
    if (!(cyclesPerTable > 1.0F)) return 0U;
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(cyclesPerTable);
    const int exponent = static_cast<int>((bits >> 23U) & 0xFFU) - 127;
    return static_cast<std::size_t>(std::clamp(exponent, 0,
        static_cast<int>(kWavetableMipCount - 1U)));
}

// Stable odd rational saturator used in the nonlinear analogue filter cores.
// It closely tracks tanh through the musically relevant range and clamps at
// large drive values, avoiding repeated libm calls in oversampled filters.
float fast_tanh(float value) noexcept {
    if (value <= -3.0F) return -1.0F;
    if (value >= 3.0F) return 1.0F;
    const float squared = value * value;
    return value * (27.0F + squared) / (27.0F + 9.0F * squared);
}

template <class T, std::size_t Capacity>
class BoundedQueue {
    static_assert(std::is_trivially_copyable_v<T>);
    struct Cell { std::atomic<std::size_t> sequence{}; T value{}; };
public:
    BoundedQueue() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) cells_[i].sequence.store(i, std::memory_order_relaxed);
    }
    bool push(const T& value) noexcept {
        Cell* cell = nullptr;
        std::size_t position = enqueue_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[position % Capacity];
            const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
            const std::intptr_t difference = static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position);
            if (difference == 0) {
                if (enqueue_.compare_exchange_weak(position, position + 1U, std::memory_order_relaxed)) break;
            } else if (difference < 0) {
                return false;
            } else {
                position = enqueue_.load(std::memory_order_relaxed);
            }
        }
        cell->value = value;
        cell->sequence.store(position + 1U, std::memory_order_release);
        return true;
    }
    bool pop(T& value) noexcept {
        Cell* cell = nullptr;
        std::size_t position = dequeue_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[position % Capacity];
            const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
            const std::intptr_t difference = static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position + 1U);
            if (difference == 0) {
                if (dequeue_.compare_exchange_weak(position, position + 1U, std::memory_order_relaxed)) break;
            } else if (difference < 0) {
                return false;
            } else {
                position = dequeue_.load(std::memory_order_relaxed);
            }
        }
        value = cell->value;
        cell->sequence.store(position + Capacity, std::memory_order_release);
        return true;
    }
private:
    std::array<Cell, Capacity> cells_{};
    alignas(64) std::atomic<std::size_t> enqueue_{};
    alignas(64) std::atomic<std::size_t> dequeue_{};
};

struct RealtimeMicrotuning {
    bool enabled{};
    std::uint8_t referenceNote{69};
    float referenceHertz{440.0F};
    std::array<float, kMicrotuningNoteCount> centsOffset{};
};

struct RealtimeWavetable {
    bool enabled{};
    std::uint8_t frameCount{};
    std::array<float, kWavetableMipCount * kWavetableFrameCount * kWavetableSampleCount> samples{};
};

struct RealtimeSampleBank {
    bool enabled{};
    std::uint32_t sampleRate{kDefaultSynthSampleRate};
    std::uint8_t rootNote{60};
    std::uint32_t frameCount{};
    std::array<float, kSynthSampleMaxFrames> samples{};
};

struct RealtimePreset {
    std::array<OscillatorParameters, kSynthOscillatorCount> oscillators{};
    AdsrParameters ampEnvelope{};
    FilterParameters filter{};
    TuningParameters tuning{};
    ChordParameters chord{};
    ArpeggiatorParameters arpeggiator{};
    std::array<LfoParameters, kSynthLfoCount> lfos{};
    std::array<ModulationSlot, kSynthModulationSlotCount> modulation{};
    std::array<ModulationSlot, kSynthModulationSlotCount> activeModulation{};
    std::array<std::uint8_t, kSynthModulationSlotCount> activeModulationIndices{};
    std::uint8_t activeModulationCount{};
    std::array<float, kSynthOscillatorCount> oscillatorPanLeft{};
    std::array<float, kSynthOscillatorCount> oscillatorPanRight{};
    std::array<float, kSynthMacroCount> macroValues{};
    std::array<MidiLearnMapping, kSynthMidiLearnCount> midiLearn{};
    RealtimeWavetable wavetable{};
    RealtimeSampleBank sampleBank{};
    MpeParameters mpe{};
    RealtimeMicrotuning microtuning{};
    UnisonParameters unison{};
    OscillatorQuality oscillatorQuality{OscillatorQuality::Normal};
    FilterQuality filterQuality{FilterQuality::Standard};
    DistortionParameters distortion{};
    EqParameters eq{};
    ChorusParameters chorus{};
    PhaserParameters phaser{};
    DelayParameters delay{};
    ReverbParameters reverb{};
    CompressorParameters compressor{};
    LimiterParameters limiter{};
    float masterGain{};
    float masterPan{};
    float masterPanLeft{};
    float masterPanRight{};
    float pitchBendRangeSemitones{};
    bool midiThru{};
};
static_assert(std::is_trivially_copyable_v<RealtimePreset>);

ChordParameters resolved_chord_parameters(const SynthPreset& source) noexcept {
    ChordParameters result = source.chord;
    if (result.useMemory && result.memorySlot < source.chordMemory.size()) {
        const auto& memory = source.chordMemory[result.memorySlot];
        result.type = ChordType::Custom;
        result.customIntervals = memory.intervals;
        result.noteCount = memory.noteCount;
        result.useMemory = false;
    }
    return result;
}

RealtimePreset realtime_preset(const SynthPreset& source) noexcept {
    RealtimePreset result;
    result.oscillators = source.oscillators;
    result.ampEnvelope = source.ampEnvelope;
    result.filter = source.filter;
    result.tuning = source.tuning;
    result.chord = resolved_chord_parameters(source);
    result.arpeggiator = source.arpeggiator;
    result.lfos = source.lfos;
    result.modulation = source.modulation;
    for (std::size_t slotIndex = 0; slotIndex < source.modulation.size(); ++slotIndex) {
        const ModulationSlot& slot = source.modulation[slotIndex];
        if (slot.enabled && slot.source != ModulationSource::Off &&
            slot.destination != ModulationDestination::Off) {
            result.activeModulation[result.activeModulationCount] = slot;
            result.activeModulationIndices[result.activeModulationCount] = static_cast<std::uint8_t>(slotIndex);
            ++result.activeModulationCount;
        }
    }
    for (std::size_t i = 0; i < source.oscillators.size(); ++i) {
        const float pan = clampf(source.oscillators[i].pan, -1.0F, 1.0F);
        result.oscillatorPanLeft[i] = std::sqrt(0.5F * (1.0F - pan));
        result.oscillatorPanRight[i] = std::sqrt(0.5F * (1.0F + pan));
    }
    result.macroValues = source.macros.values;
    result.midiLearn = source.midiLearn;
    result.mpe = source.mpe;
    result.microtuning.enabled = source.microtuning.enabled;
    result.microtuning.referenceNote = source.microtuning.referenceNote;
    result.microtuning.referenceHertz = source.microtuning.referenceHertz;
    result.microtuning.centsOffset = source.microtuning.centsOffset;
    result.unison = source.unison;
    result.oscillatorQuality = source.oscillatorQuality;
    result.filterQuality = source.filterQuality;
    result.sampleBank.enabled = source.sampleBank.enabled;
    result.sampleBank.sampleRate = source.sampleBank.sampleRate;
    result.sampleBank.rootNote = source.sampleBank.rootNote;
    result.sampleBank.frameCount = source.sampleBank.frameCount;
    std::copy_n(source.sampleBank.samples.begin(), source.sampleBank.frameCount, result.sampleBank.samples.begin());
    result.wavetable.enabled = source.wavetable.enabled;
    result.wavetable.frameCount = source.wavetable.frameCount;
    const std::size_t baseStride = kWavetableFrameCount * kWavetableSampleCount;
    for (std::size_t frame = 0; frame < kWavetableFrameCount; ++frame) {
        for (std::size_t sample = 0; sample < kWavetableSampleCount; ++sample) {
            result.wavetable.samples[frame * kWavetableSampleCount + sample] =
                source.wavetable.samples[frame * kWavetableSampleCount + sample];
        }
    }
    for (std::size_t mip = 1; mip < kWavetableMipCount; ++mip) {
        const std::size_t previous = (mip - 1U) * baseStride;
        const std::size_t current = mip * baseStride;
        const std::size_t radius = std::size_t{1} << (mip - 1U);
        for (std::size_t frame = 0; frame < kWavetableFrameCount; ++frame) {
            for (std::size_t sample = 0; sample < kWavetableSampleCount; ++sample) {
                float sum = 0.0F;
                constexpr std::size_t taps = 5;
                for (int tap = -2; tap <= 2; ++tap) {
                    const auto wrapped = static_cast<std::size_t>((static_cast<long long>(sample) +
                        static_cast<long long>(tap) * static_cast<long long>(radius) +
                        static_cast<long long>(kWavetableSampleCount) * 4LL) %
                        static_cast<long long>(kWavetableSampleCount));
                    sum += result.wavetable.samples[previous + frame * kWavetableSampleCount + wrapped];
                }
                result.wavetable.samples[current + frame * kWavetableSampleCount + sample] = sum / static_cast<float>(taps);
            }
        }
    }
    result.distortion = source.distortion;
    result.eq = source.eq;
    result.chorus = source.chorus;
    result.phaser = source.phaser;
    result.delay = source.delay;
    result.reverb = source.reverb;
    result.compressor = source.compressor;
    result.limiter = source.limiter;
    result.masterGain = source.masterGain;
    result.masterPan = source.masterPan;
    const float masterPan = clampf(source.masterPan, -1.0F, 1.0F);
    result.masterPanLeft = std::sqrt(0.5F * (1.0F - masterPan));
    result.masterPanRight = std::sqrt(0.5F * (1.0F + masterPan));
    result.pitchBendRangeSemitones = source.pitchBendRangeSemitones;
    result.midiThru = source.midiThru;
    return result;
}

struct Envelope {
    VoiceStage stage{VoiceStage::Idle};
    float value{};
    float releaseStart{};
    float elapsed{};

    void note_on(const AdsrParameters& parameters) noexcept {
        stage = parameters.delaySeconds > 0.00001F ? VoiceStage::Delay : VoiceStage::Attack;
        elapsed = 0.0F;
        if (value < 0.0F) value = 0.0F;
    }
    void note_off() noexcept {
        if (stage == VoiceStage::Idle || stage == VoiceStage::Release) return;
        stage = VoiceStage::Release;
        releaseStart = value;
        elapsed = 0.0F;
    }
    void kill() noexcept { stage = VoiceStage::Idle; value = 0.0F; elapsed = 0.0F; }
    [[nodiscard]] bool active() const noexcept { return stage != VoiceStage::Idle; }

    float advance(const AdsrParameters& p, float sampleRate) noexcept {
        const float dt = 1.0F / sampleRate;
        elapsed += dt;
        auto shaped = [&](float x) {
            x = clampf(x, 0.0F, 1.0F);
            return p.curve == EnvelopeCurve::Linear ? x : (1.0F - std::exp(-6.0F * x)) / (1.0F - std::exp(-6.0F));
        };
        switch (stage) {
            case VoiceStage::Idle: value = 0.0F; break;
            case VoiceStage::Delay: {
                value = 0.0F;
                if (p.delaySeconds <= 0.00001F || elapsed >= p.delaySeconds) {
                    stage = VoiceStage::Attack; elapsed = 0.0F;
                }
                break;
            }
            case VoiceStage::Attack: {
                if (p.attackSeconds <= 0.00001F || elapsed >= p.attackSeconds) {
                    value = 1.0F;
                    stage = p.holdSeconds > 0.00001F ? VoiceStage::Hold : VoiceStage::Decay;
                    elapsed = 0.0F;
                } else value = shaped(elapsed / p.attackSeconds);
                break;
            }
            case VoiceStage::Hold: {
                value = 1.0F;
                if (p.holdSeconds <= 0.00001F || elapsed >= p.holdSeconds) {
                    stage = VoiceStage::Decay; elapsed = 0.0F;
                }
                break;
            }
            case VoiceStage::Decay: {
                if (p.decaySeconds <= 0.00001F || elapsed >= p.decaySeconds) {
                    value = p.sustainLevel; stage = VoiceStage::Sustain; elapsed = 0.0F;
                } else value = 1.0F + (p.sustainLevel - 1.0F) * shaped(elapsed / p.decaySeconds);
                break;
            }
            case VoiceStage::Sustain: value = p.sustainLevel; break;
            case VoiceStage::Release: {
                if (p.releaseSeconds <= 0.00001F || elapsed >= p.releaseSeconds) {
                    kill();
                } else value = releaseStart * (1.0F - shaped(elapsed / p.releaseSeconds));
                break;
            }
        }
        return value;
    }
};

struct FilterOutputs {
    float low{};
    float band{};
    float high{};
    float notch{};
};

float select_filter_output(const FilterOutputs& value, FilterMode mode) noexcept {
    switch (mode) {
        case FilterMode::LowPass: return value.low;
        case FilterMode::BandPass: return value.band;
        case FilterMode::HighPass: return value.high;
        case FilterMode::Notch: return value.notch;
    }
    return value.low;
}

struct StateVariableFilter {
    float ic1{};
    float ic2{};
    void reset() noexcept { ic1 = 0.0F; ic2 = 0.0F; }
    FilterOutputs process(float input, float cutoff, float resonance, float sampleRate) noexcept {
        cutoff = clampf(cutoff, 18.0F, sampleRate * 0.45F);
        const float q = 0.5F + clampf(resonance, 0.0F, 1.0F) * 19.5F;
        const float g = std::tan(kPi * cutoff / sampleRate);
        const float k = 1.0F / q;
        const float a1 = 1.0F / (1.0F + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;
        const float v3 = input - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0F * v1 - ic1;
        ic2 = 2.0F * v2 - ic2;
        const float high = input - k * v1 - v2;
        return {v2, v1, high, v2 + high};
    }
};

struct MoogLadderFilter {
    std::array<float, 4> stage{};
    void reset() noexcept { stage.fill(0.0F); }
    FilterOutputs process(float input, float cutoff, float resonance, float drive,
                          float bassCompensation, float sampleRate) noexcept {
        cutoff = clampf(cutoff, 18.0F, sampleRate * 0.45F);
        const float coefficient = clampf(1.0F - std::exp(-kTwoPi * cutoff / sampleRate), 0.0001F, 0.96F);
        const float feedback = clampf(resonance, 0.0F, 1.0F) * 4.15F;
        float value = fast_tanh(input * drive - feedback * stage[3]);
        for (float& state : stage) {
            state += coefficient * (fast_tanh(value) - fast_tanh(state));
            value = state;
        }
        const float low = stage[3] + input * clampf(bassCompensation, 0.0F, 1.0F) * resonance * 0.16F;
        const float high = input - low;
        const float band = stage[1] - stage[3];
        return {low, band, high, low + high};
    }
};

struct Ms20Filter {
    float stage1{};
    float stage2{};
    float highPassState{};
    void reset() noexcept { stage1 = 0.0F; stage2 = 0.0F; highPassState = 0.0F; }
    FilterOutputs process(float input, float cutoff, float resonance, float drive,
                          bool alternateRevision, float highPassCutoff, float selfOscillation,
                          float sampleRate) noexcept {
        cutoff = clampf(cutoff, 18.0F, sampleRate * 0.45F);
        highPassCutoff = clampf(highPassCutoff, 12.0F, std::min(cutoff * 0.95F, sampleRate * 0.35F));
        const float hpCoefficient = clampf(1.0F - std::exp(-kTwoPi * highPassCutoff / sampleRate), 0.0001F, 0.98F);
        highPassState += hpCoefficient * (input - highPassState);
        const float serialHighPass = input - highPassState;
        const float coefficient = clampf(1.0F - std::exp(-kTwoPi * cutoff / sampleRate), 0.0001F, 0.94F);
        const float revisionDrive = alternateRevision ? 1.42F : 1.0F;
        const float feedback = clampf(resonance, 0.0F, 1.0F) *
            (alternateRevision ? 3.35F : 2.85F) * clampf(selfOscillation, 0.5F, 1.35F);
        const float driven = fast_tanh((serialHighPass - feedback * stage2) * drive * revisionDrive);
        stage1 += coefficient * (driven - fast_tanh(stage1));
        stage2 += coefficient * (fast_tanh(stage1) - fast_tanh(stage2));
        const float low = fast_tanh(stage2 * revisionDrive);
        const float band = fast_tanh((stage1 - stage2) * (1.0F + resonance));
        const float high = fast_tanh(serialHighPass * drive) - low - band * (0.45F + 0.35F * resonance);
        return {low, band, high, low + high};
    }
};

struct AnalogFilter {
    StateVariableFilter stateVariable;
    MoogLadderFilter ladder;
    Ms20Filter ms20;
    float previousInput{};

    void reset() noexcept {
        stateVariable.reset();
        ladder.reset();
        ms20.reset();
        previousInput = 0.0F;
    }

    float process_once(float input, float cutoff, float resonance, const FilterParameters& parameters,
                       float sampleRate) noexcept {
        switch (parameters.topology) {
            case FilterTopology::CleanStateVariable:
                return select_filter_output(stateVariable.process(fast_tanh(input * parameters.drive), cutoff, resonance, sampleRate), parameters.mode);
            case FilterTopology::MoogLadder:
                return select_filter_output(ladder.process(input, cutoff, resonance * parameters.selfOscillation,
                                                          parameters.drive, parameters.bassCompensation, sampleRate), parameters.mode);
            case FilterTopology::KorgMs20:
                return select_filter_output(ms20.process(input, cutoff, resonance, parameters.drive,
                                                        parameters.alternateRevision,
                                                        parameters.ms20HighPassCutoffHertz,
                                                        parameters.selfOscillation, sampleRate), parameters.mode);
            case FilterTopology::OberheimSem: {
                const FilterOutputs outputs = stateVariable.process(fast_tanh(input * parameters.drive), cutoff,
                                                                   resonance * parameters.selfOscillation, sampleRate);
                if (parameters.mode == FilterMode::BandPass) return outputs.band;
                const float morph = clampf(parameters.morph, 0.0F, 1.0F);
                return morph < 0.5F
                    ? outputs.low + (outputs.notch - outputs.low) * (morph * 2.0F)
                    : outputs.notch + (outputs.high - outputs.notch) * ((morph - 0.5F) * 2.0F);
            }
        }
        return input;
    }

    float process(float input, float cutoff, float resonance, const FilterParameters& parameters,
                  float sampleRate, FilterQuality quality = FilterQuality::Standard) noexcept {
        const unsigned configured = static_cast<unsigned>(parameters.oversampling);
        unsigned oversampling = configured == 2U || configured == 4U ? configured : 1U;
        if (quality == FilterQuality::Eco) oversampling = 1U;
        else if (quality == FilterQuality::High) oversampling = std::max(oversampling, 2U);
        else if (quality == FilterQuality::Offline) oversampling = 4U;
        float output = 0.0F;
        for (unsigned step = 0; step < oversampling; ++step) {
            const float blend = static_cast<float>(step + 1U) / static_cast<float>(oversampling);
            const float interpolated = previousInput + (input - previousInput) * blend;
            output = process_once(interpolated, cutoff, resonance, parameters,
                                  sampleRate * static_cast<float>(oversampling));
        }
        previousInput = input;
        return output;
    }
};

struct GrainState {
    bool active{};
    float position{};
    float increment{1.0F};
    float age{};
    float duration{1.0F};
    float pan{};
    float panEnd{};
    std::uint8_t zoneIndex{kInvalidSampleZone};
};

struct PhysicalModelState {
    std::array<float, kPhysicalModelMaxDelay> delay{};
    std::size_t writeIndex{};
    float delayLengthSamples{100.0F};
    float lowpassState{};
    float dispersionState{};
    float lastOutput{};
    struct Mode {
        float frequencyRatio{1.0F};
        float freqHz{};
        float decayCoeff{0.999F};
        float y1{};
        float y2{};
        float gain{1.0F};
    };
    std::array<Mode, kPhysicalModelMaxModes> modes{};
    std::uint8_t activeModes{};
    float continuousLevel{};
    float excitationEnvelope{};
    std::uint32_t noiseState{0xA5A5A5A5U};
    bool initialized{};

    // VL-style wind / brass driver state
    float reedState{};          // previous pressure difference / reed opening
    float breathPressure{};     // smoothed continuous breath
    float throatX1{};           // formant biquad input history
    float throatX2{};
    float throatY1{};           // formant biquad output history
    float throatY2{};
    float jetDelay{};           // for air-jet (flute) edge tone
};

struct Voice {
    bool active{};
    bool keyHeld{};
    bool sustained{};
    std::uint8_t note{};
    std::uint8_t channel{};
    float velocity{};
    float pressure{};
    float timbre{};
    float pitchBendSemitones{};
    std::uint64_t age{};
    std::uint64_t startFrame{};
    Envelope amp;
    Envelope filterEnvelope;
    std::array<float, kSynthOscillatorCount> phases{};
    std::array<std::array<float, 4>, kSynthOscillatorCount> auxiliaryPhases{};
    std::array<std::uint32_t, kSynthOscillatorCount> noiseState{};
    std::array<float, kSynthOscillatorCount> previousOscillatorSamples{};
    std::array<float, kSynthOscillatorCount> subPhases{};
    std::array<float, kSynthOscillatorCount> samplePositions{};
    std::array<float, kSynthOscillatorCount> sampleMapPositions{};
    std::array<float, kSynthOscillatorCount> releaseSamplePositions{};
    std::array<bool, kSynthOscillatorCount> sampleFinished{};
    std::array<bool, kSynthOscillatorCount> releaseSampleActive{};
    std::uint8_t sampleAttackZone{kInvalidSampleZone};
    std::uint8_t sampleReleaseZone{kInvalidSampleZone};
    std::array<float, kSynthOscillatorCount> grainCountdown{};
    std::array<std::array<GrainState, kSynthGrainsPerOscillator>, kSynthOscillatorCount> grains{};
    std::array<std::array<float, kSynthUnisonMax - 1U>, kSynthOscillatorCount> unisonPhases{};
    std::array<float, kSynthModulationSlotCount> modulationSmoothing{};
    std::array<bool, kSynthOscillatorCount> oscillatorWrapped{};
    std::array<float, kSynthLfoCount> lfoPhases{};
    std::array<float, kSynthLfoCount> lfoRandomValues{};
    std::array<float, kSynthLfoCount> lfoPreviousRandomValues{};
    std::array<std::uint32_t, kSynthLfoCount> lfoNoiseState{};
    std::array<PhysicalModelState, kSynthOscillatorCount> physicalModels{};
    float noteRandom{};
    AnalogFilter filterL;
    AnalogFilter filterR;

    void start(std::uint8_t newChannel, std::uint8_t newNote, float newVelocity, std::uint64_t newAge,
               std::uint64_t newStartFrame, const RealtimePreset& preset, bool retrigger = true) noexcept {
        active = true; keyHeld = true; sustained = false; channel = newChannel; note = newNote;
        velocity = newVelocity; pressure = 0.0F; timbre = 0.0F; pitchBendSemitones = 0.0F; age = newAge; startFrame = newStartFrame;
        if (retrigger || !amp.active()) {
            amp.kill(); filterEnvelope.kill(); amp.note_on(preset.ampEnvelope); filterEnvelope.note_on(preset.filter.envelope);
            filterL.reset(); filterR.reset();
        }
        sampleAttackZone = kInvalidSampleZone;
        sampleReleaseZone = kInvalidSampleZone;
        sampleMapPositions.fill(0.0F);
        releaseSamplePositions.fill(0.0F);
        sampleFinished.fill(false);
        releaseSampleActive.fill(false);
        for (std::size_t i = 0; i < phases.size(); ++i) {
            if (preset.oscillators[i].keySync || retrigger) {
                phases[i] = wrap_phase(preset.oscillators[i].phaseOffset);
                subPhases[i] = wrap_phase(preset.oscillators[i].phaseOffset * 0.5F);
                samplePositions[i] = preset.oscillators[i].sampleReverse
                    ? preset.oscillators[i].sampleEnd : preset.oscillators[i].sampleStart;
                grainCountdown[i] = 0.0F;
                for (auto& grain : grains[i]) grain = {};
                for (std::size_t j = 0; j < auxiliaryPhases[i].size(); ++j)
                    auxiliaryPhases[i][j] = wrap_phase(preset.oscillators[i].phaseOffset + 0.173F * static_cast<float>(j + 1U));
                for (std::size_t j = 0; j < unisonPhases[i].size(); ++j)
                    unisonPhases[i][j] = wrap_phase(preset.oscillators[i].phaseOffset +
                        preset.unison.phaseSpread * static_cast<float>(j + 1U));
            }
            noiseState[i] = 0x9E3779B9U ^ (static_cast<std::uint32_t>(newNote) << 16U) ^
                            (static_cast<std::uint32_t>(i) * 0x85EBCA6BU) ^ static_cast<std::uint32_t>(newAge);
            if (noiseState[i] == 0) noiseState[i] = 1;
            previousOscillatorSamples[i] = 0.0F;
            oscillatorWrapped[i] = false;
            physicalModels[i].initialized = false;
        }
        for (std::size_t i = 0; i < kSynthLfoCount; ++i) {
            if (preset.lfos[i].keySync || retrigger) lfoPhases[i] = wrap_phase(preset.lfos[i].phase);
            lfoNoiseState[i] = 0xA511E9B3U ^ (static_cast<std::uint32_t>(newNote) << 8U) ^
                               static_cast<std::uint32_t>(i * 0x9E3779B9U) ^ static_cast<std::uint32_t>(newAge);
            if (lfoNoiseState[i] == 0U) lfoNoiseState[i] = 1U;
            lfoPreviousRandomValues[i] = 0.0F;
            lfoRandomValues[i] = static_cast<float>(static_cast<std::int32_t>(lfoNoiseState[i])) /
                                 static_cast<float>(std::numeric_limits<std::int32_t>::max());
        }
        const std::uint32_t randomHash = static_cast<std::uint32_t>(newAge) * 0x9E3779B9U ^
                                         static_cast<std::uint32_t>(newNote) * 0x85EBCA6BU;
        noteRandom = static_cast<float>(randomHash & 0x00FFFFFFU) / static_cast<float>(0x00800000U) - 1.0F;
        modulationSmoothing.fill(0.0F);
    }
    void release() noexcept { keyHeld = false; sustained = false; amp.note_off(); filterEnvelope.note_off(); }
    void kill() noexcept { active = false; keyHeld = false; sustained = false; amp.kill(); filterEnvelope.kill(); }
};

struct DelayLine {
    std::vector<float> data;
    std::size_t write{};
    explicit DelayLine(std::size_t size = 1) : data(std::max<std::size_t>(1, size), 0.0F) {}
    float read_fractional(float delaySamples) const noexcept {
        const float bounded = clampf(delaySamples, 1.0F, static_cast<float>(data.size() - 1U));
        float position = static_cast<float>(write) - bounded;
        while (position < 0.0F) position += static_cast<float>(data.size());
        const std::size_t i0 = static_cast<std::size_t>(position) % data.size();
        const std::size_t i1 = (i0 + 1U) % data.size();
        const float fraction = position - std::floor(position);
        return data[i0] + (data[i1] - data[i0]) * fraction;
    }
    void push(float value) noexcept { data[write] = value; write = (write + 1U) % data.size(); }
};

struct AllpassStage {
    float x1{};
    float y1{};
    float process(float input, float coefficient) noexcept {
        const float output = -coefficient * input + x1 + coefficient * y1;
        x1 = input; y1 = output; return output;
    }
};

struct CombFilter {
    std::vector<float> data;
    std::size_t index{};
    float filterStore{};
    explicit CombFilter(std::size_t size = 1) : data(std::max<std::size_t>(1, size), 0.0F) {}
    float process(float input, float feedback, float damping) noexcept {
        const float output = data[index];
        filterStore = output * (1.0F - damping) + filterStore * damping;
        data[index] = input + filterStore * feedback;
        index = (index + 1U) % data.size();
        return output;
    }
};

struct ReverbState {
    std::array<CombFilter, 4> combL;
    std::array<CombFilter, 4> combR;
    std::array<DelayLine, 2> allpassL;
    std::array<DelayLine, 2> allpassR;
    explicit ReverbState(std::uint32_t sampleRate)
        : combL{CombFilter(scale(1557, sampleRate)), CombFilter(scale(1617, sampleRate)),
                CombFilter(scale(1491, sampleRate)), CombFilter(scale(1422, sampleRate))},
          combR{CombFilter(scale(1580, sampleRate)), CombFilter(scale(1640, sampleRate)),
                CombFilter(scale(1514, sampleRate)), CombFilter(scale(1445, sampleRate))},
          allpassL{DelayLine(scale(225, sampleRate)), DelayLine(scale(556, sampleRate))},
          allpassR{DelayLine(scale(248, sampleRate)), DelayLine(scale(579, sampleRate))} {}
    static std::size_t scale(int samplesAt44100, std::uint32_t sampleRate) {
        return std::max<std::size_t>(2, static_cast<std::size_t>(static_cast<double>(samplesAt44100) * sampleRate / 44100.0));
    }
    static float allpass(DelayLine& line, float input) noexcept {
        const float delayed = line.read_fractional(static_cast<float>(line.data.size() - 1U));
        const float output = -input + delayed;
        line.push(input + delayed * 0.5F);
        return output;
    }
    void process(float inL, float inR, const ReverbParameters& p, float& outL, float& outR) noexcept {
        const float mono = (inL + inR) * 0.025F;
        const float feedback = 0.72F + clampf(p.roomSize, 0.0F, 1.0F) * 0.25F;
        float left = 0.0F; float right = 0.0F;
        for (auto& comb : combL) left += comb.process(mono, feedback, clampf(p.damping, 0.0F, 0.98F));
        for (auto& comb : combR) right += comb.process(mono, feedback, clampf(p.damping, 0.0F, 0.98F));
        for (auto& ap : allpassL) left = allpass(ap, left);
        for (auto& ap : allpassR) right = allpass(ap, right);
        const float width = clampf(p.width, 0.0F, 1.0F);
        outL = left * (0.5F + 0.5F * width) + right * (0.5F - 0.5F * width);
        outR = right * (0.5F + 0.5F * width) + left * (0.5F - 0.5F * width);
    }
};

float bandlimited_saw(float phase, float increment) noexcept {
    return (2.0F * phase - 1.0F) - poly_blep(phase, increment);
}

float bandlimited_pulse(float phase, float increment, float width) noexcept {
    width = clampf(width, 0.03F, 0.97F);
    float value = phase < width ? 1.0F : -1.0F;
    value += poly_blep(phase, increment);
    value -= poly_blep(wrap_phase(phase + 1.0F - width), increment);
    return value;
}

float oscillator_sample(OscillatorWaveform waveform, float phase, float increment, float pulseWidth,
                        float shape, std::array<float, 4>& auxiliaryPhases,
                        std::uint32_t& noiseState) noexcept {
    switch (waveform) {
        case OscillatorWaveform::Sine:
            return fast_sin_phase(phase);
        case OscillatorWaveform::Saw:
            return bandlimited_saw(phase, increment);
        case OscillatorWaveform::Square:
            return bandlimited_pulse(phase, increment, 0.5F);
        case OscillatorWaveform::Triangle:
            return 1.0F - 4.0F * std::abs(phase - 0.5F);
        case OscillatorWaveform::Pulse:
            return bandlimited_pulse(phase, increment, pulseWidth);
        case OscillatorWaveform::Noise:
            noiseState ^= noiseState << 13U; noiseState ^= noiseState >> 17U; noiseState ^= noiseState << 5U;
            return static_cast<float>(static_cast<std::int32_t>(noiseState)) /
                   static_cast<float>(std::numeric_limits<std::int32_t>::max());
        case OscillatorWaveform::SuperSaw: {
            static constexpr std::array<float, 4> ratios{0.993377F, 0.996996F, 1.003008F, 1.006665F};
            float result = bandlimited_saw(phase, increment);
            for (std::size_t i = 0; i < auxiliaryPhases.size(); ++i) {
                const float detunedIncrement = increment * ratios[i];
                result += bandlimited_saw(auxiliaryPhases[i], detunedIncrement);
                auxiliaryPhases[i] = wrap_phase(auxiliaryPhases[i] + detunedIncrement);
            }
            return result * 0.2F;
        }
        case OscillatorWaveform::Organ: {
            const float drawbar = clampf(shape, 0.0F, 1.0F);
            return (fast_sin_phase(phase) +
                    (0.55F + 0.25F * drawbar) * fast_sin_phase(phase * 2.0F) +
                    (0.32F + 0.28F * drawbar) * fast_sin_phase(phase * 3.0F) +
                    0.18F * fast_sin_phase(phase * 4.0F)) * 0.48F;
        }
        case OscillatorWaveform::FoldedSine: {
            const float drive = 1.0F + clampf(shape, 0.0F, 1.0F) * 7.0F;
            const float value = fast_sin_phase(phase) * drive;
            const float folded = std::abs(std::fmod(value + 3.0F, 4.0F) - 2.0F) - 1.0F;
            return folded;
        }
        case OscillatorWaveform::Digital: {
            const float amount = clampf(shape, 0.0F, 1.0F);
            const float warped = wrap_phase(phase + fast_sin_phase(phase) * amount * 0.18F);
            const float quantized = std::floor(warped * (8.0F + 56.0F * (1.0F - amount))) /
                                    (8.0F + 56.0F * (1.0F - amount));
            return fast_sin_phase(quantized) * 0.72F + bandlimited_saw(warped, increment) * 0.28F;
        }
        case OscillatorWaveform::Wavetable:
            return 0.0F;
        case OscillatorWaveform::Sample:
        case OscillatorWaveform::Granular:
        case OscillatorWaveform::PhysicalModel:
            return 0.0F;
    }
    return 0.0F;
}

// ---------------------------------------------------------------------------
// Physical modeling core (waveguide + modal) — allocation-free, realtime safe.
// ---------------------------------------------------------------------------
namespace physical {

inline float material_brightness_scale(PhysicalMaterial m) noexcept {
    switch (m) {
        case PhysicalMaterial::String: return 0.92F;
        case PhysicalMaterial::Metal: return 1.15F;
        case PhysicalMaterial::Brass: return 1.08F;
        case PhysicalMaterial::Glass: return 1.28F;
        case PhysicalMaterial::Wood: return 0.78F;
        case PhysicalMaterial::Membrane: return 0.85F;
        case PhysicalMaterial::Synthetic: return 1.00F;
    }
    return 1.0F;
}
inline float material_damping_scale(PhysicalMaterial m) noexcept {
    switch (m) {
        case PhysicalMaterial::String: return 1.00F;
        case PhysicalMaterial::Metal: return 0.72F;
        case PhysicalMaterial::Brass: return 0.78F;
        case PhysicalMaterial::Glass: return 0.55F;
        case PhysicalMaterial::Wood: return 1.35F;
        case PhysicalMaterial::Membrane: return 1.20F;
        case PhysicalMaterial::Synthetic: return 1.00F;
    }
    return 1.0F;
}
inline float material_stiffness_scale(PhysicalMaterial m) noexcept {
    switch (m) {
        case PhysicalMaterial::String: return 0.35F;
        case PhysicalMaterial::Metal: return 1.10F;
        case PhysicalMaterial::Brass: return 0.95F;
        case PhysicalMaterial::Glass: return 1.40F;
        case PhysicalMaterial::Wood: return 0.55F;
        case PhysicalMaterial::Membrane: return 0.25F;
        case PhysicalMaterial::Synthetic: return 0.80F;
    }
    return 0.5F;
}

void initialize(PhysicalModelState& state, const OscillatorParameters& osc, float frequencyHz,
                float velocity, float sampleRate) noexcept {
    state = PhysicalModelState{};
    state.initialized = true;
    state.noiseState = 0xC0FFEEU ^ static_cast<std::uint32_t>(frequencyHz * 1000.0F);
    const float sr = std::max(sampleRate, 8000.0F);
    const float size = std::clamp(osc.physicalSizeMeters, 0.05F, 4.0F);
    const float tension = std::clamp(osc.physicalTension, 0.05F, 1.0F);
    const float stiffness = std::clamp(osc.physicalStiffness, 0.0F, 1.0F)
                            * material_stiffness_scale(osc.physicalMaterial);
    const float damping = std::clamp(osc.physicalDamping * material_damping_scale(osc.physicalMaterial),
                                     0.01F, 0.98F);
    const float brightness = std::clamp(osc.physicalBrightness * material_brightness_scale(osc.physicalMaterial),
                                        0.0F, 1.0F);
    const float pos = std::clamp(osc.physicalExcitationPosition, 0.02F, 0.98F);
    const float hardness = std::clamp(osc.physicalHardness + velocity * osc.physicalVelocityToHardness,
                                      0.0F, 1.0F);

    // MIDI pitch is authoritative. Earlier imported code multiplied pitch by size and tension,
    // detuning presets by as much as an octave. Size/tension now shape losses and modal colour;
    // they do not silently retune the played note.
    const float effectiveFreq = std::clamp(frequencyHz, 20.0F, sr * 0.45F);
    float period = sr / effectiveFreq;
    period = std::clamp(period, 8.0F, static_cast<float>(kPhysicalModelMaxDelay - 4U));
    state.delayLengthSamples = period;
    const std::size_t delayLen = static_cast<std::size_t>(period);
    state.writeIndex = delayLen % kPhysicalModelMaxDelay;
    state.delay.fill(0.0F);
    const float energy = 0.35F + 0.65F * velocity;
    const float excitationWidth = 0.04F + (1.0F - hardness) * 0.22F;
    const std::size_t center = static_cast<std::size_t>(pos * static_cast<float>(delayLen));
    const std::size_t halfWidth = std::max<std::size_t>(1U, static_cast<std::size_t>(excitationWidth * static_cast<float>(delayLen)));
    if (osc.physicalExcitation == PhysicalExcitation::Pluck || osc.physicalExcitation == PhysicalExcitation::Strike) {
        for (std::size_t i = 0; i < delayLen; ++i) {
            const float d = std::abs(static_cast<float>(i) - static_cast<float>(center));
            float env = 0.0F;
            if (d < static_cast<float>(halfWidth)) {
                const float t = d / static_cast<float>(halfWidth);
                env = (1.0F - t) * (1.0F - t);
            }
            const float noise = (static_cast<float>(state.noiseState & 0xFFFFU) / 32768.0F - 1.0F) * 0.15F;
            state.noiseState = state.noiseState * 1664525U + 1013904223U;
            state.delay[i] = (env + noise * hardness * env) * energy * (0.6F + 0.4F * hardness);
        }
        if (hardness < 0.7F) {
            float lp = 0.0F;
            const float coeff = 0.35F + hardness * 0.5F;
            for (std::size_t i = 0; i < delayLen; ++i) {
                lp = lp + coeff * (state.delay[i] - lp);
                state.delay[i] = lp;
            }
        }
    }
    state.excitationEnvelope = energy;
    const std::uint8_t modeCount = std::clamp(osc.physicalModeCount, std::uint8_t{4}, std::uint8_t{kPhysicalModelMaxModes});
    state.activeModes = modeCount;
    static constexpr std::array<float, kPhysicalModelMaxModes> kPlateRatios{
        1.0000F, 1.5933F, 2.1355F, 2.2958F, 2.6539F, 2.9173F, 3.1557F, 3.5000F,
        3.5988F, 3.8890F, 4.2132F, 4.3760F, 4.5850F, 4.8180F, 5.1050F, 5.3500F};
    for (std::uint8_t m = 0; m < modeCount; ++m) {
        auto& mode = state.modes[m];
        float ratio = static_cast<float>(m + 1U)
                      * (1.0F + stiffness * 0.0035F * static_cast<float>(m * m));
        if (osc.physicalModel == PhysicalModelType::ModalPlate || osc.physicalModel == PhysicalModelType::Hybrid) {
            // Normalized so mode zero is exactly the requested note. Size and tension vary the
            // inharmonic spread modestly instead of transposing the entire instrument.
            const float spread = 0.82F + 0.18F * tension + 0.04F * std::log2(std::max(size, 0.05F) / 0.65F);
            ratio = 1.0F + (kPlateRatios[m] - 1.0F) * std::clamp(spread, 0.72F, 1.12F);
            ratio *= 1.0F + stiffness * 0.0025F * static_cast<float>(m * m);
        }
        mode.frequencyRatio = ratio;
        const float rawFrequency = effectiveFreq * ratio;
        mode.freqHz = std::min(rawFrequency, sr * 0.45F);
        const float sizeLoss = 0.12F / std::sqrt(std::max(size, 0.05F));
        const float tensionLoss = (1.0F - tension) * 0.18F;
        const float baseDecaySeconds = std::max(0.06F,
            0.55F + (1.0F - damping) * 5.0F - sizeLoss - tensionLoss);
        const float modeDecay = std::max(0.025F,
            baseDecaySeconds / (1.0F + 0.35F * static_cast<float>(m) * (1.2F - brightness)));
        mode.decayCoeff = std::exp(-1.0F / (modeDecay * sr));
        mode.y1 = 0.0F;
        mode.y2 = 0.0F;
        const float spatial = std::sin(3.14159265F * pos * ratio);
        mode.gain = (0.45F + 0.55F * std::abs(spatial))
                    * (1.0F / (1.0F + 0.12F * static_cast<float>(m)));
        if (rawFrequency >= sr * 0.45F) mode.gain = 0.0F;
        if (osc.physicalModel != PhysicalModelType::WaveguideString) {
            mode.y1 = energy * mode.gain * 0.25F * (m == 0 ? 1.0F : 0.4F);
        }
    }
}

float process(PhysicalModelState& state, const OscillatorParameters& osc, float frequencyHz,
              float velocity, float pressure, float timbre, float sampleRate) noexcept {
    if (!state.initialized) initialize(state, osc, frequencyHz, velocity, sampleRate);
    const float sr = std::max(sampleRate, 8000.0F);
    const float damping = std::clamp(osc.physicalDamping * material_damping_scale(osc.physicalMaterial),
                                     0.01F, 0.98F);
    const float brightness = std::clamp(osc.physicalBrightness + timbre * osc.physicalTimbreToBrightness, 0.0F, 1.2F)
                             * material_brightness_scale(osc.physicalMaterial);
    const float stiffness = std::clamp(osc.physicalStiffness, 0.0F, 1.0F) * material_stiffness_scale(osc.physicalMaterial);
    const float pickup = std::clamp(osc.physicalPickupPosition, 0.02F, 0.98F);
    const float targetPeriod = std::clamp(sr / std::clamp(frequencyHz, 20.0F, sr * 0.45F),
                                          8.0F, static_cast<float>(kPhysicalModelMaxDelay - 4U));
    // Smooth fractional-delay changes so pitch bend and MPE work without discontinuous jumps.
    state.delayLengthSamples += (targetPeriod - state.delayLengthSamples) * 0.0025F;

    // Continuous excitation: bow / breath
    float continuous = osc.physicalContinuousAmount + pressure * osc.physicalPressureToBow;
    if (osc.physicalDriver != PhysicalDriver::Disabled || osc.physicalExcitation == PhysicalExcitation::Blow)
        continuous = osc.physicalContinuousAmount + pressure * osc.physicalPressureToBreath;
    continuous = std::clamp(continuous, 0.0F, 1.5F);
    if (osc.physicalExcitation == PhysicalExcitation::Bow || osc.physicalExcitation == PhysicalExcitation::Blow
        || osc.physicalDriver != PhysicalDriver::Disabled)
        continuous = std::max(continuous, 0.12F + pressure * 0.85F);
    state.continuousLevel = state.continuousLevel * 0.92F + continuous * 0.08F;
    state.breathPressure = state.breathPressure * 0.90F + continuous * 0.10F;

    const bool isWind = (osc.physicalDriver != PhysicalDriver::Disabled)
                        || (osc.physicalExcitation == PhysicalExcitation::Blow);

    float waveguideOut = 0.0F;
    float modalOut = 0.0F;

    // ------------------------------------------------------------------
    // Waveguide path (strings + wind bore)
    // ------------------------------------------------------------------
    if (osc.physicalModel == PhysicalModelType::WaveguideString
        || osc.physicalModel == PhysicalModelType::Hybrid
        || isWind) {
        const float period = state.delayLengthSamples;
        const auto read_delay = [&](float delaySamples) noexcept {
            float readPos = static_cast<float>(state.writeIndex) - delaySamples;
            while (readPos < 0.0F) readPos += static_cast<float>(kPhysicalModelMaxDelay);
            while (readPos >= static_cast<float>(kPhysicalModelMaxDelay))
                readPos -= static_cast<float>(kPhysicalModelMaxDelay);
            const std::size_t i0 = static_cast<std::size_t>(readPos) % kPhysicalModelMaxDelay;
            const std::size_t i1 = (i0 + 1U) % kPhysicalModelMaxDelay;
            const float frac = readPos - std::floor(readPos);
            return state.delay[i0] + (state.delay[i1] - state.delay[i0]) * frac;
        };

        // Feedback must use the full requested period. The imported implementation used the
        // pickup tap for feedback, so moving the pickup also changed pitch by several semitones.
        float feedbackSample = read_delay(period);
        const float pickupDelay = period * std::clamp(0.08F + 0.84F * pickup, 0.08F, 0.92F);
        const float pickupSample = read_delay(pickupDelay);

        // Brightness low-pass belongs in the loop filter.
        const float lpCoeff = 0.12F + (1.0F - std::clamp(brightness, 0.0F, 1.0F)) * 0.78F;
        state.lowpassState += (feedbackSample - state.lowpassState) * (1.0F - lpCoeff);
        feedbackSample = state.lowpassState;

        // Dispersion (stiffness)
        if (stiffness > 0.01F) {
            const float apCoeff = 0.15F + stiffness * 0.55F;
            const float apOut = -apCoeff * feedbackSample + state.dispersionState;
            state.dispersionState = feedbackSample + apCoeff * apOut;
            feedbackSample = apOut;
        }

        float writeValue = 0.0F;

        if (isWind && osc.physicalDriver != PhysicalDriver::Disabled) {
            // ----- VL1-style nonlinear drivers -----
            const float emb = std::clamp(osc.physicalEmbouchure
                                         + velocity * osc.physicalVelocityToEmbouchure, 0.05F, 0.95F);
            const float reedK = std::clamp(osc.physicalReedStiffness, 0.05F, 0.95F);
            const float breath = state.breathPressure;
            const float bore = feedbackSample;  // pressure arriving from the bore

            float driverOut = 0.0F;

            if (osc.physicalDriver == PhysicalDriver::Reed) {
                // Simplified single-reed (clarinet/sax) scattering
                // deltaP = mouth pressure - bore pressure
                const float delta = breath * 1.4F - bore;
                // Reed opening (nonlinear)
                float opening = emb - reedK * delta;
                opening = std::clamp(opening, 0.0F, 1.0F);
                // Flow ~ opening * sign(delta) * sqrt(|delta|)
                const float flow = opening * ((delta >= 0.0F) ? 1.0F : -1.0F)
                                   * std::sqrt(std::abs(delta) + 1.0e-6F);
                driverOut = flow * 0.82F;
                // Soft saturation. The imported coefficient left several reed presets near
                // numerical silence; this keeps them comfortably below the brass driver while
                // providing a stable playable range across reed stiffness/embouchure settings.
                driverOut = std::tanh(driverOut * 2.0F);
                state.reedState = state.reedState * 0.68F + driverOut * 0.32F;
                const float taper = std::clamp(osc.physicalBoreTaper, 0.0F, 1.0F);
                const float reflection = -0.95F + 0.08F * taper;
                writeValue = reflection * bore + state.reedState * (0.72F + 0.42F * breath);
            } else if (osc.physicalDriver == PhysicalDriver::Lip) {
                // Lip-reed (brass) — mass-spring like lip + nonlinear
                const float delta = breath * 1.6F - bore;
                float lip = state.reedState;
                // Simple 2nd-order-ish lip motion
                const float lipStiff = 0.25F + reedK * 0.6F;
                lip = lip + (delta * emb - lipStiff * lip) * 0.18F;
                lip = std::tanh(lip);
                state.reedState = lip;
                // Pulse-like when lips open
                const float open = std::max(0.0F, lip + emb * 0.4F - 0.25F);
                driverOut = open * breath * 1.1F;
                writeValue = -0.92F * bore + driverOut * 0.7F;
            } else { // Jet (flute / recorder)
                // Air-jet edge tone: delayed feedback + nonlinear
                const float jet = state.jetDelay;
                state.jetDelay = bore * 0.85F + breath * 0.15F;
                const float edge = std::tanh((breath * 1.2F - jet) * (1.5F + emb));
                driverOut = edge * (0.4F + 0.5F * breath);
                // Flute is open-open-ish → inversion + lower reflection
                writeValue = -0.88F * bore + driverOut * 0.65F;
            }

            // Breath noise (turbulence)
            if (osc.physicalBreathNoise > 0.001F && breath > 0.02F) {
                state.noiseState ^= state.noiseState << 13U;
                state.noiseState ^= state.noiseState >> 17U;
                state.noiseState ^= state.noiseState << 5U;
                const float n = static_cast<float>(static_cast<std::int32_t>(state.noiseState))
                                * (1.0F / static_cast<float>(std::numeric_limits<std::int32_t>::max()));
                writeValue += n * osc.physicalBreathNoise * breath * 0.22F;
            }
        } else {
            // Original string / plate feedback
            const float feedback = 0.96F - damping * 0.22F;
            writeValue = feedbackSample * feedback;
            if (state.continuousLevel > 0.001F) {
                state.noiseState ^= state.noiseState << 13U;
                state.noiseState ^= state.noiseState >> 17U;
                state.noiseState ^= state.noiseState << 5U;
                const float noise = static_cast<float>(static_cast<std::int32_t>(state.noiseState))
                                    * (1.0F / static_cast<float>(std::numeric_limits<std::int32_t>::max()));
                writeValue += noise * state.continuousLevel * 0.08F * (0.4F + 0.6F * brightness);
            }
        }

        state.delay[state.writeIndex] = writeValue;
        state.writeIndex = (state.writeIndex + 1U) % kPhysicalModelMaxDelay;
        waveguideOut = 0.72F * pickupSample + 0.28F * feedbackSample;
        state.lastOutput = waveguideOut;
    }

    // ------------------------------------------------------------------
    // Modal path (plates / hybrids)
    // ------------------------------------------------------------------
    if (osc.physicalModel == PhysicalModelType::ModalPlate || osc.physicalModel == PhysicalModelType::Hybrid) {
        float exc = 0.0F;
        if (state.continuousLevel > 0.001F || state.excitationEnvelope > 0.001F) {
            state.noiseState = state.noiseState * 1664525U + 1013904223U;
            const float n = static_cast<float>(static_cast<std::int32_t>(state.noiseState & 0xFFFFU)) / 32768.0F - 1.0F;
            exc = n * (state.continuousLevel * 0.04F + state.excitationEnvelope * 0.12F);
            state.excitationEnvelope *= 0.9992F;
        }
        for (std::uint8_t m = 0; m < state.activeModes; ++m) {
            auto& mode = state.modes[m];
            const float rawFrequency = std::clamp(frequencyHz, 20.0F, sr * 0.45F) * mode.frequencyRatio;
            if (rawFrequency >= sr * 0.45F || mode.gain <= 0.0F) continue;
            mode.freqHz = rawFrequency;
            const float w = 2.0F * 3.14159265F * mode.freqHz / sr;
            const float r = mode.decayCoeff;
            const float cosw = std::cos(w);
            const float y = 2.0F * r * cosw * mode.y1 - r * r * mode.y2 + exc * mode.gain;
            mode.y2 = mode.y1;
            mode.y1 = y;
            modalOut += y * mode.gain;
        }
        modalOut = std::tanh(modalOut * 0.85F);
    }

    float out = 0.0F;
    if (isWind && osc.physicalDriver != PhysicalDriver::Disabled) {
        out = waveguideOut;
    } else if (osc.physicalModel == PhysicalModelType::WaveguideString) {
        out = waveguideOut;
    } else if (osc.physicalModel == PhysicalModelType::ModalPlate) {
        out = modalOut;
    } else {
        const float body = std::clamp(osc.physicalBodyAmount, 0.0F, 1.0F);
        out = waveguideOut * (1.0F - 0.55F * body) + modalOut * body;
    }

    // Throat / formant filter (important for wind realism)
    if (isWind && osc.physicalThroatFreqHz > 100.0F) {
        const float f = std::clamp(osc.physicalThroatFreqHz, 200.0F, 8000.0F);
        const float q = std::clamp(osc.physicalThroatQ, 0.5F, 8.0F);
        const float w = 2.0F * 3.14159265F * f / sr;
        const float alpha = std::sin(w) / (2.0F * q);
        const float b0 = alpha;
        const float b1 = 0.0F;
        const float b2 = -alpha;
        const float a0 = 1.0F + alpha;
        const float a1 = -2.0F * std::cos(w);
        const float a2 = 1.0F - alpha;
        // Direct-form-I biquad. The imported code reused two variables as both input and
        // output history, which was not the stated transfer function and could ring incorrectly.
        const float x = out;
        const float y = (b0 / a0) * x + (b1 / a0) * state.throatX1
                        + (b2 / a0) * state.throatX2 - (a1 / a0) * state.throatY1
                        - (a2 / a0) * state.throatY2;
        state.throatX2 = state.throatX1;
        state.throatX1 = x;
        state.throatY2 = state.throatY1;
        state.throatY1 = y;
        // Mix formant with dry
        out = out * 0.55F + y * 0.45F;
    }

    return std::tanh(out * 1.15F);
}

} // namespace physical

bool finite(float value) noexcept { return std::isfinite(value); }

bool validate_adsr(const AdsrParameters& p) noexcept {
    return finite(p.attackSeconds) && finite(p.decaySeconds) && finite(p.sustainLevel) && finite(p.releaseSeconds) &&
           finite(p.delaySeconds) && finite(p.holdSeconds) &&
           p.attackSeconds >= 0.0F && p.attackSeconds <= 60.0F && p.decaySeconds >= 0.0F && p.decaySeconds <= 60.0F &&
           p.sustainLevel >= 0.0F && p.sustainLevel <= 1.0F && p.releaseSeconds >= 0.0F && p.releaseSeconds <= 60.0F &&
           p.delaySeconds >= 0.0F && p.delaySeconds <= 60.0F && p.holdSeconds >= 0.0F && p.holdSeconds <= 60.0F;
}

template <class T>
bool parse_number(std::string_view text, T& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

std::vector<std::pair<std::string, std::string>> parse_lines(std::string_view text) {
    std::vector<std::pair<std::string, std::string>> result;
    std::istringstream input{std::string(text)};
    std::string line;
    while (std::getline(input, line)) {
        const auto equals = line.find('=');
        if (equals != std::string::npos) result.emplace_back(line.substr(0, equals), line.substr(equals + 1U));
    }
    return result;
}

std::string waveform_name(OscillatorWaveform waveform) {
    switch (waveform) {
        case OscillatorWaveform::Sine: return "sine";
        case OscillatorWaveform::Saw: return "saw";
        case OscillatorWaveform::Square: return "square";
        case OscillatorWaveform::Triangle: return "triangle";
        case OscillatorWaveform::Pulse: return "pulse";
        case OscillatorWaveform::Noise: return "noise";
        case OscillatorWaveform::SuperSaw: return "supersaw";
        case OscillatorWaveform::Organ: return "organ";
        case OscillatorWaveform::FoldedSine: return "foldedsine";
        case OscillatorWaveform::Digital: return "digital";
        case OscillatorWaveform::Wavetable: return "wavetable";
        case OscillatorWaveform::Sample: return "sample";
        case OscillatorWaveform::Granular: return "granular";
        case OscillatorWaveform::PhysicalModel: return "physicalmodel";
    }
    return "saw";
}
std::optional<OscillatorWaveform> parse_waveform(std::string_view value) {
    if (value == "sine") return OscillatorWaveform::Sine;
    if (value == "saw") return OscillatorWaveform::Saw;
    if (value == "square") return OscillatorWaveform::Square;
    if (value == "triangle") return OscillatorWaveform::Triangle;
    if (value == "pulse") return OscillatorWaveform::Pulse;
    if (value == "noise") return OscillatorWaveform::Noise;
    if (value == "supersaw") return OscillatorWaveform::SuperSaw;
    if (value == "organ") return OscillatorWaveform::Organ;
    if (value == "foldedsine") return OscillatorWaveform::FoldedSine;
    if (value == "digital") return OscillatorWaveform::Digital;
    if (value == "wavetable") return OscillatorWaveform::Wavetable;
    if (value == "sample") return OscillatorWaveform::Sample;
    if (value == "granular") return OscillatorWaveform::Granular;
    if (value == "physicalmodel") return OscillatorWaveform::PhysicalModel;
    return std::nullopt;
}

std::string_view physical_model_token(PhysicalModelType t) noexcept {
    switch (t) {
        case PhysicalModelType::WaveguideString: return "waveguide";
        case PhysicalModelType::ModalPlate: return "modal";
        case PhysicalModelType::Hybrid: return "hybrid";
    }
    return "hybrid";
}
std::optional<PhysicalModelType> parse_physical_model(std::string_view value) noexcept {
    if (value == "waveguide") return PhysicalModelType::WaveguideString;
    if (value == "modal") return PhysicalModelType::ModalPlate;
    if (value == "hybrid") return PhysicalModelType::Hybrid;
    return std::nullopt;
}
std::string_view physical_material_token(PhysicalMaterial m) noexcept {
    switch (m) {
        case PhysicalMaterial::String: return "string";
        case PhysicalMaterial::Metal: return "metal";
        case PhysicalMaterial::Brass: return "brass";
        case PhysicalMaterial::Glass: return "glass";
        case PhysicalMaterial::Wood: return "wood";
        case PhysicalMaterial::Membrane: return "membrane";
        case PhysicalMaterial::Synthetic: return "synthetic";
    }
    return "string";
}
std::optional<PhysicalMaterial> parse_physical_material(std::string_view value) noexcept {
    if (value == "string") return PhysicalMaterial::String;
    if (value == "metal") return PhysicalMaterial::Metal;
    if (value == "brass") return PhysicalMaterial::Brass;
    if (value == "glass") return PhysicalMaterial::Glass;
    if (value == "wood") return PhysicalMaterial::Wood;
    if (value == "membrane") return PhysicalMaterial::Membrane;
    if (value == "synthetic") return PhysicalMaterial::Synthetic;
    return std::nullopt;
}
std::string_view physical_excitation_token(PhysicalExcitation e) noexcept {
    switch (e) {
        case PhysicalExcitation::Pluck: return "pluck";
        case PhysicalExcitation::Strike: return "strike";
        case PhysicalExcitation::Bow: return "bow";
        case PhysicalExcitation::Blow: return "blow";
    }
    return "pluck";
}
std::optional<PhysicalExcitation> parse_physical_excitation(std::string_view value) noexcept {
    if (value == "pluck") return PhysicalExcitation::Pluck;
    if (value == "strike") return PhysicalExcitation::Strike;
    if (value == "bow") return PhysicalExcitation::Bow;
    if (value == "blow") return PhysicalExcitation::Blow;
    return std::nullopt;
}

std::string_view physical_driver_token(PhysicalDriver d) noexcept {
    switch (d) {
        case PhysicalDriver::Disabled: return "none";
        case PhysicalDriver::Reed: return "reed";
        case PhysicalDriver::Lip: return "lip";
        case PhysicalDriver::Jet: return "jet";
    }
    return "none";
}
std::optional<PhysicalDriver> parse_physical_driver(std::string_view value) noexcept {
    if (value == "none") return PhysicalDriver::Disabled;
    if (value == "reed") return PhysicalDriver::Reed;
    if (value == "lip") return PhysicalDriver::Lip;
    if (value == "jet") return PhysicalDriver::Jet;
    return std::nullopt;
}

std::string_view filter_mode_token(FilterMode mode) noexcept {
    switch (mode) {
        case FilterMode::LowPass: return "lowpass";
        case FilterMode::BandPass: return "bandpass";
        case FilterMode::HighPass: return "highpass";
        case FilterMode::Notch: return "notch";
    }
    return "lowpass";
}
std::optional<FilterMode> parse_filter_mode(std::string_view value) noexcept {
    if (value == "lowpass") return FilterMode::LowPass;
    if (value == "bandpass") return FilterMode::BandPass;
    if (value == "highpass") return FilterMode::HighPass;
    if (value == "notch") return FilterMode::Notch;
    return std::nullopt;
}
std::string_view filter_topology_token(FilterTopology topology) noexcept {
    switch (topology) {
        case FilterTopology::CleanStateVariable: return "clean";
        case FilterTopology::MoogLadder: return "moog_ladder";
        case FilterTopology::KorgMs20: return "korg_ms20";
        case FilterTopology::OberheimSem: return "oberheim_sem";
    }
    return "clean";
}
std::optional<FilterTopology> parse_filter_topology(std::string_view value) noexcept {
    if (value == "clean") return FilterTopology::CleanStateVariable;
    if (value == "moog_ladder") return FilterTopology::MoogLadder;
    if (value == "korg_ms20") return FilterTopology::KorgMs20;
    if (value == "oberheim_sem") return FilterTopology::OberheimSem;
    return std::nullopt;
}
std::string_view arpeggiator_mode_token(ArpeggiatorMode mode) noexcept {
    switch (mode) {
        case ArpeggiatorMode::Up: return "up";
        case ArpeggiatorMode::Down: return "down";
        case ArpeggiatorMode::UpDown: return "updown";
        case ArpeggiatorMode::DownUp: return "downup";
        case ArpeggiatorMode::Played: return "played";
        case ArpeggiatorMode::Random: return "random";
        case ArpeggiatorMode::Chord: return "chord";
    }
    return "up";
}
std::optional<ArpeggiatorMode> parse_arpeggiator_mode(std::string_view value) noexcept {
    if (value == "up") return ArpeggiatorMode::Up;
    if (value == "down") return ArpeggiatorMode::Down;
    if (value == "updown") return ArpeggiatorMode::UpDown;
    if (value == "downup") return ArpeggiatorMode::DownUp;
    if (value == "played") return ArpeggiatorMode::Played;
    if (value == "random") return ArpeggiatorMode::Random;
    if (value == "chord") return ArpeggiatorMode::Chord;
    return std::nullopt;
}
std::string_view arpeggiator_division_token(ArpeggiatorDivision division) noexcept {
    switch (division) {
        case ArpeggiatorDivision::Quarter: return "1/4";
        case ArpeggiatorDivision::Eighth: return "1/8";
        case ArpeggiatorDivision::EighthTriplet: return "1/8T";
        case ArpeggiatorDivision::Sixteenth: return "1/16";
        case ArpeggiatorDivision::SixteenthTriplet: return "1/16T";
        case ArpeggiatorDivision::ThirtySecond: return "1/32";
    }
    return "1/16";
}
std::optional<ArpeggiatorDivision> parse_arpeggiator_division(std::string_view value) noexcept {
    if (value == "1/4") return ArpeggiatorDivision::Quarter;
    if (value == "1/8") return ArpeggiatorDivision::Eighth;
    if (value == "1/8T") return ArpeggiatorDivision::EighthTriplet;
    if (value == "1/16") return ArpeggiatorDivision::Sixteenth;
    if (value == "1/16T") return ArpeggiatorDivision::SixteenthTriplet;
    if (value == "1/32") return ArpeggiatorDivision::ThirtySecond;
    return std::nullopt;
}
std::string_view chord_type_token(ChordType type) noexcept {
    switch (type) {
        case ChordType::Custom: return "custom";
        case ChordType::Major: return "major";
        case ChordType::Minor: return "minor";
        case ChordType::Diminished: return "diminished";
        case ChordType::Augmented: return "augmented";
        case ChordType::Sus2: return "sus2";
        case ChordType::Sus4: return "sus4";
        case ChordType::Fifth: return "fifth";
        case ChordType::Major6: return "major6";
        case ChordType::Minor6: return "minor6";
        case ChordType::Dominant7: return "dominant7";
        case ChordType::Major7: return "major7";
        case ChordType::Minor7: return "minor7";
        case ChordType::Diminished7: return "diminished7";
        case ChordType::Add9: return "add9";
        case ChordType::Minor9: return "minor9";
    }
    return "major";
}
std::optional<ChordType> parse_chord_type(std::string_view value) noexcept {
    if (value == "custom") return ChordType::Custom;
    if (value == "major") return ChordType::Major;
    if (value == "minor") return ChordType::Minor;
    if (value == "diminished") return ChordType::Diminished;
    if (value == "augmented") return ChordType::Augmented;
    if (value == "sus2") return ChordType::Sus2;
    if (value == "sus4") return ChordType::Sus4;
    if (value == "fifth") return ChordType::Fifth;
    if (value == "major6") return ChordType::Major6;
    if (value == "minor6") return ChordType::Minor6;
    if (value == "dominant7") return ChordType::Dominant7;
    if (value == "major7") return ChordType::Major7;
    if (value == "minor7") return ChordType::Minor7;
    if (value == "diminished7") return ChordType::Diminished7;
    if (value == "add9") return ChordType::Add9;
    if (value == "minor9") return ChordType::Minor9;
    return std::nullopt;
}
std::string_view envelope_curve_name(EnvelopeCurve curve) noexcept {
    return curve == EnvelopeCurve::Linear ? "linear" : "exponential";
}
std::optional<EnvelopeCurve> parse_envelope_curve(std::string_view value) noexcept {
    if (value == "linear") return EnvelopeCurve::Linear;
    if (value == "exponential") return EnvelopeCurve::Exponential;
    return std::nullopt;
}


std::string_view lfo_waveform_token(LfoWaveform waveform) noexcept {
    switch (waveform) {
        case LfoWaveform::Sine: return "sine";
        case LfoWaveform::Triangle: return "triangle";
        case LfoWaveform::Saw: return "saw";
        case LfoWaveform::Square: return "square";
        case LfoWaveform::SampleAndHold: return "sample_hold";
        case LfoWaveform::SmoothRandom: return "smooth_random";
    }
    return "sine";
}
std::optional<LfoWaveform> parse_lfo_waveform(std::string_view value) noexcept {
    if (value == "sine") return LfoWaveform::Sine;
    if (value == "triangle") return LfoWaveform::Triangle;
    if (value == "saw") return LfoWaveform::Saw;
    if (value == "square") return LfoWaveform::Square;
    if (value == "sample_hold") return LfoWaveform::SampleAndHold;
    if (value == "smooth_random") return LfoWaveform::SmoothRandom;
    return std::nullopt;
}
std::string_view modulation_source_token(ModulationSource source) noexcept {
    static constexpr std::array<std::string_view, 14> names{
        "none","lfo1","lfo2","amp_env","filter_env","velocity","keytrack",
        "modwheel","aftertouch","random","macro1","macro2","macro3","macro4"};
    const auto index = static_cast<std::size_t>(source);
    return index < names.size() ? names[index] : names[0];
}
std::optional<ModulationSource> parse_modulation_source(std::string_view value) noexcept {
    for (std::size_t i = 0; i <= static_cast<std::size_t>(ModulationSource::Macro4); ++i)
        if (modulation_source_token(static_cast<ModulationSource>(i)) == value) return static_cast<ModulationSource>(i);
    return std::nullopt;
}
std::string_view modulation_destination_token(ModulationDestination destination) noexcept {
    static constexpr std::array<std::string_view, 39> names{
        "none","global_pitch","filter_cutoff","filter_resonance","filter_drive","voice_gain","voice_pan",
        "osc1_pitch","osc2_pitch","osc3_pitch","osc4_pitch","osc5_pitch","osc6_pitch","osc7_pitch","osc8_pitch",
        "osc1_shape","osc2_shape","osc3_shape","osc4_shape","osc5_shape","osc6_shape","osc7_shape","osc8_shape",
        "osc1_pw","osc2_pw","osc3_pw","osc4_pw","osc5_pw","osc6_pw","osc7_pw","osc8_pw",
        "osc1_gain","osc2_gain","osc3_gain","osc4_gain","osc5_gain","osc6_gain","osc7_gain","osc8_gain"};
    const auto index = static_cast<std::size_t>(destination);
    return index < names.size() ? names[index] : names[0];
}
std::optional<ModulationDestination> parse_modulation_destination(std::string_view value) noexcept {
    for (std::size_t i = 0; i <= static_cast<std::size_t>(ModulationDestination::Osc8Gain); ++i)
        if (modulation_destination_token(static_cast<ModulationDestination>(i)) == value) return static_cast<ModulationDestination>(i);
    return std::nullopt;
}
std::string_view modulation_curve_token(ModulationCurve curve) noexcept {
    switch (curve) { case ModulationCurve::Linear: return "linear"; case ModulationCurve::Quadratic: return "quadratic"; case ModulationCurve::Cubic: return "cubic"; }
    return "linear";
}
std::optional<ModulationCurve> parse_modulation_curve(std::string_view value) noexcept {
    if (value == "linear") return ModulationCurve::Linear;
    if (value == "quadratic") return ModulationCurve::Quadratic;
    if (value == "cubic") return ModulationCurve::Cubic;
    return std::nullopt;
}
std::string_view fm_mode_token(FrequencyModulationMode mode) noexcept {
    switch (mode) { case FrequencyModulationMode::Off: return "off"; case FrequencyModulationMode::Linear: return "linear"; case FrequencyModulationMode::Exponential: return "exponential"; }
    return "off";
}
std::optional<FrequencyModulationMode> parse_fm_mode(std::string_view value) noexcept {
    if (value == "off") return FrequencyModulationMode::Off;
    if (value == "linear") return FrequencyModulationMode::Linear;
    if (value == "exponential") return FrequencyModulationMode::Exponential;
    return std::nullopt;
}
std::string_view chord_scale_token(ChordScale scale) noexcept {
    switch (scale) {
        case ChordScale::Chromatic: return "chromatic"; case ChordScale::Major: return "major";
        case ChordScale::NaturalMinor: return "natural_minor"; case ChordScale::HarmonicMinor: return "harmonic_minor";
        case ChordScale::Dorian: return "dorian"; case ChordScale::Mixolydian: return "mixolydian";
        case ChordScale::Pentatonic: return "pentatonic";
    }
    return "chromatic";
}
std::optional<ChordScale> parse_chord_scale(std::string_view value) noexcept {
    for (std::size_t i = 0; i <= static_cast<std::size_t>(ChordScale::Pentatonic); ++i)
        if (chord_scale_token(static_cast<ChordScale>(i)) == value) return static_cast<ChordScale>(i);
    return std::nullopt;
}
std::string_view arp_clock_token(ArpeggiatorClockSource source) noexcept {
    switch (source) { case ArpeggiatorClockSource::Internal: return "internal"; case ArpeggiatorClockSource::GameClock: return "game"; case ArpeggiatorClockSource::MidiClock: return "midi"; }
    return "internal";
}
std::optional<ArpeggiatorClockSource> parse_arp_clock(std::string_view value) noexcept {
    if (value == "internal") return ArpeggiatorClockSource::Internal;
    if (value == "game") return ArpeggiatorClockSource::GameClock;
    if (value == "midi") return ArpeggiatorClockSource::MidiClock;
    return std::nullopt;
}

struct ChordVoicing {
    std::array<std::uint8_t, kChordIntervalCount> notes{};
    std::uint8_t count{};
};

int quantize_note_to_scale(int note, ChordScale scale, std::uint8_t root) noexcept {
    if (scale == ChordScale::Chromatic) return std::clamp(note, 0, 127);
    static constexpr std::array<std::array<int, 7>, 6> scales{{
        {{0,2,4,5,7,9,11}}, {{0,2,3,5,7,8,10}}, {{0,2,3,5,7,8,11}},
        {{0,2,3,5,7,9,10}}, {{0,2,4,5,7,9,10}}, {{0,2,4,7,9,-1,-1}}
    }};
    const std::size_t index = static_cast<std::size_t>(scale) - 1U;
    const auto& degrees = scales[std::min(index, scales.size() - 1U)];
    int best = note;
    int bestDistance = 99;
    for (int octave = -2; octave <= 2; ++octave) {
        for (int degree : degrees) {
            if (degree < 0) continue;
            const int candidate = static_cast<int>(root % 12U) + degree + octave * 12 + (note / 12) * 12;
            const int distance = std::abs(candidate - note);
            if (distance < bestDistance || (distance == bestDistance && candidate > best)) {
                best = candidate; bestDistance = distance;
            }
        }
    }
    return std::clamp(best, 0, 127);
}

ChordVoicing make_chord_voicing(std::uint8_t root, const ChordParameters& parameters) noexcept {
    std::array<int, kChordIntervalCount> intervals{};
    std::uint8_t count = parameters.noteCount;
    switch (parameters.type) {
        case ChordType::Custom:
            for (std::size_t i = 0; i < intervals.size(); ++i) intervals[i] = parameters.customIntervals[i];
            break;
        case ChordType::Major: intervals = {0,4,7,0,0,0,0,0}; count = 3; break;
        case ChordType::Minor: intervals = {0,3,7,0,0,0,0,0}; count = 3; break;
        case ChordType::Diminished: intervals = {0,3,6,0,0,0,0,0}; count = 3; break;
        case ChordType::Augmented: intervals = {0,4,8,0,0,0,0,0}; count = 3; break;
        case ChordType::Sus2: intervals = {0,2,7,0,0,0,0,0}; count = 3; break;
        case ChordType::Sus4: intervals = {0,5,7,0,0,0,0,0}; count = 3; break;
        case ChordType::Fifth: intervals = {0,7,12,0,0,0,0,0}; count = 3; break;
        case ChordType::Major6: intervals = {0,4,7,9,0,0,0,0}; count = 4; break;
        case ChordType::Minor6: intervals = {0,3,7,9,0,0,0,0}; count = 4; break;
        case ChordType::Dominant7: intervals = {0,4,7,10,0,0,0,0}; count = 4; break;
        case ChordType::Major7: intervals = {0,4,7,11,0,0,0,0}; count = 4; break;
        case ChordType::Minor7: intervals = {0,3,7,10,0,0,0,0}; count = 4; break;
        case ChordType::Diminished7: intervals = {0,3,6,9,0,0,0,0}; count = 4; break;
        case ChordType::Add9: intervals = {0,4,7,14,0,0,0,0}; count = 4; break;
        case ChordType::Minor9: intervals = {0,3,7,10,14,0,0,0}; count = 5; break;
    }
    count = static_cast<std::uint8_t>(std::clamp<unsigned>(count, 1U, static_cast<unsigned>(kChordIntervalCount)));
    const auto sort_intervals = [&]() noexcept {
        for (std::size_t i = 1; i < static_cast<std::size_t>(count); ++i) {
            const int value = intervals[i];
            std::size_t j = i;
            while (j > 0U && intervals[j - 1U] > value) {
                intervals[j] = intervals[j - 1U];
                --j;
            }
            intervals[j] = value;
        }
    };
    sort_intervals();
    int inversion = parameters.inversion;
    while (inversion > 0) {
        const int first = intervals[0] + 12;
        for (std::size_t i = 1; i < count; ++i) intervals[i - 1U] = intervals[i];
        intervals[count - 1U] = first;
        --inversion;
    }
    while (inversion < 0) {
        const int last = intervals[count - 1U] - 12;
        for (std::size_t i = count - 1U; i > 0U; --i) intervals[i] = intervals[i - 1U];
        intervals[0] = last;
        ++inversion;
    }
    sort_intervals();
    if (parameters.spreadOctaves > 0U && count > 1U) {
        for (std::size_t i = 1; i < count; ++i) {
            const unsigned numerator = static_cast<unsigned>(i) * parameters.spreadOctaves;
            intervals[i] += 12 * static_cast<int>(numerator / static_cast<unsigned>(count - 1U));
        }
    }
    ChordVoicing result;
    for (std::size_t i = 0; i < count; ++i) {
        const int note = quantize_note_to_scale(static_cast<int>(root) + intervals[i], parameters.scale,
                                                parameters.scaleRoot);
        result.notes[i] = static_cast<std::uint8_t>(note);
    }
    result.count = count;
    return result;
}

float arpeggiator_step_beats(ArpeggiatorDivision division) noexcept {
    switch (division) {
        case ArpeggiatorDivision::Quarter: return 1.0F;
        case ArpeggiatorDivision::Eighth: return 0.5F;
        case ArpeggiatorDivision::EighthTriplet: return 1.0F / 3.0F;
        case ArpeggiatorDivision::Sixteenth: return 0.25F;
        case ArpeggiatorDivision::SixteenthTriplet: return 1.0F / 6.0F;
        case ArpeggiatorDivision::ThirtySecond: return 0.125F;
    }
    return 0.25F;
}

} // namespace

std::string_view oscillator_waveform_name(OscillatorWaveform waveform) noexcept {
    switch (waveform) {
        case OscillatorWaveform::Sine: return "Sine";
        case OscillatorWaveform::Saw: return "Saw";
        case OscillatorWaveform::Square: return "Square";
        case OscillatorWaveform::Triangle: return "Triangle";
        case OscillatorWaveform::Pulse: return "Pulse";
        case OscillatorWaveform::Noise: return "Noise";
        case OscillatorWaveform::SuperSaw: return "SuperSaw";
        case OscillatorWaveform::Organ: return "Organ";
        case OscillatorWaveform::FoldedSine: return "Folded Sine";
        case OscillatorWaveform::Digital: return "Digital";
        case OscillatorWaveform::Wavetable: return "Wavetable";
        case OscillatorWaveform::Sample: return "Sample";
        case OscillatorWaveform::Granular: return "Granular";
        case OscillatorWaveform::PhysicalModel: return "Physical Model";
    }
    return "Saw";
}
std::string_view filter_topology_name(FilterTopology topology) noexcept {
    switch (topology) {
        case FilterTopology::CleanStateVariable: return "Clean SVF";
        case FilterTopology::MoogLadder: return "Moog Ladder";
        case FilterTopology::KorgMs20: return "Korg MS-20";
        case FilterTopology::OberheimSem: return "Oberheim SEM";
    }
    return "Clean SVF";
}
std::string_view filter_mode_name(FilterMode mode) noexcept {
    switch (mode) {
        case FilterMode::LowPass: return "Low-pass";
        case FilterMode::BandPass: return "Band-pass";
        case FilterMode::HighPass: return "High-pass";
        case FilterMode::Notch: return "Notch";
    }
    return "Low-pass";
}
std::string_view arpeggiator_mode_name(ArpeggiatorMode mode) noexcept {
    switch (mode) {
        case ArpeggiatorMode::Up: return "Up";
        case ArpeggiatorMode::Down: return "Down";
        case ArpeggiatorMode::UpDown: return "Up/Down";
        case ArpeggiatorMode::DownUp: return "Down/Up";
        case ArpeggiatorMode::Played: return "Played";
        case ArpeggiatorMode::Random: return "Random";
        case ArpeggiatorMode::Chord: return "Chord";
    }
    return "Up";
}
std::string_view arpeggiator_division_name(ArpeggiatorDivision division) noexcept {
    return arpeggiator_division_token(division);
}
std::string_view chord_type_name(ChordType type) noexcept {
    switch (type) {
        case ChordType::Custom: return "Custom";
        case ChordType::Major: return "Major";
        case ChordType::Minor: return "Minor";
        case ChordType::Diminished: return "Diminished";
        case ChordType::Augmented: return "Augmented";
        case ChordType::Sus2: return "Sus2";
        case ChordType::Sus4: return "Sus4";
        case ChordType::Fifth: return "Fifth";
        case ChordType::Major6: return "Major 6";
        case ChordType::Minor6: return "Minor 6";
        case ChordType::Dominant7: return "Dominant 7";
        case ChordType::Major7: return "Major 7";
        case ChordType::Minor7: return "Minor 7";
        case ChordType::Diminished7: return "Diminished 7";
        case ChordType::Add9: return "Add 9";
        case ChordType::Minor9: return "Minor 9";
    }
    return "Major";
}


ChordDetection detect_chord(std::span<const std::uint8_t> notes) noexcept {
    ChordDetection result;
    if (notes.empty()) return result;
    std::array<bool, 12> present{};
    std::uint8_t lowest = 127U;
    for (const std::uint8_t note : notes) {
        if (note > 127U) continue;
        present[note % 12U] = true;
        lowest = std::min(lowest, note);
    }
    const std::size_t uniqueCount = static_cast<std::size_t>(std::count(present.begin(), present.end(), true));
    if (uniqueCount < 2U) return result;
    struct Pattern { ChordType type; std::array<int, 5> intervals; std::uint8_t count; };
    static constexpr std::array patterns{
        Pattern{ChordType::Major, {0,4,7,0,0}, 3}, Pattern{ChordType::Minor, {0,3,7,0,0}, 3},
        Pattern{ChordType::Diminished, {0,3,6,0,0}, 3}, Pattern{ChordType::Augmented, {0,4,8,0,0}, 3},
        Pattern{ChordType::Sus2, {0,2,7,0,0}, 3}, Pattern{ChordType::Sus4, {0,5,7,0,0}, 3},
        Pattern{ChordType::Fifth, {0,7,0,0,0}, 2}, Pattern{ChordType::Major6, {0,4,7,9,0}, 4},
        Pattern{ChordType::Minor6, {0,3,7,9,0}, 4}, Pattern{ChordType::Dominant7, {0,4,7,10,0}, 4},
        Pattern{ChordType::Major7, {0,4,7,11,0}, 4}, Pattern{ChordType::Minor7, {0,3,7,10,0}, 4},
        Pattern{ChordType::Diminished7, {0,3,6,9,0}, 4}, Pattern{ChordType::Add9, {0,2,4,7,0}, 4},
        Pattern{ChordType::Minor9, {0,2,3,7,10}, 5},
    };
    float bestScore = 0.0F;
    int bestRoot = 0;
    const Pattern* bestPattern = nullptr;
    for (int root = 0; root < 12; ++root) {
        for (const Pattern& pattern : patterns) {
            std::array<bool, 12> expected{};
            for (std::size_t i = 0; i < pattern.count; ++i)
                expected[static_cast<std::size_t>((root + pattern.intervals[i]) % 12)] = true;
            std::size_t intersection = 0U;
            std::size_t unionCount = 0U;
            for (std::size_t pc = 0; pc < 12U; ++pc) {
                if (present[pc] && expected[pc]) ++intersection;
                if (present[pc] || expected[pc]) ++unionCount;
            }
            const float score = unionCount == 0U ? 0.0F :
                static_cast<float>(intersection) / static_cast<float>(unionCount);
            if (score > bestScore) { bestScore = score; bestRoot = root; bestPattern = &pattern; }
        }
    }
    if (bestPattern == nullptr || bestScore < 0.60F) return result;
    result.matched = true;
    result.type = bestPattern->type;
    result.rootPitchClass = static_cast<std::uint8_t>(bestRoot);
    result.confidence = bestScore;
    const int bassPc = static_cast<int>(lowest % 12U);
    result.inversion = 0;
    for (std::size_t i = 0; i < bestPattern->count; ++i) {
        if ((bestRoot + bestPattern->intervals[i]) % 12 == bassPc) {
            result.inversion = static_cast<std::int8_t>(i);
            break;
        }
    }
    return result;
}

std::string_view lfo_waveform_name(LfoWaveform waveform) noexcept {
    switch (waveform) {
        case LfoWaveform::Sine: return "Sine";
        case LfoWaveform::Triangle: return "Triangle";
        case LfoWaveform::Saw: return "Saw";
        case LfoWaveform::Square: return "Square";
        case LfoWaveform::SampleAndHold: return "Sample & Hold";
        case LfoWaveform::SmoothRandom: return "Smooth Random";
    }
    return "Sine";
}
std::string_view modulation_source_name(ModulationSource source) noexcept { return modulation_source_token(source); }
std::string_view modulation_destination_name(ModulationDestination destination) noexcept { return modulation_destination_token(destination); }


struct Synthesizer::Impl {
    struct VoiceTelemetry {
        std::atomic<bool> active{};
        std::atomic<std::uint8_t> channel{};
        std::atomic<std::uint8_t> note{};
        std::atomic<float> velocity{};
        std::atomic<float> envelope{};
        std::atomic<float> pressure{};
        std::atomic<float> timbre{};
        std::atomic<float> pitchBendSemitones{};
        std::atomic<VoiceStage> stage{VoiceStage::Idle};
        std::atomic<std::uint64_t> age{};
    };

    struct HeldNote {
        bool active{};
        bool keyHeld{};
        bool sustained{};
        std::uint8_t channel{};
        std::uint8_t note{};
        std::uint8_t velocity{};
        std::uint64_t order{};
    };

    struct ChordTrigger {
        bool active{};
        std::uint8_t channel{};
        std::uint8_t root{};
        std::uint8_t count{};
        std::array<std::uint8_t, kChordIntervalCount> notes{};
    };

    explicit Impl(std::uint32_t rate, std::atomic<std::uint64_t>& frameCounter)
        : sampleRate(static_cast<float>(rate)), currentFrame(frameCounter),
          chorusL(static_cast<std::size_t>(rate / 10U + 32U)), chorusR(static_cast<std::size_t>(rate / 10U + 32U)),
          delayL(static_cast<std::size_t>(rate * 2U + 2U)), delayR(static_cast<std::size_t>(rate * 2U + 2U)),
          reverb(rate) {}

    float sampleRate{};
    std::atomic<std::uint64_t>& currentFrame;
    RealtimePreset parameters{};
    BoundedQueue<MidiMessage, kMidiQueueCapacity> midiIn;
    BoundedQueue<MidiMessage, kMidiOutQueueCapacity> midiOut;
    BoundedQueue<RealtimePreset, kPresetQueueCapacity> presetIn;
    std::mutex sampleMapPublishMutex;
    std::array<SynthSampleMap, 3> sampleMaps{};
    std::atomic<int> activeSampleMapIndex{0};
    std::atomic<int> pendingSampleMapIndex{-1};
    std::atomic<std::uint64_t> controlSampleMapGeneration{};
    std::uint64_t activeSampleMapGeneration{};
    SampleStreamCache sampleStreamCache{};
    std::array<std::uint32_t, 16> sampleRoundRobinCounters{};
    std::atomic<std::uint64_t> requestedGrains{};
    std::atomic<std::uint64_t> admittedGrains{};
    std::atomic<std::uint64_t> grainSteals{};
    std::atomic<std::uint64_t> grainMisses{};
    std::atomic<std::uint64_t> samplePageUnderruns{};
    std::atomic<std::uint32_t> activeGrainTelemetry{};
    std::atomic<std::uint32_t> maximumActiveGrains{};
    std::uint32_t activeGrains{};
    std::array<Voice, kSynthVoiceCount> voice{};
    std::array<VoiceTelemetry, kSynthVoiceCount> telemetry{};
    std::array<float, 16> pitchBend{};
    std::array<float, 16> channelPressure{};
    std::array<float, 16> channelTimbre{};
    std::array<bool, 16> sustain{};
    std::array<float, 128> controller{};
    std::array<float, kSynthMacroCount> macroValues{};
    std::atomic<float> gameClockTempo{120.0F};
    float midiClockTempo{120.0F};
    std::uint64_t lastMidiClockFrame{};
    std::uint32_t midiClockTickCount{};
    std::uint64_t ageCounter{};
    std::uint64_t renderFrame{};
    std::atomic<std::uint64_t> droppedMidi{};
    std::atomic<bool> arpeggiatorFill{};
    std::atomic<bool> transportRestartRequested{};
    std::atomic<std::uint64_t> transportRestartFrame{};
    std::array<float, kSynthModulationSlotCount> modulationScratch{};
    std::array<std::atomic<float>, kSynthModulationSlotCount> modulationTelemetry{};

    std::array<HeldNote, 16U * 128U> arpHeld{};
    std::array<ChordTrigger, kSynthVoiceCount> chordTriggers{};
    std::array<std::uint8_t, kSynthVoiceCount> arpActiveNotes{};
    std::array<std::uint8_t, kSynthVoiceCount> arpActiveChannels{};
    std::uint8_t arpActiveCount{};
    std::uint64_t arpOrderCounter{};
    std::uint64_t nextArpFrame{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t arpGateOffFrame{std::numeric_limits<std::uint64_t>::max()};
    std::uint32_t arpProgress{};
    std::uint32_t arpStepCounter{};
    std::uint32_t arpRandomState{0x51A3D8E7U};
    std::uint8_t arpRatchetCount{1};
    std::uint8_t arpRatchetIndex{};
    std::uint64_t arpRatchetDuration{1};
    std::uint64_t nextRatchetFrame{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t arpStepEndFrame{std::numeric_limits<std::uint64_t>::max()};
    bool arpStepTie{};
    std::array<std::uint8_t, kSynthVoiceCount> arpPatternNotes{};
    std::array<std::uint8_t, kSynthVoiceCount> arpPatternChannels{};
    std::array<std::uint8_t, kSynthVoiceCount> arpPatternVelocities{};
    std::uint8_t arpPatternCount{};

    struct ScheduledVoiceEvent {
        bool active{};
        bool noteOn{};
        bool emitOutput{};
        std::uint8_t channel{};
        std::uint8_t note{};
        std::uint8_t velocity{};
        std::uint64_t frame{};
    };
    std::array<ScheduledVoiceEvent, 128> scheduledEvents{};

    DelayLine chorusL;
    DelayLine chorusR;
    float chorusPhase{};
    std::array<AllpassStage, 4> phaserL{};
    std::array<AllpassStage, 4> phaserR{};
    float phaserPhase{};
    float phaserFeedbackL{};
    float phaserFeedbackR{};
    DelayLine delayL;
    DelayLine delayR;
    ReverbState reverb;
    float eqLowL{}; float eqLowR{}; float eqHighL{}; float eqHighR{};
    float compressorEnvelope{};
    float limiterEnvelope{};

    std::atomic<float> peakL{}; std::atomic<float> peakR{};
    std::atomic<float> rmsL{}; std::atomic<float> rmsR{};
    std::atomic<std::uint32_t> activeVoiceCount{};
    std::atomic<std::uint32_t> heldArpNoteCount{};
    std::atomic<std::uint32_t> activeArpStep{};
    std::atomic<std::uint64_t> renderedFrames{};

    static std::size_t held_index(std::uint8_t channel, std::uint8_t note) noexcept {
        return static_cast<std::size_t>(channel & 0x0FU) * 128U + note;
    }

    Voice& allocate_voice(std::uint8_t channel, std::uint8_t note) noexcept {
        for (Voice& candidate : voice)
            if (candidate.active && candidate.channel == channel && candidate.note == note) return candidate;
        for (Voice& candidate : voice) if (!candidate.active) return candidate;
        auto released = std::min_element(voice.begin(), voice.end(), [](const Voice& a, const Voice& b) {
            const bool ar = a.amp.stage == VoiceStage::Release;
            const bool br = b.amp.stage == VoiceStage::Release;
            if (ar != br) return ar;
            if (a.amp.value != b.amp.value) return a.amp.value < b.amp.value;
            return a.age < b.age;
        });
        return *released;
    }

    void retire_voice_grains(Voice& target) noexcept {
        for (auto& oscillatorGrains : target.grains) {
            for (GrainState& grain : oscillatorGrains) {
                if (!grain.active) continue;
                grain.active = false;
                if (activeGrains > 0U) --activeGrains;
            }
        }
        activeGrainTelemetry.store(activeGrains, std::memory_order_relaxed);
    }

    void stage_sample_map(SynthSampleMap map) {
        std::lock_guard lock(sampleMapPublishMutex);
        const std::uint64_t generation = controlSampleMapGeneration.fetch_add(1U, std::memory_order_acq_rel) + 1U;
        map.runtimeGeneration = generation;
        const int active = activeSampleMapIndex.load(std::memory_order_acquire);
        const int pending = pendingSampleMapIndex.load(std::memory_order_acquire);
        int target = 0;
        for (int candidate = 0; candidate < static_cast<int>(sampleMaps.size()); ++candidate) {
            if (candidate != active && candidate != pending) { target = candidate; break; }
        }
        sampleMaps[static_cast<std::size_t>(target)] = std::move(map);
        pendingSampleMapIndex.store(target, std::memory_order_release);
    }

    void adopt_pending_sample_map() noexcept {
        const int pending = pendingSampleMapIndex.exchange(-1, std::memory_order_acq_rel);
        if (pending < 0) return;
        for (Voice& candidate : voice) {
            retire_voice_grains(candidate);
            candidate.kill();
        }
        activeSampleMapIndex.store(pending, std::memory_order_release);
        const SynthSampleMap& map = sampleMaps[static_cast<std::size_t>(pending)];
        activeSampleMapGeneration = map.runtimeGeneration;
        sampleRoundRobinCounters.fill(0U);
    }

    [[nodiscard]] const SynthSampleMap* active_sample_map() const noexcept {
        const SynthSampleMap& map = sampleMaps[static_cast<std::size_t>(
            activeSampleMapIndex.load(std::memory_order_acquire))];
        return map.enabled() ? &map : nullptr;
    }

    [[nodiscard]] std::uint8_t select_sample_zone(std::uint8_t note, std::uint8_t velocity,
                                                   SampleTrigger trigger) noexcept {
        const SynthSampleMap* map = active_sample_map();
        if (map == nullptr) return kInvalidSampleZone;
        std::uint8_t best = kInvalidSampleZone;
        std::uint8_t fallback = kInvalidSampleZone;
        std::uint32_t bestScore = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t fallbackScore = std::numeric_limits<std::uint32_t>::max();
        for (std::size_t index = 0; index < map->zoneCount; ++index) {
            const SampleMapZone& zone = map->zones[index];
            if (!zone.enabled || zone.trigger != trigger || note < zone.keyLow || note > zone.keyHigh ||
                velocity < zone.velocityLow || velocity > zone.velocityHigh) continue;
            const std::uint32_t score = static_cast<std::uint32_t>(zone.keyHigh - zone.keyLow) * 128U +
                                        static_cast<std::uint32_t>(zone.velocityHigh - zone.velocityLow);
            if (score < fallbackScore) {
                fallbackScore = score;
                fallback = static_cast<std::uint8_t>(index);
            }
            const bool roundRobinMatch = zone.roundRobinGroup == 0U ||
                zone.roundRobinIndex == sampleRoundRobinCounters[zone.roundRobinGroup - 1U] % zone.roundRobinCount;
            if (roundRobinMatch && score < bestScore) {
                bestScore = score;
                best = static_cast<std::uint8_t>(index);
            }
        }
        if (best == kInvalidSampleZone) best = fallback;
        if (best != kInvalidSampleZone) {
            const SampleMapZone& zone = map->zones[best];
            if (zone.roundRobinGroup > 0U) ++sampleRoundRobinCounters[zone.roundRobinGroup - 1U];
        }
        return best;
    }

    // Returns a half-open playback interval [start, endExclusive). Keeping the
    // cooked map's half-open frame convention here prevents off-by-one loop
    // reads and makes internal loop endpoints independent of the zone tail.
    [[nodiscard]] std::pair<float, float> sample_zone_bounds(const SampleMapZone& zone,
                                                              const OscillatorParameters& osc) const noexcept {
        const float zoneStart = static_cast<float>(zone.startFrame);
        const float zoneEndExclusive = static_cast<float>(zone.endFrame);
        const float span = std::max(2.0F, zoneEndExclusive - zoneStart);
        const float normalizedStart = clampf(std::min(osc.sampleStart, osc.sampleEnd), 0.0F, 1.0F);
        const float normalizedEnd = clampf(std::max(osc.sampleStart, osc.sampleEnd),
                                           normalizedStart + 1.0e-5F, 1.0F);
        const float start = zoneStart + normalizedStart * span;
        const float endExclusive = zoneStart + normalizedEnd * span;
        return {std::min(start, endExclusive - 1.0e-4F),
                std::max(start + 1.0e-4F, endExclusive)};
    }

    [[nodiscard]] static float last_sample_position(float start, float endExclusive) noexcept {
        return std::nextafter(endExclusive, start);
    }

    [[nodiscard]] static bool sample_loop_bounds(const SampleMapZone& zone, float start,
                                                  float endExclusive, float& loopStart,
                                                  float& loopEndExclusive) noexcept {
        if (zone.loopMode == SampleLoopMode::Disabled) return false;
        loopStart = std::max(start, static_cast<float>(zone.loopStartFrame));
        loopEndExclusive = std::min(endExclusive, static_cast<float>(zone.loopEndFrame));
        return loopEndExclusive > loopStart + 1.0F;
    }

    [[nodiscard]] static float wrap_forward_loop(float position, float loopStart,
                                                  float loopEndExclusive) noexcept {
        const float length = loopEndExclusive - loopStart;
        float overshoot = std::fmod(position - loopEndExclusive, length);
        if (overshoot < 0.0F) overshoot += length;
        return loopStart + overshoot;
    }

    [[nodiscard]] static float wrap_reverse_loop(float position, float loopStart,
                                                  float loopEndExclusive) noexcept {
        const float length = loopEndExclusive - loopStart;
        float overshoot = std::fmod(loopStart - position, length);
        if (overshoot < 0.0F) overshoot += length;
        float wrapped = loopEndExclusive - overshoot;
        if (wrapped >= loopEndExclusive) wrapped = last_sample_position(loopStart, loopEndExclusive);
        return wrapped;
    }

    void assign_sample_zones(Voice& target) noexcept {
        const std::uint8_t velocity = static_cast<std::uint8_t>(
            clampf(target.velocity * 127.0F + 0.5F, 1.0F, 127.0F));
        target.sampleAttackZone = select_sample_zone(target.note, velocity, SampleTrigger::Attack);
        target.sampleReleaseZone = select_sample_zone(target.note, velocity, SampleTrigger::Release);
        const SynthSampleMap* map = active_sample_map();
        if (map == nullptr || target.sampleAttackZone == kInvalidSampleZone) return;
        const SampleMapZone& zone = map->zones[target.sampleAttackZone];
        for (std::size_t oscillator = 0; oscillator < kSynthOscillatorCount; ++oscillator) {
            const OscillatorParameters& osc = parameters.oscillators[oscillator];
            const auto [start, end] = sample_zone_bounds(zone, osc);
            const bool reverse = osc.sampleReverse != zone.reverse;
            target.sampleMapPositions[oscillator] = reverse ? last_sample_position(start, end) : start;
            target.sampleFinished[oscillator] = false;
        }
    }

    void activate_release_samples(Voice& target) noexcept {
        const SynthSampleMap* map = active_sample_map();
        if (map == nullptr || target.sampleReleaseZone == kInvalidSampleZone) return;
        const SampleMapZone& zone = map->zones[target.sampleReleaseZone];
        for (std::size_t oscillator = 0; oscillator < kSynthOscillatorCount; ++oscillator) {
            const OscillatorParameters& osc = parameters.oscillators[oscillator];
            if (osc.waveform != OscillatorWaveform::Sample && osc.waveform != OscillatorWaveform::Granular) continue;
            const auto [start, end] = sample_zone_bounds(zone, osc);
            const bool reverse = osc.sampleReverse != zone.reverse;
            target.releaseSamplePositions[oscillator] = reverse ? last_sample_position(start, end) : start;
            target.releaseSampleActive[oscillator] = true;
        }
    }

    [[nodiscard]] bool sample_source_frame(const SynthSampleMap& map, const SampleMapSource& source,
                                           std::uint8_t sourceIndex, std::uint32_t frame,
                                           float& value) noexcept {
        if (frame >= source.totalFrameCount) return false;
        if (frame >= source.residentSourceStartFrame &&
            frame < source.residentSourceStartFrame + source.residentFrameCount) {
            const std::size_t residentIndex = static_cast<std::size_t>(source.residentFrameOffset) +
                static_cast<std::size_t>(frame - source.residentSourceStartFrame);
            if (residentIndex >= map.residentFrameCount) return false;
            value = map.residentSamples[residentIndex];
            return true;
        }
        if (sampleStreamCache.read_frame_value(map.runtimeGeneration, sourceIndex, frame, value)) return true;
        samplePageUnderruns.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }

    [[nodiscard]] bool sample_source_linear(const SynthSampleMap& map, std::uint8_t sourceIndex,
                                            float framePosition, float& value) noexcept {
        if (sourceIndex >= map.sourceCount || !finite(framePosition) || framePosition < 0.0F) return false;
        const SampleMapSource& source = map.sources[sourceIndex];
        const float bounded = clampf(framePosition, 0.0F,
            static_cast<float>(source.totalFrameCount - 1U));
        const std::uint32_t frame0 = static_cast<std::uint32_t>(bounded);
        const std::uint32_t frame1 = std::min(frame0 + 1U, source.totalFrameCount - 1U);
        float a{}, b{};
        if (!sample_source_frame(map, source, sourceIndex, frame0, a) ||
            !sample_source_frame(map, source, sourceIndex, frame1, b)) return false;
        value = a + (b - a) * (bounded - static_cast<float>(frame0));
        return true;
    }

    [[nodiscard]] bool sample_zone_value(const SynthSampleMap& map, const SampleMapZone& zone,
                                         float position, bool reverse, float loopStart,
                                         float loopEndExclusive, bool loopEnabled,
                                         float& value) noexcept {
        if (!sample_source_linear(map, zone.sourceIndex, position, value)) return false;
        if (!loopEnabled || zone.loopCrossfadeFrames == 0U) return true;
        const float crossfade = std::min(static_cast<float>(zone.loopCrossfadeFrames),
                                         (loopEndExclusive - loopStart) * 0.5F);
        if (crossfade <= 0.0F) return true;
        float phase = -1.0F;
        float alternatePosition{};
        if (!reverse && position >= loopEndExclusive - crossfade && position < loopEndExclusive) {
            phase = (position - (loopEndExclusive - crossfade)) / crossfade;
            alternatePosition = loopStart + (position - (loopEndExclusive - crossfade));
        } else if (reverse && position >= loopStart && position < loopStart + crossfade) {
            phase = (loopStart + crossfade - position) / crossfade;
            alternatePosition = loopEndExclusive - (loopStart + crossfade - position);
            alternatePosition = std::min(alternatePosition,
                                         last_sample_position(loopStart, loopEndExclusive));
        }
        if (phase < 0.0F) return true;
        float alternate{};
        if (!sample_source_linear(map, zone.sourceIndex, alternatePosition, alternate)) return false;
        phase = clampf(phase, 0.0F, 1.0F);
        const float primaryGain = fast_sin_phase(0.25F - phase * 0.25F);
        const float alternateGain = fast_sin_phase(phase * 0.25F);
        value = value * primaryGain + alternate * alternateGain;
        return true;
    }

    [[nodiscard]] float sample_map_pitch_ratio(const SampleMapZone& zone,
                                                const OscillatorParameters& osc,
                                                float frequency) const noexcept {
        if (!osc.sampleKeyTrack) return 1.0F;
        const float root = tuned_frequency(zone.rootNote, zone.tuningCents * 0.01F);
        return root > 0.0001F ? frequency / root : 1.0F;
    }

    [[nodiscard]] std::pair<float, float> render_release_sample(Voice& target,
                                                                       std::size_t oscillatorIndex,
                                                                       const OscillatorParameters& osc,
                                                                       float frequency) noexcept {
        const SynthSampleMap* map = active_sample_map();
        if (map == nullptr || target.sampleReleaseZone == kInvalidSampleZone ||
            !target.releaseSampleActive[oscillatorIndex]) return {};
        const SampleMapZone& zone = map->zones[target.sampleReleaseZone];
        const SampleMapSource& source = map->sources[zone.sourceIndex];
        const auto [start, end] = sample_zone_bounds(zone, osc);
        const bool reverse = osc.sampleReverse != zone.reverse;
        float& position = target.releaseSamplePositions[oscillatorIndex];
        if (position < start || position >= end) {
            target.releaseSampleActive[oscillatorIndex] = false;
            return {};
        }
        float loopStart{}, loopEnd{};
        const bool loopEnabled = sample_loop_bounds(zone, start, end, loopStart, loopEnd);
        float sample{};
        const bool available = sample_zone_value(*map, zone, position, reverse, loopStart, loopEnd,
                                                 loopEnabled, sample);
        const float step = sample_map_pitch_ratio(zone, osc, frequency) *
                           static_cast<float>(source.sampleRate) / sampleRate;
        position += reverse ? -step : step;
        if (loopEnabled && ((!reverse && position >= loopEnd) || (reverse && position < loopStart))) {
            position = reverse ? wrap_reverse_loop(position, loopStart, loopEnd)
                               : wrap_forward_loop(position, loopStart, loopEnd);
        } else if ((!reverse && position >= end) || (reverse && position < start)) {
            target.releaseSampleActive[oscillatorIndex] = false;
        }
        if (!available) return {};
        const float gain = zone.gain * ((1.0F - osc.sampleVelocityToGain) +
                                        osc.sampleVelocityToGain * target.velocity);
        const float pan = clampf(zone.pan, -1.0F, 1.0F);
        return {sample * gain * std::sqrt(0.5F * (1.0F - pan)),
                sample * gain * std::sqrt(0.5F * (1.0F + pan))};
    }

    [[nodiscard]] bool admit_grain(const Voice& target) noexcept {
        std::uint32_t activeVoices = 0U;
        for (const Voice& candidate : voice) if (candidate.active) ++activeVoices;
        const std::uint32_t globalBudget = activeVoices > 12U ? 96U : (activeVoices > 8U ? 128U : 192U);
        std::uint32_t voiceGrains = 0U;
        for (const auto& oscillatorGrains : target.grains)
            for (const GrainState& grain : oscillatorGrains) if (grain.active) ++voiceGrains;
        std::uint32_t quota = std::max(2U, globalBudget / std::max(1U, activeVoices));
        if (target.amp.stage == VoiceStage::Attack || target.amp.stage == VoiceStage::Delay) quota += 2U;
        return activeGrains < globalBudget && voiceGrains < quota;
    }

    void note_grain_admitted() noexcept {
        ++activeGrains;
        activeGrainTelemetry.store(activeGrains, std::memory_order_relaxed);
        std::uint32_t observed = maximumActiveGrains.load(std::memory_order_relaxed);
        while (activeGrains > observed &&
               !maximumActiveGrains.compare_exchange_weak(observed, activeGrains, std::memory_order_relaxed)) {}
    }

    void emit_note(bool on, std::uint8_t channel, std::uint8_t note, std::uint8_t velocity,
                   std::uint64_t sampleFrame) noexcept {
        if (!parameters.arpeggiator.sendMidiOutput) return;
        const MidiMessage message = on ? MidiMessage::note_on(channel, note, velocity, sampleFrame)
                                       : MidiMessage::note_off(channel, note, 0, sampleFrame);
        if (!midiOut.push(message)) droppedMidi.fetch_add(1U, std::memory_order_relaxed);
    }

    void direct_note_on(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity,
                        bool retrigger, bool emitOutput) noexcept {
        Voice& target = allocate_voice(channel, note);
        const bool sameNote = target.active && target.channel == channel && target.note == note;
        const bool restart = retrigger || !sameNote;
        if (restart) retire_voice_grains(target);
        target.start(channel, note, static_cast<float>(velocity) / 127.0F, ++ageCounter, renderFrame,
                     parameters, restart);
        if (restart) assign_sample_zones(target);
        if (emitOutput) emit_note(true, channel, note, velocity, renderFrame);
    }

    void direct_note_off(std::uint8_t channel, std::uint8_t note, bool emitOutput) noexcept {
        for (Voice& candidate : voice) {
            if (!candidate.active || candidate.channel != channel || candidate.note != note) continue;
            candidate.keyHeld = false;
            if (sustain[channel]) candidate.sustained = true;
            else {
                activate_release_samples(candidate);
                candidate.release();
            }
        }
        if (emitOutput) emit_note(false, channel, note, 0, renderFrame);
    }

    bool schedule_voice_event(bool noteOn, std::uint8_t channel, std::uint8_t note,
                              std::uint8_t velocity, std::uint64_t frame, bool emitOutput) noexcept {
        for (ScheduledVoiceEvent& event : scheduledEvents) {
            if (event.active) continue;
            event = {true, noteOn, emitOutput, channel, note, velocity, frame};
            return true;
        }
        droppedMidi.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }

    void process_scheduled_events() noexcept {
        for (ScheduledVoiceEvent& event : scheduledEvents) {
            if (!event.active || event.frame > renderFrame) continue;
            if (event.noteOn) direct_note_on(event.channel, event.note, event.velocity, true, event.emitOutput);
            else direct_note_off(event.channel, event.note, event.emitOutput);
            event.active = false;
        }
    }

    void release_arp_notes() noexcept {
        for (std::size_t i = 0; i < arpActiveCount; ++i)
            direct_note_off(arpActiveChannels[i], arpActiveNotes[i], true);
        arpActiveCount = 0;
        arpGateOffFrame = std::numeric_limits<std::uint64_t>::max();
    }

    void clear_arp_held(bool releaseCurrent) noexcept {
        if (releaseCurrent) release_arp_notes();
        for (HeldNote& note : arpHeld) note = {};
        nextArpFrame = std::numeric_limits<std::uint64_t>::max();
        arpProgress = 0;
        arpStepCounter = 0;
        arpPatternCount = 0U;
        arpRatchetIndex = 0U;
        arpRatchetCount = 1U;
        nextRatchetFrame = std::numeric_limits<std::uint64_t>::max();
        arpStepEndFrame = std::numeric_limits<std::uint64_t>::max();
        arpStepTie = false;
    }

    std::size_t physical_held_count() const noexcept {
        return static_cast<std::size_t>(std::count_if(arpHeld.begin(), arpHeld.end(),
            [](const HeldNote& note) { return note.active && note.keyHeld; }));
    }

    std::size_t active_held_count() const noexcept {
        return static_cast<std::size_t>(std::count_if(arpHeld.begin(), arpHeld.end(),
            [](const HeldNote& note) { return note.active; }));
    }

    void arp_note_on(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity) noexcept {
        if (parameters.arpeggiator.latch && physical_held_count() == 0U && active_held_count() > 0U)
            clear_arp_held(true);
        HeldNote& held = arpHeld[held_index(channel, note)];
        held.active = true;
        held.keyHeld = true;
        held.sustained = false;
        held.channel = channel;
        held.note = note;
        held.velocity = velocity;
        held.order = ++arpOrderCounter;
        if (nextArpFrame == std::numeric_limits<std::uint64_t>::max()) nextArpFrame = renderFrame;
    }

    void arp_note_off(std::uint8_t channel, std::uint8_t note) noexcept {
        HeldNote& held = arpHeld[held_index(channel, note)];
        if (!held.active) return;
        held.keyHeld = false;
        if (sustain[channel]) held.sustained = true;
        else if (!parameters.arpeggiator.latch) held = {};
        if (active_held_count() == 0U) {
            release_arp_notes();
            nextArpFrame = std::numeric_limits<std::uint64_t>::max();
        }
    }

    ChordTrigger& chord_trigger_slot(std::uint8_t channel, std::uint8_t root) noexcept {
        for (ChordTrigger& trigger : chordTriggers)
            if (trigger.active && trigger.channel == channel && trigger.root == root) return trigger;
        for (ChordTrigger& trigger : chordTriggers) if (!trigger.active) return trigger;
        return chordTriggers[0];
    }

    void chord_note_on(std::uint8_t channel, std::uint8_t root, std::uint8_t velocity) noexcept {
        ChordTrigger& trigger = chord_trigger_slot(channel, root);
        if (trigger.active) {
            for (std::size_t i = 0; i < trigger.count; ++i) direct_note_off(trigger.channel, trigger.notes[i], false);
        }
        const ChordVoicing voicing = make_chord_voicing(root, parameters.chord);
        trigger = {};
        trigger.active = true;
        trigger.channel = channel;
        trigger.root = root;
        trigger.count = voicing.count;
        const std::uint8_t scaledVelocity = static_cast<std::uint8_t>(clampf(
            static_cast<float>(velocity) * parameters.chord.velocityScale, 1.0F, 127.0F));
        const std::uint64_t strumFrames = static_cast<std::uint64_t>(std::llround(
            clampf(parameters.chord.strumMilliseconds, 0.0F, 250.0F) * 0.001F * sampleRate));
        for (std::size_t i = 0; i < voicing.count; ++i) {
            trigger.notes[i] = voicing.notes[i];
            const std::uint64_t frame = renderFrame + strumFrames * i;
            if (frame == renderFrame) direct_note_on(channel, voicing.notes[i], scaledVelocity, true, true);
            else (void)schedule_voice_event(true, channel, voicing.notes[i], scaledVelocity, frame, true);
        }
    }

    void chord_note_off(std::uint8_t channel, std::uint8_t root) noexcept {
        for (ChordTrigger& trigger : chordTriggers) {
            if (!trigger.active || trigger.channel != channel || trigger.root != root) continue;
            for (std::size_t i = 0; i < trigger.count; ++i) {
                for (ScheduledVoiceEvent& event : scheduledEvents)
                    if (event.active && event.noteOn && event.channel == channel && event.note == trigger.notes[i]) event.active = false;
                direct_note_off(channel, trigger.notes[i], true);
            }
            trigger = {};
        }
    }

    std::uint32_t random_u32() noexcept {
        arpRandomState ^= arpRandomState << 13U;
        arpRandomState ^= arpRandomState >> 17U;
        arpRandomState ^= arpRandomState << 5U;
        return arpRandomState;
    }

    float random_unit() noexcept {
        return static_cast<float>(random_u32() & 0x00FFFFFFU) / static_cast<float>(0x01000000U);
    }

    std::size_t collect_held(std::array<HeldNote, 128>& result, bool playedOrder) const noexcept {
        std::size_t count = 0;
        for (const HeldNote& note : arpHeld) {
            if (!note.active || count == result.size()) continue;
            result[count++] = note;
        }
        if (playedOrder) {
            std::sort(result.begin(), result.begin() + static_cast<std::ptrdiff_t>(count),
                      [](const HeldNote& a, const HeldNote& b) { return a.order < b.order; });
        } else {
            std::sort(result.begin(), result.begin() + static_cast<std::ptrdiff_t>(count),
                      [](const HeldNote& a, const HeldNote& b) {
                          if (a.note != b.note) return a.note < b.note;
                          return a.channel < b.channel;
                      });
        }
        return count;
    }

    std::uint64_t arpeggiator_duration_frames(std::uint32_t step) const noexcept {
        const float bpm = clampf(effective_arpeggiator_tempo(), 20.0F, 400.0F);
        float frames = sampleRate * 60.0F / bpm * arpeggiator_step_beats(parameters.arpeggiator.division);
        const float swing = clampf(parameters.arpeggiator.swing, 0.0F, 0.75F);
        frames *= (step & 1U) != 0U ? 1.0F + swing : 1.0F - swing;
        return std::max<std::uint64_t>(1U, static_cast<std::uint64_t>(std::llround(frames)));
    }

    bool active_pattern_matches() const noexcept {
        if (arpActiveCount != arpPatternCount) return false;
        for (std::size_t i = 0; i < arpPatternCount; ++i) {
            if (arpActiveNotes[i] != arpPatternNotes[i] || arpActiveChannels[i] != arpPatternChannels[i]) return false;
        }
        return true;
    }

    void trigger_arpeggiator_ratchet(bool allowCarry) noexcept {
        if (arpPatternCount == 0U) return;
        const bool carry = allowCarry && active_pattern_matches();
        if (!carry) {
            release_arp_notes();
            for (std::size_t i = 0; i < arpPatternCount && arpActiveCount < kSynthVoiceCount; ++i) {
                direct_note_on(arpPatternChannels[i], arpPatternNotes[i], arpPatternVelocities[i],
                               parameters.arpeggiator.retriggerEnvelopes, true);
                arpActiveNotes[arpActiveCount] = arpPatternNotes[i];
                arpActiveChannels[arpActiveCount] = arpPatternChannels[i];
                ++arpActiveCount;
            }
        }
        const std::uint8_t configuredStepCount = static_cast<std::uint8_t>(
            std::clamp<unsigned>(parameters.arpeggiator.stepCount, 1U, static_cast<unsigned>(kArpeggiatorStepCount)));
        const std::uint32_t stepIndex = (arpStepCounter == 0U ? 0U : arpStepCounter - 1U) % configuredStepCount;
        const ArpeggiatorStep& step = parameters.arpeggiator.steps[stepIndex];
        const float gate = clampf(parameters.arpeggiator.gate * step.gateScale, 0.02F, 1.0F);
        const std::uint64_t segmentEnd = std::min(arpStepEndFrame, renderFrame + arpRatchetDuration);
        arpGateOffFrame = step.tie && arpRatchetCount == 1U ? arpStepEndFrame :
            renderFrame + std::max<std::uint64_t>(1U,
                static_cast<std::uint64_t>(static_cast<double>(arpRatchetDuration) * gate));
        arpGateOffFrame = std::min(arpGateOffFrame, segmentEnd);
    }

    void start_arpeggiator_step() noexcept {
        const bool carryFromPrevious = arpStepTie;
        std::array<HeldNote, 128> held{};
        const bool played = parameters.arpeggiator.mode == ArpeggiatorMode::Played;
        const std::size_t heldCount = collect_held(held, played);
        if (heldCount == 0U) {
            release_arp_notes();
            nextArpFrame = std::numeric_limits<std::uint64_t>::max();
            nextRatchetFrame = std::numeric_limits<std::uint64_t>::max();
            arpPatternCount = 0U;
            return;
        }

        const std::uint8_t configuredStepCount = static_cast<std::uint8_t>(
            std::clamp<unsigned>(parameters.arpeggiator.stepCount, 1U, static_cast<unsigned>(kArpeggiatorStepCount)));
        const std::uint32_t stepIndex = arpStepCounter % configuredStepCount;
        const ArpeggiatorStep& step = parameters.arpeggiator.steps[stepIndex];
        const std::uint32_t sequencePosition = arpStepCounter;
        activeArpStep.store(stepIndex, std::memory_order_relaxed);
        const std::uint64_t duration = arpeggiator_duration_frames(arpStepCounter);
        ++arpStepCounter;
        nextArpFrame = renderFrame + duration;
        arpStepEndFrame = nextArpFrame;
        arpPatternCount = 0U;
        arpRatchetIndex = 0U;
        arpRatchetCount = std::clamp<std::uint8_t>(step.ratchets, 1U, 8U);
        arpRatchetDuration = std::max<std::uint64_t>(1U, duration / arpRatchetCount);
        nextRatchetFrame = std::numeric_limits<std::uint64_t>::max();
        arpStepTie = step.tie;
        const auto automate = [&](std::size_t macroIndex, float target) {
            if (target < 0.0F) return;
            target = clampf(target, 0.0F, 1.0F);
            if (step.automationCurve == StepAutomationCurve::Step) macroValues[macroIndex] = target;
            else {
                const float rate = step.automationCurve == StepAutomationCurve::Linear ? 0.35F : 0.18F;
                macroValues[macroIndex] += (target - macroValues[macroIndex]) * rate;
            }
        };
        automate(0U, step.macro1); automate(1U, step.macro2);
        automate(2U, step.macro3); automate(3U, step.macro4);
        bool conditionPass = true;
        switch (step.condition) {
            case ArpeggiatorCondition::Unconditional: break;
            case ArpeggiatorCondition::Every2: conditionPass = sequencePosition % 2U == 0U; break;
            case ArpeggiatorCondition::Every3: conditionPass = sequencePosition % 3U == 0U; break;
            case ArpeggiatorCondition::Every4: conditionPass = sequencePosition % 4U == 0U; break;
            case ArpeggiatorCondition::FirstOf4: conditionPass = sequencePosition % 4U == 0U; break;
            case ArpeggiatorCondition::Fill: conditionPass = arpeggiatorFill.load(std::memory_order_relaxed); break;
        }
        if (!step.enabled || !conditionPass || random_unit() > clampf(step.probability, 0.0F, 1.0F)) {
            release_arp_notes();
            return;
        }

        const std::size_t octaveRange = std::clamp<std::size_t>(parameters.arpeggiator.octaveRange, 1U, 4U);
        const std::size_t totalPositions = heldCount * octaveRange;
        auto position_note = [&](std::size_t position) {
            const std::size_t rootIndex = position % heldCount;
            const std::size_t octave = position / heldCount;
            HeldNote result = held[rootIndex];
            result.note = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(result.note) +
                static_cast<int>(octave * 12U), 0, 127));
            return result;
        };

        std::array<HeldNote, kSynthVoiceCount> roots{};
        std::size_t rootCount = 0;
        if (parameters.arpeggiator.mode == ArpeggiatorMode::Chord) {
            rootCount = std::min<std::size_t>(heldCount, roots.size());
            for (std::size_t i = 0; i < rootCount; ++i) roots[i] = held[i];
        } else {
            std::size_t position = 0;
            switch (parameters.arpeggiator.mode) {
                case ArpeggiatorMode::Up:
                case ArpeggiatorMode::Played: position = arpProgress % totalPositions; break;
                case ArpeggiatorMode::Down: position = totalPositions - 1U - (arpProgress % totalPositions); break;
                case ArpeggiatorMode::UpDown: {
                    const std::size_t cycle = totalPositions <= 1U ? 1U : totalPositions * 2U - 2U;
                    const std::size_t phase = arpProgress % cycle;
                    position = phase < totalPositions ? phase : cycle - phase; break;
                }
                case ArpeggiatorMode::DownUp: {
                    const std::size_t cycle = totalPositions <= 1U ? 1U : totalPositions * 2U - 2U;
                    const std::size_t phase = arpProgress % cycle;
                    const std::size_t forward = phase < totalPositions ? phase : cycle - phase;
                    position = totalPositions - 1U - forward; break;
                }
                case ArpeggiatorMode::Random: position = static_cast<std::size_t>(random_u32()) % totalPositions; break;
                case ArpeggiatorMode::Chord: break;
            }
            roots[0] = position_note(position);
            rootCount = 1;
        }
        ++arpProgress;

        const float velocityScale = clampf(step.velocityScale * (step.accent ? 1.22F : 1.0F), 0.0F, 2.0F);
        for (std::size_t rootIndex = 0; rootIndex < rootCount && arpPatternCount < kSynthVoiceCount; ++rootIndex) {
            const HeldNote& root = roots[rootIndex];
            const int transposed = static_cast<int>(root.note) + static_cast<int>(step.transpose) +
                                   static_cast<int>(step.octaveOffset) * 12;
            const std::uint8_t baseNote = static_cast<std::uint8_t>(std::clamp(transposed, 0, 127));
            const std::uint8_t velocity = static_cast<std::uint8_t>(clampf(
                static_cast<float>(root.velocity) * velocityScale, 1.0F, 127.0F));
            if (parameters.chord.enabled) {
                const ChordVoicing voicing = make_chord_voicing(baseNote, parameters.chord);
                for (std::size_t i = 0; i < voicing.count && arpPatternCount < kSynthVoiceCount; ++i) {
                    arpPatternNotes[arpPatternCount] = voicing.notes[i];
                    arpPatternChannels[arpPatternCount] = root.channel;
                    arpPatternVelocities[arpPatternCount] = static_cast<std::uint8_t>(clampf(
                        static_cast<float>(velocity) * parameters.chord.velocityScale, 1.0F, 127.0F));
                    ++arpPatternCount;
                }
            } else {
                arpPatternNotes[arpPatternCount] = baseNote;
                arpPatternChannels[arpPatternCount] = root.channel;
                arpPatternVelocities[arpPatternCount] = velocity;
                ++arpPatternCount;
            }
        }

        trigger_arpeggiator_ratchet((carryFromPrevious || step.slide) && arpRatchetCount == 1U);
        arpRatchetIndex = 1U;
        nextRatchetFrame = arpRatchetIndex < arpRatchetCount
            ? renderFrame + arpRatchetDuration : std::numeric_limits<std::uint64_t>::max();
    }

    void advance_arpeggiator() noexcept {
        if (!parameters.arpeggiator.enabled) return;
        if (nextArpFrame == std::numeric_limits<std::uint64_t>::max() && active_held_count() > 0U)
            nextArpFrame = renderFrame;
        if (renderFrame >= nextArpFrame) start_arpeggiator_step();
        if (renderFrame >= nextRatchetFrame && arpRatchetIndex < arpRatchetCount) {
            trigger_arpeggiator_ratchet(false);
            ++arpRatchetIndex;
            nextRatchetFrame = arpRatchetIndex < arpRatchetCount
                ? renderFrame + arpRatchetDuration : std::numeric_limits<std::uint64_t>::max();
        }
        if (arpActiveCount > 0U && renderFrame >= arpGateOffFrame && !arpStepTie) release_arp_notes();
    }

    void adopt_preset(const RealtimePreset& next) noexcept {
        const bool wasArpeggiating = parameters.arpeggiator.enabled;
        parameters = next;
        macroValues = parameters.macroValues;
        gameClockTempo.store(parameters.arpeggiator.externalTempoBpm, std::memory_order_relaxed);
        arpRandomState = parameters.arpeggiator.randomSeed == 0U ? 0x51A3D8E7U : parameters.arpeggiator.randomSeed;
        if (wasArpeggiating && !parameters.arpeggiator.enabled) clear_arp_held(true);
    }

    void handle_midi(const MidiMessage& message) noexcept {
        const std::uint8_t channel = static_cast<std::uint8_t>(message.channel & 0x0FU);
        if (parameters.midiThru && !midiOut.push(message)) droppedMidi.fetch_add(1U, std::memory_order_relaxed);
        if (message.type == MidiMessageType::SystemRealtime) {
            if (message.status == 0xF8U) {
                if (lastMidiClockFrame != 0U && message.sampleFrame > lastMidiClockFrame) {
                    const std::uint64_t delta = message.sampleFrame - lastMidiClockFrame;
                    const float instantaneous = sampleRate * 60.0F / (static_cast<float>(delta) * 24.0F);
                    if (finite(instantaneous) && instantaneous >= 20.0F && instantaneous <= 400.0F)
                        midiClockTempo += (instantaneous - midiClockTempo) * 0.12F;
                }
                lastMidiClockFrame = message.sampleFrame;
                ++midiClockTickCount;
            } else if (message.status == 0xFAU) {
                clear_arp_held(true);
            } else if (message.status == 0xFCU) {
                release_arp_notes();
            }
            return;
        }
        if (message.is_note_on()) {
            if (parameters.arpeggiator.enabled) arp_note_on(channel, message.data1, message.data2);
            else if (parameters.chord.enabled) chord_note_on(channel, message.data1, message.data2);
            else direct_note_on(channel, message.data1, message.data2, true, false);
            return;
        }
        if (message.is_note_off()) {
            if (parameters.arpeggiator.enabled) arp_note_off(channel, message.data1);
            else if (parameters.chord.enabled) chord_note_off(channel, message.data1);
            else direct_note_off(channel, message.data1, false);
            return;
        }
        switch (message.type) {
            case MidiMessageType::PolyPressure:
                for (Voice& candidate : voice)
                    if (candidate.active && candidate.channel == channel && candidate.note == message.data1)
                        candidate.pressure = static_cast<float>(message.data2) / 127.0F;
                break;
            case MidiMessageType::ChannelPressure:
                channelPressure[channel] = static_cast<float>(message.data1) / 127.0F;
                break;
            case MidiMessageType::PitchBend: {
                const int value = static_cast<int>(message.data1) | (static_cast<int>(message.data2) << 7);
                pitchBend[channel] = static_cast<float>(value - 8192) / 8192.0F;
                break;
            }
            case MidiMessageType::ControlChange: {
                const float normalizedController = static_cast<float>(message.data2) / 127.0F;
                controller[message.data1] = normalizedController;
                if (mpe_master(channel) && message.data1 == parameters.mpe.timbreController)
                    channelTimbre[channel] = normalizedController;
                for (const MidiLearnMapping& mapping : parameters.midiLearn) {
                    if (!mapping.enabled || mapping.controller != message.data1 || mapping.macroIndex >= kSynthMacroCount) continue;
                    float normalized = static_cast<float>(message.data2) / 127.0F;
                    if (mapping.inverted) normalized = 1.0F - normalized;
                    macroValues[mapping.macroIndex] = clampf(mapping.minimum +
                        (mapping.maximum - mapping.minimum) * normalized, 0.0F, 1.0F);
                }
                if (message.data1 == 64U) {
                    const bool next = message.data2 >= 64U;
                    const auto applySustain = [&](std::uint8_t targetChannel) {
                        if (sustain[targetChannel] && !next) {
                            for (Voice& candidate : voice)
                                if (candidate.active && candidate.channel == targetChannel && candidate.sustained && !candidate.keyHeld)
                                    candidate.release();
                            for (std::uint8_t note = 0; note < 128U; ++note) {
                                HeldNote& held = arpHeld[held_index(targetChannel, note)];
                                if (held.active && held.sustained && !held.keyHeld) {
                                    held.sustained = false;
                                    if (!parameters.arpeggiator.latch) held = {};
                                }
                            }
                        }
                        sustain[targetChannel] = next;
                    };
                    applySustain(channel);
                    if (parameters.mpe.masterSustainToMembers && lower_zone_enabled() &&
                        channel == parameters.mpe.lowerMasterChannel) {
                        const unsigned last = std::min<unsigned>(15U, channel + parameters.mpe.lowerMemberCount);
                        for (unsigned member = channel + 1U; member <= last; ++member)
                            applySustain(static_cast<std::uint8_t>(member));
                    }
                    if (parameters.mpe.masterSustainToMembers && upper_zone_enabled() &&
                        channel == parameters.mpe.upperMasterChannel) {
                        const unsigned first = channel >= parameters.mpe.upperMemberCount
                            ? channel - parameters.mpe.upperMemberCount : 0U;
                        for (unsigned member = first; member < channel; ++member)
                            applySustain(static_cast<std::uint8_t>(member));
                    }
                } else if (message.data1 == 120U || message.data1 == 123U) {
                    clear_arp_held(true);
                    for (ChordTrigger& trigger : chordTriggers) trigger = {};
                    for (Voice& candidate : voice) {
                        if (candidate.channel != channel) continue;
                        if (message.data1 == 120U) { retire_voice_grains(candidate); candidate.kill(); }
                        else { activate_release_samples(candidate); candidate.release(); }
                    }
                }
                break;
            }
            default: break;
        }
    }

    float effective_arpeggiator_tempo() const noexcept {
        switch (parameters.arpeggiator.clockSource) {
            case ArpeggiatorClockSource::Internal: return parameters.arpeggiator.tempoBpm;
            case ArpeggiatorClockSource::GameClock: return gameClockTempo.load(std::memory_order_relaxed);
            case ArpeggiatorClockSource::MidiClock: return midiClockTempo;
        }
        return parameters.arpeggiator.tempoBpm;
    }

    float advance_lfo(Voice& v, std::size_t index) noexcept {
        const LfoParameters& lfo = parameters.lfos[index];
        if (!lfo.enabled || lfo.depth <= 0.0F) return 0.0F;
        float rate = lfo.rateHertz;
        if (lfo.tempoSync) {
            const float beats = clampf(lfo.beatsPerCycle, 0.03125F, 32.0F);
            rate = clampf(effective_arpeggiator_tempo(), 20.0F, 400.0F) / (60.0F * beats);
        }
        const float increment = clampf(rate, 0.001F, 100.0F) / sampleRate;
        const float phase = v.lfoPhases[index];
        float value = 0.0F;
        switch (lfo.waveform) {
            case LfoWaveform::Sine: value = fast_sin_phase(phase); break;
            case LfoWaveform::Triangle: value = 1.0F - 4.0F * std::abs(phase - 0.5F); break;
            case LfoWaveform::Saw: value = 2.0F * phase - 1.0F; break;
            case LfoWaveform::Square: value = phase < 0.5F ? 1.0F : -1.0F; break;
            case LfoWaveform::SampleAndHold: value = v.lfoRandomValues[index]; break;
            case LfoWaveform::SmoothRandom: {
                const float smooth = phase * phase * (3.0F - 2.0F * phase);
                value = v.lfoPreviousRandomValues[index] +
                        (v.lfoRandomValues[index] - v.lfoPreviousRandomValues[index]) * smooth;
                break;
            }
        }
        float next = phase + increment;
        if (next >= 1.0F) {
            next = wrap_phase(next);
            v.lfoNoiseState[index] ^= v.lfoNoiseState[index] << 13U;
            v.lfoNoiseState[index] ^= v.lfoNoiseState[index] >> 17U;
            v.lfoNoiseState[index] ^= v.lfoNoiseState[index] << 5U;
            v.lfoPreviousRandomValues[index] = v.lfoRandomValues[index];
            v.lfoRandomValues[index] = static_cast<float>(static_cast<std::int32_t>(v.lfoNoiseState[index])) /
                                       static_cast<float>(std::numeric_limits<std::int32_t>::max());
        }
        v.lfoPhases[index] = next;
        const float ageSeconds = static_cast<float>(renderFrame - v.startFrame) / sampleRate;
        const float fade = lfo.fadeInSeconds <= 0.00001F ? 1.0F : clampf(ageSeconds / lfo.fadeInSeconds, 0.0F, 1.0F);
        return value * clampf(lfo.depth, 0.0F, 1.0F) * fade;
    }

    struct ModulationValues {
        float globalPitch{};
        float filterCutoff{};
        float filterResonance{};
        float filterDrive{};
        float voiceGain{};
        float voicePan{};
        std::array<float, kSynthOscillatorCount> pitch{};
        std::array<float, kSynthOscillatorCount> shape{};
        std::array<float, kSynthOscillatorCount> pulseWidth{};
        std::array<float, kSynthOscillatorCount> gain{};
    };

    static float shape_modulation(float value, ModulationCurve curve) noexcept {
        value = clampf(value, -1.0F, 1.0F);
        const float sign = value < 0.0F ? -1.0F : 1.0F;
        const float magnitude = std::abs(value);
        switch (curve) {
            case ModulationCurve::Linear: return value;
            case ModulationCurve::Quadratic: return sign * magnitude * magnitude;
            case ModulationCurve::Cubic: return value * value * value;
        }
        return value;
    }

    float modulation_source_value(ModulationSource source, Voice& v, float amp, float filterEnv,
                                  const std::array<float, kSynthLfoCount>& lfoValues) const noexcept {
        switch (source) {
            case ModulationSource::Off: return 0.0F;
            case ModulationSource::Lfo1: return lfoValues[0];
            case ModulationSource::Lfo2: return lfoValues[1];
            case ModulationSource::AmpEnvelope: return amp;
            case ModulationSource::FilterEnvelope: return filterEnv;
            case ModulationSource::Velocity: return v.velocity;
            case ModulationSource::KeyTrack: return clampf((static_cast<float>(v.note) - 60.0F) / 60.0F, -1.0F, 1.0F);
            case ModulationSource::ModWheel: return controller[1];
            case ModulationSource::Aftertouch: return std::max(v.pressure, channelPressure[v.channel]);
            case ModulationSource::Random: return v.noteRandom;
            case ModulationSource::Macro1: return macroValues[0];
            case ModulationSource::Macro2: return macroValues[1];
            case ModulationSource::Macro3: return macroValues[2];
            case ModulationSource::Macro4: return macroValues[3];
        }
        return 0.0F;
    }

    static bool native_bipolar(ModulationSource source) noexcept {
        return source == ModulationSource::Lfo1 || source == ModulationSource::Lfo2 ||
               source == ModulationSource::KeyTrack || source == ModulationSource::Random;
    }

    ModulationValues evaluate_modulation(Voice& v, float amp, float filterEnv,
                                         const std::array<float, kSynthLfoCount>& lfoValues) noexcept {
        ModulationValues values;
        for (std::size_t activeIndex = 0; activeIndex < parameters.activeModulationCount; ++activeIndex) {
            const ModulationSlot& slot = parameters.activeModulation[activeIndex];
            const std::size_t originalIndex = parameters.activeModulationIndices[activeIndex];
            float source = modulation_source_value(slot.source, v, amp, filterEnv, lfoValues);
            const bool bipolar = native_bipolar(slot.source);
            if (slot.polarity == ModulationPolarity::Unipolar && bipolar) source = source * 0.5F + 0.5F;
            else if (slot.polarity == ModulationPolarity::Bipolar && !bipolar) source = source * 2.0F - 1.0F;
            source = shape_modulation(source, slot.curve);
            const float smoothingMs = clampf(slot.smoothingMilliseconds, 0.0F, 2000.0F);
            if (smoothingMs > 0.001F) {
                const float coefficient = 1.0F - std::exp(-1.0F / (smoothingMs * 0.001F * sampleRate));
                v.modulationSmoothing[originalIndex] +=
                    (source - v.modulationSmoothing[originalIndex]) * coefficient;
                source = v.modulationSmoothing[originalIndex];
            } else {
                v.modulationSmoothing[originalIndex] = source;
            }
            const float amount = source * clampf(slot.amount, -1.0F, 1.0F);
            modulationScratch[originalIndex] = amount;
            const auto destination = static_cast<unsigned>(slot.destination);
            if (slot.destination == ModulationDestination::GlobalPitch) values.globalPitch += amount * 24.0F;
            else if (slot.destination == ModulationDestination::FilterCutoff) values.filterCutoff += amount * 8.0F;
            else if (slot.destination == ModulationDestination::FilterResonance) values.filterResonance += amount * 0.75F;
            else if (slot.destination == ModulationDestination::FilterDrive) values.filterDrive += amount * 8.0F;
            else if (slot.destination == ModulationDestination::VoiceGain) values.voiceGain += amount;
            else if (slot.destination == ModulationDestination::VoicePan) values.voicePan += amount;
            else if (destination >= static_cast<unsigned>(ModulationDestination::Osc1Pitch) &&
                     destination <= static_cast<unsigned>(ModulationDestination::Osc8Pitch))
                values.pitch[destination - static_cast<unsigned>(ModulationDestination::Osc1Pitch)] += amount * 24.0F;
            else if (destination >= static_cast<unsigned>(ModulationDestination::Osc1Shape) &&
                     destination <= static_cast<unsigned>(ModulationDestination::Osc8Shape))
                values.shape[destination - static_cast<unsigned>(ModulationDestination::Osc1Shape)] += amount;
            else if (destination >= static_cast<unsigned>(ModulationDestination::Osc1PulseWidth) &&
                     destination <= static_cast<unsigned>(ModulationDestination::Osc8PulseWidth))
                values.pulseWidth[destination - static_cast<unsigned>(ModulationDestination::Osc1PulseWidth)] += amount * 0.5F;
            else if (destination >= static_cast<unsigned>(ModulationDestination::Osc1Gain) &&
                     destination <= static_cast<unsigned>(ModulationDestination::Osc8Gain))
                values.gain[destination - static_cast<unsigned>(ModulationDestination::Osc1Gain)] += amount;
        }
        values.filterCutoff = clampf(values.filterCutoff, -12.0F, 12.0F);
        values.filterResonance = clampf(values.filterResonance, -1.0F, 1.0F);
        values.filterDrive = clampf(values.filterDrive, -16.0F, 16.0F);
        values.voiceGain = clampf(values.voiceGain, -1.0F, 2.0F);
        values.voicePan = clampf(values.voicePan, -1.0F, 1.0F);
        return values;
    }

    float wavetable_sample(float phase, float position, float increment) const noexcept {
        if (!parameters.wavetable.enabled || parameters.wavetable.frameCount == 0U) return fast_sin_phase(phase);
        const std::size_t frameCount = std::clamp<std::size_t>(parameters.wavetable.frameCount, 1U, kWavetableFrameCount);
        const float framePosition = clampf(position, 0.0F, 1.0F) * static_cast<float>(frameCount - 1U);
        const std::size_t frame0 = static_cast<std::size_t>(framePosition);
        const std::size_t frame1 = std::min(frame0 + 1U, frameCount - 1U);
        const float frameFraction = framePosition - static_cast<float>(frame0);
        const float samplePosition = wrap_phase(phase) * static_cast<float>(kWavetableSampleCount);
        const std::size_t sample0 = static_cast<std::size_t>(samplePosition) % kWavetableSampleCount;
        const std::size_t sample1 = (sample0 + 1U) % kWavetableSampleCount;
        const float sampleFraction = samplePosition - std::floor(samplePosition);
        const float cyclesPerTable = increment * static_cast<float>(kWavetableSampleCount);
        const std::size_t mip = wavetable_mip(cyclesPerTable);
        const std::size_t mipStride = kWavetableFrameCount * kWavetableSampleCount;
        const auto at = [&](std::size_t frame, std::size_t sample) noexcept {
            return parameters.wavetable.samples[mip * mipStride + frame * kWavetableSampleCount + sample];
        };
        const float a = at(frame0, sample0) + (at(frame0, sample1) - at(frame0, sample0)) * sampleFraction;
        const float b = at(frame1, sample0) + (at(frame1, sample1) - at(frame1, sample0)) * sampleFraction;
        return a + (b - a) * frameFraction;
    }

    float resident_sample(float normalizedPosition) const noexcept {
        if (!parameters.sampleBank.enabled || parameters.sampleBank.frameCount < 2U) return 0.0F;
        const float bounded = clampf(normalizedPosition, 0.0F, 1.0F) *
                              static_cast<float>(parameters.sampleBank.frameCount - 1U);
        const std::size_t i0 = std::min<std::size_t>(static_cast<std::size_t>(bounded),
                                                    parameters.sampleBank.frameCount - 1U);
        const std::size_t i1 = std::min(i0 + 1U,
                                       static_cast<std::size_t>(parameters.sampleBank.frameCount - 1U));
        const float fraction = bounded - static_cast<float>(i0);
        return parameters.sampleBank.samples[i0] +
               (parameters.sampleBank.samples[i1] - parameters.sampleBank.samples[i0]) * fraction;
    }

    float sample_pitch_ratio(const OscillatorParameters& osc, float frequency) const noexcept {
        if (!osc.sampleKeyTrack || !parameters.sampleBank.enabled) return 1.0F;
        const float root = tuned_frequency(parameters.sampleBank.rootNote, 0.0F);
        return root > 0.0001F ? frequency / root : 1.0F;
    }

    std::pair<float, float> render_sample_oscillator(Voice& v, std::size_t oscillatorIndex,
                                                        const OscillatorParameters& osc,
                                                        float frequency) noexcept {
        const SynthSampleMap* map = active_sample_map();
        if (map != nullptr && v.sampleAttackZone != kInvalidSampleZone) {
            const SampleMapZone& zone = map->zones[v.sampleAttackZone];
            const SampleMapSource& source = map->sources[zone.sourceIndex];
            const auto [start, end] = sample_zone_bounds(zone, osc);
            const bool reverse = osc.sampleReverse != zone.reverse;
            float attackSample{};
            bool available = false;
            if (!v.sampleFinished[oscillatorIndex]) {
                float& position = v.sampleMapPositions[oscillatorIndex];
                if (position < start || position >= end)
                    position = reverse ? last_sample_position(start, end) : start;
                float loopStart{}, loopEnd{};
                const bool loopEnabled = sample_loop_bounds(zone, start, end, loopStart, loopEnd);
                available = sample_zone_value(*map, zone, position, reverse, loopStart, loopEnd,
                                              loopEnabled, attackSample);
                const float step = sample_map_pitch_ratio(zone, osc, frequency) *
                                   static_cast<float>(source.sampleRate) / sampleRate;
                position += reverse ? -step : step;
                if (loopEnabled && ((!reverse && position >= loopEnd) || (reverse && position < loopStart))) {
                    position = reverse ? wrap_reverse_loop(position, loopStart, loopEnd)
                                       : wrap_forward_loop(position, loopStart, loopEnd);
                } else if ((!reverse && position >= end) || (reverse && position < start)) {
                    position = reverse ? start : last_sample_position(start, end);
                    v.sampleFinished[oscillatorIndex] = osc.sampleOneShot;
                }
            }
            const float velocityGain = (1.0F - osc.sampleVelocityToGain) +
                                       osc.sampleVelocityToGain * v.velocity;
            const float pan = clampf(zone.pan, -1.0F, 1.0F);
            std::pair<float, float> result{};
            if (available) {
                result.first = attackSample * zone.gain * velocityGain * std::sqrt(0.5F * (1.0F - pan));
                result.second = attackSample * zone.gain * velocityGain * std::sqrt(0.5F * (1.0F + pan));
            }
            const auto release = render_release_sample(v, oscillatorIndex, osc, frequency);
            result.first += release.first;
            result.second += release.second;
            return result;
        }

        if (!parameters.sampleBank.enabled || parameters.sampleBank.frameCount < 2U) return {};
        const float start = clampf(std::min(osc.sampleStart, osc.sampleEnd), 0.0F, 1.0F);
        const float end = clampf(std::max(osc.sampleStart, osc.sampleEnd), start + 1.0e-5F, 1.0F);
        const float loopStart = clampf(std::min(osc.sampleLoopStart, osc.sampleLoopEnd), start, end);
        const float loopEnd = clampf(std::max(osc.sampleLoopStart, osc.sampleLoopEnd), loopStart + 1.0e-5F, end);
        float& position = v.samplePositions[oscillatorIndex];
        if (position < start || position > end) position = osc.sampleReverse ? end : start;
        const float output = resident_sample(position);
        const float ratio = sample_pitch_ratio(osc, frequency) *
                            static_cast<float>(parameters.sampleBank.sampleRate) / sampleRate;
        const float step = ratio / static_cast<float>(parameters.sampleBank.frameCount - 1U);
        position += osc.sampleReverse ? -step : step;
        const bool crossed = osc.sampleReverse ? position <= start : position >= end;
        if (crossed) {
            if (osc.sampleLoop) {
                const float length = std::max(loopEnd - loopStart, 1.0e-5F);
                if (osc.sampleReverse) position = loopEnd - std::fmod(loopStart - position, length);
                else position = loopStart + std::fmod(position - loopEnd, length);
            } else {
                position = osc.sampleReverse ? start : end;
                if (osc.sampleOneShot) return {};
            }
        }
        const float gain = (1.0F - osc.sampleVelocityToGain) + osc.sampleVelocityToGain * v.velocity;
        const float mono = output * gain * 0.70710678F;
        return {mono, mono};
    }

    static float grain_window(GrainWindow window, float phase) noexcept {
        phase = clampf(phase, 0.0F, 1.0F);
        switch (window) {
            case GrainWindow::Hann: return 0.5F - 0.5F * fast_sin_phase(phase - 0.25F);
            case GrainWindow::Triangle: return 1.0F - std::abs(phase * 2.0F - 1.0F);
            case GrainWindow::Tukey: {
                constexpr float edge = 0.25F;
                if (phase < edge) return 0.5F - 0.5F * fast_sin_phase(phase / (edge * 2.0F) - 0.25F);
                if (phase > 1.0F - edge) return 0.5F - 0.5F * fast_sin_phase((1.0F - phase) / (edge * 2.0F) - 0.25F);
                return 1.0F;
            }
        }
        return 1.0F;
    }

    std::pair<float, float> render_granular_oscillator(Voice& v, std::size_t oscillatorIndex,
                                                        const OscillatorParameters& osc,
                                                        float frequency) noexcept {
        const SynthSampleMap* map = active_sample_map();
        const bool mapped = map != nullptr && v.sampleAttackZone != kInvalidSampleZone;
        if (!mapped && (!parameters.sampleBank.enabled || parameters.sampleBank.frameCount < 2U)) return {};

        auto random01 = [&]() noexcept {
            std::uint32_t& state = v.noiseState[oscillatorIndex];
            state ^= state << 13U; state ^= state >> 17U; state ^= state << 5U;
            return static_cast<float>(state & 0x00FFFFFFU) / static_cast<float>(0x01000000U);
        };
        float& countdown = v.grainCountdown[oscillatorIndex];
        countdown -= 1.0F;
        if (countdown <= 0.0F) {
            requestedGrains.fetch_add(1U, std::memory_order_relaxed);
            const float velocityMod = 1.0F + clampf(osc.grainDensityVelocity, -1.0F, 1.0F) *
                (v.velocity * 2.0F - 1.0F);
            const float timbreMod = 1.0F + clampf(osc.grainDensityTimbre, -1.0F, 1.0F) *
                (v.timbre * 2.0F - 1.0F);
            const float density = clampf(osc.grainDensityHertz * velocityMod * timbreMod, 0.5F, 240.0F);
            countdown += sampleRate / density;

            if (!admit_grain(v)) {
                grainMisses.fetch_add(1U, std::memory_order_relaxed);
            } else {
                GrainState* target = nullptr;
                for (auto& grain : v.grains[oscillatorIndex]) {
                    if (!grain.active) { target = &grain; break; }
                }
                bool stole = false;
                if (target == nullptr) {
                    target = &*std::max_element(v.grains[oscillatorIndex].begin(),
                        v.grains[oscillatorIndex].end(), [](const GrainState& a, const GrainState& b) {
                            return a.age / std::max(a.duration, 1.0F) < b.age / std::max(b.duration, 1.0F);
                        });
                    stole = true;
                    grainSteals.fetch_add(1U, std::memory_order_relaxed);
                }

                float pitch = clampf(osc.grainPitchSemitones, -48.0F, 48.0F) +
                    (random01() * 2.0F - 1.0F) * clampf(osc.grainPitchRandomSemitones, 0.0F, 48.0F);
                const float quantize = clampf(osc.grainPitchQuantizeSemitones, 0.0F, 24.0F);
                if (quantize >= 0.01F) pitch = std::round(pitch / quantize) * quantize;

                target->active = true;
                target->zoneIndex = mapped ? v.sampleAttackZone : kInvalidSampleZone;
                if (mapped) {
                    const SampleMapZone& zone = map->zones[v.sampleAttackZone];
                    const SampleMapSource& source = map->sources[zone.sourceIndex];
                    const auto [start, end] = sample_zone_bounds(zone, osc);
                    const float span = std::max(1.0F, end - start);
                    const float endPosition = last_sample_position(start, end);
                    const float center = osc.grainFreeze
                        ? start + clampf(osc.grainPosition, 0.0F, 1.0F) * (endPosition - start)
                        : clampf(v.sampleMapPositions[oscillatorIndex], start, endPosition);
                    const float spray = (random01() * 2.0F - 1.0F) *
                                        clampf(osc.grainSpray, 0.0F, 1.0F) * span;
                    target->position = clampf(center + spray, start, endPosition);
                    target->increment = sample_map_pitch_ratio(zone, osc, frequency) *
                        std::exp2(pitch / 12.0F) * static_cast<float>(source.sampleRate) / sampleRate;
                    bool reverse = osc.sampleReverse != zone.reverse;
                    if (random01() < clampf(osc.grainReverseProbability, 0.0F, 1.0F)) reverse = !reverse;
                    if (reverse) target->increment = -target->increment;
                    if (!osc.grainFreeze) {
                        v.sampleMapPositions[oscillatorIndex] +=
                            sample_map_pitch_ratio(zone, osc, frequency) * 0.25F;
                        if (v.sampleMapPositions[oscillatorIndex] >= end)
                            v.sampleMapPositions[oscillatorIndex] = start;
                    }
                } else {
                    const float center = osc.grainFreeze ? osc.grainPosition :
                        wrap_phase(v.samplePositions[oscillatorIndex] + osc.grainPosition);
                    const float spray = (random01() * 2.0F - 1.0F) * clampf(osc.grainSpray, 0.0F, 1.0F);
                    target->position = clampf(center + spray, 0.0F, 1.0F);
                    target->increment = sample_pitch_ratio(osc, frequency) * std::exp2(pitch / 12.0F) *
                        static_cast<float>(parameters.sampleBank.sampleRate) / sampleRate /
                        static_cast<float>(parameters.sampleBank.frameCount - 1U);
                    bool reverse = osc.sampleReverse;
                    if (random01() < clampf(osc.grainReverseProbability, 0.0F, 1.0F)) reverse = !reverse;
                    if (reverse) target->increment = -target->increment;
                }
                target->age = 0.0F;
                target->duration = clampf(osc.grainSizeMilliseconds, 5.0F, 500.0F) * 0.001F * sampleRate;
                target->pan = (random01() * 2.0F - 1.0F) * clampf(osc.grainStereoSpread, 0.0F, 1.0F);
                target->panEnd = clampf(target->pan + (random01() * 2.0F - 1.0F) *
                    clampf(osc.grainStereoMotion, 0.0F, 1.0F), -1.0F, 1.0F);
                admittedGrains.fetch_add(1U, std::memory_order_relaxed);
                if (!stole) note_grain_admitted();
            }
        }
        if (!mapped && !osc.grainFreeze) {
            v.samplePositions[oscillatorIndex] = wrap_phase(v.samplePositions[oscillatorIndex] +
                sample_pitch_ratio(osc, frequency) / sampleRate * 0.25F);
        }

        float left = 0.0F;
        float right = 0.0F;
        unsigned active = 0U;
        for (auto& grain : v.grains[oscillatorIndex]) {
            if (!grain.active) continue;
            const float phase = grain.age / std::max(grain.duration, 1.0F);
            bool expired = phase >= 1.0F;
            float sample{};
            bool available = false;
            float zoneGain = 1.0F;
            float zonePan = 0.0F;
            if (!expired && grain.zoneIndex != kInvalidSampleZone && map != nullptr) {
                const SampleMapZone& zone = map->zones[grain.zoneIndex];
                const auto [start, end] = sample_zone_bounds(zone, osc);
                expired = grain.position < start || grain.position >= end;
                if (!expired) {
                    available = sample_source_linear(*map, zone.sourceIndex, grain.position, sample);
                    zoneGain = zone.gain;
                    zonePan = zone.pan;
                }
            } else if (!expired) {
                expired = grain.position < 0.0F || grain.position > 1.0F;
                if (!expired) {
                    sample = resident_sample(grain.position);
                    available = true;
                }
            }
            if (expired || !available) {
                grain.active = false;
                if (activeGrains > 0U) --activeGrains;
                activeGrainTelemetry.store(activeGrains, std::memory_order_relaxed);
                if (!expired) grainMisses.fetch_add(1U, std::memory_order_relaxed);
                continue;
            }
            float window = grain_window(osc.grainWindow, phase);
            const float curvature = clampf(osc.grainEnvelopeCurve, 0.25F, 4.0F);
            if (curvature < 1.0F) {
                const float opened = std::sqrt(std::max(window, 0.0F));
                window += (opened - window) * (1.0F - curvature);
            } else if (curvature <= 2.0F) {
                const float squared = window * window;
                window += (squared - window) * (curvature - 1.0F);
            } else {
                const float squared = window * window;
                const float fourth = squared * squared;
                window = squared + (fourth - squared) * ((curvature - 2.0F) * 0.5F);
            }
            const float panPhase = 0.5F - 0.5F * fast_sin_phase(phase * 0.5F + 0.25F);
            const float pan = clampf(zonePan + grain.pan + (grain.panEnd - grain.pan) * panPhase, -1.0F, 1.0F);
            const float value = sample * window * zoneGain;
            left += value * std::sqrt(0.5F * (1.0F - pan));
            right += value * std::sqrt(0.5F * (1.0F + pan));
            grain.position += grain.increment;
            grain.age += 1.0F;
            ++active;
        }
        if (active > 1U) {
            const float normalization = 1.0F / std::sqrt(static_cast<float>(active));
            left *= normalization;
            right *= normalization;
        }
        const float velocity = (1.0F - osc.sampleVelocityToGain) + osc.sampleVelocityToGain * v.velocity;
        left *= velocity;
        right *= velocity;
        const auto release = render_release_sample(v, oscillatorIndex, osc, frequency);
        return {left + release.first, right + release.second};
    }

    bool lower_zone_enabled() const noexcept {
        return parameters.mpe.zoneMode == MpeZoneMode::Lower || parameters.mpe.zoneMode == MpeZoneMode::Dual;
    }
    bool upper_zone_enabled() const noexcept {
        return parameters.mpe.zoneMode == MpeZoneMode::Upper || parameters.mpe.zoneMode == MpeZoneMode::Dual;
    }
    bool lower_member(std::uint8_t channel) const noexcept {
        return lower_zone_enabled() && channel > parameters.mpe.lowerMasterChannel &&
               channel <= static_cast<std::uint8_t>(std::min<unsigned>(15U,
                   static_cast<unsigned>(parameters.mpe.lowerMasterChannel) + parameters.mpe.lowerMemberCount));
    }
    bool upper_member(std::uint8_t channel) const noexcept {
        const unsigned first = parameters.mpe.upperMasterChannel >= parameters.mpe.upperMemberCount
            ? parameters.mpe.upperMasterChannel - parameters.mpe.upperMemberCount : 0U;
        return upper_zone_enabled() && channel >= first && channel < parameters.mpe.upperMasterChannel;
    }
    std::optional<std::uint8_t> mpe_master(std::uint8_t channel) const noexcept {
        if (lower_member(channel)) return parameters.mpe.lowerMasterChannel;
        if (upper_member(channel)) return parameters.mpe.upperMasterChannel;
        return std::nullopt;
    }
    float effective_pitch_bend(std::uint8_t channel) const noexcept {
        if (const auto master = mpe_master(channel))
            return pitchBend[*master] * parameters.mpe.masterPitchBendRangeSemitones +
                   pitchBend[channel] * parameters.mpe.memberPitchBendRangeSemitones;
        return pitchBend[channel] * parameters.pitchBendRangeSemitones;
    }
    float tuned_frequency(std::uint8_t midiNote, float additionalSemitones) const noexcept {
        if (parameters.microtuning.enabled) {
            const float cents = (static_cast<float>(midiNote) - static_cast<float>(parameters.microtuning.referenceNote)) * 100.0F +
                                parameters.microtuning.centsOffset[midiNote] + additionalSemitones * 100.0F;
            return parameters.microtuning.referenceHertz * std::exp2(cents / 1200.0F);
        }
        return parameters.tuning.referenceHertz *
               std::exp2((static_cast<float>(midiNote) + additionalSemitones - 69.0F) / 12.0F);
    }

    std::pair<float, float> render_voice(Voice& v) noexcept {
        if (!v.active) return {};
        const float amp = v.amp.advance(parameters.ampEnvelope, sampleRate);
        const float filterEnv = v.filterEnvelope.advance(parameters.filter.envelope, sampleRate);
        if (!v.amp.active()) { retire_voice_grains(v); v.kill(); return {}; }

        std::array<float, kSynthLfoCount> lfoValues{};
        for (std::size_t i = 0; i < lfoValues.size(); ++i) lfoValues[i] = advance_lfo(v, i);
        const ModulationValues mod = evaluate_modulation(v, amp, filterEnv, lfoValues);
        const float bendSemitones = effective_pitch_bend(v.channel);
        v.pitchBendSemitones = bendSemitones;
        if (mpe_master(v.channel)) {
            v.timbre = channelTimbre[v.channel];
            v.pressure = channelPressure[v.channel];
        }
        const float frameSeconds = static_cast<float>(renderFrame % static_cast<std::uint64_t>(sampleRate * 4096.0F)) / sampleRate;
        const float legacyModulation = controller[1] > 0.0F ? controller[1] * 0.22F * fast_sin_phase(frameSeconds * 5.2F) : 0.0F;
        const float slowDrift = parameters.tuning.analogDriftCents > 0.0F
            ? fast_sin_phase(frameSeconds * 0.023F + static_cast<float>(v.note) * (0.371F / kTwoPi)) * 0.35F
            : 0.0F;

        float left = 0.0F;
        float right = 0.0F;
        std::array<float, kSynthOscillatorCount> currentSamples{};
        std::array<bool, kSynthOscillatorCount> currentWrapped{};
        for (std::size_t i = 0; i < parameters.oscillators.size(); ++i) {
            const OscillatorParameters& osc = parameters.oscillators[i];
            if (!osc.enabled || osc.gain <= 0.0F) continue;

            const auto source_sample = [&](std::int8_t source) noexcept {
                if (source < 0 || source >= static_cast<std::int8_t>(kSynthOscillatorCount) ||
                    source == static_cast<std::int8_t>(i)) return 0.0F;
                const std::size_t index = static_cast<std::size_t>(source);
                return index < i ? currentSamples[index] : v.previousOscillatorSamples[index];
            };
            const auto source_wrapped = [&](std::int8_t source) noexcept {
                if (source < 0 || source >= static_cast<std::int8_t>(kSynthOscillatorCount) ||
                    source == static_cast<std::int8_t>(i)) return false;
                const std::size_t index = static_cast<std::size_t>(source);
                return index < i ? currentWrapped[index] : v.oscillatorWrapped[index];
            };

            if (source_wrapped(osc.hardSyncSource)) v.phases[i] = wrap_phase(osc.phaseOffset);
            const std::uint32_t hash = static_cast<std::uint32_t>(v.age) * 0x9E3779B9U ^
                                       static_cast<std::uint32_t>(i + 1U) * 0x85EBCA6BU;
            const float staticDrift = (static_cast<float>(hash & 0xFFFFU) / 32767.5F - 1.0F) * 0.65F;
            const float driftCents = parameters.tuning.analogDriftCents * (staticDrift + slowDrift);
            const float additionalSemitones = parameters.tuning.transposeSemitones +
                         (parameters.tuning.fineCents + driftCents) * 0.01F + osc.semitones +
                         osc.cents * 0.01F + bendSemitones + legacyModulation + mod.globalPitch + mod.pitch[i];
            float note = static_cast<float>(v.note) + additionalSemitones;
            const float fmSource = source_sample(osc.frequencyModSource);
            if (osc.frequencyModMode == FrequencyModulationMode::Exponential)
                note += fmSource * clampf(osc.frequencyModAmount, -4.0F, 4.0F) * 24.0F;
            float frequency = tuned_frequency(v.note, note - static_cast<float>(v.note));
            if (osc.frequencyModMode == FrequencyModulationMode::Linear)
                frequency += fmSource * frequency * clampf(osc.frequencyModAmount, -2.0F, 2.0F);
            frequency = clampf(frequency, 0.1F, sampleRate * 0.45F);
            const float increment = frequency / sampleRate;

            float pulseWidth = osc.pulseWidth + mod.pulseWidth[i];
            if (osc.pwmDepth > 0.0F && (osc.waveform == OscillatorWaveform::Pulse || osc.waveform == OscillatorWaveform::Square)) {
                const float pwm = fast_sin_phase(frameSeconds * osc.pwmRateHertz + osc.phaseOffset);
                pulseWidth += pwm * osc.pwmDepth * 0.45F;
            }
            pulseWidth = clampf(pulseWidth, 0.03F, 0.97F);
            const float shape = clampf(osc.shape + mod.shape[i], 0.0F, 1.0F);
            unsigned qualityFactor = 1U;
            if (parameters.oscillatorQuality == OscillatorQuality::High) qualityFactor = 2U;
            else if (parameters.oscillatorQuality == OscillatorQuality::Offline) qualityFactor = 4U;
            const bool needsQuality = osc.waveform == OscillatorWaveform::Saw ||
                                      osc.waveform == OscillatorWaveform::Square ||
                                      osc.waveform == OscillatorWaveform::Pulse ||
                                      osc.waveform == OscillatorWaveform::FoldedSine ||
                                      osc.waveform == OscillatorWaveform::Digital ||
                                      osc.frequencyModMode != FrequencyModulationMode::Off ||
                                      osc.hardSyncSource >= 0;
            if (!needsQuality) qualityFactor = 1U;

            auto renderPhase = [&](float& phase, float phaseIncrement, bool primary,
                                   std::array<float, 4>& auxiliary, std::uint32_t& noise) noexcept {
                const float subIncrement = phaseIncrement / static_cast<float>(qualityFactor);
                float total = 0.0F;
                for (unsigned q = 0; q < qualityFactor; ++q) {
                    total += osc.waveform == OscillatorWaveform::Wavetable
                        ? wavetable_sample(phase, clampf(osc.wavetablePosition + shape, 0.0F, 1.0F), subIncrement)
                        : oscillator_sample(osc.waveform, phase, subIncrement, pulseWidth, shape, auxiliary, noise);
                    const float next = phase + subIncrement;
                    if (primary && next >= 1.0F) currentWrapped[i] = true;
                    phase = wrap_phase(next);
                }
                return total / static_cast<float>(qualityFactor);
            };

            float sample = 0.0F;
            float stereoLeft = 0.0F;
            float stereoRight = 0.0F;
            if (osc.waveform == OscillatorWaveform::Sample) {
                const auto sampled = render_sample_oscillator(v, i, osc, frequency);
                stereoLeft = sampled.first;
                stereoRight = sampled.second;
                sample = (stereoLeft + stereoRight) * 0.70710678F;
            } else if (osc.waveform == OscillatorWaveform::Granular) {
                const auto granular = render_granular_oscillator(v, i, osc, frequency);
                stereoLeft = granular.first;
                stereoRight = granular.second;
                sample = (stereoLeft + stereoRight) * 0.70710678F;
            } else if (osc.waveform == OscillatorWaveform::PhysicalModel) {
                sample = physical::process(v.physicalModels[i], osc, frequency, v.velocity,
                                           v.pressure, v.timbre, sampleRate);
                const float width = 0.18F + 0.22F * std::abs(osc.physicalPickupPosition - 0.5F);
                stereoLeft = sample * (0.70710678F + width * 0.25F);
                stereoRight = sample * (0.70710678F - width * 0.25F);
            } else {
                sample = renderPhase(v.phases[i], increment, true, v.auxiliaryPhases[i], v.noiseState[i]);
                stereoLeft = sample * 0.70710678F;
                stereoRight = sample * 0.70710678F;
            }
            std::uint8_t unisonVoices = parameters.unison.enabled
                ? std::clamp<std::uint8_t>(parameters.unison.voices, 1U, static_cast<std::uint8_t>(kSynthUnisonMax)) : 1U;
            if (osc.waveform == OscillatorWaveform::Noise || osc.waveform == OscillatorWaveform::SuperSaw ||
                osc.waveform == OscillatorWaveform::Sample || osc.waveform == OscillatorWaveform::Granular ||
                osc.waveform == OscillatorWaveform::PhysicalModel)
                unisonVoices = 1U;
            for (std::uint8_t copy = 1U; copy < unisonVoices; ++copy) {
                const float centered = static_cast<float>(copy) - 0.5F * static_cast<float>(unisonVoices - 1U);
                const float detune = centered * parameters.unison.detuneCents;
                const float copyIncrement = increment * std::exp2(detune / 1200.0F);
                auto auxiliary = v.auxiliaryPhases[i];
                std::uint32_t noise = v.noiseState[i] ^ (0x9E3779B9U * static_cast<std::uint32_t>(copy + 1U));
                float copySample = renderPhase(v.unisonPhases[i][copy - 1U], copyIncrement, false, auxiliary, noise);
                sample += copySample;
                const float spread = clampf(parameters.unison.stereoSpread, 0.0F, 1.0F);
                const float pan = unisonVoices <= 1U ? 0.0F :
                    ((static_cast<float>(copy) / static_cast<float>(unisonVoices - 1U)) * 2.0F - 1.0F) * spread;
                stereoLeft += copySample * std::sqrt(0.5F * (1.0F - pan));
                stereoRight += copySample * std::sqrt(0.5F * (1.0F + pan));
            }
            const float normalization = parameters.unison.preserveLevel
                ? 1.0F / std::sqrt(static_cast<float>(unisonVoices)) : 1.0F / static_cast<float>(unisonVoices);
            sample *= normalization;
            stereoLeft *= normalization;
            stereoRight *= normalization;

            if (osc.subOscillatorLevel > 0.0F && osc.waveform != OscillatorWaveform::Sample &&
                osc.waveform != OscillatorWaveform::Granular &&
                osc.waveform != OscillatorWaveform::PhysicalModel) {
                const unsigned octaves = std::clamp<unsigned>(osc.subOscillatorOctaves, 1U, 3U);
                const float subIncrement = increment / static_cast<float>(1U << octaves);
                const float sub = bandlimited_pulse(v.subPhases[i], subIncrement, 0.5F) *
                                  clampf(osc.subOscillatorLevel, 0.0F, 1.0F);
                v.subPhases[i] = wrap_phase(v.subPhases[i] + subIncrement);
                sample += sub;
                stereoLeft += sub * 0.70710678F;
                stereoRight += sub * 0.70710678F;
            }
            if (osc.ringModDepth > 0.0F) {
                const float ring = source_sample(osc.ringModSource);
                const float depth = clampf(osc.ringModDepth, 0.0F, 1.0F);
                const float factor = (1.0F - depth) + ring * depth;
                sample *= factor; stereoLeft *= factor; stereoRight *= factor;
            }

            const float gainMod = clampf(1.0F + mod.gain[i], 0.0F, 3.0F);
            const float oscillatorGain = osc.gain * gainMod;
            sample *= oscillatorGain; stereoLeft *= oscillatorGain; stereoRight *= oscillatorGain;
            currentSamples[i] = clampf(sample, -8.0F, 8.0F);
            const float basePan = clampf(osc.pan + mod.voicePan, -1.0F, 1.0F);
            const float panLeft = std::sqrt(0.5F * (1.0F - basePan));
            const float panRight = std::sqrt(0.5F * (1.0F + basePan));
            const bool intrinsicStereo = osc.waveform == OscillatorWaveform::Sample ||
                                         osc.waveform == OscillatorWaveform::Granular ||
                                         osc.waveform == OscillatorWaveform::PhysicalModel;
            if (intrinsicStereo) {
                left += stereoLeft * panLeft * 1.41421356F;
                right += stereoRight * panRight * 1.41421356F;
            } else if (unisonVoices > 1U) {
                left += stereoLeft * (0.75F + 0.25F * panLeft);
                right += stereoRight * (0.75F + 0.25F * panRight);
            } else {
                left += currentSamples[i] * panLeft;
                right += currentSamples[i] * panRight;
            }
        }
        v.previousOscillatorSamples = currentSamples;
        v.oscillatorWrapped = currentWrapped;

        const float pressureGain = 0.85F + 0.15F * std::max(v.pressure, channelPressure[v.channel]);
        const float gain = amp * v.velocity * pressureGain * clampf(1.0F + mod.voiceGain, 0.0F, 3.0F);
        left *= gain;
        right *= gain;
        if (parameters.filter.enabled) {
            const float timbreValue = mpe_master(v.channel) ? v.timbre : controller[74];
            const float cutoffCc = timbreValue > 0.0F ? std::exp2((timbreValue - 0.5F) * 8.0F) : 1.0F;
            const float keyTrackOctaves = (static_cast<float>(v.note) - 60.0F) / 12.0F * parameters.filter.keyTrack;
            const float cutoff = parameters.filter.cutoffHertz * cutoffCc *
                                 std::exp2(parameters.filter.envelopeAmountOctaves * filterEnv +
                                           keyTrackOctaves + mod.filterCutoff);
            const float resonance = clampf(parameters.filter.resonance + controller[71] * 0.5F +
                                           mod.filterResonance, 0.0F, 1.0F);
            FilterParameters modulatedFilter = parameters.filter;
            modulatedFilter.drive = clampf(parameters.filter.drive + mod.filterDrive, 0.1F, 24.0F);
            left = v.filterL.process(left, cutoff, resonance, modulatedFilter, sampleRate, parameters.filterQuality);
            right = v.filterR.process(right, cutoff, resonance, modulatedFilter, sampleRate, parameters.filterQuality);
        }
        return {left, right};
    }

    void effects(float& left, float& right) noexcept {
        if (parameters.distortion.enabled) {
            const float wetL = fast_tanh(left * parameters.distortion.drive);
            const float wetR = fast_tanh(right * parameters.distortion.drive);
            const float mix = clampf(parameters.distortion.mix, 0.0F, 1.0F);
            left += (wetL - left) * mix; right += (wetR - right) * mix;
        }
        if (parameters.eq.enabled) {
            const float lowCoefficient = 1.0F - std::exp(-kTwoPi * 220.0F / sampleRate);
            const float highCoefficient = 1.0F - std::exp(-kTwoPi * 4200.0F / sampleRate);
            eqLowL += lowCoefficient * (left - eqLowL); eqLowR += lowCoefficient * (right - eqLowR);
            eqHighL += highCoefficient * (left - eqHighL); eqHighR += highCoefficient * (right - eqHighR);
            const float lowL = eqLowL; const float lowR = eqLowR;
            const float highL = left - eqHighL; const float highR = right - eqHighR;
            const float midL = left - lowL - highL; const float midR = right - lowR - highR;
            left = lowL * db_to_gain(parameters.eq.lowGainDb) + midL * db_to_gain(parameters.eq.midGainDb) +
                   highL * db_to_gain(parameters.eq.highGainDb);
            right = lowR * db_to_gain(parameters.eq.lowGainDb) + midR * db_to_gain(parameters.eq.midGainDb) +
                    highR * db_to_gain(parameters.eq.highGainDb);
        }
        if (parameters.chorus.enabled) {
            chorusPhase = wrap_phase(chorusPhase + parameters.chorus.rateHertz / sampleRate);
            const float base = 12.0F * sampleRate / 1000.0F;
            const float depth = parameters.chorus.depthMilliseconds * sampleRate / 1000.0F;
            const float wetL = chorusL.read_fractional(base + depth * (0.5F + 0.5F * fast_sin_phase(chorusPhase)));
            const float wetR = chorusR.read_fractional(base + depth * (0.5F + 0.5F * fast_sin_phase(chorusPhase + 0.25F)));
            chorusL.push(left); chorusR.push(right);
            const float mix = clampf(parameters.chorus.mix + controller[93] * 0.25F, 0.0F, 1.0F);
            left += (wetL - left) * mix; right += (wetR - right) * mix;
        } else { chorusL.push(left); chorusR.push(right); }
        if (parameters.phaser.enabled) {
            phaserPhase = wrap_phase(phaserPhase + parameters.phaser.rateHertz / sampleRate);
            const float lfo = 0.5F + 0.5F * fast_sin_phase(phaserPhase);
            const float coefficient = clampf(0.05F + lfo * parameters.phaser.depth * 0.85F, 0.02F, 0.92F);
            float wetL = left + phaserFeedbackL * parameters.phaser.feedback;
            float wetR = right + phaserFeedbackR * parameters.phaser.feedback;
            for (auto& stage : phaserL) wetL = stage.process(wetL, coefficient);
            for (auto& stage : phaserR) wetR = stage.process(wetR, coefficient * 0.97F);
            phaserFeedbackL = wetL; phaserFeedbackR = wetR;
            const float mix = clampf(parameters.phaser.mix, 0.0F, 1.0F);
            left += (wetL - left) * mix; right += (wetR - right) * mix;
        }
        if (parameters.delay.enabled) {
            const float delaySamples = clampf(parameters.delay.timeSeconds, 0.01F, 1.95F) * sampleRate;
            const float delayedL = delayL.read_fractional(delaySamples);
            const float delayedR = delayR.read_fractional(delaySamples);
            const float feedback = clampf(parameters.delay.feedback, 0.0F, 0.94F);
            delayL.push(left + (parameters.delay.pingPong ? delayedR : delayedL) * feedback);
            delayR.push(right + (parameters.delay.pingPong ? delayedL : delayedR) * feedback);
            const float mix = clampf(parameters.delay.mix, 0.0F, 1.0F);
            left += (delayedL - left) * mix; right += (delayedR - right) * mix;
        } else { delayL.push(left); delayR.push(right); }
        if (parameters.reverb.enabled) {
            float wetL = 0.0F; float wetR = 0.0F;
            reverb.process(left, right, parameters.reverb, wetL, wetR);
            const float mix = clampf(parameters.reverb.mix + controller[91] * 0.25F, 0.0F, 1.0F);
            left += (wetL - left) * mix; right += (wetR - right) * mix;
        }
        if (parameters.compressor.enabled) {
            const float detector = std::max(std::abs(left), std::abs(right));
            const float attack = std::exp(-1.0F / (sampleRate * std::max(0.0001F, parameters.compressor.attackMilliseconds * 0.001F)));
            const float release = std::exp(-1.0F / (sampleRate * std::max(0.0001F, parameters.compressor.releaseMilliseconds * 0.001F)));
            compressorEnvelope = detector > compressorEnvelope
                ? attack * compressorEnvelope + (1.0F - attack) * detector
                : release * compressorEnvelope + (1.0F - release) * detector;
            const float envelopeDb = 20.0F * std::log10(std::max(compressorEnvelope, 1.0e-9F));
            float reductionDb = 0.0F;
            if (envelopeDb > parameters.compressor.thresholdDb)
                reductionDb = (parameters.compressor.thresholdDb +
                               (envelopeDb - parameters.compressor.thresholdDb) / std::max(1.0F, parameters.compressor.ratio)) - envelopeDb;
            const float gain = db_to_gain(reductionDb + parameters.compressor.makeupDb);
            left *= gain; right *= gain;
        }
        if (parameters.limiter.enabled) {
            const float peak = std::max(std::abs(left), std::abs(right));
            const float ceiling = db_to_gain(parameters.limiter.ceilingDb);
            const float required = peak > ceiling ? ceiling / std::max(peak, 1.0e-9F) : 1.0F;
            const float release = std::exp(-1.0F / (sampleRate * std::max(0.001F, parameters.limiter.releaseMilliseconds * 0.001F)));
            if (required < limiterEnvelope || limiterEnvelope == 0.0F) limiterEnvelope = required;
            else limiterEnvelope = release * limiterEnvelope + (1.0F - release);
            left *= limiterEnvelope; right *= limiterEnvelope;
        }
    }

    void publish_telemetry() noexcept {
        std::uint32_t count = 0;
        for (std::size_t i = 0; i < voice.size(); ++i) {
            const Voice& source = voice[i];
            telemetry[i].active.store(source.active, std::memory_order_relaxed);
            telemetry[i].channel.store(source.channel, std::memory_order_relaxed);
            telemetry[i].note.store(source.note, std::memory_order_relaxed);
            telemetry[i].velocity.store(source.velocity, std::memory_order_relaxed);
            telemetry[i].envelope.store(source.amp.value, std::memory_order_relaxed);
            telemetry[i].pressure.store(source.pressure, std::memory_order_relaxed);
            telemetry[i].timbre.store(source.timbre, std::memory_order_relaxed);
            telemetry[i].pitchBendSemitones.store(source.pitchBendSemitones, std::memory_order_relaxed);
            telemetry[i].stage.store(source.amp.stage, std::memory_order_relaxed);
            telemetry[i].age.store(source.age, std::memory_order_relaxed);
            if (source.active) ++count;
        }
        activeVoiceCount.store(count, std::memory_order_relaxed);
        heldArpNoteCount.store(static_cast<std::uint32_t>(active_held_count()), std::memory_order_relaxed);
        for (std::size_t i = 0; i < modulationTelemetry.size(); ++i)
            modulationTelemetry[i].store(modulationScratch[i], std::memory_order_relaxed);
    }
};

WavetableBank WavetableBank::make_default() {
    WavetableBank result;
    result.enabled = true;
    result.frameCount = 4;
    for (std::size_t sample = 0; sample < kWavetableSampleCount; ++sample) {
        const float phase = static_cast<float>(sample) / static_cast<float>(kWavetableSampleCount);
        result.samples[0 * kWavetableSampleCount + sample] = fast_sin_phase(phase);
        result.samples[1 * kWavetableSampleCount + sample] = 2.0F * phase - 1.0F;
        result.samples[2 * kWavetableSampleCount + sample] = phase < 0.5F ? 1.0F : -1.0F;
        result.samples[3 * kWavetableSampleCount + sample] = 1.0F - 4.0F * std::abs(phase - 0.5F);
    }
    std::uint64_t hash = 1469598103934665603ULL;
    for (float sample : result.samples) {
        const auto bits = std::bit_cast<std::uint32_t>(sample);
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            hash ^= static_cast<std::uint8_t>((bits >> shift) & 0xFFU);
            hash *= 1099511628211ULL;
        }
    }
    result.contentHash = hash;
    return result;
}

bool WavetableBank::validate(std::string* error) const {
    auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    if (name.empty() || name.size() > 128U) return fail("wavetable name must contain 1 to 128 characters");
    if (frameCount < 1U || frameCount > kWavetableFrameCount) return fail("wavetable frame count is out of range");
    for (std::size_t i = 0; i < static_cast<std::size_t>(frameCount) * kWavetableSampleCount; ++i)
        if (!finite(samples[i]) || std::abs(samples[i]) > 4.0F) return fail("wavetable contains invalid samples");
    return true;
}


MicrotuningTable MicrotuningTable::equal_temperament(float reference, std::uint8_t note) noexcept {
    MicrotuningTable result;
    result.enabled = false;
    result.name = "12-TET";
    result.referenceNote = note;
    result.referenceHertz = reference;
    result.centsOffset.fill(0.0F);
    return result;
}

bool SynthSampleBank::validate(std::string* error) const {
    auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    if (name.empty() || name.size() > 128U) return fail("sample bank name must contain 1 to 128 characters");
    if (sampleRate < 8000U || sampleRate > 192000U || rootNote > 127U || frameCount > kSynthSampleMaxFrames)
        return fail("sample bank metadata is out of range");
    if (enabled && frameCount < 2U) return fail("enabled sample bank requires at least two frames");
    for (std::size_t i = 0; i < frameCount; ++i)
        if (!finite(samples[i]) || std::abs(samples[i]) > 4.0F) return fail("sample bank contains invalid samples");
    return true;
}

bool MicrotuningTable::validate(std::string* error) const {
    auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    if (name.empty() || name.size() > 160U) return fail("microtuning name must contain 1 to 160 characters");
    if (referenceNote >= 128U || !finite(referenceHertz) || referenceHertz < 8.0F || referenceHertz > 20000.0F)
        return fail("microtuning reference is out of range");
    for (float cents : centsOffset)
        if (!finite(cents) || cents < -9600.0F || cents > 9600.0F)
            return fail("microtuning cents offset is out of range");
    return true;
}

float MicrotuningTable::frequency(std::uint8_t note, float additionalSemitones) const noexcept {
    const std::uint8_t bounded = static_cast<std::uint8_t>(std::min<unsigned>(note, 127U));
    const float cents = (static_cast<float>(bounded) - static_cast<float>(referenceNote)) * 100.0F +
                        centsOffset[bounded] + additionalSemitones * 100.0F;
    return referenceHertz * std::exp2(cents / 1200.0F);
}

namespace {
std::vector<std::string> data_lines(const std::filesystem::path& path, std::string* error) {
    std::ifstream input(path);
    if (!input) { if (error) *error = "could not open tuning file: " + path.string(); return {}; }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t comment = line.find('!');
        if (comment != std::string::npos) line.resize(comment);
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        const auto last = line.find_last_not_of(" \t\r\n");
        lines.push_back(line.substr(first, last - first + 1U));
    }
    return lines;
}

double scala_interval_cents(std::string_view token, bool* ok) {
    const std::size_t slash = token.find('/');
    try {
        if (slash != std::string_view::npos) {
            const double numerator = std::stod(std::string(token.substr(0, slash)));
            const double denominator = std::stod(std::string(token.substr(slash + 1U)));
            if (!(numerator > 0.0) || !(denominator > 0.0)) { *ok = false; return 0.0; }
            *ok = true; return 1200.0 * std::log2(numerator / denominator);
        }
        const std::string text(token);
        if (text.find('.') == std::string::npos) {
            const double ratio = std::stod(text);
            if (!(ratio > 0.0)) { *ok = false; return 0.0; }
            *ok = true; return 1200.0 * std::log2(ratio);
        }
        *ok = true; return std::stod(text);
    } catch (...) { *ok = false; return 0.0; }
}

std::uint64_t hash_microtuning(const MicrotuningTable& table) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (float value : table.centsOffset) {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            hash ^= static_cast<std::uint8_t>((bits >> shift) & 0xFFU);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

void rebuild_wavetable_hash(WavetableBank& bank) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const std::size_t count = static_cast<std::size_t>(bank.frameCount) * kWavetableSampleCount;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(bank.samples[i]);
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            hash ^= static_cast<std::uint8_t>((bits >> shift) & 0xFFU);
            hash *= 1099511628211ULL;
        }
    }
    bank.contentHash = hash;
}
}

std::optional<MicrotuningTable> import_scala_tuning(
    const std::filesystem::path& scalaPath,
    const std::optional<std::filesystem::path>& keyboardMapPath,
    std::string* error) {
    const auto lines = data_lines(scalaPath, error);
    if (lines.size() < 3U) { if (error && error->empty()) *error = "Scala file is incomplete"; return std::nullopt; }
    std::size_t scaleCount = 0;
    try {
        scaleCount = static_cast<std::size_t>(std::stoul(lines[1]));
    } catch (...) {
        if (error) *error = "Scala scale count is invalid";
        return std::nullopt;
    }
    if (scaleCount == 0U || scaleCount > 256U || lines.size() < scaleCount + 2U) {
        if (error) *error = "Scala scale count is out of range";
        return std::nullopt;
    }
    std::vector<double> degrees(scaleCount + 1U, 0.0);
    for (std::size_t i = 0; i < scaleCount; ++i) {
        bool ok = false; degrees[i + 1U] = scala_interval_cents(lines[i + 2U], &ok);
        if (!ok || !std::isfinite(degrees[i + 1U])) { if (error) *error = "invalid Scala interval"; return std::nullopt; }
    }
    const double period = degrees.back();
    if (!(period > 0.0)) { if (error) *error = "Scala period must be positive"; return std::nullopt; }

    int baseMidi = 60;
    int referenceMidi = 69;
    double referenceHertz = 440.0;
    int formalOctave = static_cast<int>(scaleCount);
    std::vector<int> mapping;
    int firstMidi = 0;
    int lastMidi = 127;
    if (keyboardMapPath) {
        const auto mapLines = data_lines(*keyboardMapPath, error);
        if (mapLines.size() < 7U) { if (error && error->empty()) *error = "keyboard map is incomplete"; return std::nullopt; }
        try {
            const int mapSize = std::stoi(mapLines[0]);
            firstMidi = std::stoi(mapLines[1]); lastMidi = std::stoi(mapLines[2]);
            baseMidi = std::stoi(mapLines[3]); referenceMidi = std::stoi(mapLines[4]);
            referenceHertz = std::stod(mapLines[5]); formalOctave = std::stoi(mapLines[6]);
            if (mapSize < 0 || mapSize > 256 || formalOctave <= 0) throw std::runtime_error("range");
            for (int i = 0; i < mapSize && 7U + static_cast<std::size_t>(i) < mapLines.size(); ++i) {
                const std::string& value = mapLines[7U + static_cast<std::size_t>(i)];
                mapping.push_back(value == "x" || value == "X" ? std::numeric_limits<int>::min() : std::stoi(value));
            }
        } catch (...) { if (error) *error = "keyboard map contains invalid values"; return std::nullopt; }
    }

    auto raw_cents = [&](int midi) -> double {
        const int delta = midi - baseMidi;
        if (!mapping.empty()) {
            const int mapSize = static_cast<int>(mapping.size());
            int octave = delta >= 0 ? delta / mapSize : -(((-delta) + mapSize - 1) / mapSize);
            int slot = delta - octave * mapSize;
            if (slot < 0) { slot += mapSize; --octave; }
            const int degree = mapping[static_cast<std::size_t>(slot)];
            if (degree == std::numeric_limits<int>::min()) return std::numeric_limits<double>::quiet_NaN();
            const int absoluteDegree = degree + octave * formalOctave;
            int scaleOctave = absoluteDegree >= 0 ? absoluteDegree / static_cast<int>(scaleCount)
                                                  : -(((-absoluteDegree) + static_cast<int>(scaleCount) - 1) / static_cast<int>(scaleCount));
            int scaleDegree = absoluteDegree - scaleOctave * static_cast<int>(scaleCount);
            if (scaleDegree < 0) { scaleDegree += static_cast<int>(scaleCount); --scaleOctave; }
            return static_cast<double>(scaleOctave) * period + degrees[static_cast<std::size_t>(scaleDegree)];
        }
        int octave = delta >= 0 ? delta / static_cast<int>(scaleCount)
                                : -(((-delta) + static_cast<int>(scaleCount) - 1) / static_cast<int>(scaleCount));
        int degree = delta - octave * static_cast<int>(scaleCount);
        if (degree < 0) { degree += static_cast<int>(scaleCount); --octave; }
        return static_cast<double>(octave) * period + degrees[static_cast<std::size_t>(degree)];
    };

    const double referenceRaw = raw_cents(referenceMidi);
    if (!std::isfinite(referenceRaw)) { if (error) *error = "reference note is unmapped"; return std::nullopt; }
    MicrotuningTable result;
    result.enabled = true; result.name = lines[0];
    result.referenceNote = static_cast<std::uint8_t>(std::clamp(referenceMidi, 0, 127));
    result.referenceHertz = static_cast<float>(referenceHertz);
    for (int midi = 0; midi < 128; ++midi) {
        const double raw = (midi < firstMidi || midi > lastMidi) ? std::numeric_limits<double>::quiet_NaN() : raw_cents(midi);
        if (!std::isfinite(raw)) { result.centsOffset[static_cast<std::size_t>(midi)] = 0.0F; continue; }
        const double relative = raw - referenceRaw;
        result.centsOffset[static_cast<std::size_t>(midi)] = static_cast<float>(relative - (midi - referenceMidi) * 100.0);
    }
    result.contentHash = hash_microtuning(result);
    std::string validation;
    if (!result.validate(&validation)) { if (error) *error = validation; return std::nullopt; }
    return result;
}

bool apply_midi_tuning_standard(std::span<const std::uint8_t> message,
                                MicrotuningTable& table, std::string* error) {
    auto fail = [&](std::string text) { if (error) *error = std::move(text); return false; };
    if (message.size() < 8U || message.front() != 0xF0U || message.back() != 0xF7U ||
        (message[1] != 0x7EU && message[1] != 0x7FU) || message[3] != 0x08U)
        return fail("unsupported MIDI Tuning Standard message");
    if (message[4] == 0x02U) {
        const std::size_t count = message[6];
        if (message.size() < 8U + count * 4U) return fail("truncated single-note tuning message");
        std::size_t cursor = 7U;
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t note = message[cursor++];
            const std::uint8_t semitone = message[cursor++];
            const std::uint8_t fractionMsb = message[cursor++];
            const std::uint8_t fractionLsb = message[cursor++];
            const unsigned fraction = (static_cast<unsigned>(fractionMsb) << 7U) |
                                      static_cast<unsigned>(fractionLsb);
            if (note >= 128U || semitone >= 128U) continue;
            const float target = static_cast<float>(semitone) + static_cast<float>(fraction) / 16384.0F;
            table.centsOffset[note] = (target - static_cast<float>(note)) * 100.0F;
        }
    } else if (message[4] == 0x01U) {
        constexpr std::size_t dataStart = 22U;
        if (message.size() < dataStart + 128U * 3U + 2U) return fail("truncated bulk tuning dump");
        for (std::size_t note = 0; note < 128U; ++note) {
            const std::size_t cursor = dataStart + note * 3U;
            const std::uint8_t semitone = message[cursor];
            const unsigned fraction = (static_cast<unsigned>(message[cursor + 1U]) << 7U) | message[cursor + 2U];
            if (semitone == 0x7FU && fraction == 0x3FFFU) continue;
            const float target = static_cast<float>(semitone) + static_cast<float>(fraction) / 16384.0F;
            table.centsOffset[note] = (target - static_cast<float>(note)) * 100.0F;
        }
    } else return fail("unsupported MIDI Tuning Standard sub-message");
    table.enabled = true; table.name = "MIDI Tuning Standard"; table.contentHash = hash_microtuning(table);
    return table.validate(error);
}

bool wavetable_remove_dc(WavetableBank& bank) noexcept {
    if (bank.frameCount == 0U || bank.frameCount > kWavetableFrameCount) return false;
    for (std::size_t frame = 0; frame < bank.frameCount; ++frame) {
        float mean = 0.0F;
        for (std::size_t i = 0; i < kWavetableSampleCount; ++i) mean += bank.samples[frame * kWavetableSampleCount + i];
        mean /= static_cast<float>(kWavetableSampleCount);
        for (std::size_t i = 0; i < kWavetableSampleCount; ++i) bank.samples[frame * kWavetableSampleCount + i] -= mean;
    }
    rebuild_wavetable_hash(bank); return true;
}

bool wavetable_normalize(WavetableBank& bank) noexcept {
    if (bank.frameCount == 0U || bank.frameCount > kWavetableFrameCount) return false;
    for (std::size_t frame = 0; frame < bank.frameCount; ++frame) {
        float peak = 0.0F;
        for (std::size_t i = 0; i < kWavetableSampleCount; ++i) peak = std::max(peak, std::abs(bank.samples[frame * kWavetableSampleCount + i]));
        if (peak > 1.0e-8F) for (std::size_t i = 0; i < kWavetableSampleCount; ++i) bank.samples[frame * kWavetableSampleCount + i] /= peak;
    }
    rebuild_wavetable_hash(bank); return true;
}

bool wavetable_align_phases(WavetableBank& bank) noexcept {
    if (bank.frameCount < 2U || bank.frameCount > kWavetableFrameCount) return bank.frameCount == 1U;
    std::array<float, kWavetableSampleCount> scratch{};
    for (std::size_t frame = 1; frame < bank.frameCount; ++frame) {
        std::size_t bestShift = 0U; double best = -std::numeric_limits<double>::infinity();
        for (std::size_t shift = 0; shift < kWavetableSampleCount; ++shift) {
            double correlation = 0.0;
            for (std::size_t i = 0; i < kWavetableSampleCount; ++i)
                correlation += static_cast<double>(bank.samples[i]) * bank.samples[frame * kWavetableSampleCount + ((i + shift) % kWavetableSampleCount)];
            if (correlation > best) { best = correlation; bestShift = shift; }
        }
        for (std::size_t i = 0; i < kWavetableSampleCount; ++i)
            scratch[i] = bank.samples[frame * kWavetableSampleCount + ((i + bestShift) % kWavetableSampleCount)];
        std::copy(scratch.begin(), scratch.end(), bank.samples.begin() + static_cast<std::ptrdiff_t>(frame * kWavetableSampleCount));
    }
    rebuild_wavetable_hash(bank); return true;
}

bool wavetable_draw_frame(WavetableBank& bank, std::size_t frame,
                          std::span<const float> samples, std::string* error) {
    if (frame >= kWavetableFrameCount || samples.size() < 2U) { if (error) *error = "invalid wavetable frame or drawing"; return false; }
    for (std::size_t i = 0; i < kWavetableSampleCount; ++i) {
        const float position = static_cast<float>(i) * static_cast<float>(samples.size() - 1U) / static_cast<float>(kWavetableSampleCount - 1U);
        const std::size_t a = static_cast<std::size_t>(position);
        const std::size_t b = std::min(a + 1U, samples.size() - 1U);
        const float fraction = position - static_cast<float>(a);
        bank.samples[frame * kWavetableSampleCount + i] = clampf(samples[a] + (samples[b] - samples[a]) * fraction, -4.0F, 4.0F);
    }
    bank.frameCount = static_cast<std::uint8_t>(std::max<std::size_t>(bank.frameCount, frame + 1U));
    rebuild_wavetable_hash(bank); return true;
}

bool wavetable_spectral_morph(WavetableBank& bank, std::size_t frameA, std::size_t frameB,
                              std::size_t destinationFrame, float amount, std::string* error) {
    if (frameA >= bank.frameCount || frameB >= bank.frameCount ||
        destinationFrame >= kWavetableFrameCount) {
        if (error) *error = "spectral morph frame is out of range";
        return false;
    }
    const double t = std::clamp<double>(amount, 0.0, 1.0);
    std::array<std::complex<double>, kWavetableSampleCount> spectrumA{}, spectrumB{}, mixed{};
    for (std::size_t k = 0; k < kWavetableSampleCount; ++k) {
        for (std::size_t n = 0; n < kWavetableSampleCount; ++n) {
            const double angle = -2.0 * std::numbers::pi * static_cast<double>(k * n) / static_cast<double>(kWavetableSampleCount);
            const std::complex<double> basis{std::cos(angle), std::sin(angle)};
            spectrumA[k] += static_cast<double>(bank.samples[frameA * kWavetableSampleCount + n]) * basis;
            spectrumB[k] += static_cast<double>(bank.samples[frameB * kWavetableSampleCount + n]) * basis;
        }
        const double magnitude = std::abs(spectrumA[k]) * (1.0 - t) + std::abs(spectrumB[k]) * t;
        const double phaseA = std::arg(spectrumA[k]); const double phaseB = std::arg(spectrumB[k]);
        const double delta = std::remainder(phaseB - phaseA, 2.0 * std::numbers::pi);
        mixed[k] = std::polar(magnitude, phaseA + delta * t);
    }
    for (std::size_t n = 0; n < kWavetableSampleCount; ++n) {
        std::complex<double> value{};
        for (std::size_t k = 0; k < kWavetableSampleCount; ++k) {
            const double angle = 2.0 * std::numbers::pi * static_cast<double>(k * n) / static_cast<double>(kWavetableSampleCount);
            value += mixed[k] * std::complex<double>{std::cos(angle), std::sin(angle)};
        }
        bank.samples[destinationFrame * kWavetableSampleCount + n] = static_cast<float>(value.real() / static_cast<double>(kWavetableSampleCount));
    }
    bank.frameCount = static_cast<std::uint8_t>(std::max<std::size_t>(bank.frameCount, destinationFrame + 1U));
    wavetable_remove_dc(bank); wavetable_normalize(bank); return true;
}

std::optional<WavetableBank> import_wavetable_frames(std::span<const std::filesystem::path> paths, std::string* error) {
    if (paths.empty() || paths.size() > kWavetableFrameCount) { if (error) *error = "provide 1 to 8 wavetable frame files"; return std::nullopt; }
    WavetableBank result = WavetableBank::make_default(); result.frameCount = 0U; result.enabled = true; result.name = "Multi-frame Import";
    for (std::size_t i = 0; i < paths.size(); ++i) {
        auto frame = import_wavetable_from_audio(paths[i], error); if (!frame) return std::nullopt;
        std::copy_n(frame->samples.begin(), kWavetableSampleCount, result.samples.begin() + static_cast<std::ptrdiff_t>(i * kWavetableSampleCount));
        ++result.frameCount;
    }
    wavetable_remove_dc(result); wavetable_normalize(result); wavetable_align_phases(result); return result;
}

bool capture_chord_memory(SynthPreset& preset, std::size_t slot,
                          std::span<const std::uint8_t> notes, std::string_view name,
                          std::string* error) {
    if (slot >= preset.chordMemory.size() || notes.empty() ||
        notes.size() > kChordIntervalCount) {
        if (error) *error = "invalid chord-memory slot or note count";
        return false;
    }
    std::array<std::uint8_t, kChordIntervalCount> sorted{};
    const std::size_t noteCount = notes.size();
    std::copy_n(notes.begin(), noteCount, sorted.begin());
    for (std::size_t i = 1; i < noteCount; ++i) {
        const std::uint8_t value = sorted[i];
        std::size_t j = i;
        while (j > 0U && sorted[j - 1U] > value) {
            sorted[j] = sorted[j - 1U];
            --j;
        }
        sorted[j] = value;
    }
    auto& memory = preset.chordMemory[slot]; memory.noteCount = static_cast<std::uint8_t>(notes.size());
    memory.name = name.empty() ? "Captured Chord" : std::string(name.substr(0, 64U));
    for (std::size_t i = 0; i < notes.size(); ++i)
        memory.intervals[i] = static_cast<std::int8_t>(std::clamp<int>(static_cast<int>(sorted[i]) - sorted[0], -48, 96));
    return true;
}

std::vector<std::string> diff_synth_presets(const SynthPreset& a, const SynthPreset& b) {
    std::vector<std::string> differences;
    if (a.name != b.name) differences.push_back("name");
    if (a.oscillatorQuality != b.oscillatorQuality) differences.push_back("oscillator quality");
    if (a.filterQuality != b.filterQuality) differences.push_back("filter quality");
    if (a.mpe.zoneMode != b.mpe.zoneMode) differences.push_back("MPE zone");
    if (a.microtuning.contentHash != b.microtuning.contentHash || a.microtuning.enabled != b.microtuning.enabled) differences.push_back("microtuning");
    if (a.unison.enabled != b.unison.enabled || a.unison.voices != b.unison.voices || a.unison.detuneCents != b.unison.detuneCents) differences.push_back("unison");
    for (std::size_t i = 0; i < kSynthOscillatorCount; ++i)
        if (a.oscillators[i].waveform != b.oscillators[i].waveform || a.oscillators[i].gain != b.oscillators[i].gain || a.oscillators[i].semitones != b.oscillators[i].semitones) { differences.push_back("oscillators"); break; }
    if (a.filter.topology != b.filter.topology || a.filter.cutoffHertz != b.filter.cutoffHertz || a.filter.resonance != b.filter.resonance) differences.push_back("filter");
    if (a.ampEnvelope.attackSeconds != b.ampEnvelope.attackSeconds || a.ampEnvelope.releaseSeconds != b.ampEnvelope.releaseSeconds) differences.push_back("amplitude envelope");
    if (a.arpeggiator.enabled != b.arpeggiator.enabled || a.arpeggiator.mode != b.arpeggiator.mode || a.arpeggiator.stepCount != b.arpeggiator.stepCount) differences.push_back("arpeggiator");
    return differences;
}

SynthPreset SynthPreset::make_default() {
    SynthPreset result;
    const std::array<OscillatorWaveform, kSynthOscillatorCount> waves{
        OscillatorWaveform::SuperSaw, OscillatorWaveform::Saw, OscillatorWaveform::Pulse, OscillatorWaveform::Sine,
        OscillatorWaveform::Triangle, OscillatorWaveform::Organ, OscillatorWaveform::Noise, OscillatorWaveform::FoldedSine};
    const std::array<float, kSynthOscillatorCount> semitones{0.0F, 0.0F, -12.0F, -24.0F, 12.0F, 7.0F, 0.0F, 19.0F};
    const std::array<float, kSynthOscillatorCount> cents{-7.0F, 7.0F, 0.0F, 0.0F, 0.0F, -4.0F, 0.0F, 3.0F};
    const std::array<float, kSynthOscillatorCount> gains{0.20F,0.18F,0.14F,0.08F,0.07F,0.06F,0.018F,0.04F};
    for (std::size_t i = 0; i < result.oscillators.size(); ++i) {
        auto& osc = result.oscillators[i];
        osc.waveform = waves[i]; osc.semitones = semitones[i]; osc.cents = cents[i]; osc.gain = gains[i];
        osc.pan = i % 2U == 0U ? -0.18F : 0.18F;
        osc.enabled = i < 6U;
        osc.shape = i == 5U ? 0.68F : 0.5F;
        if (osc.waveform == OscillatorWaveform::Pulse) {
            osc.pulseWidth = 0.42F;
            osc.pwmDepth = 0.32F;
            osc.pwmRateHertz = 0.23F;
        }
    }
    result.filter.topology = FilterTopology::MoogLadder;
    result.filter.bassCompensation = 0.4F;
    result.wavetable = WavetableBank::make_default();
    result.microtuning = MicrotuningTable::equal_temperament();
    result.metadata.version = "1.25";
    result.lfos[0] = {false, LfoWaveform::Sine, 5.2F, 1.0F, 0.0F, 0.0F, true, false, 1.0F};
    result.lfos[1] = {false, LfoWaveform::Triangle, 0.25F, 1.0F, 0.25F, 0.0F, true, true, 4.0F};
    static constexpr std::array<std::string_view, kChordMemorySlotCount> chordNames{
        "Major", "Minor", "Dominant 7", "Minor 7", "Sus 2", "Sus 4", "Fifth", "User"};
    static constexpr std::array<std::array<std::int8_t, kChordIntervalCount>, kChordMemorySlotCount> chordIntervals{{
        {{0,4,7,12,16,19,24,28}}, {{0,3,7,12,15,19,24,27}},
        {{0,4,7,10,12,16,19,22}}, {{0,3,7,10,12,15,19,22}},
        {{0,2,7,12,14,19,24,26}}, {{0,5,7,12,17,19,24,29}},
        {{0,7,12,19,24,31,36,43}}, {{0,4,7,11,14,19,24,28}}
    }};
    static constexpr std::array<std::uint8_t, kChordMemorySlotCount> chordCounts{3,3,4,4,3,3,3,5};
    for (std::size_t i = 0; i < result.chordMemory.size(); ++i) {
        result.chordMemory[i].name = chordNames[i];
        result.chordMemory[i].intervals = chordIntervals[i];
        result.chordMemory[i].noteCount = chordCounts[i];
    }
    result.arpeggiator.stepCount = 8;
    for (auto& step : result.arpeggiator.steps) step = {};
    return result;
}

bool SynthPreset::validate(std::string* error) const {
    auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    auto in_range = [](float value, float low, float high) {
        return finite(value) && value >= low && value <= high;
    };
    if (name.empty() || name.size() > 128U) return fail("preset name must contain 1 to 128 characters");
    if (!validate_adsr(ampEnvelope) || !validate_adsr(filter.envelope)) return fail("invalid ADSR envelope");
    for (const auto& osc : oscillators) {
        if (!in_range(osc.gain, 0.0F, 2.0F) || !in_range(osc.pan, -1.0F, 1.0F) ||
            !in_range(osc.semitones, -96.0F, 96.0F) || !in_range(osc.cents, -100.0F, 100.0F) ||
            !in_range(osc.pulseWidth, 0.03F, 0.97F) || !in_range(osc.pwmDepth, 0.0F, 1.0F) ||
            !in_range(osc.pwmRateHertz, 0.01F, 40.0F) || !in_range(osc.shape, 0.0F, 1.0F) ||
            !in_range(osc.phaseOffset, -64.0F, 64.0F) ||
            osc.hardSyncSource < -1 || osc.hardSyncSource >= static_cast<std::int8_t>(kSynthOscillatorCount) ||
            osc.frequencyModSource < -1 || osc.frequencyModSource >= static_cast<std::int8_t>(kSynthOscillatorCount) ||
            osc.ringModSource < -1 || osc.ringModSource >= static_cast<std::int8_t>(kSynthOscillatorCount) ||
            !in_range(osc.frequencyModAmount, -4.0F, 4.0F) || !in_range(osc.ringModDepth, 0.0F, 1.0F) ||
            !in_range(osc.subOscillatorLevel, 0.0F, 1.0F) || osc.subOscillatorOctaves < 1U ||
            osc.subOscillatorOctaves > 3U || !in_range(osc.wavetablePosition, 0.0F, 1.0F) ||
            !in_range(osc.sampleStart, 0.0F, 1.0F) || !in_range(osc.sampleEnd, 0.0F, 1.0F) ||
            osc.sampleEnd <= osc.sampleStart || !in_range(osc.sampleLoopStart, 0.0F, 1.0F) ||
            !in_range(osc.sampleLoopEnd, 0.0F, 1.0F) || osc.sampleLoopEnd <= osc.sampleLoopStart ||
            !in_range(osc.sampleVelocityToGain, 0.0F, 1.0F) || !in_range(osc.grainPosition, 0.0F, 1.0F) ||
            !in_range(osc.grainSizeMilliseconds, 5.0F, 500.0F) ||
            !in_range(osc.grainDensityHertz, 0.5F, 120.0F) || !in_range(osc.grainSpray, 0.0F, 1.0F) ||
            !in_range(osc.grainPitchSemitones, -48.0F, 48.0F) ||
            !in_range(osc.grainStereoSpread, 0.0F, 1.0F) ||
            !in_range(osc.grainStereoMotion, 0.0F, 1.0F) ||
            !in_range(osc.grainEnvelopeCurve, 0.25F, 4.0F) ||
            !in_range(osc.grainReverseProbability, 0.0F, 1.0F) ||
            !in_range(osc.grainPitchRandomSemitones, 0.0F, 48.0F) ||
            !in_range(osc.grainPitchQuantizeSemitones, 0.0F, 24.0F) ||
            !in_range(osc.grainDensityVelocity, -1.0F, 1.0F) ||
            !in_range(osc.grainDensityTimbre, -1.0F, 1.0F) ||
            !in_range(osc.physicalSizeMeters, 0.05F, 4.0F) ||
            !in_range(osc.physicalTension, 0.05F, 1.0F) ||
            !in_range(osc.physicalStiffness, 0.0F, 1.0F) ||
            !in_range(osc.physicalDamping, 0.01F, 0.98F) ||
            !in_range(osc.physicalBrightness, 0.0F, 1.0F) ||
            !in_range(osc.physicalExcitationPosition, 0.02F, 0.98F) ||
            !in_range(osc.physicalHardness, 0.0F, 1.0F) ||
            !in_range(osc.physicalPickupPosition, 0.02F, 0.98F) ||
            !in_range(osc.physicalBodyAmount, 0.0F, 1.0F) ||
            !in_range(osc.physicalContinuousAmount, 0.0F, 1.5F) ||
            !in_range(osc.physicalVelocityToHardness, 0.0F, 1.5F) ||
            !in_range(osc.physicalPressureToBow, 0.0F, 1.5F) ||
            !in_range(osc.physicalTimbreToBrightness, 0.0F, 1.5F) ||
            osc.physicalModeCount < 4U || osc.physicalModeCount > kPhysicalModelMaxModes ||
            !in_range(osc.physicalReedStiffness, 0.05F, 0.95F) ||
            !in_range(osc.physicalEmbouchure, 0.05F, 0.95F) ||
            !in_range(osc.physicalBreathNoise, 0.0F, 1.0F) ||
            !in_range(osc.physicalThroatFreqHz, 200.0F, 8000.0F) ||
            !in_range(osc.physicalThroatQ, 0.5F, 8.0F) ||
            !in_range(osc.physicalBoreTaper, 0.0F, 1.0F) ||
            !in_range(osc.physicalPressureToBreath, 0.0F, 1.5F) ||
            !in_range(osc.physicalVelocityToEmbouchure, 0.0F, 1.0F))
            return fail("invalid oscillator parameters");
    }
    if (!in_range(filter.cutoffHertz, 18.0F, 24000.0F) || !in_range(filter.resonance, 0.0F, 1.0F) ||
        !in_range(filter.envelopeAmountOctaves, -12.0F, 12.0F) || !in_range(filter.keyTrack, -2.0F, 2.0F) ||
        !in_range(filter.drive, 0.05F, 24.0F) || !in_range(filter.bassCompensation, 0.0F, 1.0F) ||
        !in_range(filter.morph, 0.0F, 1.0F) || !in_range(filter.ms20HighPassCutoffHertz, 12.0F, 18000.0F) ||
        !in_range(filter.selfOscillation, 0.5F, 1.35F) ||
        (filter.oversampling != FilterOversampling::X1 && filter.oversampling != FilterOversampling::X2 &&
         filter.oversampling != FilterOversampling::X4))
        return fail("invalid filter parameters");
    if (!in_range(tuning.referenceHertz, 400.0F, 480.0F) ||
        !in_range(tuning.transposeSemitones, -48.0F, 48.0F) || !in_range(tuning.fineCents, -100.0F, 100.0F) ||
        !in_range(tuning.analogDriftCents, 0.0F, 30.0F))
        return fail("invalid tuning parameters");
    for (const auto& lfo : lfos) {
        if (!in_range(lfo.rateHertz, 0.001F, 100.0F) || !in_range(lfo.depth, 0.0F, 1.0F) ||
            !in_range(lfo.phase, -64.0F, 64.0F) || !in_range(lfo.fadeInSeconds, 0.0F, 60.0F) ||
            !in_range(lfo.beatsPerCycle, 0.03125F, 32.0F)) return fail("invalid LFO parameters");
    }
    for (const auto& slot : modulation) {
        if (!in_range(slot.amount, -1.0F, 1.0F) || !in_range(slot.smoothingMilliseconds, 0.0F, 2000.0F) ||
            static_cast<unsigned>(slot.source) > static_cast<unsigned>(ModulationSource::Macro4) ||
            static_cast<unsigned>(slot.destination) > static_cast<unsigned>(ModulationDestination::Osc8Gain))
            return fail("invalid modulation matrix slot");
    }
    for (float value : macros.values) if (!in_range(value, 0.0F, 1.0F)) return fail("invalid macro value");
    for (const auto& nameValue : macros.names) if (nameValue.empty() || nameValue.size() > 32U) return fail("invalid macro name");
    for (const auto& mapping : midiLearn) {
        if (mapping.controller > 127U || mapping.macroIndex >= kSynthMacroCount ||
            !in_range(mapping.minimum, 0.0F, 1.0F) || !in_range(mapping.maximum, 0.0F, 1.0F))
            return fail("invalid MIDI learn mapping");
    }
    std::string wavetableError;
    if (!wavetable.validate(&wavetableError)) return fail(wavetableError);
    std::string sampleError;
    if (!sampleBank.validate(&sampleError)) return fail(sampleError);
    std::string tuningError;
    if (!microtuning.validate(&tuningError)) return fail(tuningError);
    if (mpe.lowerMasterChannel > 15U || mpe.upperMasterChannel > 15U ||
        mpe.lowerMemberCount > 15U || mpe.upperMemberCount > 15U || mpe.timbreController > 127U ||
        !in_range(mpe.masterPitchBendRangeSemitones, 0.0F, 96.0F) ||
        !in_range(mpe.memberPitchBendRangeSemitones, 0.0F, 96.0F)) return fail("invalid MPE parameters");
    if (mpe.zoneMode == MpeZoneMode::Dual) {
        const unsigned lowerLast = std::min<unsigned>(15U, mpe.lowerMasterChannel + mpe.lowerMemberCount);
        const unsigned upperFirst = mpe.upperMasterChannel >= mpe.upperMemberCount
            ? mpe.upperMasterChannel - mpe.upperMemberCount : 0U;
        if (lowerLast >= upperFirst) return fail("MPE lower and upper zones overlap");
    }
    if (unison.voices < 1U || unison.voices > kSynthUnisonMax ||
        !in_range(unison.detuneCents, 0.0F, 100.0F) || !in_range(unison.stereoSpread, 0.0F, 1.0F) ||
        !in_range(unison.phaseSpread, 0.0F, 1.0F)) return fail("invalid unison parameters");
    if (metadata.author.size() > 128U || metadata.category.size() > 64U || metadata.version.size() > 32U ||
        metadata.tagCount > metadata.tags.size()) return fail("invalid preset metadata");
    if (chord.noteCount < 1U || chord.noteCount > kChordIntervalCount || chord.inversion < -7 || chord.inversion > 7 ||
        chord.spreadOctaves > 4U || !in_range(chord.velocityScale, 0.1F, 1.5F) ||
        chord.scaleRoot > 11U || !in_range(chord.strumMilliseconds, 0.0F, 250.0F))
        return fail("invalid chord parameters");
    for (const std::int8_t interval : chord.customIntervals)
        if (interval < -48 || interval > 96) return fail("invalid custom chord interval");
    if (chord.memorySlot >= kChordMemorySlotCount) return fail("invalid chord memory slot");
    for (const auto& memory : chordMemory) {
        if (memory.name.empty() || memory.name.size() > 32U || memory.noteCount < 1U ||
            memory.noteCount > kChordIntervalCount) return fail("invalid chord memory");
        for (const std::int8_t interval : memory.intervals)
            if (interval < -48 || interval > 96) return fail("invalid chord memory interval");
    }
    if (!in_range(arpeggiator.tempoBpm, 20.0F, 400.0F) || !in_range(arpeggiator.gate, 0.02F, 1.0F) ||
        !in_range(arpeggiator.swing, 0.0F, 0.75F) || arpeggiator.octaveRange < 1U ||
        arpeggiator.octaveRange > 4U || arpeggiator.stepCount < 1U ||
        arpeggiator.stepCount > kArpeggiatorStepCount ||
        !in_range(arpeggiator.externalTempoBpm, 20.0F, 400.0F))
        return fail("invalid arpeggiator parameters");
    for (const auto& step : arpeggiator.steps) {
        if (step.transpose < -48 || step.transpose > 48 || step.octaveOffset < -4 || step.octaveOffset > 4 ||
            !in_range(step.velocityScale, 0.0F, 2.0F) || !in_range(step.gateScale, 0.1F, 2.0F) ||
            !in_range(step.probability, 0.0F, 1.0F) || step.ratchets < 1U || step.ratchets > 8U ||
            !in_range(step.macro1, -1.0F, 1.0F) || !in_range(step.macro2, -1.0F, 1.0F) ||
            !in_range(step.macro3, -1.0F, 1.0F) || !in_range(step.macro4, -1.0F, 1.0F))
            return fail("invalid arpeggiator step");
    }
    if (!in_range(distortion.drive, 0.05F, 32.0F) || !in_range(distortion.mix, 0.0F, 1.0F))
        return fail("invalid distortion parameters");
    if (!in_range(eq.lowGainDb, -24.0F, 24.0F) || !in_range(eq.midGainDb, -24.0F, 24.0F) ||
        !in_range(eq.highGainDb, -24.0F, 24.0F))
        return fail("invalid equalizer parameters");
    if (!in_range(chorus.rateHertz, 0.01F, 20.0F) || !in_range(chorus.depthMilliseconds, 0.0F, 30.0F) ||
        !in_range(chorus.mix, 0.0F, 1.0F))
        return fail("invalid chorus parameters");
    if (!in_range(phaser.rateHertz, 0.01F, 20.0F) || !in_range(phaser.depth, 0.0F, 1.0F) ||
        !in_range(phaser.feedback, -0.95F, 0.95F) || !in_range(phaser.mix, 0.0F, 1.0F))
        return fail("invalid phaser parameters");
    if (!in_range(delay.timeSeconds, 0.01F, 1.95F) || !in_range(delay.feedback, 0.0F, 0.94F) ||
        !in_range(delay.mix, 0.0F, 1.0F))
        return fail("invalid delay parameters");
    if (!in_range(reverb.roomSize, 0.0F, 1.0F) || !in_range(reverb.damping, 0.0F, 0.98F) ||
        !in_range(reverb.width, 0.0F, 1.0F) || !in_range(reverb.mix, 0.0F, 1.0F))
        return fail("invalid reverb parameters");
    if (!in_range(compressor.thresholdDb, -60.0F, 0.0F) || !in_range(compressor.ratio, 1.0F, 30.0F) ||
        !in_range(compressor.attackMilliseconds, 0.05F, 500.0F) ||
        !in_range(compressor.releaseMilliseconds, 1.0F, 5000.0F) ||
        !in_range(compressor.makeupDb, -24.0F, 24.0F))
        return fail("invalid compressor parameters");
    if (!in_range(limiter.ceilingDb, -24.0F, 0.0F) ||
        !in_range(limiter.releaseMilliseconds, 1.0F, 5000.0F))
        return fail("invalid limiter parameters");
    if (!in_range(masterGain, 0.0F, 2.0F) || !in_range(masterPan, -1.0F, 1.0F) ||
        !in_range(pitchBendRangeSemitones, 0.0F, 48.0F))
        return fail("invalid master parameters");
    return true;
}

std::string SynthPreset::serialize() const {
    std::ostringstream out;
    out << "DVE_SYNTH_PRESET=5\nname=" << name << '\n'
        << "master.gain=" << masterGain << "\nmaster.pan=" << masterPan
        << "\nmaster.bend=" << pitchBendRangeSemitones << "\nmaster.midiThru=" << midiThru << '\n'
        << "tuning.reference=" << tuning.referenceHertz << "\ntuning.transpose=" << tuning.transposeSemitones
        << "\ntuning.fineCents=" << tuning.fineCents << "\ntuning.driftCents=" << tuning.analogDriftCents << '\n'
        << "quality.oscillator=" << static_cast<unsigned>(oscillatorQuality)
        << "\nquality.filter=" << static_cast<unsigned>(filterQuality)
        << "\nunison.enabled=" << unison.enabled
        << "\nunison.voices=" << static_cast<unsigned>(unison.voices)
        << "\nunison.detune=" << unison.detuneCents
        << "\nunison.spread=" << unison.stereoSpread
        << "\nunison.phase=" << unison.phaseSpread
        << "\nunison.preserve=" << unison.preserveLevel
        << "\nmpe.zone=" << static_cast<unsigned>(mpe.zoneMode)
        << "\nmpe.lowerMaster=" << static_cast<unsigned>(mpe.lowerMasterChannel)
        << "\nmpe.lowerMembers=" << static_cast<unsigned>(mpe.lowerMemberCount)
        << "\nmpe.upperMaster=" << static_cast<unsigned>(mpe.upperMasterChannel)
        << "\nmpe.upperMembers=" << static_cast<unsigned>(mpe.upperMemberCount)
        << "\nmpe.masterBend=" << mpe.masterPitchBendRangeSemitones
        << "\nmpe.memberBend=" << mpe.memberPitchBendRangeSemitones
        << "\nmpe.timbreCc=" << static_cast<unsigned>(mpe.timbreController)
        << "\nmpe.masterSustain=" << mpe.masterSustainToMembers
        << "\nmicro.enabled=" << microtuning.enabled
        << "\nmicro.name=" << microtuning.name
        << "\nmicro.referenceNote=" << static_cast<unsigned>(microtuning.referenceNote)
        << "\nmicro.referenceHertz=" << microtuning.referenceHertz
        << "\nmicro.hash=" << microtuning.contentHash
        << "\nmetadata.author=" << metadata.author
        << "\nmetadata.category=" << metadata.category
        << "\nmetadata.version=" << metadata.version
        << "\nmetadata.favorite=" << metadata.favorite
        << "\nmetadata.tagCount=" << static_cast<unsigned>(metadata.tagCount) << '\n'
        << "amp.attack=" << ampEnvelope.attackSeconds << "\namp.decay=" << ampEnvelope.decaySeconds
        << "\namp.sustain=" << ampEnvelope.sustainLevel << "\namp.release=" << ampEnvelope.releaseSeconds
        << "\namp.curve=" << envelope_curve_name(ampEnvelope.curve)
        << "\namp.delay=" << ampEnvelope.delaySeconds << "\namp.hold=" << ampEnvelope.holdSeconds << '\n'
        << "filter.enabled=" << filter.enabled << "\nfilter.topology=" << filter_topology_token(filter.topology)
        << "\nfilter.mode=" << filter_mode_token(filter.mode)
        << "\nfilter.cutoff=" << filter.cutoffHertz << "\nfilter.resonance=" << filter.resonance
        << "\nfilter.envAmount=" << filter.envelopeAmountOctaves << "\nfilter.keyTrack=" << filter.keyTrack
        << "\nfilter.drive=" << filter.drive << "\nfilter.bassComp=" << filter.bassCompensation
        << "\nfilter.morph=" << filter.morph << "\nfilter.altRevision=" << filter.alternateRevision
        << "\nfilter.env.attack=" << filter.envelope.attackSeconds
        << "\nfilter.env.decay=" << filter.envelope.decaySeconds
        << "\nfilter.env.sustain=" << filter.envelope.sustainLevel
        << "\nfilter.env.release=" << filter.envelope.releaseSeconds
        << "\nfilter.env.curve=" << envelope_curve_name(filter.envelope.curve)
        << "\nfilter.env.delay=" << filter.envelope.delaySeconds
        << "\nfilter.env.hold=" << filter.envelope.holdSeconds
        << "\nfilter.oversampling=" << static_cast<unsigned>(filter.oversampling)
        << "\nfilter.ms20HighPass=" << filter.ms20HighPassCutoffHertz
        << "\nfilter.selfOscillation=" << filter.selfOscillation << '\n'
        << "chord.enabled=" << chord.enabled << "\nchord.type=" << chord_type_token(chord.type)
        << "\nchord.noteCount=" << static_cast<unsigned>(chord.noteCount)
        << "\nchord.inversion=" << static_cast<int>(chord.inversion)
        << "\nchord.spread=" << static_cast<unsigned>(chord.spreadOctaves)
        << "\nchord.velocity=" << chord.velocityScale
        << "\nchord.scale=" << chord_scale_token(chord.scale)
        << "\nchord.scaleRoot=" << static_cast<unsigned>(chord.scaleRoot)
        << "\nchord.strumMs=" << chord.strumMilliseconds
        << "\nchord.useMemory=" << chord.useMemory
        << "\nchord.memorySlot=" << static_cast<unsigned>(chord.memorySlot) << '\n'
        << "arp.enabled=" << arpeggiator.enabled << "\narp.latch=" << arpeggiator.latch
        << "\narp.midiOut=" << arpeggiator.sendMidiOutput
        << "\narp.retrigger=" << arpeggiator.retriggerEnvelopes
        << "\narp.mode=" << arpeggiator_mode_token(arpeggiator.mode)
        << "\narp.division=" << arpeggiator_division_token(arpeggiator.division)
        << "\narp.tempo=" << arpeggiator.tempoBpm << "\narp.gate=" << arpeggiator.gate
        << "\narp.swing=" << arpeggiator.swing
        << "\narp.octaves=" << static_cast<unsigned>(arpeggiator.octaveRange)
        << "\narp.stepCount=" << static_cast<unsigned>(arpeggiator.stepCount)
        << "\narp.seed=" << arpeggiator.randomSeed
        << "\narp.clock=" << arp_clock_token(arpeggiator.clockSource)
        << "\narp.externalTempo=" << arpeggiator.externalTempoBpm << '\n';
    for (std::size_t i = 0; i < chord.customIntervals.size(); ++i)
        out << "chord.interval" << i << '=' << static_cast<int>(chord.customIntervals[i]) << '\n';
    for (std::size_t slot = 0; slot < chordMemory.size(); ++slot) {
        const std::string prefix = "chordMemory" + std::to_string(slot) + ".";
        out << prefix << "name=" << chordMemory[slot].name << '\n'
            << prefix << "noteCount=" << static_cast<unsigned>(chordMemory[slot].noteCount) << '\n';
        for (std::size_t interval = 0; interval < chordMemory[slot].intervals.size(); ++interval)
            out << prefix << "interval" << interval << '='
                << static_cast<int>(chordMemory[slot].intervals[interval]) << '\n';
    }
    for (std::size_t i = 0; i < metadata.tagCount; ++i) out << "metadata.tag" << i << '=' << metadata.tags[i] << '\n';
    for (std::size_t i = 0; i < microtuning.centsOffset.size(); ++i)
        out << "micro.offset" << i << '=' << microtuning.centsOffset[i] << '\n';
    for (std::size_t i = 0; i < oscillators.size(); ++i) {
        const auto& osc = oscillators[i];
        const std::string prefix = "osc" + std::to_string(i) + ".";
        out << prefix << "enabled=" << osc.enabled << '\n' << prefix << "wave=" << waveform_name(osc.waveform) << '\n'
            << prefix << "gain=" << osc.gain << '\n' << prefix << "pan=" << osc.pan << '\n'
            << prefix << "semitones=" << osc.semitones << '\n' << prefix << "cents=" << osc.cents << '\n'
            << prefix << "pulseWidth=" << osc.pulseWidth << '\n' << prefix << "pwmDepth=" << osc.pwmDepth << '\n'
            << prefix << "pwmRate=" << osc.pwmRateHertz << '\n' << prefix << "shape=" << osc.shape << '\n'
            << prefix << "phaseOffset=" << osc.phaseOffset << '\n' << prefix << "keySync=" << osc.keySync << '\n'
            << prefix << "hardSync=" << static_cast<int>(osc.hardSyncSource) << '\n'
            << prefix << "fmSource=" << static_cast<int>(osc.frequencyModSource) << '\n'
            << prefix << "fmMode=" << fm_mode_token(osc.frequencyModMode) << '\n'
            << prefix << "fmAmount=" << osc.frequencyModAmount << '\n'
            << prefix << "ringSource=" << static_cast<int>(osc.ringModSource) << '\n'
            << prefix << "ringDepth=" << osc.ringModDepth << '\n'
            << prefix << "subLevel=" << osc.subOscillatorLevel << '\n'
            << prefix << "subOctaves=" << static_cast<unsigned>(osc.subOscillatorOctaves) << '\n'
            << prefix << "wavetablePosition=" << osc.wavetablePosition << '\n'
            << prefix << "sampleStart=" << osc.sampleStart << '\n'
            << prefix << "sampleEnd=" << osc.sampleEnd << '\n'
            << prefix << "sampleLoopStart=" << osc.sampleLoopStart << '\n'
            << prefix << "sampleLoopEnd=" << osc.sampleLoopEnd << '\n'
            << prefix << "sampleLoop=" << osc.sampleLoop << '\n'
            << prefix << "sampleReverse=" << osc.sampleReverse << '\n'
            << prefix << "sampleOneShot=" << osc.sampleOneShot << '\n'
            << prefix << "sampleKeyTrack=" << osc.sampleKeyTrack << '\n'
            << prefix << "sampleVelocity=" << osc.sampleVelocityToGain << '\n'
            << prefix << "grainPosition=" << osc.grainPosition << '\n'
            << prefix << "grainSizeMs=" << osc.grainSizeMilliseconds << '\n'
            << prefix << "grainDensity=" << osc.grainDensityHertz << '\n'
            << prefix << "grainSpray=" << osc.grainSpray << '\n'
            << prefix << "grainPitch=" << osc.grainPitchSemitones << '\n'
            << prefix << "grainSpread=" << osc.grainStereoSpread << '\n'
            << prefix << "grainStereoMotion=" << osc.grainStereoMotion << '\n'
            << prefix << "grainEnvelopeCurve=" << osc.grainEnvelopeCurve << '\n'
            << prefix << "grainReverseProbability=" << osc.grainReverseProbability << '\n'
            << prefix << "grainPitchRandom=" << osc.grainPitchRandomSemitones << '\n'
            << prefix << "grainPitchQuantize=" << osc.grainPitchQuantizeSemitones << '\n'
            << prefix << "grainDensityVelocity=" << osc.grainDensityVelocity << '\n'
            << prefix << "grainDensityTimbre=" << osc.grainDensityTimbre << '\n'
            << prefix << "grainFreeze=" << osc.grainFreeze << '\n'
            << prefix << "grainWindow=" << static_cast<unsigned>(osc.grainWindow) << '\n'
            << prefix << "physicalModel=" << physical_model_token(osc.physicalModel) << '\n'
            << prefix << "physicalMaterial=" << physical_material_token(osc.physicalMaterial) << '\n'
            << prefix << "physicalExcitation=" << physical_excitation_token(osc.physicalExcitation) << '\n'
            << prefix << "physicalSizeMeters=" << osc.physicalSizeMeters << '\n'
            << prefix << "physicalTension=" << osc.physicalTension << '\n'
            << prefix << "physicalStiffness=" << osc.physicalStiffness << '\n'
            << prefix << "physicalDamping=" << osc.physicalDamping << '\n'
            << prefix << "physicalBrightness=" << osc.physicalBrightness << '\n'
            << prefix << "physicalExcitationPosition=" << osc.physicalExcitationPosition << '\n'
            << prefix << "physicalHardness=" << osc.physicalHardness << '\n'
            << prefix << "physicalPickupPosition=" << osc.physicalPickupPosition << '\n'
            << prefix << "physicalBodyAmount=" << osc.physicalBodyAmount << '\n'
            << prefix << "physicalContinuousAmount=" << osc.physicalContinuousAmount << '\n'
            << prefix << "physicalVelocityToHardness=" << osc.physicalVelocityToHardness << '\n'
            << prefix << "physicalPressureToBow=" << osc.physicalPressureToBow << '\n'
            << prefix << "physicalTimbreToBrightness=" << osc.physicalTimbreToBrightness << '\n'
            << prefix << "physicalModeCount=" << static_cast<unsigned>(osc.physicalModeCount) << '\n'
            << prefix << "physicalDriver=" << physical_driver_token(osc.physicalDriver) << '\n'
            << prefix << "physicalReedStiffness=" << osc.physicalReedStiffness << '\n'
            << prefix << "physicalEmbouchure=" << osc.physicalEmbouchure << '\n'
            << prefix << "physicalBreathNoise=" << osc.physicalBreathNoise << '\n'
            << prefix << "physicalThroatFreqHz=" << osc.physicalThroatFreqHz << '\n'
            << prefix << "physicalThroatQ=" << osc.physicalThroatQ << '\n'
            << prefix << "physicalBoreTaper=" << osc.physicalBoreTaper << '\n'
            << prefix << "physicalPressureToBreath=" << osc.physicalPressureToBreath << '\n'
            << prefix << "physicalVelocityToEmbouchure=" << osc.physicalVelocityToEmbouchure << '\n';
    }
    for (std::size_t i = 0; i < arpeggiator.steps.size(); ++i) {
        const auto& step = arpeggiator.steps[i];
        const std::string prefix = "arp.step" + std::to_string(i) + ".";
        out << prefix << "enabled=" << step.enabled << '\n'
            << prefix << "condition=" << static_cast<unsigned>(step.condition) << '\n'
            << prefix << "automation=" << static_cast<unsigned>(step.automationCurve) << '\n'
            << prefix << "accent=" << step.accent << '\n'
            << prefix << "slide=" << step.slide << '\n'
            << prefix << "transpose=" << static_cast<int>(step.transpose) << '\n'
            << prefix << "octave=" << static_cast<int>(step.octaveOffset) << '\n'
            << prefix << "velocity=" << step.velocityScale << '\n'
            << prefix << "gate=" << step.gateScale << '\n'
            << prefix << "probability=" << step.probability << '\n'
            << prefix << "ratchets=" << static_cast<unsigned>(step.ratchets) << '\n'
            << prefix << "tie=" << step.tie << '\n'
            << prefix << "macro1=" << step.macro1 << '\n'
            << prefix << "macro2=" << step.macro2 << '\n'
            << prefix << "macro3=" << step.macro3 << '\n'
            << prefix << "macro4=" << step.macro4 << '\n';
    }
    for (std::size_t i = 0; i < lfos.size(); ++i) {
        const auto& lfo = lfos[i];
        const std::string prefix = "lfo" + std::to_string(i) + ".";
        out << prefix << "enabled=" << lfo.enabled << '\n'
            << prefix << "wave=" << lfo_waveform_token(lfo.waveform) << '\n'
            << prefix << "rate=" << lfo.rateHertz << '\n'
            << prefix << "depth=" << lfo.depth << '\n'
            << prefix << "phase=" << lfo.phase << '\n'
            << prefix << "fade=" << lfo.fadeInSeconds << '\n'
            << prefix << "keySync=" << lfo.keySync << '\n'
            << prefix << "tempoSync=" << lfo.tempoSync << '\n'
            << prefix << "beats=" << lfo.beatsPerCycle << '\n';
    }
    for (std::size_t i = 0; i < modulation.size(); ++i) {
        const auto& slot = modulation[i];
        const std::string prefix = "mod" + std::to_string(i) + ".";
        out << prefix << "enabled=" << slot.enabled << '\n'
            << prefix << "source=" << modulation_source_token(slot.source) << '\n'
            << prefix << "destination=" << modulation_destination_token(slot.destination) << '\n'
            << prefix << "amount=" << slot.amount << '\n'
            << prefix << "curve=" << modulation_curve_token(slot.curve) << '\n'
            << prefix << "polarity=" << static_cast<unsigned>(slot.polarity) << '\n'
            << prefix << "smoothingMs=" << slot.smoothingMilliseconds << '\n';
    }
    for (std::size_t i = 0; i < macros.values.size(); ++i)
        out << "macro" << i << ".name=" << macros.names[i] << '\n'
            << "macro" << i << ".value=" << macros.values[i] << '\n';
    for (std::size_t i = 0; i < midiLearn.size(); ++i) {
        const auto& mapping = midiLearn[i];
        const std::string prefix = "midiLearn" + std::to_string(i) + ".";
        out << prefix << "enabled=" << mapping.enabled << '\n'
            << prefix << "controller=" << static_cast<unsigned>(mapping.controller) << '\n'
            << prefix << "macro=" << static_cast<unsigned>(mapping.macroIndex) << '\n'
            << prefix << "minimum=" << mapping.minimum << '\n'
            << prefix << "maximum=" << mapping.maximum << '\n'
            << prefix << "inverted=" << mapping.inverted << '\n';
    }
    out << "wavetable.name=" << wavetable.name << '\n'
        << "wavetable.enabled=" << wavetable.enabled << '\n'
        << "wavetable.frameCount=" << static_cast<unsigned>(wavetable.frameCount) << '\n'
        << "wavetable.hash=" << wavetable.contentHash << '\n';
    for (std::size_t frame = 0; frame < wavetable.frameCount; ++frame) {
        out << "wavetable.frame" << frame << '=';
        for (std::size_t sample = 0; sample < kWavetableSampleCount; ++sample) {
            if (sample != 0U) out << ',';
            out << wavetable.samples[frame * kWavetableSampleCount + sample];
        }
        out << '\n';
    }
    out << "sample.name=" << sampleBank.name << '\n'
        << "sample.enabled=" << sampleBank.enabled << '\n'
        << "sample.rate=" << sampleBank.sampleRate << '\n'
        << "sample.root=" << static_cast<unsigned>(sampleBank.rootNote) << '\n'
        << "sample.frames=" << sampleBank.frameCount << '\n'
        << "sample.hash=" << sampleBank.contentHash << '\n';
    if (sampleBank.frameCount > 0U) {
        out << "sample.data=";
        for (std::size_t i = 0; i < sampleBank.frameCount; ++i) {
            if (i != 0U) out << ',';
            out << sampleBank.samples[i];
        }
        out << '\n';
    }
    out << "distortion.enabled=" << distortion.enabled << "\ndistortion.drive=" << distortion.drive
        << "\ndistortion.mix=" << distortion.mix << '\n'
        << "eq.enabled=" << eq.enabled << "\neq.lowDb=" << eq.lowGainDb
        << "\neq.midDb=" << eq.midGainDb << "\neq.highDb=" << eq.highGainDb << '\n'
        << "chorus.enabled=" << chorus.enabled << "\nchorus.rate=" << chorus.rateHertz
        << "\nchorus.depthMs=" << chorus.depthMilliseconds << "\nchorus.mix=" << chorus.mix << '\n'
        << "phaser.enabled=" << phaser.enabled << "\nphaser.rate=" << phaser.rateHertz
        << "\nphaser.depth=" << phaser.depth << "\nphaser.feedback=" << phaser.feedback
        << "\nphaser.mix=" << phaser.mix << '\n'
        << "delay.enabled=" << delay.enabled << "\ndelay.time=" << delay.timeSeconds
        << "\ndelay.feedback=" << delay.feedback << "\ndelay.mix=" << delay.mix
        << "\ndelay.pingPong=" << delay.pingPong << '\n'
        << "reverb.enabled=" << reverb.enabled << "\nreverb.room=" << reverb.roomSize
        << "\nreverb.damping=" << reverb.damping << "\nreverb.width=" << reverb.width
        << "\nreverb.mix=" << reverb.mix << '\n'
        << "compressor.enabled=" << compressor.enabled << "\ncompressor.thresholdDb=" << compressor.thresholdDb
        << "\ncompressor.ratio=" << compressor.ratio << "\ncompressor.attackMs=" << compressor.attackMilliseconds
        << "\ncompressor.releaseMs=" << compressor.releaseMilliseconds << "\ncompressor.makeupDb=" << compressor.makeupDb << '\n'
        << "limiter.enabled=" << limiter.enabled << "\nlimiter.ceilingDb=" << limiter.ceilingDb
        << "\nlimiter.releaseMs=" << limiter.releaseMilliseconds << '\n';
    return out.str();
}

std::optional<SynthPreset> SynthPreset::parse(std::string_view text, std::string* error) {
    SynthPreset result = make_default();
    int version = 0;
    auto fail = [&](std::string message) -> std::optional<SynthPreset> { if (error) *error = std::move(message); return std::nullopt; };
    for (const auto& [key, value] : parse_lines(text)) {
        if (key == "DVE_SYNTH_PRESET") { if (!parse_number<int>(value, version)) return fail("invalid preset version"); continue; }
        if (key == "name") { result.name = value; continue; }
        auto readFloat = [&](float& target) { return parse_number<float>(value, target); };
        auto readUInt = [&](auto& target, unsigned maximum) {
            unsigned parsedValue = 0;
            if (!parse_number<unsigned>(value, parsedValue) || parsedValue > maximum) return false;
            target = static_cast<std::remove_reference_t<decltype(target)>>(parsedValue);
            return true;
        };
        auto readInt8 = [&](std::int8_t& target, int minimum, int maximum) {
            int parsedValue = 0;
            if (!parse_number<int>(value, parsedValue) || parsedValue < minimum || parsedValue > maximum) return false;
            target = static_cast<std::int8_t>(parsedValue);
            return true;
        };
        auto readBool = [&](bool& target) {
            if (value == "1" || value == "true") { target = true; return true; }
            if (value == "0" || value == "false") { target = false; return true; }
            return false;
        };
        auto readCurve = [&](EnvelopeCurve& target) {
            const auto parsedCurve = parse_envelope_curve(value);
            if (parsedCurve) target = *parsedCurve;
            return parsedCurve.has_value();
        };
        bool recognized = true;
        bool parsed = true;
        if (key == "master.gain") parsed = readFloat(result.masterGain);
        else if (key == "master.pan") parsed = readFloat(result.masterPan);
        else if (key == "master.bend") parsed = readFloat(result.pitchBendRangeSemitones);
        else if (key == "master.midiThru") parsed = readBool(result.midiThru);
        else if (key == "tuning.reference") parsed = readFloat(result.tuning.referenceHertz);
        else if (key == "tuning.transpose") parsed = readFloat(result.tuning.transposeSemitones);
        else if (key == "tuning.fineCents") parsed = readFloat(result.tuning.fineCents);
        else if (key == "tuning.driftCents") parsed = readFloat(result.tuning.analogDriftCents);
        else if (key == "quality.oscillator") { unsigned v=0; parsed=parse_number<unsigned>(value,v) && v<=2U; if(parsed) result.oscillatorQuality=static_cast<OscillatorQuality>(v); }
        else if (key == "quality.filter") { unsigned v=0; parsed=parse_number<unsigned>(value,v) && v<=3U; if(parsed) result.filterQuality=static_cast<FilterQuality>(v); }
        else if (key == "unison.enabled") parsed = readBool(result.unison.enabled);
        else if (key == "unison.voices") parsed = readUInt(result.unison.voices, static_cast<unsigned>(kSynthUnisonMax));
        else if (key == "unison.detune") parsed = readFloat(result.unison.detuneCents);
        else if (key == "unison.spread") parsed = readFloat(result.unison.stereoSpread);
        else if (key == "unison.phase") parsed = readFloat(result.unison.phaseSpread);
        else if (key == "unison.preserve") parsed = readBool(result.unison.preserveLevel);
        else if (key == "mpe.zone") { unsigned v=0; parsed=parse_number<unsigned>(value,v) && v<=3U; if(parsed) result.mpe.zoneMode=static_cast<MpeZoneMode>(v); }
        else if (key == "mpe.lowerMaster") parsed = readUInt(result.mpe.lowerMasterChannel, 15U);
        else if (key == "mpe.lowerMembers") parsed = readUInt(result.mpe.lowerMemberCount, 15U);
        else if (key == "mpe.upperMaster") parsed = readUInt(result.mpe.upperMasterChannel, 15U);
        else if (key == "mpe.upperMembers") parsed = readUInt(result.mpe.upperMemberCount, 15U);
        else if (key == "mpe.masterBend") parsed = readFloat(result.mpe.masterPitchBendRangeSemitones);
        else if (key == "mpe.memberBend") parsed = readFloat(result.mpe.memberPitchBendRangeSemitones);
        else if (key == "mpe.timbreCc") parsed = readUInt(result.mpe.timbreController, 127U);
        else if (key == "mpe.masterSustain") parsed = readBool(result.mpe.masterSustainToMembers);
        else if (key == "micro.enabled") parsed = readBool(result.microtuning.enabled);
        else if (key == "micro.name") result.microtuning.name = value;
        else if (key == "micro.referenceNote") parsed = readUInt(result.microtuning.referenceNote, 127U);
        else if (key == "micro.referenceHertz") parsed = readFloat(result.microtuning.referenceHertz);
        else if (key == "micro.hash") parsed = parse_number<std::uint64_t>(value, result.microtuning.contentHash);
        else if (key == "metadata.author") result.metadata.author = value;
        else if (key == "metadata.category") result.metadata.category = value;
        else if (key == "metadata.version") result.metadata.version = value;
        else if (key == "metadata.favorite") parsed = readBool(result.metadata.favorite);
        else if (key == "metadata.tagCount") parsed = readUInt(result.metadata.tagCount, static_cast<unsigned>(result.metadata.tags.size()));
        else if (key == "amp.attack") parsed = readFloat(result.ampEnvelope.attackSeconds);
        else if (key == "amp.decay") parsed = readFloat(result.ampEnvelope.decaySeconds);
        else if (key == "amp.sustain") parsed = readFloat(result.ampEnvelope.sustainLevel);
        else if (key == "amp.release") parsed = readFloat(result.ampEnvelope.releaseSeconds);
        else if (key == "amp.curve") parsed = readCurve(result.ampEnvelope.curve);
        else if (key == "amp.delay") parsed = readFloat(result.ampEnvelope.delaySeconds);
        else if (key == "amp.hold") parsed = readFloat(result.ampEnvelope.holdSeconds);
        else if (key == "filter.enabled") parsed = readBool(result.filter.enabled);
        else if (key == "filter.topology") { const auto topology = parse_filter_topology(value); parsed = topology.has_value(); if (topology) result.filter.topology = *topology; }
        else if (key == "filter.mode") { const auto mode = parse_filter_mode(value); parsed = mode.has_value(); if (mode) result.filter.mode = *mode; }
        else if (key == "filter.cutoff") parsed = readFloat(result.filter.cutoffHertz);
        else if (key == "filter.resonance") parsed = readFloat(result.filter.resonance);
        else if (key == "filter.envAmount") parsed = readFloat(result.filter.envelopeAmountOctaves);
        else if (key == "filter.keyTrack") parsed = readFloat(result.filter.keyTrack);
        else if (key == "filter.drive") parsed = readFloat(result.filter.drive);
        else if (key == "filter.bassComp") parsed = readFloat(result.filter.bassCompensation);
        else if (key == "filter.morph") parsed = readFloat(result.filter.morph);
        else if (key == "filter.altRevision") parsed = readBool(result.filter.alternateRevision);
        else if (key == "filter.env.attack") parsed = readFloat(result.filter.envelope.attackSeconds);
        else if (key == "filter.env.decay") parsed = readFloat(result.filter.envelope.decaySeconds);
        else if (key == "filter.env.sustain") parsed = readFloat(result.filter.envelope.sustainLevel);
        else if (key == "filter.env.release") parsed = readFloat(result.filter.envelope.releaseSeconds);
        else if (key == "filter.env.curve") parsed = readCurve(result.filter.envelope.curve);
        else if (key == "filter.env.delay") parsed = readFloat(result.filter.envelope.delaySeconds);
        else if (key == "filter.env.hold") parsed = readFloat(result.filter.envelope.holdSeconds);
        else if (key == "filter.oversampling") { unsigned v=0; parsed=parse_number<unsigned>(value,v) && (v==1U||v==2U||v==4U); if(parsed) result.filter.oversampling=static_cast<FilterOversampling>(v); }
        else if (key == "filter.ms20HighPass") parsed = readFloat(result.filter.ms20HighPassCutoffHertz);
        else if (key == "filter.selfOscillation") parsed = readFloat(result.filter.selfOscillation);
        else if (key == "chord.enabled") parsed = readBool(result.chord.enabled);
        else if (key == "chord.type") { const auto type = parse_chord_type(value); parsed = type.has_value(); if (type) result.chord.type = *type; }
        else if (key == "chord.noteCount") parsed = readUInt(result.chord.noteCount, static_cast<unsigned>(kChordIntervalCount));
        else if (key == "chord.inversion") parsed = readInt8(result.chord.inversion, -7, 7);
        else if (key == "chord.spread") parsed = readUInt(result.chord.spreadOctaves, 4U);
        else if (key == "chord.velocity") parsed = readFloat(result.chord.velocityScale);
        else if (key == "chord.scale") { const auto scale = parse_chord_scale(value); parsed = scale.has_value(); if (scale) result.chord.scale = *scale; }
        else if (key == "chord.scaleRoot") parsed = readUInt(result.chord.scaleRoot, 11U);
        else if (key == "chord.strumMs") parsed = readFloat(result.chord.strumMilliseconds);
        else if (key == "chord.useMemory") parsed = readBool(result.chord.useMemory);
        else if (key == "chord.memorySlot") parsed = readUInt(result.chord.memorySlot, static_cast<unsigned>(kChordMemorySlotCount - 1U));
        else if (key == "arp.enabled") parsed = readBool(result.arpeggiator.enabled);
        else if (key == "arp.latch") parsed = readBool(result.arpeggiator.latch);
        else if (key == "arp.midiOut") parsed = readBool(result.arpeggiator.sendMidiOutput);
        else if (key == "arp.retrigger") parsed = readBool(result.arpeggiator.retriggerEnvelopes);
        else if (key == "arp.mode") { const auto mode = parse_arpeggiator_mode(value); parsed = mode.has_value(); if (mode) result.arpeggiator.mode = *mode; }
        else if (key == "arp.division") { const auto division = parse_arpeggiator_division(value); parsed = division.has_value(); if (division) result.arpeggiator.division = *division; }
        else if (key == "arp.tempo") parsed = readFloat(result.arpeggiator.tempoBpm);
        else if (key == "arp.gate") parsed = readFloat(result.arpeggiator.gate);
        else if (key == "arp.swing") parsed = readFloat(result.arpeggiator.swing);
        else if (key == "arp.octaves") parsed = readUInt(result.arpeggiator.octaveRange, 4U);
        else if (key == "arp.stepCount") parsed = readUInt(result.arpeggiator.stepCount, static_cast<unsigned>(kArpeggiatorStepCount));
        else if (key == "arp.seed") parsed = parse_number<std::uint32_t>(value, result.arpeggiator.randomSeed);
        else if (key == "arp.clock") { const auto clock = parse_arp_clock(value); parsed = clock.has_value(); if (clock) result.arpeggiator.clockSource = *clock; }
        else if (key == "arp.externalTempo") parsed = readFloat(result.arpeggiator.externalTempoBpm);
        else if (key == "wavetable.name") result.wavetable.name = value;
        else if (key == "wavetable.enabled") parsed = readBool(result.wavetable.enabled);
        else if (key == "wavetable.frameCount") parsed = readUInt(result.wavetable.frameCount, static_cast<unsigned>(kWavetableFrameCount));
        else if (key == "wavetable.hash") parsed = parse_number<std::uint64_t>(value, result.wavetable.contentHash);
        else if (key == "sample.name") result.sampleBank.name = value;
        else if (key == "sample.enabled") parsed = readBool(result.sampleBank.enabled);
        else if (key == "sample.rate") parsed = parse_number<std::uint32_t>(value, result.sampleBank.sampleRate);
        else if (key == "sample.root") parsed = readUInt(result.sampleBank.rootNote, 127U);
        else if (key == "sample.frames") parsed = parse_number<std::uint32_t>(value, result.sampleBank.frameCount) && result.sampleBank.frameCount <= kSynthSampleMaxFrames;
        else if (key == "sample.hash") parsed = parse_number<std::uint64_t>(value, result.sampleBank.contentHash);
        else if (key == "sample.data") {
            std::size_t begin = 0U;
            std::size_t index = 0U;
            parsed = true;
            while (begin <= value.size()) {
                const std::size_t comma = value.find(',', begin);
                const std::string_view field(value.data() + begin,
                    (comma == std::string::npos ? value.size() : comma) - begin);
                if (index >= result.sampleBank.frameCount ||
                    !parse_number<float>(field, result.sampleBank.samples[index])) { parsed = false; break; }
                ++index;
                if (comma == std::string::npos) break;
                begin = comma + 1U;
            }
            parsed = parsed && index == result.sampleBank.frameCount;
        }
        else recognized = false;

        if (!recognized && key.starts_with("micro.offset")) {
            std::size_t index = 0;
            recognized = parse_number<std::size_t>(std::string_view(key).substr(12U), index) && index < result.microtuning.centsOffset.size();
            if (recognized) parsed = readFloat(result.microtuning.centsOffset[index]);
        }
        if (!recognized && key.starts_with("metadata.tag")) {
            std::size_t index = 0;
            recognized = parse_number<std::size_t>(std::string_view(key).substr(12U), index) && index < result.metadata.tags.size();
            if (recognized) result.metadata.tags[index] = value;
        }
        if (!recognized && key.starts_with("chord.interval")) {
            std::size_t index = 0;
            recognized = parse_number<std::size_t>(std::string_view(key).substr(14U), index) && index < result.chord.customIntervals.size();
            if (recognized) parsed = readInt8(result.chord.customIntervals[index], -48, 96);
        }
        if (!recognized && key.starts_with("chordMemory")) {
            const std::size_t dot = key.find('.');
            std::size_t slot = 0;
            recognized = dot != std::string::npos &&
                parse_number<std::size_t>(std::string_view(key).substr(11U, dot - 11U), slot) &&
                slot < result.chordMemory.size();
            if (recognized) {
                const std::string_view property = std::string_view(key).substr(dot + 1U);
                auto& memory = result.chordMemory[slot];
                if (property == "name") { memory.name = value; parsed = true; }
                else if (property == "noteCount") parsed = readUInt(memory.noteCount, static_cast<unsigned>(kChordIntervalCount));
                else if (property.starts_with("interval")) {
                    std::size_t interval = 0;
                    parsed = parse_number<std::size_t>(property.substr(8U), interval) &&
                        interval < memory.intervals.size() && readInt8(memory.intervals[interval], -48, 96);
                } else recognized = false;
            }
        }
        if (!recognized && key.starts_with("osc")) {
            const auto dot = key.find('.');
            std::size_t index = 0;
            if (dot == std::string::npos || !parse_number<std::size_t>(std::string_view(key).substr(3, dot - 3), index) || index >= result.oscillators.size())
                return fail("invalid oscillator preset key: " + key);
            auto& osc = result.oscillators[index];
            const std::string_view field = std::string_view(key).substr(dot + 1U);
            recognized = true;
            if (field == "enabled") parsed = readBool(osc.enabled);
            else if (field == "wave") { const auto wave = parse_waveform(value); parsed = wave.has_value(); if (wave) osc.waveform = *wave; }
            else if (field == "gain") parsed = readFloat(osc.gain);
            else if (field == "pan") parsed = readFloat(osc.pan);
            else if (field == "semitones") parsed = readFloat(osc.semitones);
            else if (field == "cents") parsed = readFloat(osc.cents);
            else if (field == "pulseWidth") parsed = readFloat(osc.pulseWidth);
            else if (field == "pwmDepth") parsed = readFloat(osc.pwmDepth);
            else if (field == "pwmRate") parsed = readFloat(osc.pwmRateHertz);
            else if (field == "shape") parsed = readFloat(osc.shape);
            else if (field == "phaseOffset") parsed = readFloat(osc.phaseOffset);
            else if (field == "keySync") parsed = readBool(osc.keySync);
            else if (field == "hardSync") parsed = readInt8(osc.hardSyncSource, -1, static_cast<int>(kSynthOscillatorCount - 1U));
            else if (field == "fmSource") parsed = readInt8(osc.frequencyModSource, -1, static_cast<int>(kSynthOscillatorCount - 1U));
            else if (field == "fmMode") { const auto mode = parse_fm_mode(value); parsed = mode.has_value(); if (mode) osc.frequencyModMode = *mode; }
            else if (field == "fmAmount") parsed = readFloat(osc.frequencyModAmount);
            else if (field == "ringSource") parsed = readInt8(osc.ringModSource, -1, static_cast<int>(kSynthOscillatorCount - 1U));
            else if (field == "ringDepth") parsed = readFloat(osc.ringModDepth);
            else if (field == "subLevel") parsed = readFloat(osc.subOscillatorLevel);
            else if (field == "subOctaves") parsed = readUInt(osc.subOscillatorOctaves, 3U);
            else if (field == "wavetablePosition") parsed = readFloat(osc.wavetablePosition);
            else if (field == "sampleStart") parsed = readFloat(osc.sampleStart);
            else if (field == "sampleEnd") parsed = readFloat(osc.sampleEnd);
            else if (field == "sampleLoopStart") parsed = readFloat(osc.sampleLoopStart);
            else if (field == "sampleLoopEnd") parsed = readFloat(osc.sampleLoopEnd);
            else if (field == "sampleLoop") parsed = readBool(osc.sampleLoop);
            else if (field == "sampleReverse") parsed = readBool(osc.sampleReverse);
            else if (field == "sampleOneShot") parsed = readBool(osc.sampleOneShot);
            else if (field == "sampleKeyTrack") parsed = readBool(osc.sampleKeyTrack);
            else if (field == "sampleVelocity") parsed = readFloat(osc.sampleVelocityToGain);
            else if (field == "grainPosition") parsed = readFloat(osc.grainPosition);
            else if (field == "grainSizeMs") parsed = readFloat(osc.grainSizeMilliseconds);
            else if (field == "grainDensity") parsed = readFloat(osc.grainDensityHertz);
            else if (field == "grainSpray") parsed = readFloat(osc.grainSpray);
            else if (field == "grainPitch") parsed = readFloat(osc.grainPitchSemitones);
            else if (field == "grainSpread") parsed = readFloat(osc.grainStereoSpread);
            else if (field == "grainStereoMotion") parsed = readFloat(osc.grainStereoMotion);
            else if (field == "grainEnvelopeCurve") parsed = readFloat(osc.grainEnvelopeCurve);
            else if (field == "grainReverseProbability") parsed = readFloat(osc.grainReverseProbability);
            else if (field == "grainPitchRandom") parsed = readFloat(osc.grainPitchRandomSemitones);
            else if (field == "grainPitchQuantize") parsed = readFloat(osc.grainPitchQuantizeSemitones);
            else if (field == "grainDensityVelocity") parsed = readFloat(osc.grainDensityVelocity);
            else if (field == "grainDensityTimbre") parsed = readFloat(osc.grainDensityTimbre);
            else if (field == "grainFreeze") parsed = readBool(osc.grainFreeze);
            else if (field == "grainWindow") { unsigned v=0; parsed=parse_number<unsigned>(value,v) && v<=2U; if(parsed) osc.grainWindow=static_cast<GrainWindow>(v); }
            else if (field == "physicalModel") { const auto v = parse_physical_model(value); parsed = v.has_value(); if (v) osc.physicalModel = *v; }
            else if (field == "physicalMaterial") { const auto v = parse_physical_material(value); parsed = v.has_value(); if (v) osc.physicalMaterial = *v; }
            else if (field == "physicalExcitation") { const auto v = parse_physical_excitation(value); parsed = v.has_value(); if (v) osc.physicalExcitation = *v; }
            else if (field == "physicalSizeMeters") parsed = readFloat(osc.physicalSizeMeters);
            else if (field == "physicalTension") parsed = readFloat(osc.physicalTension);
            else if (field == "physicalStiffness") parsed = readFloat(osc.physicalStiffness);
            else if (field == "physicalDamping") parsed = readFloat(osc.physicalDamping);
            else if (field == "physicalBrightness") parsed = readFloat(osc.physicalBrightness);
            else if (field == "physicalExcitationPosition") parsed = readFloat(osc.physicalExcitationPosition);
            else if (field == "physicalHardness") parsed = readFloat(osc.physicalHardness);
            else if (field == "physicalPickupPosition") parsed = readFloat(osc.physicalPickupPosition);
            else if (field == "physicalBodyAmount") parsed = readFloat(osc.physicalBodyAmount);
            else if (field == "physicalContinuousAmount") parsed = readFloat(osc.physicalContinuousAmount);
            else if (field == "physicalVelocityToHardness") parsed = readFloat(osc.physicalVelocityToHardness);
            else if (field == "physicalPressureToBow") parsed = readFloat(osc.physicalPressureToBow);
            else if (field == "physicalTimbreToBrightness") parsed = readFloat(osc.physicalTimbreToBrightness);
            else if (field == "physicalModeCount") { unsigned v = 0; parsed = parse_number<unsigned>(value, v) && v >= 4U && v <= 16U; if (parsed) osc.physicalModeCount = static_cast<std::uint8_t>(v); }
            else if (field == "physicalDriver") { const auto v = parse_physical_driver(value); parsed = v.has_value(); if (v) osc.physicalDriver = *v; }
            else if (field == "physicalReedStiffness") parsed = readFloat(osc.physicalReedStiffness);
            else if (field == "physicalEmbouchure") parsed = readFloat(osc.physicalEmbouchure);
            else if (field == "physicalBreathNoise") parsed = readFloat(osc.physicalBreathNoise);
            else if (field == "physicalThroatFreqHz") parsed = readFloat(osc.physicalThroatFreqHz);
            else if (field == "physicalThroatQ") parsed = readFloat(osc.physicalThroatQ);
            else if (field == "physicalBoreTaper") parsed = readFloat(osc.physicalBoreTaper);
            else if (field == "physicalPressureToBreath") parsed = readFloat(osc.physicalPressureToBreath);
            else if (field == "physicalVelocityToEmbouchure") parsed = readFloat(osc.physicalVelocityToEmbouchure);
            else recognized = false;
        }
        if (!recognized && key.starts_with("arp.step")) {
            const auto dot = key.find('.', 8U);
            std::size_t index = 0;
            if (dot == std::string::npos || !parse_number<std::size_t>(std::string_view(key).substr(8U, dot - 8U), index) || index >= result.arpeggiator.steps.size())
                return fail("invalid arpeggiator step key: " + key);
            auto& step = result.arpeggiator.steps[index];
            const std::string_view field = std::string_view(key).substr(dot + 1U);
            recognized = true;
            if (field == "enabled") parsed = readBool(step.enabled);
            else if (field == "condition") { unsigned v = 0; parsed = parse_number<unsigned>(value, v) && v <= 5U; if (parsed) step.condition = static_cast<ArpeggiatorCondition>(v); }
            else if (field == "automation") { unsigned v = 0; parsed = parse_number<unsigned>(value, v) && v <= 2U; if (parsed) step.automationCurve = static_cast<StepAutomationCurve>(v); }
            else if (field == "accent") parsed = readBool(step.accent);
            else if (field == "slide") parsed = readBool(step.slide);
            else if (field == "transpose") parsed = readInt8(step.transpose, -48, 48);
            else if (field == "octave") parsed = readInt8(step.octaveOffset, -4, 4);
            else if (field == "velocity") parsed = readFloat(step.velocityScale);
            else if (field == "gate") parsed = readFloat(step.gateScale);
            else if (field == "probability") parsed = readFloat(step.probability);
            else if (field == "ratchets") parsed = readUInt(step.ratchets, 8U);
            else if (field == "tie") parsed = readBool(step.tie);
            else if (field == "macro1") parsed = readFloat(step.macro1);
            else if (field == "macro2") parsed = readFloat(step.macro2);
            else if (field == "macro3") parsed = readFloat(step.macro3);
            else if (field == "macro4") parsed = readFloat(step.macro4);
            else recognized = false;
        }
        if (!recognized && key.starts_with("lfo")) {
            const auto dot = key.find('.');
            std::size_t index = 0;
            if (dot == std::string::npos || !parse_number<std::size_t>(std::string_view(key).substr(3U, dot - 3U), index) || index >= result.lfos.size())
                return fail("invalid LFO preset key: " + key);
            auto& lfo = result.lfos[index];
            const std::string_view field = std::string_view(key).substr(dot + 1U);
            recognized = true;
            if (field == "enabled") parsed = readBool(lfo.enabled);
            else if (field == "wave") { const auto wave = parse_lfo_waveform(value); parsed = wave.has_value(); if (wave) lfo.waveform = *wave; }
            else if (field == "rate") parsed = readFloat(lfo.rateHertz);
            else if (field == "depth") parsed = readFloat(lfo.depth);
            else if (field == "phase") parsed = readFloat(lfo.phase);
            else if (field == "fade") parsed = readFloat(lfo.fadeInSeconds);
            else if (field == "keySync") parsed = readBool(lfo.keySync);
            else if (field == "tempoSync") parsed = readBool(lfo.tempoSync);
            else if (field == "beats") parsed = readFloat(lfo.beatsPerCycle);
            else recognized = false;
        }
        if (!recognized && key.starts_with("mod")) {
            const auto dot = key.find('.');
            std::size_t index = 0;
            if (dot == std::string::npos || !parse_number<std::size_t>(std::string_view(key).substr(3U, dot - 3U), index) || index >= result.modulation.size())
                return fail("invalid modulation preset key: " + key);
            auto& slot = result.modulation[index];
            const std::string_view field = std::string_view(key).substr(dot + 1U);
            recognized = true;
            if (field == "enabled") parsed = readBool(slot.enabled);
            else if (field == "source") { const auto source = parse_modulation_source(value); parsed = source.has_value(); if (source) slot.source = *source; }
            else if (field == "destination") { const auto destination = parse_modulation_destination(value); parsed = destination.has_value(); if (destination) slot.destination = *destination; }
            else if (field == "amount") parsed = readFloat(slot.amount);
            else if (field == "curve") { const auto curve = parse_modulation_curve(value); parsed = curve.has_value(); if (curve) slot.curve = *curve; }
            else if (field == "polarity") { unsigned v=0; parsed=parse_number<unsigned>(value,v) && v<=1U; if(parsed) slot.polarity=static_cast<ModulationPolarity>(v); }
            else if (field == "smoothingMs") parsed = readFloat(slot.smoothingMilliseconds);
            else recognized = false;
        }
        if (!recognized && key.starts_with("macro")) {
            const auto dot = key.find('.');
            std::size_t index = 0;
            if (dot == std::string::npos || !parse_number<std::size_t>(std::string_view(key).substr(5U, dot - 5U), index) || index >= kSynthMacroCount)
                return fail("invalid macro preset key: " + key);
            const std::string_view field = std::string_view(key).substr(dot + 1U);
            recognized = true;
            if (field == "name") result.macros.names[index] = value;
            else if (field == "value") parsed = readFloat(result.macros.values[index]);
            else recognized = false;
        }
        if (!recognized && key.starts_with("midiLearn")) {
            const auto dot = key.find('.');
            std::size_t index = 0;
            if (dot == std::string::npos || !parse_number<std::size_t>(std::string_view(key).substr(9U, dot - 9U), index) || index >= result.midiLearn.size())
                return fail("invalid MIDI learn preset key: " + key);
            auto& mapping = result.midiLearn[index];
            const std::string_view field = std::string_view(key).substr(dot + 1U);
            recognized = true;
            if (field == "enabled") parsed = readBool(mapping.enabled);
            else if (field == "controller") parsed = readUInt(mapping.controller, 127U);
            else if (field == "macro") parsed = readUInt(mapping.macroIndex, static_cast<unsigned>(kSynthMacroCount - 1U));
            else if (field == "minimum") parsed = readFloat(mapping.minimum);
            else if (field == "maximum") parsed = readFloat(mapping.maximum);
            else if (field == "inverted") parsed = readBool(mapping.inverted);
            else recognized = false;
        }
        if (!recognized && key.starts_with("wavetable.frame")) {
            std::size_t frame = 0;
            recognized = parse_number<std::size_t>(std::string_view(key).substr(15U), frame) && frame < kWavetableFrameCount;
            if (recognized) {
                std::size_t sample = 0;
                std::size_t begin = 0;
                while (sample < kWavetableSampleCount) {
                    const std::size_t comma = value.find(',', begin);
                    const std::string_view token(value.data() + begin,
                        (comma == std::string::npos ? value.size() : comma) - begin);
                    if (!parse_number<float>(token, result.wavetable.samples[frame * kWavetableSampleCount + sample])) { parsed = false; break; }
                    ++sample;
                    if (comma == std::string::npos) break;
                    begin = comma + 1U;
                }
                parsed = parsed && sample == kWavetableSampleCount;
            }
        }
        if (!recognized) {
            recognized = true;
            if (key == "distortion.enabled") parsed = readBool(result.distortion.enabled);
            else if (key == "distortion.drive") parsed = readFloat(result.distortion.drive);
            else if (key == "distortion.mix") parsed = readFloat(result.distortion.mix);
            else if (key == "eq.enabled") parsed = readBool(result.eq.enabled);
            else if (key == "eq.lowDb") parsed = readFloat(result.eq.lowGainDb);
            else if (key == "eq.midDb") parsed = readFloat(result.eq.midGainDb);
            else if (key == "eq.highDb") parsed = readFloat(result.eq.highGainDb);
            else if (key == "chorus.enabled") parsed = readBool(result.chorus.enabled);
            else if (key == "chorus.rate") parsed = readFloat(result.chorus.rateHertz);
            else if (key == "chorus.depthMs") parsed = readFloat(result.chorus.depthMilliseconds);
            else if (key == "chorus.mix") parsed = readFloat(result.chorus.mix);
            else if (key == "phaser.enabled") parsed = readBool(result.phaser.enabled);
            else if (key == "phaser.rate") parsed = readFloat(result.phaser.rateHertz);
            else if (key == "phaser.depth") parsed = readFloat(result.phaser.depth);
            else if (key == "phaser.feedback") parsed = readFloat(result.phaser.feedback);
            else if (key == "phaser.mix") parsed = readFloat(result.phaser.mix);
            else if (key == "delay.enabled") parsed = readBool(result.delay.enabled);
            else if (key == "delay.time") parsed = readFloat(result.delay.timeSeconds);
            else if (key == "delay.feedback") parsed = readFloat(result.delay.feedback);
            else if (key == "delay.mix") parsed = readFloat(result.delay.mix);
            else if (key == "delay.pingPong") parsed = readBool(result.delay.pingPong);
            else if (key == "reverb.enabled") parsed = readBool(result.reverb.enabled);
            else if (key == "reverb.room") parsed = readFloat(result.reverb.roomSize);
            else if (key == "reverb.damping") parsed = readFloat(result.reverb.damping);
            else if (key == "reverb.width") parsed = readFloat(result.reverb.width);
            else if (key == "reverb.mix") parsed = readFloat(result.reverb.mix);
            else if (key == "compressor.enabled") parsed = readBool(result.compressor.enabled);
            else if (key == "compressor.thresholdDb") parsed = readFloat(result.compressor.thresholdDb);
            else if (key == "compressor.ratio") parsed = readFloat(result.compressor.ratio);
            else if (key == "compressor.attackMs") parsed = readFloat(result.compressor.attackMilliseconds);
            else if (key == "compressor.releaseMs") parsed = readFloat(result.compressor.releaseMilliseconds);
            else if (key == "compressor.makeupDb") parsed = readFloat(result.compressor.makeupDb);
            else if (key == "limiter.enabled") parsed = readBool(result.limiter.enabled);
            else if (key == "limiter.ceilingDb") parsed = readFloat(result.limiter.ceilingDb);
            else if (key == "limiter.releaseMs") parsed = readFloat(result.limiter.releaseMilliseconds);
            else recognized = false;
        }
        if (recognized && !parsed) return fail("invalid value for " + key);
    }
    if (version != 1 && version != 2 && version != 3 && version != 4 && version != 5) return fail("missing or unsupported synth preset version");
    std::string validation;
    if (!result.validate(&validation)) return fail(validation);
    return result;
}
bool SynthPreset::save(const std::filesystem::path& path, std::string* error) const {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) { if (error) *error = "could not open synth preset for writing"; return false; }
    const std::string data = serialize();
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!output) { if (error) *error = "could not write synth preset"; return false; }
    return true;
}
std::optional<SynthPreset> SynthPreset::load(const std::filesystem::path& path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { if (error) *error = "could not open synth preset"; return std::nullopt; }
    std::ostringstream data; data << input.rdbuf();
    return parse(data.str(), error);
}

std::optional<SynthSampleBank> import_synth_sample_from_audio(const std::filesystem::path& path,
                                                                std::uint8_t rootNote,
                                                                std::string* error) {
    AudioImportOptions options;
    options.targetSampleRate = kDefaultSynthSampleRate;
    options.storagePolicy = AudioStoragePolicy::Resident;
    auto asset = import_audio_file(path, options, error);
    if (!asset) return std::nullopt;
    if (asset->metadata.channels == 0U || asset->metadata.frameCount < 2U) {
        if (error) *error = "sample source contains no usable audio";
        return std::nullopt;
    }
    SynthSampleBank result;
    result.name = asset->metadata.name.empty() ? path.stem().string() : asset->metadata.name;
    result.enabled = true;
    result.sampleRate = asset->metadata.sampleRate;
    result.rootNote = rootNote;
    result.frameCount = static_cast<std::uint32_t>(std::min<std::uint64_t>(asset->metadata.frameCount,
                                                                         kSynthSampleMaxFrames));
    const std::size_t channels = asset->metadata.channels;
    float peak = 0.0F;
    for (std::size_t frame = 0; frame < result.frameCount; ++frame) {
        float mono = 0.0F;
        for (std::size_t channel = 0; channel < channels; ++channel)
            mono += asset->samples[frame * channels + channel];
        mono /= static_cast<float>(channels);
        result.samples[frame] = mono;
        peak = std::max(peak, std::abs(mono));
    }
    if (peak > 1.0F) for (std::size_t i = 0; i < result.frameCount; ++i) result.samples[i] /= peak;
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t i = 0; i < result.frameCount; ++i) {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(result.samples[i]);
        hash ^= bits; hash *= 1099511628211ULL;
    }
    hash ^= result.frameCount; hash *= 1099511628211ULL;
    hash ^= result.sampleRate; hash *= 1099511628211ULL;
    result.contentHash = hash;
    return result;
}

std::optional<WavetableBank> import_wavetable_from_audio(const std::filesystem::path& path,
                                                            std::string* error) {
    AudioImportOptions options;
    options.targetSampleRate = 48000;
    options.storagePolicy = AudioStoragePolicy::Resident;
    auto decoded = import_audio_file(path, options, error);
    if (!decoded || decoded->metadata.channels == 0U || decoded->metadata.frameCount < 32U) {
        if (error && error->empty()) *error = "wavetable source must contain at least 32 audio frames";
        return std::nullopt;
    }
    WavetableBank result = WavetableBank::make_default();
    result.name = path.stem().string().empty() ? "Imported Wavetable" : path.stem().string();
    result.enabled = true;
    const std::size_t sourceFrames = static_cast<std::size_t>(decoded->metadata.frameCount);
    result.frameCount = static_cast<std::uint8_t>(std::clamp<std::size_t>(
        sourceFrames / kWavetableSampleCount, 1U, kWavetableFrameCount));
    const std::size_t channels = decoded->metadata.channels;
    for (std::size_t frame = 0; frame < result.frameCount; ++frame) {
        const double segmentStart = static_cast<double>(frame) * static_cast<double>(sourceFrames) / static_cast<double>(result.frameCount);
        const double segmentEnd = static_cast<double>(frame + 1U) * static_cast<double>(sourceFrames) / static_cast<double>(result.frameCount);
        const double segmentLength = std::max(1.0, segmentEnd - segmentStart);
        for (std::size_t sample = 0; sample < kWavetableSampleCount; ++sample) {
            const double position = segmentStart + static_cast<double>(sample) * segmentLength /
                                    static_cast<double>(kWavetableSampleCount);
            const std::size_t i0 = std::min(static_cast<std::size_t>(position), sourceFrames - 1U);
            const std::size_t i1 = std::min(i0 + 1U, sourceFrames - 1U);
            const float fraction = static_cast<float>(position - static_cast<double>(i0));
            const float a = decoded->samples[i0 * channels];
            const float b = decoded->samples[i1 * channels];
            result.samples[frame * kWavetableSampleCount + sample] = a + (b - a) * fraction;
        }
        float mean = 0.0F;
        float peak = 0.0F;
        for (std::size_t sample = 0; sample < kWavetableSampleCount; ++sample)
            mean += result.samples[frame * kWavetableSampleCount + sample];
        mean /= static_cast<float>(kWavetableSampleCount);
        for (std::size_t sample = 0; sample < kWavetableSampleCount; ++sample) {
            float& value = result.samples[frame * kWavetableSampleCount + sample];
            value -= mean;
            peak = std::max(peak, std::abs(value));
        }
        if (peak > 1.0e-6F)
            for (std::size_t sample = 0; sample < kWavetableSampleCount; ++sample)
                result.samples[frame * kWavetableSampleCount + sample] /= peak;
    }
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t i = 0; i < static_cast<std::size_t>(result.frameCount) * kWavetableSampleCount; ++i) {
        const auto bits = std::bit_cast<std::uint32_t>(result.samples[i]);
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            hash ^= static_cast<std::uint8_t>((bits >> shift) & 0xFFU);
            hash *= 1099511628211ULL;
        }
    }
    result.contentHash = hash;
    std::string validation;
    if (!result.validate(&validation)) { if (error) *error = validation; return std::nullopt; }
    return result;
}

SynthPreset morph_synth_presets(const SynthPreset& a, const SynthPreset& b, float amount) {
    const float t = clampf(amount, 0.0F, 1.0F);
    const bool chooseB = t >= 0.5F;
    auto lerp = [t](float x, float y) { return x + (y - x) * t; };
    SynthPreset result = chooseB ? b : a;
    result.name = "Morph: " + a.name + " / " + b.name;
    for (std::size_t i = 0; i < result.oscillators.size(); ++i) {
        auto& r = result.oscillators[i]; const auto& x = a.oscillators[i]; const auto& y = b.oscillators[i];
        r.gain=lerp(x.gain,y.gain); r.pan=lerp(x.pan,y.pan); r.semitones=lerp(x.semitones,y.semitones);
        r.cents=lerp(x.cents,y.cents); r.pulseWidth=lerp(x.pulseWidth,y.pulseWidth); r.pwmDepth=lerp(x.pwmDepth,y.pwmDepth);
        r.pwmRateHertz=lerp(x.pwmRateHertz,y.pwmRateHertz); r.shape=lerp(x.shape,y.shape);
        r.phaseOffset=lerp(x.phaseOffset,y.phaseOffset); r.frequencyModAmount=lerp(x.frequencyModAmount,y.frequencyModAmount);
        r.ringModDepth=lerp(x.ringModDepth,y.ringModDepth); r.subOscillatorLevel=lerp(x.subOscillatorLevel,y.subOscillatorLevel);
        r.wavetablePosition=lerp(x.wavetablePosition,y.wavetablePosition);
    }
    auto morphEnvelope = [&](AdsrParameters& r, const AdsrParameters& x, const AdsrParameters& y) {
        r.attackSeconds=lerp(x.attackSeconds,y.attackSeconds); r.decaySeconds=lerp(x.decaySeconds,y.decaySeconds);
        r.sustainLevel=lerp(x.sustainLevel,y.sustainLevel); r.releaseSeconds=lerp(x.releaseSeconds,y.releaseSeconds);
        r.delaySeconds=lerp(x.delaySeconds,y.delaySeconds); r.holdSeconds=lerp(x.holdSeconds,y.holdSeconds);
    };
    morphEnvelope(result.ampEnvelope,a.ampEnvelope,b.ampEnvelope);
    morphEnvelope(result.filter.envelope,a.filter.envelope,b.filter.envelope);
    result.filter.cutoffHertz=std::exp(lerp(std::log(std::max(18.0F,a.filter.cutoffHertz)),std::log(std::max(18.0F,b.filter.cutoffHertz))));
    result.filter.resonance=lerp(a.filter.resonance,b.filter.resonance); result.filter.envelopeAmountOctaves=lerp(a.filter.envelopeAmountOctaves,b.filter.envelopeAmountOctaves);
    result.filter.keyTrack=lerp(a.filter.keyTrack,b.filter.keyTrack); result.filter.drive=lerp(a.filter.drive,b.filter.drive);
    result.filter.bassCompensation=lerp(a.filter.bassCompensation,b.filter.bassCompensation); result.filter.morph=lerp(a.filter.morph,b.filter.morph);
    result.filter.ms20HighPassCutoffHertz=lerp(a.filter.ms20HighPassCutoffHertz,b.filter.ms20HighPassCutoffHertz);
    result.filter.selfOscillation=lerp(a.filter.selfOscillation,b.filter.selfOscillation);
    result.tuning.referenceHertz=lerp(a.tuning.referenceHertz,b.tuning.referenceHertz); result.tuning.transposeSemitones=lerp(a.tuning.transposeSemitones,b.tuning.transposeSemitones);
    result.tuning.fineCents=lerp(a.tuning.fineCents,b.tuning.fineCents); result.tuning.analogDriftCents=lerp(a.tuning.analogDriftCents,b.tuning.analogDriftCents);
    for (std::size_t i=0;i<kSynthLfoCount;++i) { result.lfos[i].rateHertz=lerp(a.lfos[i].rateHertz,b.lfos[i].rateHertz); result.lfos[i].depth=lerp(a.lfos[i].depth,b.lfos[i].depth); result.lfos[i].phase=lerp(a.lfos[i].phase,b.lfos[i].phase); result.lfos[i].fadeInSeconds=lerp(a.lfos[i].fadeInSeconds,b.lfos[i].fadeInSeconds); result.lfos[i].beatsPerCycle=lerp(a.lfos[i].beatsPerCycle,b.lfos[i].beatsPerCycle); }
    for (std::size_t i=0;i<kSynthModulationSlotCount;++i) result.modulation[i].amount=lerp(a.modulation[i].amount,b.modulation[i].amount);
    for (std::size_t i=0;i<kSynthMacroCount;++i) result.macros.values[i]=lerp(a.macros.values[i],b.macros.values[i]);
    result.masterGain=lerp(a.masterGain,b.masterGain); result.masterPan=lerp(a.masterPan,b.masterPan); result.pitchBendRangeSemitones=lerp(a.pitchBendRangeSemitones,b.pitchBendRangeSemitones);
    result.distortion.drive=lerp(a.distortion.drive,b.distortion.drive); result.distortion.mix=lerp(a.distortion.mix,b.distortion.mix);
    result.eq.lowGainDb=lerp(a.eq.lowGainDb,b.eq.lowGainDb); result.eq.midGainDb=lerp(a.eq.midGainDb,b.eq.midGainDb); result.eq.highGainDb=lerp(a.eq.highGainDb,b.eq.highGainDb);
    result.chorus.rateHertz=lerp(a.chorus.rateHertz,b.chorus.rateHertz); result.chorus.depthMilliseconds=lerp(a.chorus.depthMilliseconds,b.chorus.depthMilliseconds); result.chorus.mix=lerp(a.chorus.mix,b.chorus.mix);
    result.phaser.rateHertz=lerp(a.phaser.rateHertz,b.phaser.rateHertz); result.phaser.depth=lerp(a.phaser.depth,b.phaser.depth); result.phaser.feedback=lerp(a.phaser.feedback,b.phaser.feedback); result.phaser.mix=lerp(a.phaser.mix,b.phaser.mix);
    result.delay.timeSeconds=lerp(a.delay.timeSeconds,b.delay.timeSeconds); result.delay.feedback=lerp(a.delay.feedback,b.delay.feedback); result.delay.mix=lerp(a.delay.mix,b.delay.mix);
    result.reverb.roomSize=lerp(a.reverb.roomSize,b.reverb.roomSize); result.reverb.damping=lerp(a.reverb.damping,b.reverb.damping); result.reverb.width=lerp(a.reverb.width,b.reverb.width); result.reverb.mix=lerp(a.reverb.mix,b.reverb.mix);
    result.wavetable.enabled = a.wavetable.enabled || b.wavetable.enabled;
    result.wavetable.frameCount = std::max(a.wavetable.frameCount,b.wavetable.frameCount);
    for (std::size_t i=0;i<result.wavetable.samples.size();++i) result.wavetable.samples[i]=lerp(a.wavetable.samples[i],b.wavetable.samples[i]);
    result.wavetable.contentHash = 0U;
    return result;
}

namespace {
void write_be16(std::ostream& out, std::uint16_t value) { out.put(static_cast<char>((value>>8U)&0xFFU)); out.put(static_cast<char>(value&0xFFU)); }
void write_be32(std::ostream& out, std::uint32_t value) { out.put(static_cast<char>((value>>24U)&0xFFU)); out.put(static_cast<char>((value>>16U)&0xFFU)); out.put(static_cast<char>((value>>8U)&0xFFU)); out.put(static_cast<char>(value&0xFFU)); }
void append_vlq(std::vector<std::uint8_t>& out, std::uint32_t value) {
    std::array<std::uint8_t,5> buffer{}; std::size_t count=0; buffer[count++]=static_cast<std::uint8_t>(value&0x7FU);
    while ((value >>= 7U) != 0U) buffer[count++]=static_cast<std::uint8_t>((value&0x7FU)|0x80U);
    while (count>0U) out.push_back(buffer[--count]);
}
}

bool export_arpeggiator_midi_file(const SynthPreset& preset, const std::filesystem::path& path,
                                  std::uint8_t rootNote, std::string* error) {
    std::string validation; if (!preset.validate(&validation)) { if(error)*error=validation; return false; }
    struct Event { std::uint32_t tick{}; bool on{}; std::uint8_t note{}; std::uint8_t velocity{}; };
    std::vector<Event> events;
    constexpr std::uint32_t ppq=480U;
    std::uint32_t cursor=0U;
    const std::size_t stepCount=std::clamp<std::size_t>(preset.arpeggiator.stepCount,1U,kArpeggiatorStepCount);
    for (std::size_t stepIndex=0; stepIndex<stepCount; ++stepIndex) {
        const auto& step=preset.arpeggiator.steps[stepIndex];
        const float swing=(stepIndex&1U)?1.0F+preset.arpeggiator.swing:1.0F-preset.arpeggiator.swing;
        const std::uint32_t duration=std::max<std::uint32_t>(1U,static_cast<std::uint32_t>(std::llround(arpeggiator_step_beats(preset.arpeggiator.division)*ppq*swing)));
        if (step.enabled && step.probability>0.0F) {
            const std::uint8_t base=static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(rootNote)+step.transpose+step.octaveOffset*12,0,127));
            ChordVoicing voicing;
            if (preset.chord.enabled) voicing = make_chord_voicing(base, resolved_chord_parameters(preset));
            else { voicing.notes[0] = base; voicing.count = 1; }
            const std::uint8_t ratchets=std::clamp<std::uint8_t>(step.ratchets,1U,8U);
            const std::uint32_t segment=std::max<std::uint32_t>(1U,duration/ratchets);
            for(std::uint8_t r=0;r<ratchets;++r){
                const std::uint32_t start=cursor+r*segment;
                const std::uint32_t gate=step.tie&&r+1U==ratchets?duration-r*segment:std::max<std::uint32_t>(1U,static_cast<std::uint32_t>(static_cast<float>(segment) * preset.arpeggiator.gate * step.gateScale));
                for(std::size_t i=0;i<voicing.count;++i){
                    const auto velocity=static_cast<std::uint8_t>(clampf(100.0F*step.velocityScale,1.0F,127.0F));
                    events.push_back({start,true,voicing.notes[i],velocity}); events.push_back({start+gate,false,voicing.notes[i],0});
                }
            }
        }
        cursor+=duration;
    }
    std::stable_sort(events.begin(),events.end(),[](const Event&a,const Event&b){return a.tick!=b.tick?a.tick<b.tick:a.on<b.on;});
    std::vector<std::uint8_t> track;
    const std::uint32_t micros=static_cast<std::uint32_t>(std::llround(60000000.0/std::clamp(preset.arpeggiator.tempoBpm,20.0F,400.0F)));
    append_vlq(track,0U); track.insert(track.end(),{0xFFU,0x51U,0x03U,static_cast<std::uint8_t>((micros>>16U)&0xFFU),static_cast<std::uint8_t>((micros>>8U)&0xFFU),static_cast<std::uint8_t>(micros&0xFFU)});
    std::uint32_t previous=0U;
    for(const auto&e:events){append_vlq(track,e.tick-previous);previous=e.tick;track.push_back(e.on?0x90U:0x80U);track.push_back(e.note);track.push_back(e.velocity);}
    append_vlq(track,0U); track.insert(track.end(),{0xFFU,0x2FU,0x00U});
    std::ofstream out(path,std::ios::binary|std::ios::trunc); if(!out){if(error)*error="could not open MIDI file";return false;}
    out.write("MThd",4);write_be32(out,6U);write_be16(out,0U);write_be16(out,1U);write_be16(out,static_cast<std::uint16_t>(ppq));
    out.write("MTrk",4);write_be32(out,static_cast<std::uint32_t>(track.size()));out.write(reinterpret_cast<const char*>(track.data()),static_cast<std::streamsize>(track.size()));
    if(!out){if(error)*error="could not write MIDI file";return false;}return true;
}

bool SynthPresetLibrary::scan(const std::filesystem::path& directory, std::string* error) {
    entries_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(directory,ec) || !std::filesystem::is_directory(directory,ec)) { if(error)*error="preset directory does not exist"; return false; }
    for (std::filesystem::recursive_directory_iterator it(directory,ec), end; it!=end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".dvesynth") continue;
        std::string loadError; auto preset=SynthPreset::load(it->path(),&loadError); if(!preset) continue;
        SynthPresetEntry entry; entry.path=it->path(); entry.name=preset->name;
        for (auto parent=it->path().parent_path(); !parent.empty() && parent!=directory.parent_path(); parent=parent.parent_path()) {
            if (parent == directory) break;
            if (!parent.filename().empty()) entry.tags.push_back(parent.filename().string());
        }
        std::string token; const std::string stem=it->path().stem().string();
        for(char c:stem){if(c=='_'||c=='-'||c==' '){if(!token.empty()){entry.tags.push_back(token);token.clear();}}else token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));}
        if(!token.empty()) entry.tags.push_back(token);
        std::sort(entry.tags.begin(),entry.tags.end()); entry.tags.erase(std::unique(entry.tags.begin(),entry.tags.end()),entry.tags.end());
        entries_.push_back(std::move(entry));
    }
    if(ec){if(error)*error="could not scan preset directory";return false;}
    std::sort(entries_.begin(),entries_.end(),[](const auto&a,const auto&b){return a.name<b.name;});return true;
}
std::vector<std::size_t> SynthPresetLibrary::find_by_tag(std::string_view tag) const {
    std::vector<std::size_t> result; std::string query(tag); std::transform(query.begin(),query.end(),query.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    for(std::size_t i=0;i<entries_.size();++i) for(const auto&candidate:entries_[i].tags){std::string normalized=candidate;std::transform(normalized.begin(),normalized.end(),normalized.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});if(normalized==query){result.push_back(i);break;}}
    return result;
}

Synthesizer::Synthesizer(std::uint32_t sampleRate)
    : sampleRate_(std::clamp<std::uint32_t>(sampleRate, 8000U, 192000U)), preset_(SynthPreset::make_default()) {
    impl_ = new Impl(sampleRate_, currentFrame_);
    impl_->adopt_preset(realtime_preset(preset_));
    impl_->limiterEnvelope = 1.0F;
}
Synthesizer::~Synthesizer() { delete impl_; }

void Synthesizer::set_preset(const SynthPreset& preset) {
    std::string error;
    if (!preset.validate(&error)) return;
    preset_ = preset;
    const RealtimePreset realtime = realtime_preset(preset_);
    while (!impl_->presetIn.push(realtime)) {
        RealtimePreset discarded{};
        if (!impl_->presetIn.pop(discarded)) break;
    }
}

bool Synthesizer::set_sample_map(const SynthSampleMap& sampleMap, std::string* error) {
    if (!sampleMap.validate(error)) return false;
    SynthSampleMap cooked = sampleMap;
    cooked.contentHash = cooked.calculate_content_hash();
    impl_->stage_sample_map(std::move(cooked));
    return true;
}

void Synthesizer::clear_sample_map() {
    impl_->stage_sample_map(SynthSampleMap{});
}

std::uint64_t Synthesizer::sample_map_generation() const noexcept {
    return impl_->controlSampleMapGeneration.load(std::memory_order_acquire);
}

bool Synthesizer::publish_sample_stream_page(std::uint8_t sourceIndex,
                                             std::uint32_t firstFrame,
                                             std::span<const float> monoFrames) noexcept {
    const std::uint64_t generation = impl_->controlSampleMapGeneration.load(std::memory_order_acquire);
    return generation != 0U && impl_->sampleStreamCache.publish_page(
        generation, sourceIndex, firstFrame, monoFrames);
}

SynthGranularProfiler Synthesizer::granular_profiler() const noexcept {
    return {impl_->requestedGrains.load(std::memory_order_relaxed),
            impl_->admittedGrains.load(std::memory_order_relaxed),
            impl_->grainSteals.load(std::memory_order_relaxed),
            impl_->grainMisses.load(std::memory_order_relaxed),
            impl_->samplePageUnderruns.load(std::memory_order_relaxed),
            impl_->activeGrainTelemetry.load(std::memory_order_relaxed),
            impl_->maximumActiveGrains.load(std::memory_order_relaxed),
            impl_->sampleStreamCache.metrics()};
}

void Synthesizer::reset_granular_profiler() noexcept {
    impl_->requestedGrains.store(0U, std::memory_order_relaxed);
    impl_->admittedGrains.store(0U, std::memory_order_relaxed);
    impl_->grainSteals.store(0U, std::memory_order_relaxed);
    impl_->grainMisses.store(0U, std::memory_order_relaxed);
    impl_->samplePageUnderruns.store(0U, std::memory_order_relaxed);
    impl_->maximumActiveGrains.store(impl_->activeGrains, std::memory_order_relaxed);
    impl_->sampleStreamCache.reset_metrics();
}

void Synthesizer::all_notes_off(bool immediate) noexcept {
    for (std::uint8_t channel = 0; channel < 16U; ++channel)
        (void)post_midi(MidiMessage::control_change(channel, immediate ? 120U : 123U, 0, current_frame()));
}

bool Synthesizer::post_midi(MidiMessage message) noexcept {
    if (!message.valid()) return false;
    if (message.sampleFrame == 0) message.sampleFrame = currentFrame_.load(std::memory_order_relaxed);
    if (!impl_->midiIn.push(message)) {
        impl_->droppedMidi.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    return true;
}
bool Synthesizer::note_on(std::uint8_t note, float velocity, std::uint8_t channel, std::uint64_t sampleFrame) noexcept {
    return post_midi(MidiMessage::note_on(channel, note, static_cast<std::uint8_t>(clampf(velocity,0.0F,1.0F)*127.0F+0.5F), sampleFrame));
}
bool Synthesizer::note_off(std::uint8_t note, float velocity, std::uint8_t channel, std::uint64_t sampleFrame) noexcept {
    return post_midi(MidiMessage::note_off(channel, note, static_cast<std::uint8_t>(clampf(velocity,0.0F,1.0F)*127.0F+0.5F), sampleFrame));
}
bool Synthesizer::control_change(std::uint8_t controller, std::uint8_t value, std::uint8_t channel, std::uint64_t sampleFrame) noexcept {
    return post_midi(MidiMessage::control_change(channel, controller, value, sampleFrame));
}
bool Synthesizer::pitch_bend(std::int16_t centeredValue, std::uint8_t channel, std::uint64_t sampleFrame) noexcept {
    return post_midi(MidiMessage::pitch_bend(channel, centeredValue, sampleFrame));
}
void Synthesizer::set_game_clock_tempo(float bpm) noexcept {
    impl_->gameClockTempo.store(clampf(bpm, 20.0F, 400.0F), std::memory_order_relaxed);
}
void Synthesizer::set_arpeggiator_fill(bool enabled) noexcept {
    impl_->arpeggiatorFill.store(enabled, std::memory_order_relaxed);
}
void Synthesizer::restart_performance_transport(std::uint64_t sampleFrame) noexcept {
    impl_->transportRestartFrame.store(sampleFrame, std::memory_order_relaxed);
    impl_->transportRestartRequested.store(true, std::memory_order_release);
}
bool Synthesizer::post_midi_clock(std::uint64_t sampleFrame) noexcept {
    return post_midi(MidiMessage::realtime(0xF8U, sampleFrame));
}

void Synthesizer::render(std::span<float> interleavedStereo) noexcept {
    render(interleavedStereo.data(), interleavedStereo.size() / 2U);
}
void Synthesizer::render(float* output, std::size_t frameCount) noexcept {
    if (output == nullptr || frameCount == 0) return;
    impl_->adopt_pending_sample_map();
    RealtimePreset latest{};
    while (impl_->presetIn.pop(latest)) impl_->adopt_preset(latest);
    if (impl_->transportRestartRequested.exchange(false, std::memory_order_acq_rel)) {
        impl_->clear_arp_held(true);
        impl_->renderFrame = impl_->transportRestartFrame.load(std::memory_order_relaxed);
        impl_->arpStepCounter = 0U;
        impl_->arpProgress = 0U;
        impl_->activeArpStep.store(0U, std::memory_order_relaxed);
    }

    std::array<MidiMessage, 256> pending{};
    std::size_t pendingCount = 0;
    MidiMessage message;
    while (pendingCount < pending.size() && impl_->midiIn.pop(message)) pending[pendingCount++] = message;
    // Fixed-capacity insertion sort: unlike std::stable_sort this cannot allocate inside the
    // real-time callback. MIDI batches are normally tiny; the bounded 256-event worst case is
    // still deterministic.
    for (std::size_t i = 1; i < pendingCount; ++i) {
        const MidiMessage value = pending[i];
        std::size_t j = i;
        while (j > 0U && pending[j - 1U].sampleFrame > value.sampleFrame) {
            pending[j] = pending[j - 1U];
            --j;
        }
        pending[j] = value;
    }

    const std::uint64_t blockStart = currentFrame_.load(std::memory_order_relaxed);
    std::size_t eventIndex = 0;
    float peakLeft = 0.0F; float peakRight = 0.0F; double squareLeft = 0.0; double squareRight = 0.0;
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const std::uint64_t absoluteFrame = blockStart + frame;
        impl_->renderFrame = absoluteFrame;
        while (eventIndex < pendingCount && pending[eventIndex].sampleFrame <= absoluteFrame)
            impl_->handle_midi(pending[eventIndex++]);
        impl_->process_scheduled_events();
        impl_->advance_arpeggiator();
        float left = 0.0F; float right = 0.0F;
        for (Voice& voice : impl_->voice) {
            const auto [voiceLeft, voiceRight] = impl_->render_voice(voice);
            left += voiceLeft; right += voiceRight;
        }
        const float channelVolume = impl_->controller[7] > 0.0F ? impl_->controller[7] : 1.0F;
        const float panController = impl_->controller[10];
        if (panController <= 0.0F) {
            left *= impl_->parameters.masterGain * channelVolume * impl_->parameters.masterPanLeft;
            right *= impl_->parameters.masterGain * channelVolume * impl_->parameters.masterPanRight;
        } else {
            const float masterPan = clampf(impl_->parameters.masterPan + panController * 2.0F - 1.0F, -1.0F, 1.0F);
            left *= impl_->parameters.masterGain * channelVolume * std::sqrt(0.5F * (1.0F - masterPan));
            right *= impl_->parameters.masterGain * channelVolume * std::sqrt(0.5F * (1.0F + masterPan));
        }
        impl_->effects(left, right);
        if (!finite(left)) left = 0.0F;
        if (!finite(right)) right = 0.0F;
        output[frame * 2U] = left;
        output[frame * 2U + 1U] = right;
        peakLeft = std::max(peakLeft, std::abs(left)); peakRight = std::max(peakRight, std::abs(right));
        squareLeft += static_cast<double>(left) * left; squareRight += static_cast<double>(right) * right;
    }
    currentFrame_.store(blockStart + frameCount, std::memory_order_relaxed);
    // Preserve events beyond this render quantum by requeuing them. This is bounded and does
    // not allocate; if the queue is saturated the drop counter makes the loss observable.
    while (eventIndex < pendingCount) {
        if (!impl_->midiIn.push(pending[eventIndex])) impl_->droppedMidi.fetch_add(1U, std::memory_order_relaxed);
        ++eventIndex;
    }
    impl_->peakL.store(peakLeft, std::memory_order_relaxed); impl_->peakR.store(peakRight, std::memory_order_relaxed);
    impl_->rmsL.store(static_cast<float>(std::sqrt(squareLeft / static_cast<double>(frameCount))), std::memory_order_relaxed);
    impl_->rmsR.store(static_cast<float>(std::sqrt(squareRight / static_cast<double>(frameCount))), std::memory_order_relaxed);
    impl_->renderedFrames.fetch_add(frameCount, std::memory_order_relaxed);
    impl_->publish_telemetry();
}

std::array<SynthVoiceInfo, kSynthVoiceCount> Synthesizer::voices() const noexcept {
    std::array<SynthVoiceInfo, kSynthVoiceCount> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i].active = impl_->telemetry[i].active.load(std::memory_order_relaxed);
        result[i].channel = impl_->telemetry[i].channel.load(std::memory_order_relaxed);
        result[i].note = impl_->telemetry[i].note.load(std::memory_order_relaxed);
        result[i].velocity = impl_->telemetry[i].velocity.load(std::memory_order_relaxed);
        result[i].envelope = impl_->telemetry[i].envelope.load(std::memory_order_relaxed);
        result[i].pressure = impl_->telemetry[i].pressure.load(std::memory_order_relaxed);
        result[i].timbre = impl_->telemetry[i].timbre.load(std::memory_order_relaxed);
        result[i].pitchBendSemitones = impl_->telemetry[i].pitchBendSemitones.load(std::memory_order_relaxed);
        result[i].stage = impl_->telemetry[i].stage.load(std::memory_order_relaxed);
        result[i].age = impl_->telemetry[i].age.load(std::memory_order_relaxed);
    }
    return result;
}
SynthMeters Synthesizer::meters() const noexcept {
    return {impl_->peakL.load(std::memory_order_relaxed), impl_->peakR.load(std::memory_order_relaxed),
            impl_->rmsL.load(std::memory_order_relaxed), impl_->rmsR.load(std::memory_order_relaxed),
            impl_->activeVoiceCount.load(std::memory_order_relaxed),
            impl_->heldArpNoteCount.load(std::memory_order_relaxed),
            impl_->activeArpStep.load(std::memory_order_relaxed),
            impl_->renderedFrames.load(std::memory_order_relaxed), impl_->droppedMidi.load(std::memory_order_relaxed)};
}
std::array<SynthModulationInfo, kSynthModulationSlotCount> Synthesizer::modulation_activity() const noexcept {
    std::array<SynthModulationInfo, kSynthModulationSlotCount> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const ModulationSlot& slot = preset_.modulation[i];
        result[i] = {slot.enabled, slot.source, slot.destination, slot.polarity, slot.amount,
                     impl_->modulationTelemetry[i].load(std::memory_order_relaxed)};
    }
    return result;
}
bool Synthesizer::poll_midi_output(MidiMessage& message) noexcept { return impl_->midiOut.pop(message); }

bool write_float_wav(const std::filesystem::path& path, std::span<const float> samples,
                     std::uint32_t sampleRate, std::string* error) {
    if (samples.size() % 2U != 0 || sampleRate == 0) {
        if (error) *error = "WAV output requires interleaved stereo samples and a nonzero sample rate";
        return false;
    }
    const std::uint64_t bytes64 = samples.size() * sizeof(float);
    if (bytes64 > std::numeric_limits<std::uint32_t>::max() - 36U) {
        if (error) *error = "WAV output is too large";
        return false;
    }
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(bytes64);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { if (error) *error = "could not create WAV output"; return false; }
    auto write_u16 = [&](std::uint16_t value) {
        const std::array<char,2> bytes{static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU)};
        out.write(bytes.data(), 2);
    };
    auto write_u32 = [&](std::uint32_t value) {
        const std::array<char,4> bytes{static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU),
                                       static_cast<char>((value >> 16U) & 0xFFU), static_cast<char>((value >> 24U) & 0xFFU)};
        out.write(bytes.data(), 4);
    };
    out.write("RIFF",4); write_u32(36U + dataBytes); out.write("WAVEfmt ",8); write_u32(16U);
    write_u16(3U); write_u16(2U); write_u32(sampleRate); write_u32(sampleRate * 2U * sizeof(float));
    write_u16(static_cast<std::uint16_t>(2U * sizeof(float))); write_u16(32U); out.write("data",4); write_u32(dataBytes);
    out.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(dataBytes));
    if (!out) { if (error) *error = "could not finish WAV output"; return false; }
    return true;
}

} // namespace dve::audio
