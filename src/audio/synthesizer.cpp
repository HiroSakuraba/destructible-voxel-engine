#include "dve/audio/synthesizer.hpp"

#include "dve/audio/audio_asset.hpp"
#include "dve/audio/wavetable.hpp"
#include "dve/audio/physics_modulation.hpp"
#include "dve/audio/generative_conductor.hpp"

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

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace dve::audio {
namespace {

// Real-time denormal guard: sustained voices at low levels (decaying
// envelopes, reverb/filter tails) generate denormal floats, and a single
// denormal operand can stall the FPU for microseconds — the classic cause
// of rare multi-millisecond spikes in an otherwise steady render. Enabling
// flush-to-zero + denormals-are-zero for the render call removes the stall;
// affected values are < 1.2e-38 (-758 dB), far below audibility and the
// 1e-6 A/B tolerance. MXCSR is per-thread; the previous mode is restored
// on exit so non-audio threads are untouched.
struct DenormalGuard {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned saved_;
    DenormalGuard() noexcept : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | 0x8040u); }
    ~DenormalGuard() { _mm_setcsr(saved_); }
#else
    DenormalGuard() noexcept = default;
#endif
};

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

// Phase 2: rational soft clipper for DistortionMode::SoftClip. Unity gain at
// zero, asymptotically +/-1, with a rounder knee (and darker harmonic series
// at equal drive) than tanh.
float soft_clip(float value) noexcept {
    return value / (1.0F + std::fabs(value));
}

// Phase 2: triangle wavefolder for DistortionMode::Foldback. Maps any input
// into [-1, 1] by folding overdriven peaks back instead of clipping them.
float wavefold(float value) noexcept {
    float folded = std::fmod(value + 1.0F, 4.0F);
    if (folded < 0.0F) folded += 4.0F;
    return folded < 2.0F ? folded - 1.0F : 3.0F - folded;
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

// Phase 0: per-parameter smoothing. When a preset is adopted, smoothable
// parameters don't jump to their new values instantly (which causes clicks);
// instead they glide from current toward target with a one-pole lowpass.
// Cutoff uses log-domain smoothing (perceptually uniform); others are linear.
struct SmoothedFloat {
    float current{};
    float target{};
    // Advances current toward target. coeff is the one-pole coefficient
    // (0 = frozen, 1 = instant). Returns the new current value.
    float advance(float coeff) noexcept {
        current += (target - current) * coeff;
        // Snap when close enough to avoid denormal crawl.
        if (std::fabs(target - current) < 1e-6F) current = target;
        return current;
    }
    void set_target(float v) noexcept { target = v; }
    void snap(float v) noexcept { current = target = v; }
};

struct RealtimePreset {
    std::array<OscillatorParameters, kSynthOscillatorCount> oscillators{};
    AdsrParameters ampEnvelope{};
    FilterParameters filter{};
    TuningParameters tuning{};    ChordParameters chord{};
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
    SamplerParameters sampler{};  // Phase 2: sampler generator parameters
    GranularParameters granular{};  // Phase 4: granular generator parameters (SYN-014)
    SpectralParameters spectral{};  // Phase 5: spectral resynthesis parameters (SYN-015)
    const SpectralAssetView* spectralAsset{nullptr};  // Phase 5: non-owning cooked asset view
    MpeParameters mpe{};
    RealtimeMicrotuning microtuning{};
    UnisonParameters unison{};
    OscillatorQuality oscillatorQuality{OscillatorQuality::Normal};
    FilterQuality filterQuality{FilterQuality::Standard};
    DistortionParameters distortion{};
    BitcrusherParameters bitcrusher{};
    OctaveHarmonizerParameters harmonizer{};
    EqParameters eq{};
    ChorusParameters chorus{};
    FlangerParameters flanger{};
    EnsembleParameters ensemble{};
    PhaserParameters phaser{};
    DelayParameters delay{};
    DiffusionDelayParameters diffusionDelay{};
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

// Lock-free preset handoff from the UI thread to the render thread. Carries
// everything the audio thread needs to adopt a preset: the unmorphed numeric
// base (morph source A), the morph target B, and the sequencer/conductor
// configs that used to be applied from the UI thread (a data race while the
// audio thread was inside advance_sequencer()/conductor.process()).
// Trivially copyable so the MPMC queue moves it without allocation.
struct PresetUpdate {
    RealtimePreset base{};      // unmorphed base preset (morph source A)
    RealtimePreset morphB{};    // morph target B (valid when hasMorphB)
    bool hasMorphB{false};
    bool morphEnabled{false};
    float morphAmount{0.0F};    // UI-authored morph amount
    SequencerConfig sequencer{};
    bool sequencerChanged{false};
    AttractorConfig attractor{};
    bool attractorEnabled{false};
};
static_assert(std::is_trivially_copyable_v<PresetUpdate>);

// FNV-1a hash over the wavetable mip-0 content that adopt_preset() cooks into
// the HQ table. Lets the render thread skip the re-cook (and its
// allocations) when the wavetable data hasn't actually changed.
std::uint64_t wavetable_content_hash(const RealtimeWavetable& wt) noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    h ^= wt.enabled ? 1ULL : 0ULL; h *= 1099511628211ULL;
    h ^= wt.frameCount; h *= 1099511628211ULL;
    const std::size_t n = std::min<std::size_t>(wt.frameCount, kWavetableFrameCount) *
                          kWavetableSampleCount;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(wt.samples[i]));
        h *= 1099511628211ULL;
    }
    return h;
}

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
    result.sampler = source.sampler;  // Phase 2: SamplerParameters is trivially copyable
    result.granular = source.granular;  // Phase 4: GranularParameters is trivially copyable
    result.spectral = source.spectral;  // Phase 5: SpectralParameters is trivially copyable
    result.spectralAsset = source.spectralAsset;  // Phase 5: non-owning view, copied as a pointer>>>>>>> phase5-serial
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
    result.bitcrusher = source.bitcrusher;
    result.harmonizer = source.harmonizer;
    result.eq = source.eq;
    result.chorus = source.chorus;
    result.flanger = source.flanger;
    result.ensemble = source.ensemble;
    result.phaser = source.phaser;
    result.delay = source.delay;
    result.diffusionDelay = source.diffusionDelay;
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

// Phase 2: lowest comb frequency the voice comb filter supports; sizes the delay line.
inline constexpr float kCombFilterMinFrequencyHz = 20.0F;

// Phase 2: one RBJ constant-0dB-peak-gain bandpass biquad (direct form I),
// the building block of the voice Formant topology. Promoted from the
// physical-model "throat" biquad into a first-class voice filter path.
struct FormantBiquad {
    float x1{};
    float x2{};
    float y1{};
    float y2{};

    void reset() noexcept { x1 = 0.0F; x2 = 0.0F; y1 = 0.0F; y2 = 0.0F; }

    float process(float input, float frequencyHz, float q, float sampleRate) noexcept {
        const float w = kTwoPi * clampf(frequencyHz, 10.0F, sampleRate * 0.45F) / sampleRate;
        const float alpha = std::sin(w) / (2.0F * q);
        const float b0 = alpha;
        const float b2 = -alpha;  // b1 is 0 for the bandpass form
        const float a0 = 1.0F + alpha;
        const float a1 = -2.0F * std::cos(w);
        const float a2 = 1.0F - alpha;
        const float y = (b0 / a0) * input + (b2 / a0) * x2 - (a1 / a0) * y1 - (a2 / a0) * y2;
        x2 = x1; x1 = input; y2 = y1; y1 = y;
        return y;
    }
};

// Phase 2: parallel formant bank for the voice Formant topology.
struct VoiceFormantFilter {
    std::array<FormantBiquad, FormantParameters::kBandCount> bands{};

    void reset() noexcept { for (auto& band : bands) band.reset(); }

    float process(float input, float freqScale, float q, const FormantParameters& params,
                  float sampleRate) noexcept {
        float sum = 0.0F;
        for (std::size_t i = 0; i < bands.size(); ++i)
            sum += params.gains[i] * bands[i].process(input, params.frequencyHertz[i] * freqScale,
                                                      q, sampleRate);
        return sum + clampf(params.dryMix, 0.0F, 1.0F) * input;
    }
};

// Phase 2: feedback comb with fractional delay and lowpass damping in the
// feedback loop, for the voice Comb topology. Promoted from the reverb
// CombFilter into a first-class voice filter path; the delay line is sized
// lazily so voices that never select Comb pay no memory cost.
struct VoiceCombFilter {
    std::vector<float> line;
    std::size_t writeIndex{};
    float dampingStore{};

    void reset() noexcept {
        std::fill(line.begin(), line.end(), 0.0F);
        writeIndex = 0;
        dampingStore = 0.0F;
    }

    void ensure_capacity(float oversampledRate) {
        const std::size_t needed =
            static_cast<std::size_t>(oversampledRate / kCombFilterMinFrequencyHz) + 8U;
        if (line.size() < needed) {
            line.assign(needed, 0.0F);
            writeIndex = 0;
            dampingStore = 0.0F;
        }
    }

    float process(float input, float delaySamples, float feedback, float damping) noexcept {
        const float size = static_cast<float>(line.size());
        float position = static_cast<float>(writeIndex) - delaySamples;
        while (position < 0.0F) position += size;
        while (position >= size) position -= size;
        const std::size_t i0 = static_cast<std::size_t>(position) % line.size();
        const std::size_t i1 = (i0 + 1U) % line.size();
        const float fraction = position - std::floor(position);
        const float delayed = line[i0] + (line[i1] - line[i0]) * fraction;
        dampingStore = delayed * (1.0F - damping) + dampingStore * damping;
        line[writeIndex] = input + dampingStore * feedback;
        writeIndex = (writeIndex + 1U) % line.size();
        return delayed;
    }
};

struct AnalogFilter {
    StateVariableFilter stateVariable;
    MoogLadderFilter ladder;
    Ms20Filter ms20;
    VoiceFormantFilter formant;  // Phase 2
    VoiceCombFilter comb;        // Phase 2
    float previousInput{};

    void reset() noexcept {
        stateVariable.reset();
        ladder.reset();
        ms20.reset();
        formant.reset();
        comb.reset();
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
            case FilterTopology::Comb: {
                // Comb spacing follows the cutoff (comb frequency = cutoff Hz);
                // resonance drives feedback. FilterMode is intentionally ignored:
                // the dry/wet balance is CombParameters::mix.
                const float combFrequency = clampf(cutoff, kCombFilterMinFrequencyHz, sampleRate * 0.45F);
                comb.ensure_capacity(sampleRate);
                const float delaySamples = sampleRate / combFrequency;
                const float feedback = clampf(resonance * parameters.selfOscillation *
                                              parameters.comb.feedbackScale, 0.0F, 0.97F);
                const float wet = comb.process(fast_tanh(input * parameters.drive), delaySamples,
                                               feedback, clampf(parameters.comb.damping, 0.0F, 1.0F));
                const float mix = clampf(parameters.comb.mix, 0.0F, 1.0F);
                return input * (1.0F - mix) + wet * mix;
            }
            case FilterTopology::Formant: {
                // Cutoff sweeps the whole vowel bank multiplicatively:
                // 1000 Hz leaves the authored formant frequencies untouched.
                // FilterMode is intentionally ignored: the vowel shape is the sound.
                const float freqScale = clampf(cutoff / 1000.0F, 0.25F, 4.0F);
                const float q = 0.7F + clampf(resonance, 0.0F, 1.0F) * 8.0F;
                return formant.process(fast_tanh(input * parameters.drive), freqScale, q,
                                       parameters.formant, sampleRate);
            }
        }
        return input;
    }

    float process(float input, float cutoff, float resonance, const FilterParameters& parameters,
                  float sampleRate, FilterQuality quality = FilterQuality::Standard) noexcept {
        // Phase 2: Auto oversampling resolves via effective_oversampling();
        // explicit X1/X2/X4 settings are honored exactly.
        const unsigned configured = static_cast<unsigned>(effective_oversampling(parameters));
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

// Phase 2: modal resonator voice state (SYN-011b). One per oscillator.
struct ModalResonatorState {
    struct Mode {
        float decayCoeff{0.999F};  // per-sample envelope multiplier: exp(-1/(tau*sr))
        float y1{};
        float y2{};
        float gain{1.0F};          // per-mode gain including the brightness tilt
    };
    std::array<Mode, kModalResonatorMaxModes> modes{};
    std::uint8_t activeModes{};
    bool initialized{};
    std::uint32_t noiseState{0x5BD1E995U};
    float exciterPhase{};                 // Oscillator excitation: internal saw phase
    std::uint32_t exciteSamplesRemaining{};   // NoiseBurst window (and SampleTransient fallback)
    std::uint32_t transientFramesRemaining{}; // SampleTransient window, in bank frames
    float transientPosition{};            // fractional read position in the sample bank
    float transientStep{1.0F};            // bank sample rate / voice sample rate
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
    float releaseVelocity{};
    std::uint64_t age{};
    std::uint64_t startFrame{};
    Envelope amp;
    Envelope filterEnvelope;
    std::array<float, kSynthOscillatorCount> phases{};
    std::array<float, 2> harmonizerPhases{};
    std::array<std::array<float, 4>, kSynthOscillatorCount> auxiliaryPhases{};
    std::array<std::uint32_t, kSynthOscillatorCount> noiseState{};
    std::array<float, kSynthOscillatorCount> previousOscillatorSamples{};
    // Perf: cached stereo-divergence detune ratios. divergence is a preset
    // parameter, so the exp2() pair is recomputed only when it changes
    // (per-voice cache also covers morph-driven changes).
    std::array<float, kSynthOscillatorCount> divergenceRatioL{};
    std::array<float, kSynthOscillatorCount> divergenceRatioR{};
    std::array<float, kSynthOscillatorCount> divergencePhaseOffset{};
    std::array<float, kSynthOscillatorCount> divergenceCached{};
    // Perf: cached base frequency. tuned_frequency()'s exp2 argument is split
    // into a per-block-constant base (everything but slow analog drift) and a
    // tiny drift term applied via 2nd-order Taylor (|err| < 1e-12 relative).
    // Key: (note, referenceHertz, baseSemitones). Falls back to the direct
    // path when microtuning or exponential FM is active.
    std::array<float, kSynthOscillatorCount> freqCache{};
    std::array<float, kSynthOscillatorCount> freqCacheSemitones{};
    std::uint8_t freqCacheNote{0xFF};
    float freqCacheRefHertz{0.0F};
    std::array<float, kSynthOscillatorCount> subPhases{};
    std::array<float, kSynthOscillatorCount> samplePositions{};
    std::array<float, kSynthOscillatorCount> sampleMapPositions{};
    std::array<float, kSynthOscillatorCount> releaseSamplePositions{};
    std::array<float, kSynthOscillatorCount> samplerPositions{};  // Phase 2: frame position
    std::array<bool, kSynthOscillatorCount> samplerPrimed{};      // Phase 2: start offset applied
    std::array<bool, kSynthOscillatorCount> samplerFinished{};    // Phase 2: one-shot reached end
    std::array<bool, kSynthOscillatorCount> sampleFinished{};
    std::array<bool, kSynthOscillatorCount> releaseSampleActive{};
    std::uint8_t sampleAttackZone{kInvalidSampleZone};
    std::uint8_t sampleReleaseZone{kInvalidSampleZone};
    std::array<GranularEngine, kSynthOscillatorCount> granularEngines{};  // Phase 4: one grain pool per oscillator (critique fix)
    SpectralOscillator spectralOscillator{};  // Phase 5: spectral resynthesis (one engine per voice)
    std::array<std::array<float, kSynthUnisonMax - 1U>, kSynthOscillatorCount> unisonPhases{};
    std::array<float, kSynthModulationSlotCount> modulationSmoothing{};
    std::array<bool, kSynthOscillatorCount> oscillatorWrapped{};
    std::array<float, kSynthLfoCount> lfoPhases{};
    std::array<WavetableOscState, kSynthOscillatorCount> wavetableState{};
    std::array<float, kSynthLfoCount> lfoRandomValues{};
    std::array<float, kSynthLfoCount> lfoPreviousRandomValues{};
    std::array<std::uint32_t, kSynthLfoCount> lfoNoiseState{};
    std::array<PhysicalModelState, kSynthOscillatorCount> physicalModels{};
    std::array<ModalResonatorState, kSynthOscillatorCount> modalResonators{};  // Phase 2
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
        harmonizerPhases.fill(0.0F);
        for (std::size_t i = 0; i < phases.size(); ++i) {
            if (preset.oscillators[i].keySync || retrigger) {
                phases[i] = wrap_phase(preset.oscillators[i].phaseOffset);
                subPhases[i] = wrap_phase(preset.oscillators[i].phaseOffset * 0.5F);
                samplePositions[i] = preset.oscillators[i].sampleReverse
                    ? preset.oscillators[i].sampleEnd : preset.oscillators[i].sampleStart;
                samplerPrimed[i] = false;   // Phase 2: sampler start applied lazily at first render
                samplerFinished[i] = false;
                samplerPositions[i] = 0.0F;
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
            modalResonators[i].initialized = false;  // Phase 2: re-excite at note-on
        }
        // Phase 4: fresh granular cloud per note, deterministically seeded from
        // note/age so identical notes render identical grain sequences. One
        // pool per oscillator: oscillator 0 keeps the exact legacy seed so
        // single-oscillator determinism tests stay bit-identical; the others
        // fold the oscillator index into the hash for independent clouds.
        for (std::size_t i = 0; i < kSynthOscillatorCount; ++i) {
            granularEngines[i].reset();
            granularEngines[i].set_seed(0x51ED27B9U ^ (static_cast<std::uint32_t>(newNote) << 16U) ^
                                        static_cast<std::uint32_t>(i * 0x85EBCA6BU) ^
                                        static_cast<std::uint32_t>(newAge));
        }
        // Phase 5: fresh spectral engine per note, deterministically seeded
        // from note/age so identical notes render identical output.
        spectralOscillator.reset();
        spectralOscillator.set_seed(0x5EC1A1U ^ (static_cast<std::uint32_t>(newNote) << 16U) ^
                                    static_cast<std::uint32_t>(newAge));
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
    void kill() noexcept { active = false; keyHeld = false; sustained = false; amp.kill(); filterEnvelope.kill();
        for (auto& engine : granularEngines) engine.kill_grains(); spectralOscillator.kill(); }
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

// Phase 2: size in samples for one diffusion-delay allpass stage.
std::size_t diffusion_stage_size(float milliseconds, std::uint32_t sampleRate) noexcept {
    return std::max<std::size_t>(2U, static_cast<std::size_t>(
        static_cast<double>(milliseconds) * 0.001 * static_cast<double>(sampleRate)));
}

// Phase 2: single true-allpass diffusion stage (H(z) = (z^-M - g)/(1 - g*z^-M),
// unity magnitude for |g| < 1). The DelayLine holds w[n] = x[n] + g*y[n].
float allpass_diffuse(DelayLine& line, float input, float coefficient) noexcept {
    const float delayed = line.read_fractional(static_cast<float>(line.data.size() - 1U));
    const float output = delayed - coefficient * input;
    line.push(input + coefficient * output);
    return output;
}

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
        case OscillatorWaveform::Sampler:
        case OscillatorWaveform::ModalResonator:
        case OscillatorWaveform::Spectral:  // Phase 5: rendered via SpectralOscillator, not the phase path
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

// ---------------------------------------------------------------------------
// Modal resonator core (Phase 2, SYN-011b) — allocation-free, realtime safe.
//
// Each mode is a damped 2-pole resonator (Smith's formulation):
//   y[n] = 2*R*cos(w)*y[n-1] - R^2*y[n-2] + x[n]*g_in
// with R = exp(-1/(tau*sr)), so the mode envelope decays as e^(-t/tau).
// Output is the gain-weighted sum of the modes; even modes pan left, odd right.
//
// Excitation design decisions (documented per the build spec):
// - Impulse: one-shot initial displacement of every mode at note-on (classic
//   modal synthesis). Modes ring freely afterwards.
// - NoiseBurst: white noise injected through the mode inputs for
//   noiseBurstMilliseconds at note-on, then free decay.
// - Oscillator: a DEDICATED internal exciter (sawtooth at the voice's base
//   frequency plus a touch of noise) drives the bank continuously while the key
//   is held — like a bowed string driving a resonant body. This is used instead
//   of the voice's own osc 1 to avoid feedback when osc 1 is itself the
//   resonator, and to keep the routing explicit.
// - SampleTransient: the first transientMilliseconds of the preset's resident
//   sample bank (the same bank the Sample waveform uses), resampled from the
//   bank rate to the voice rate, injected once at note-on. If the bank is
//   disabled or empty, the documented fallback is a noise burst of the same
//   length so the voice still sounds.
// ---------------------------------------------------------------------------
namespace modal_resonator {

// Piano-style inharmonicity: ratio' = ratio * sqrt(1 + B*(ratio^2 - 1)).
// ratio == 1 is untouched for any B; higher partials stretch upward.
inline float stretch_ratio(float ratio, float inharmonicity) noexcept {
    const float r = std::max(ratio, 0.01F);
    const float b = std::clamp(inharmonicity, 0.0F, 1.0F);
    return r * std::sqrt(1.0F + b * (r * r - 1.0F));
}

// Brightness gain tilt: g(m) = gain_m * (m+1)^(2*(brightness - 0.5)).
// brightness == 0.5 is flat; 0 darkens as 1/(m+1); 1 brightens as (m+1).
inline float tilt_gain(float gain, std::size_t modeIndex, float brightness) noexcept {
    const float tilt = 2.0F * (std::clamp(brightness, 0.0F, 1.0F) - 0.5F);
    return gain * std::pow(static_cast<float>(modeIndex + 1U), tilt);
}

inline float excitation_energy(const ModalResonatorParameters& params, float velocity) noexcept {
    return (0.35F + 0.65F * std::clamp(velocity, 0.0F, 1.0F)) *
           std::clamp(params.excitationLevel, 0.0F, 4.0F);
}

inline float base_frequency(const ModalResonatorParameters& params, float frequencyHz,
                            float sampleRate) noexcept {
    // MIDI pitch is authoritative when baseFrequency is 0.
    if (params.baseFrequency > 0.0F)
        return std::clamp(params.baseFrequency, 20.0F, sampleRate * 0.45F);
    return std::clamp(frequencyHz, 20.0F, sampleRate * 0.45F);
}

inline float white_noise(std::uint32_t& noiseState) noexcept {
    noiseState ^= noiseState << 13U;
    noiseState ^= noiseState >> 17U;
    noiseState ^= noiseState << 5U;
    return static_cast<float>(static_cast<std::int32_t>(noiseState)) /
           static_cast<float>(std::numeric_limits<std::int32_t>::max());
}

void initialize(ModalResonatorState& state, const ModalResonatorParameters& params,
                float frequencyHz, float velocity, const RealtimeSampleBank& bank,
                float sampleRate) noexcept {
    state = ModalResonatorState{};
    state.initialized = true;
    state.noiseState = 0x5BD1E995U ^ static_cast<std::uint32_t>(frequencyHz * 1000.0F);
    if (state.noiseState == 0U) state.noiseState = 0x5BD1E995U;
    const float sr = std::max(sampleRate, 8000.0F);
    const float base = base_frequency(params, frequencyHz, sr);
    const float energy = excitation_energy(params, velocity);
    const float damping = std::clamp(params.damping, 0.01F, 8.0F);
    // Defensive clamp: the bank never exceeds kModalResonatorMaxModes entries.
    const std::uint8_t activeModes =
        std::clamp(params.modeCount, std::uint8_t{1}, std::uint8_t{kModalResonatorMaxModes});
    state.activeModes = activeModes;
    for (std::uint8_t m = 0; m < activeModes; ++m) {
        auto& mode = state.modes[m];
        const auto& mp = params.modes[m];
        const float ratio = stretch_ratio(mp.frequencyRatio, params.inharmonicity);
        const float rawFrequency = base * ratio;
        const float decaySeconds = std::clamp(mp.decaySeconds, 0.005F, 60.0F) * damping;
        mode.decayCoeff = std::exp(-1.0F / (std::max(decaySeconds, 0.001F) * sr));
        mode.gain = rawFrequency >= sr * 0.45F
            ? 0.0F
            : tilt_gain(std::max(mp.gain, 0.0F), m, params.brightness);
        mode.y1 = 0.0F;
        mode.y2 = 0.0F;
        if (params.excitation == ExcitationSource::Impulse)
            mode.y1 = energy * mode.gain * 0.5F;
    }
    if (params.excitation == ExcitationSource::NoiseBurst) {
        const float ms = std::clamp(params.noiseBurstMilliseconds, 1.0F, 2000.0F);
        state.exciteSamplesRemaining = static_cast<std::uint32_t>(ms * 0.001F * sr);
    } else if (params.excitation == ExcitationSource::SampleTransient) {
        const bool haveSample = bank.enabled && bank.frameCount > 1U;
        const float ms = std::clamp(params.transientMilliseconds, 1.0F, 2000.0F);
        if (haveSample) {
            const std::uint32_t bankFrames = static_cast<std::uint32_t>(
                ms * 0.001F * static_cast<float>(std::max(bank.sampleRate, 1U)));
            state.transientFramesRemaining = std::min(bankFrames, bank.frameCount);
            state.transientPosition = 0.0F;
            state.transientStep = static_cast<float>(std::max(bank.sampleRate, 1U)) / sr;
        } else {
            state.exciteSamplesRemaining = static_cast<std::uint32_t>(ms * 0.001F * sr);
        }
    }
}

std::pair<float, float> process(ModalResonatorState& state, const ModalResonatorParameters& params,
                                float frequencyHz, float velocity, bool keyHeld,
                                const RealtimeSampleBank& bank, float sampleRate) noexcept {
    if (!state.initialized) initialize(state, params, frequencyHz, velocity, bank, sampleRate);
    const float sr = std::max(sampleRate, 8000.0F);
    const float base = base_frequency(params, frequencyHz, sr);
    const float energy = excitation_energy(params, velocity);

    // Excitation signal injected through every mode input this sample.
    float exciter = 0.0F;
    if (state.exciteSamplesRemaining > 0U) {
        exciter = white_noise(state.noiseState) * energy * 0.5F;
        --state.exciteSamplesRemaining;
    } else if (state.transientFramesRemaining > 0U && bank.enabled && bank.frameCount > 1U) {
        const std::uint32_t i0 = std::min(static_cast<std::uint32_t>(state.transientPosition),
                                          bank.frameCount - 1U);
        const std::uint32_t i1 = std::min(i0 + 1U, bank.frameCount - 1U);
        const float frac = state.transientPosition - std::floor(state.transientPosition);
        exciter = (bank.samples[i0] + (bank.samples[i1] - bank.samples[i0]) * frac) * energy;
        state.transientPosition += state.transientStep;
        --state.transientFramesRemaining;
    } else if (params.excitation == ExcitationSource::Oscillator && keyHeld) {
        const float increment = base / sr;
        state.exciterPhase = wrap_phase(state.exciterPhase + increment);
        const float saw = bandlimited_saw(state.exciterPhase, increment);
        exciter = (saw * 0.7F + white_noise(state.noiseState) * 0.15F) * energy * 0.35F;
    }

    float left = 0.0F;
    float right = 0.0F;
    for (std::uint8_t m = 0; m < state.activeModes; ++m) {
        auto& mode = state.modes[m];
        if (mode.gain <= 0.0F) continue;
        // Recompute per sample so pitch bend and MPE glide the whole bank.
        const float ratio = stretch_ratio(params.modes[m].frequencyRatio, params.inharmonicity);
        const float freq = std::min(base * ratio, sr * 0.45F);
        const float w = kTwoPi * freq / sr;
        const float r = mode.decayCoeff;
        const float cosw = std::cos(w);
        const float y = 2.0F * r * cosw * mode.y1 - r * r * mode.y2 + exciter * mode.gain;
        mode.y2 = mode.y1;
        mode.y1 = y;
        if ((m & 1U) == 0U) left += y * mode.gain;
        else right += y * mode.gain;
    }
    return {std::tanh(left * 0.9F), std::tanh(right * 0.9F)};
}

} // namespace modal_resonator

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
        case OscillatorWaveform::Sampler: return "sampler";
        case OscillatorWaveform::ModalResonator: return "modalresonator";
        case OscillatorWaveform::Spectral: return "spectral";
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
    if (value == "sampler") return OscillatorWaveform::Sampler;
    if (value == "modalresonator") return OscillatorWaveform::ModalResonator;
    if (value == "spectral") return OscillatorWaveform::Spectral;
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

// Phase 2: modal resonator excitation source tokens.
std::string_view modal_excitation_token(ExcitationSource e) noexcept {
    switch (e) {
        case ExcitationSource::Impulse: return "impulse";
        case ExcitationSource::NoiseBurst: return "noiseburst";
        case ExcitationSource::Oscillator: return "oscillator";
        case ExcitationSource::SampleTransient: return "sampletransient";
    }
    return "impulse";
}
std::optional<ExcitationSource> parse_modal_excitation(std::string_view value) noexcept {
    if (value == "impulse") return ExcitationSource::Impulse;
    if (value == "noiseburst") return ExcitationSource::NoiseBurst;
    if (value == "oscillator") return ExcitationSource::Oscillator;
    if (value == "sampletransient") return ExcitationSource::SampleTransient;
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
        case FilterTopology::Comb: return "comb";
        case FilterTopology::Formant: return "formant";
    }
    return "clean";
}
std::optional<FilterTopology> parse_filter_topology(std::string_view value) noexcept {
    if (value == "clean") return FilterTopology::CleanStateVariable;
    if (value == "moog_ladder") return FilterTopology::MoogLadder;
    if (value == "korg_ms20") return FilterTopology::KorgMs20;
    if (value == "oberheim_sem") return FilterTopology::OberheimSem;
    if (value == "comb") return FilterTopology::Comb;
    if (value == "formant") return FilterTopology::Formant;
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
        case ArpeggiatorDivision::DottedEighth: return "1/8D";
        case ArpeggiatorDivision::DottedQuarter: return "1/4D";
        case ArpeggiatorDivision::SixtyFourth: return "1/64";
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
    if (value == "1/8D") return ArpeggiatorDivision::DottedEighth;
    if (value == "1/4D") return ArpeggiatorDivision::DottedQuarter;
    if (value == "1/64") return ArpeggiatorDivision::SixtyFourth;
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
    static constexpr std::array<std::string_view, 24> names{
        "none","lfo1","lfo2","amp_env","filter_env","velocity","keytrack",
        "modwheel","aftertouch","random","macro1","macro2","macro3","macro4",
        "timbre","note_bend","release_vel",
        "spring","pendulum","orbiter","lorenz",
        "seq_timbre","seq_morph","seq_pan"};
    const auto index = static_cast<std::size_t>(source);
    return index < names.size() ? names[index] : names[0];
}
std::optional<ModulationSource> parse_modulation_source(std::string_view value) noexcept {
    for (std::size_t i = 0; i <= static_cast<std::size_t>(ModulationSource::SeqPan); ++i)
        if (modulation_source_token(static_cast<ModulationSource>(i)) == value) return static_cast<ModulationSource>(i);
    return std::nullopt;
}
std::string_view modulation_destination_token(ModulationDestination destination) noexcept {
    static constexpr std::array<std::string_view, 43> names{
        "none","global_pitch","filter_cutoff","filter_resonance","filter_drive","voice_gain","voice_pan",
        "osc1_pitch","osc2_pitch","osc3_pitch","osc4_pitch","osc5_pitch","osc6_pitch","osc7_pitch","osc8_pitch",
        "osc1_shape","osc2_shape","osc3_shape","osc4_shape","osc5_shape","osc6_shape","osc7_shape","osc8_shape",
        "osc1_pw","osc2_pw","osc3_pw","osc4_pw","osc5_pw","osc6_pw","osc7_pw","osc8_pw",
        "osc1_gain","osc2_gain","osc3_gain","osc4_gain","osc5_gain","osc6_gain","osc7_gain","osc8_gain",
        "wavetable_pos","morph_amount","sampler_start_pos","granular_pos"};
    const auto index = static_cast<std::size_t>(destination);
    return index < names.size() ? names[index] : names[0];
}
std::optional<ModulationDestination> parse_modulation_destination(std::string_view value) noexcept {
    for (std::size_t i = 0; i <= static_cast<std::size_t>(ModulationDestination::GranularPosition); ++i)
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
        case ArpeggiatorDivision::DottedEighth: return 0.75F;
        case ArpeggiatorDivision::DottedQuarter: return 1.5F;
        case ArpeggiatorDivision::SixtyFourth: return 0.0625F;
    }
    return 0.25F;
}

} // namespace

// Realtime-safe counterpart of morph_synth_presets(): interpolates two
// realtime presets without allocation (used by the audio thread's morph walk).
// Declared here in dve::audio scope to match the definition below.
[[nodiscard]] RealtimePreset morph_realtime_presets(const RealtimePreset& a, const RealtimePreset& b,
                                                    float amount) noexcept;

ModalResonatorParameters ModalResonatorParameters::make_default() {
    ModalResonatorParameters params;
    params.modeCount = 12;
    params.baseFrequency = 0.0F;   // follow the played note
    params.damping = 1.0F;
    params.inharmonicity = 0.0F;
    params.brightness = 0.5F;      // flat tilt
    params.excitation = ExcitationSource::Impulse;
    params.excitationLevel = 1.0F;
    params.noiseBurstMilliseconds = 40.0F;
    params.transientMilliseconds = 60.0F;
    for (std::size_t m = 0; m < params.modes.size(); ++m) {
        auto& mode = params.modes[m];
        mode.frequencyRatio = static_cast<float>(m + 1U);              // harmonic series
        mode.decaySeconds = 2.5F / (1.0F + 0.35F * static_cast<float>(m));
        mode.gain = 1.0F / (1.0F + 0.5F * static_cast<float>(m));      // gentle high rolloff
    }
    return params;
}

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
        case OscillatorWaveform::Sampler: return "Sampler";
        case OscillatorWaveform::ModalResonator: return "Modal Resonator";
        case OscillatorWaveform::Spectral: return "Spectral";
    }
    return "Saw";
}
std::string_view modal_excitation_source_name(ExcitationSource source) noexcept {
    switch (source) {
        case ExcitationSource::Impulse: return "Impulse";
        case ExcitationSource::NoiseBurst: return "Noise Burst";
        case ExcitationSource::Oscillator: return "Oscillator";
        case ExcitationSource::SampleTransient: return "Sample Transient";
    }
    return "Impulse";
}
std::string_view filter_topology_name(FilterTopology topology) noexcept {
    switch (topology) {
        case FilterTopology::CleanStateVariable: return "Clean SVF";
        case FilterTopology::MoogLadder: return "Moog Ladder";
        case FilterTopology::KorgMs20: return "Korg MS-20";
        case FilterTopology::OberheimSem: return "Oberheim SEM";
        case FilterTopology::Comb: return "Comb";
        case FilterTopology::Formant: return "Formant";
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

// Phase 2: auto oversampling policy. Applies only when oversampling is Auto:
// X1 by default, upgraded under high resonance / engaged drive (where
// nonlinear stages alias most). An explicit X1/X2/X4 setting is always honored
// exactly, so existing presets keep their exact old behavior.
FilterOversampling effective_oversampling(const FilterParameters& params) noexcept {
    if (params.oversampling != FilterOversampling::Auto) return params.oversampling;
    const bool driveEngaged = params.drive > kAutoOversampleDriveThreshold;
    if (params.resonance > kAutoOversampleExtremeResonanceThreshold && driveEngaged)
        return FilterOversampling::X4;
    if (params.resonance > kAutoOversampleResonanceThreshold || driveEngaged)
        return FilterOversampling::X2;
    return FilterOversampling::X1;
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

// Phase 2: sampler name strings.
std::string_view sampler_playback_mode_name(SamplerPlaybackMode mode) noexcept {
    switch (mode) {
        case SamplerPlaybackMode::OneShot: return "One-Shot";
        case SamplerPlaybackMode::Loop: return "Loop";
    }
    return "One-Shot";
}
std::string_view sampler_direction_name(SamplerDirection direction) noexcept {
    switch (direction) {
        case SamplerDirection::Forward: return "Forward";
        case SamplerDirection::Reverse: return "Reverse";
    }
    return "Forward";
}
std::string_view sampler_playback_mode_token(SamplerPlaybackMode mode) noexcept {
    switch (mode) {
        case SamplerPlaybackMode::OneShot: return "oneshot";
        case SamplerPlaybackMode::Loop: return "loop";
    }
    return "oneshot";
}
std::string_view sampler_direction_token(SamplerDirection direction) noexcept {
    switch (direction) {
        case SamplerDirection::Forward: return "forward";
        case SamplerDirection::Reverse: return "reverse";
    }
    return "forward";
}
std::optional<SamplerPlaybackMode> parse_sampler_playback_mode(std::string_view value) noexcept {
    if (value == "oneshot") return SamplerPlaybackMode::OneShot;
    if (value == "loop") return SamplerPlaybackMode::Loop;
    return std::nullopt;
}
std::optional<SamplerDirection> parse_sampler_direction(std::string_view value) noexcept {
    if (value == "forward") return SamplerDirection::Forward;
    if (value == "reverse") return SamplerDirection::Reverse;
    return std::nullopt;
}


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
          flangerL(static_cast<std::size_t>(rate / 40U + 32U)), flangerR(static_cast<std::size_t>(rate / 40U + 32U)),
          ensembleBufL(static_cast<std::size_t>(rate / 25U + 32U)), ensembleBufR(static_cast<std::size_t>(rate / 25U + 32U)),
          delayL(static_cast<std::size_t>(rate * 2U + 2U)), delayR(static_cast<std::size_t>(rate * 2U + 2U)),
          diffDelayL(static_cast<std::size_t>(rate * 2U + 2U)), diffDelayR(static_cast<std::size_t>(rate * 2U + 2U)),
          // Phase 2: diffusion allpass stage times (ms), decorrelated per channel.
          diffApL{DelayLine(diffusion_stage_size(5.9F, rate)), DelayLine(diffusion_stage_size(11.3F, rate)),
                  DelayLine(diffusion_stage_size(17.7F, rate)), DelayLine(diffusion_stage_size(23.1F, rate))},
          diffApR{DelayLine(diffusion_stage_size(6.7F, rate)), DelayLine(diffusion_stage_size(12.9F, rate)),
                  DelayLine(diffusion_stage_size(18.3F, rate)), DelayLine(diffusion_stage_size(25.7F, rate))},
          reverb(rate) {}

    float sampleRate{};
    std::atomic<std::uint64_t>& currentFrame;
    RealtimePreset parameters{};
    // Phase 1: high-quality wavetable bank (cooked on preset load).
    CookedWavetable hqWavetable{};
    // Phase 1: physics modulation bank (global).
    PhysicsModulationBank physicsBank{};
    // Phase 3: generative step sequencer (SYN-012). Driven once per render
    // block by advance_sequencer(); disabled by default.
    Sequencer sequencer{};
    // Phase 3: generative conductor (attractor -> live synth mapping). Driven
    // once per render block; a no-op unless enabled.
    GenerativeConductor conductor{};
    // Phase 3: conductor-driven filter cutoff multiplier (1.0 = no change).
    // Written by the conductor on the render thread, read by the voice DSP
    // on the same thread.
    float conductorCutoffMultiplier{1.0F};
    // Phase 1: morph A/B numeric presets, cached on the render thread so the
    // conductor's per-block morph walk never copies strings. Populated by
    // adopt_preset(); applied by apply_morph_amount().
    RealtimePreset morphBaseA_{};
    RealtimePreset morphBaseB_{};
    bool morphHasB_{false};
    float appliedMorphAmount_{-1.0F};  // amount baked into parameters (-1 = none)
    // Phase 1: wavetable cook gating (fix: no per-block re-cook/allocation).
    std::uint64_t cookedWavetableHash_{0};
    std::atomic<std::uint64_t> wavetableCookCount_{0};
    std::vector<float> wavetableCookScratch_;              // flat 64x512 resample target
    std::vector<std::complex<float>> wavetableCookSpectrum_;  // persistent DFT scratch
    std::vector<float> wavetableCookFiltered_;             // persistent DFT scratch
    std::vector<float> wavetableCookFrame_;                // persistent frame scratch
    // Phase 0: smoothed live parameters. Targets are set in adopt_preset();
    // Phase 0: smoothed live parameters. Targets are set in adopt_preset();
    // currents advance toward targets once per render block in
    // advance_parameter_smoothing(). The DSP reads the smoothed currents,
    // not parameters.X directly, for these fields.
    SmoothedFloat smoothedFilterCutoffLog{};  // log2(cutoffHz)
    SmoothedFloat smoothedFilterResonance{};
    SmoothedFloat smoothedMasterGain{};
    std::array<SmoothedFloat, kSynthOscillatorCount> smoothedOscGain{};
    SmoothedFloat smoothedDelayTime{};
    SmoothedFloat smoothedDiffDelayTime{};
    SmoothedFloat smoothedDiffDelayFeedback{};
    SmoothedFloat smoothedEqLowDb{};
    SmoothedFloat smoothedEqMidDb{};
    SmoothedFloat smoothedEqHighDb{};
    SmoothedFloat smoothedDistortionDrive{};
    SmoothedFloat smoothedDelayFeedback{};
    SmoothedFloat smoothedFlangerFeedback{};
    SmoothedFloat smoothedCompThresholdDb{};
    bool parameterSmoothingInitialized{};
    BoundedQueue<MidiMessage, kMidiQueueCapacity> midiIn;
    BoundedQueue<MidiMessage, kMidiOutQueueCapacity> midiOut;
    BoundedQueue<PresetUpdate, kPresetQueueCapacity> presetIn;
    std::atomic<std::uint64_t> droppedPresets{0};  // presetIn overflow (UI thread only)
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
    // Synth-wide profiler counters (see SynthProfiler). Updated on the render thread with
    // relaxed ordering; read via Synthesizer::profiler().
    std::atomic<std::uint64_t> profilerRenderCalls{};
    std::atomic<std::uint64_t> profilerSamplesRendered{};
    std::atomic<std::uint64_t> profilerVoicesStarted{};
    std::atomic<std::uint64_t> profilerVoicesStolen{};
    std::atomic<std::uint64_t> profilerVoicesRetired{};
    std::atomic<std::uint64_t> profilerOscillatorVoiceFrames{};
    std::atomic<std::uint64_t> profilerFilterFrames{};
    std::atomic<std::uint64_t> profilerFxFrames{};
    std::atomic<std::uint64_t> profilerArpSteps{};
    std::atomic<std::uint32_t> profilerActiveVoices{};
    std::atomic<std::uint32_t> profilerMaximumActiveVoices{};
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
    std::uint64_t arpStrumMaxDelay{};
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
    std::array<std::uint64_t, kSynthVoiceCount> arpPatternDelays{};
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
    DelayLine flangerL;
    DelayLine flangerR;
    float flangerPhase{};
    float flangerFeedbackL{};
    float flangerFeedbackR{};
    DelayLine ensembleBufL;
    DelayLine ensembleBufR;
    float ensemblePhase{};
    float fuzzToneL{};
    float fuzzToneR{};
    float crusherHoldL{};
    float crusherHoldR{};
    std::uint32_t crusherCount{};
    std::array<AllpassStage, 4> phaserL{};
    std::array<AllpassStage, 4> phaserR{};
    float phaserPhase{};
    float phaserFeedbackL{};
    float phaserFeedbackR{};
    DelayLine delayL;
    DelayLine delayR;
    // Phase 2: diffusion delay — recirculating delay line plus per-channel
    // cascaded allpass diffusion stages.
    DelayLine diffDelayL;
    DelayLine diffDelayR;
    std::array<DelayLine, 4> diffApL;
    std::array<DelayLine, 4> diffApR;
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
        profilerVoicesStolen.fetch_add(1U, std::memory_order_relaxed);
        auto released = std::min_element(voice.begin(), voice.end(), [](const Voice& a, const Voice& b) {
            const bool ar = a.amp.stage == VoiceStage::Release;
            const bool br = b.amp.stage == VoiceStage::Release;
            if (ar != br) return ar;
            if (a.amp.value != b.amp.value) return a.amp.value < b.amp.value;
            return a.age < b.age;
        });
        return *released;
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

    // Phase 4: forwards one voice's drained granular counters into the
    // synth-level profiler atomics (Synthesizer::granular_profiler()).
    void forward_granular_counters(GranularEngine& engine) noexcept {
        const GranularCounters drained = engine.drain_counters();
        if (drained.requestedGrains != 0U)
            requestedGrains.fetch_add(drained.requestedGrains, std::memory_order_relaxed);
        if (drained.admittedGrains != 0U)
            admittedGrains.fetch_add(drained.admittedGrains, std::memory_order_relaxed);
        if (drained.grainSteals != 0U)
            grainSteals.fetch_add(drained.grainSteals, std::memory_order_relaxed);
        if (drained.grainMisses != 0U)
            grainMisses.fetch_add(drained.grainMisses, std::memory_order_relaxed);
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
        profilerVoicesStarted.fetch_add(1U, std::memory_order_relaxed);
        const bool sameNote = target.active && target.channel == channel && target.note == note;
        const bool restart = retrigger || !sameNote;
        target.start(channel, note, static_cast<float>(velocity) / 127.0F, ++ageCounter, renderFrame,
                     parameters, restart);
        if (restart) assign_sample_zones(target);
        // Phase 1: excite physics modulators on note-on.
        physicsBank.note_on(static_cast<float>(velocity) / 127.0F);
        if (emitOutput) emit_note(true, channel, note, velocity, renderFrame);
    }

    void direct_note_off(std::uint8_t channel, std::uint8_t note, bool emitOutput,
                         float releaseVelocity = 0.5F) noexcept {
        for (Voice& candidate : voice) {
            if (!candidate.active || candidate.channel != channel || candidate.note != note) continue;
            candidate.keyHeld = false;
            candidate.releaseVelocity = clampf(releaseVelocity, 0.0F, 1.0F);
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
        for (std::size_t i = 0; i < arpActiveCount; ++i) {
            for (ScheduledVoiceEvent& event : scheduledEvents)
                if (event.active && event.noteOn && event.channel == arpActiveChannels[i] &&
                    event.note == arpActiveNotes[i]) event.active = false;
            direct_note_off(arpActiveChannels[i], arpActiveNotes[i], true);
        }
        arpActiveCount = 0;
        arpGateOffFrame = std::numeric_limits<std::uint64_t>::max();
        arpStrumMaxDelay = 0;
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
        arpStrumMaxDelay = 0;
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
            std::uint64_t maxDelay = 0;
            for (std::size_t i = 0; i < arpPatternCount && arpActiveCount < kSynthVoiceCount; ++i) {
                const std::uint64_t delay = arpPatternDelays[i];
                maxDelay = std::max(maxDelay, delay);
                if (delay == 0) {
                    direct_note_on(arpPatternChannels[i], arpPatternNotes[i], arpPatternVelocities[i],
                                   parameters.arpeggiator.retriggerEnvelopes, true);
                } else {
                    (void)schedule_voice_event(true, arpPatternChannels[i], arpPatternNotes[i],
                                               arpPatternVelocities[i], renderFrame + delay, true);
                }
                arpActiveNotes[arpActiveCount] = arpPatternNotes[i];
                arpActiveChannels[arpActiveCount] = arpPatternChannels[i];
                ++arpActiveCount;
            }
            arpStrumMaxDelay = maxDelay;
        }
        const std::uint8_t configuredStepCount = static_cast<std::uint8_t>(
            std::clamp<unsigned>(parameters.arpeggiator.stepCount, 1U, static_cast<unsigned>(kArpeggiatorStepCount)));
        const std::uint32_t stepIndex = (arpStepCounter == 0U ? 0U : arpStepCounter - 1U) % configuredStepCount;
        const ArpeggiatorStep& step = parameters.arpeggiator.steps[stepIndex];
        const float gate = clampf(parameters.arpeggiator.gate * step.gateScale, 0.02F, 1.0F);
        const std::uint64_t segmentEnd = std::min(arpStepEndFrame, renderFrame + arpRatchetDuration);
        arpGateOffFrame = step.tie && arpRatchetCount == 1U ? arpStepEndFrame :
            renderFrame + arpStrumMaxDelay + std::max<std::uint64_t>(1U,
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
        profilerArpSteps.fetch_add(1U, std::memory_order_relaxed);

        const std::uint8_t configuredStepCount = static_cast<std::uint8_t>(
            std::clamp<unsigned>(parameters.arpeggiator.stepCount, 1U, static_cast<unsigned>(kArpeggiatorStepCount)));
        const std::uint32_t stepIndex = arpStepCounter % configuredStepCount;
        const ArpeggiatorStep& step = parameters.arpeggiator.steps[stepIndex];
        const std::uint32_t sequencePosition = arpStepCounter;
        activeArpStep.store(stepIndex, std::memory_order_relaxed);
        const std::uint64_t duration = arpeggiator_duration_frames(arpStepCounter);
        ++arpStepCounter;
        nextArpFrame = renderFrame + duration;
        const float humanTiming = clampf(parameters.arpeggiator.humanizeTiming, 0.0F, 1.0F);
        if (humanTiming > 0.0F) {
            const float jitterMs = (random_unit() * 2.0F - 1.0F) * humanTiming * 12.0F;
            const std::int64_t jitterFrames = static_cast<std::int64_t>(jitterMs * 0.001F * sampleRate);
            const std::int64_t jittered = static_cast<std::int64_t>(nextArpFrame) + jitterFrames;
            nextArpFrame = static_cast<std::uint64_t>(std::max<std::int64_t>(0, jittered));
        }
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
            case ArpeggiatorCondition::AB: {
                const std::uint32_t a = std::clamp<std::uint32_t>(step.conditionA, 1U, 8U);
                const std::uint32_t b = std::clamp<std::uint32_t>(step.conditionB, 1U, 8U);
                const std::uint32_t loopPass = sequencePosition / configuredStepCount;
                conditionPass = (loopPass % b) == (a - 1U);
                break;
            }
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
        const float humanVel = clampf(parameters.arpeggiator.humanizeVelocity, 0.0F, 1.0F);
        const float velJitter = humanVel > 0.0F ? 1.0F + (random_unit() * 2.0F - 1.0F) * humanVel * 0.3F : 1.0F;
        const float phrasePos = configuredStepCount > 1U ?
            static_cast<float>(stepIndex) / static_cast<float>(configuredStepCount - 1U) : 0.0F;
        const float phraseVel = clampf(parameters.arpeggiator.phraseVelocityStart +
            (parameters.arpeggiator.phraseVelocityEnd - parameters.arpeggiator.phraseVelocityStart) * phrasePos,
            0.0F, 2.0F);
        for (std::size_t rootIndex = 0; rootIndex < rootCount && arpPatternCount < kSynthVoiceCount; ++rootIndex) {
            const HeldNote& root = roots[rootIndex];
            const int transposed = static_cast<int>(root.note) + static_cast<int>(step.transpose) +
                                   static_cast<int>(step.octaveOffset) * 12;
            const std::uint8_t baseNote = static_cast<std::uint8_t>(quantize_note_to_scale(
                transposed, parameters.arpeggiator.scale, parameters.arpeggiator.scaleRoot));
            const std::uint8_t velocity = static_cast<std::uint8_t>(clampf(
                static_cast<float>(root.velocity) * velocityScale * phraseVel * velJitter, 1.0F, 127.0F));
            if (parameters.chord.enabled) {
                const ChordVoicing voicing = make_chord_voicing(baseNote, parameters.chord);
                const std::uint64_t strumFrames = static_cast<std::uint64_t>(std::llround(
                    clampf(parameters.chord.strumMilliseconds, 0.0F, 250.0F) * 0.001F * sampleRate));
                for (std::size_t i = 0; i < voicing.count && arpPatternCount < kSynthVoiceCount; ++i) {
                    arpPatternNotes[arpPatternCount] = voicing.notes[i];
                    arpPatternChannels[arpPatternCount] = root.channel;
                    arpPatternVelocities[arpPatternCount] = static_cast<std::uint8_t>(clampf(
                        static_cast<float>(velocity) * parameters.chord.velocityScale, 1.0F, 127.0F));
                    arpPatternDelays[arpPatternCount] = strumFrames * i;
                    ++arpPatternCount;
                }
            } else {
                arpPatternNotes[arpPatternCount] = baseNote;
                arpPatternChannels[arpPatternCount] = root.channel;
                arpPatternVelocities[arpPatternCount] = velocity;
                arpPatternDelays[arpPatternCount] = 0;
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

    // Applies a morph amount to the live parameters by interpolating the
    // cached numeric A/B presets. Realtime-safe: no strings, no allocation.
    // Skipped (cheaply) when morphing is off or the amount hasn't changed.
    void apply_morph_amount(float amount, bool morphEnabled) noexcept {
        if (!morphEnabled || !morphHasB_) return;
        const float clamped = clampf(amount, 0.0F, 1.0F);
        if (clamped == appliedMorphAmount_) return;
        parameters = morph_realtime_presets(morphBaseA_, morphBaseB_, clamped);
        appliedMorphAmount_ = clamped;
        // The morphed wavetable content snaps at t >= 0.5 (see
        // morph_realtime_presets); re-cook only if the content actually changed.
        cook_wavetable_if_changed();
    }

    // Cooks parameters.wavetable into the HQ engine only when its content
    // changed since the last cook. The resample target and DFT scratch are
    // persistent members, so repeat cooks perform no allocation.
    void cook_wavetable_if_changed() noexcept {
        if (!parameters.wavetable.enabled || parameters.wavetable.frameCount == 0U) return;
        const std::uint64_t hash = wavetable_content_hash(parameters.wavetable);
        if (hash == cookedWavetableHash_) return;
        // Phase 1: cook the preset wavetable into the HQ engine (64 frames).
        // Interpolates the preset's mip-0 frames up to kHQWavetableFrames.
        wavetableCookScratch_.resize(kHQWavetableFrames * kHQWavetableSamples);
        const std::size_t srcFrames = std::min<std::size_t>(parameters.wavetable.frameCount, kWavetableFrameCount);
        const float* mip0 = parameters.wavetable.samples.data(); // mip 0 is first
        for (std::size_t f = 0; f < kHQWavetableFrames; ++f) {
            const float srcPos = static_cast<float>(f) / static_cast<float>(kHQWavetableFrames - 1) *
                                 static_cast<float>(srcFrames - 1);
            const std::size_t f0 = static_cast<std::size_t>(srcPos);
            const std::size_t f1 = std::min(f0 + 1, srcFrames - 1);
            const float frac = srcPos - static_cast<float>(f0);
            float* frame = wavetableCookScratch_.data() + f * kHQWavetableSamples;
            for (std::size_t i = 0; i < kHQWavetableSamples; ++i) {
                // Resample from 128 to 512 samples.
                const float srcSamplePos = static_cast<float>(i) / static_cast<float>(kHQWavetableSamples - 1) *
                                           static_cast<float>(kWavetableSampleCount - 1);
                const std::size_t s0 = static_cast<std::size_t>(srcSamplePos);
                const std::size_t s1 = std::min(s0 + 1, kWavetableSampleCount - 1);
                const float sFrac = srcSamplePos - static_cast<float>(s0);
                const float a0 = mip0[f0 * kWavetableSampleCount + s0];
                const float a1 = mip0[f0 * kWavetableSampleCount + s1];
                const float b0 = mip0[f1 * kWavetableSampleCount + s0];
                const float b1 = mip0[f1 * kWavetableSampleCount + s1];
                const float a = a0 + (a1 - a0) * sFrac;
                const float b = b0 + (b1 - b0) * sFrac;
                frame[i] = a + (b - a) * frac;
            }
        }
        cook_wavetable_inplace(hqWavetable, "Preset", wavetableCookScratch_.data(),
                               kHQWavetableFrames, wavetableCookSpectrum_,
                               wavetableCookFiltered_, wavetableCookFrame_);
        cookedWavetableHash_ = hash;
        wavetableCookCount_.fetch_add(1U, std::memory_order_relaxed);
    }

    void adopt_preset(const PresetUpdate& update, float morphAmount, bool morphEnabled) noexcept {
        const bool wasArpeggiating = parameters.arpeggiator.enabled;
        // Cache the morph endpoints for the render thread's per-block walk.
        morphBaseA_ = update.base;
        morphBaseB_ = update.morphB;
        morphHasB_ = update.hasMorphB;
        // Sequencer/conductor configs were applied from the UI thread (data
        // race vs advance_sequencer()/conductor.process()); they are now
        // applied here on the render thread.
        if (update.sequencerChanged) sequencer.apply_config(update.sequencer);
        conductor.configure(update.attractorEnabled, update.attractor);
        parameters = update.base;
        appliedMorphAmount_ = -1.0F;  // force re-application below
        cook_wavetable_if_changed();
        apply_morph_amount(morphAmount, morphEnabled);
        macroValues = parameters.macroValues;
        gameClockTempo.store(parameters.arpeggiator.externalTempoBpm, std::memory_order_relaxed);
        arpRandomState = parameters.arpeggiator.randomSeed == 0U ? 0x51A3D8E7U : parameters.arpeggiator.randomSeed;
        if (wasArpeggiating && !parameters.arpeggiator.enabled) clear_arp_held(true);
        // Phase 0: retarget smoothed parameters. On the very first adoption,
        // snap currents to targets so there's no glide from zero.
        auto retarget = [&](SmoothedFloat& s, float v) {
            if (!parameterSmoothingInitialized) s.snap(v);
            else s.set_target(v);
        };
        retarget(smoothedFilterCutoffLog, std::log2(std::max(parameters.filter.cutoffHertz, 1.0F)));
        retarget(smoothedFilterResonance, parameters.filter.resonance);
        retarget(smoothedMasterGain, parameters.masterGain);
        for (std::size_t i = 0; i < kSynthOscillatorCount; ++i)
            retarget(smoothedOscGain[i], parameters.oscillators[i].gain);
        // Phase 2: tempo-synced delay resolves through effective_delay_time_seconds()
        // (manual timeSeconds when tempoSync is off), so the first adoption snaps
        // to the synced time and later render blocks track live tempo changes.
        retarget(smoothedDelayTime, effective_delay_time_seconds());
        retarget(smoothedDiffDelayTime, parameters.diffusionDelay.timeSeconds);
        retarget(smoothedEqLowDb, parameters.eq.lowGainDb);
        retarget(smoothedEqMidDb, parameters.eq.midGainDb);
        retarget(smoothedEqHighDb, parameters.eq.highGainDb);
        retarget(smoothedDistortionDrive, parameters.distortion.drive);
        retarget(smoothedDelayFeedback, parameters.delay.feedback);
        retarget(smoothedDiffDelayFeedback, parameters.diffusionDelay.feedback);
        retarget(smoothedFlangerFeedback, parameters.flanger.feedback);
        retarget(smoothedCompThresholdDb, parameters.compressor.thresholdDb);
        parameterSmoothingInitialized = true;
    }

    // Advances all smoothed parameters one step. Call once per render() call
    // with the frame count — the one-pole coefficient is computed for the
    // actual time elapsed. Smoothing time is ~12 ms (fast enough to feel
    // responsive, slow enough to kill clicks).
    void advance_parameter_smoothing(std::size_t frameCount) noexcept {
        if (!parameterSmoothingInitialized || frameCount == 0) return;
        // One-pole coefficient for ~12 ms time constant.
        // coeff = 1 - exp(-elapsed / timeConstant)
        const float elapsed = static_cast<float>(frameCount) / sampleRate;
        const float coeff = 1.0F - std::exp(-elapsed / 0.012F);
        smoothedFilterCutoffLog.advance(coeff);
        smoothedFilterResonance.advance(coeff);
        smoothedMasterGain.advance(coeff);
        for (auto& s : smoothedOscGain) s.advance(coeff);
        smoothedDelayTime.advance(coeff);
        smoothedDiffDelayTime.advance(coeff);
        smoothedDiffDelayFeedback.advance(coeff);
        smoothedEqLowDb.advance(coeff);
        smoothedEqMidDb.advance(coeff);
        smoothedEqHighDb.advance(coeff);
        smoothedDistortionDrive.advance(coeff);
        smoothedDelayFeedback.advance(coeff);
        smoothedFlangerFeedback.advance(coeff);
        smoothedCompThresholdDb.advance(coeff);
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
            const float releaseVel = message.data2 > 0 ? static_cast<float>(message.data2) / 127.0F : 0.5F;
            if (parameters.arpeggiator.enabled) arp_note_off(channel, message.data1);
            else if (parameters.chord.enabled) chord_note_off(channel, message.data1);
            else direct_note_off(channel, message.data1, false, releaseVel);
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
                        if (message.data1 == 120U) { candidate.kill(); }
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

    // Phase 2: resolves the delay time honoring tempo sync, clamped to the
    // delay line's range. Manual timeSeconds when tempoSync is off; otherwise
    // syncBeats * 60 / effectiveTempoBpm using the same tempo source the LFO
    // tempo sync uses (see effective_arpeggiator_tempo above).
    float effective_delay_time_seconds() const noexcept {
        if (!parameters.delay.tempoSync) return parameters.delay.timeSeconds;
        const float tempo = clampf(effective_arpeggiator_tempo(), 20.0F, 400.0F);
        const float beats = clampf(parameters.delay.syncBeats, 0.03125F, 32.0F);
        return clampf(beats * 60.0F / tempo, 0.01F, 1.95F);
    }

    // Phase 2: keep a tempo-synced delay glued to the live tempo. The game
    // clock / MIDI clock can move at any time without a preset adoption, so
    // retarget the (already smoothed) delay time once per render block; the
    // ~12 ms one-pole smoother absorbs tempo glides without zipper noise.
    void track_tempo_synced_delay() noexcept {
        if (parameters.delay.tempoSync)
            smoothedDelayTime.set_target(effective_delay_time_seconds());
    }

    // Phase 3: advance the generative sequencer once per render block. The
    // step clock is a 16th note resolved from the same tempo source the LFO
    // tempo sync uses (effective_arpeggiator_tempo()). Note events are
    // scheduled sample-accurately via schedule_voice_event() so onsets land
    // inside the current block; process_scheduled_events() (called per frame
    // below) fires them through direct_note_on/off like the arpeggiator.
    void advance_sequencer(std::size_t frameCount) noexcept {
        if (!sequencer.enabled()) return;
        const float tempo = clampf(effective_arpeggiator_tempo(), 20.0F, 400.0F);
        const std::uint64_t blockStart = currentFrame.load(std::memory_order_relaxed);
        const std::uint8_t channel = sequencer.channel();
        sequencer.process(static_cast<std::uint32_t>(frameCount), sampleRate, tempo,
                          [&](const Sequencer::Event& event) {
                              const std::uint64_t at = blockStart +
                                  static_cast<std::uint64_t>(event.frameOffset);
                              const std::uint8_t velocity = static_cast<std::uint8_t>(
                                  clampf(event.velocity, 0.0F, 1.0F) * 127.0F);
                              (void)schedule_voice_event(event.noteOn, channel, event.note,
                                                         velocity, at, true);
                          });
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
        float wavetablePosition{};  // Phase 1: added
        float morphAmount{};        // Phase 1: added
        float samplerStartPosition{};  // Phase 2: added (normalized, scaled by sample duration at use)
        float granularPosition{};  // Phase 4: added (grain source position offset, 0..1 over the bank)
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
            case ModulationSource::Timbre: return clampf(v.timbre, -1.0F, 1.0F);
            case ModulationSource::NotePitchBend:
                return clampf(v.pitchBendSemitones / std::max(1.0F, parameters.pitchBendRangeSemitones), -1.0F, 1.0F);
            case ModulationSource::ReleaseVelocity: return v.releaseVelocity;
            case ModulationSource::Spring: return physicsBank.spring.position;
            case ModulationSource::Pendulum: return std::sin(physicsBank.pendulum.angle);
            case ModulationSource::Orbiter: return std::clamp(physicsBank.orbiter.x * 0.5F, -1.0F, 1.0F);
            case ModulationSource::Lorenz: return std::clamp(physicsBank.lorenz.x / 20.0F, -1.0F, 1.0F);
            // Phase 3: generative sequencer lane currents.
            case ModulationSource::SeqTimbre: return clampf(sequencer.timbre_value(), -1.0F, 1.0F);
            case ModulationSource::SeqMorph: return clampf(sequencer.morph_value(), 0.0F, 1.0F);
            case ModulationSource::SeqPan: return clampf(sequencer.pan_value(), -1.0F, 1.0F);
        }
        return 0.0F;
    }

    static bool native_bipolar(ModulationSource source) noexcept {
        return source == ModulationSource::Lfo1 || source == ModulationSource::Lfo2 ||
               source == ModulationSource::KeyTrack || source == ModulationSource::Random ||
               source == ModulationSource::Timbre || source == ModulationSource::NotePitchBend ||
               source == ModulationSource::SeqTimbre || source == ModulationSource::SeqPan;
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
            const float amount = source * clampf(slot.amount, -1.0F, 1.0F) + clampf(slot.bias, -1.0F, 1.0F);
            modulationScratch[originalIndex] = amount;
            const auto destination = static_cast<unsigned>(slot.destination);
            if (slot.destination == ModulationDestination::GlobalPitch) values.globalPitch += amount * 24.0F;
            else if (slot.destination == ModulationDestination::FilterCutoff) values.filterCutoff += amount * 8.0F;
            else if (slot.destination == ModulationDestination::FilterResonance) values.filterResonance += amount * 0.75F;
            else if (slot.destination == ModulationDestination::FilterDrive) values.filterDrive += amount * 8.0F;
            else if (slot.destination == ModulationDestination::VoiceGain) values.voiceGain += amount;
            else if (slot.destination == ModulationDestination::VoicePan) values.voicePan += amount;
            else if (slot.destination == ModulationDestination::WavetablePosition) values.wavetablePosition += amount;
            else if (slot.destination == ModulationDestination::MorphAmount) values.morphAmount += amount;
            else if (slot.destination == ModulationDestination::SamplerStartPosition) values.samplerStartPosition += amount;
            else if (slot.destination == ModulationDestination::GranularPosition) values.granularPosition += amount;
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
        // Phase 1: use the HQ wavetable engine if cooked, else fall back to legacy.
        if (hqWavetable.valid()) {
            const float frequency = increment * sampleRate;
            const std::size_t mip = wavetable_mip_for_frequency(frequency, sampleRate);
            return sample_wavetable(hqWavetable, phase, position, mip);
        }
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

    // Phase 2: cubic (Catmull-Rom) interpolation over the shared preset sample
    // bank. Position is in frames; out-of-range positions are clamped.
    float sampler_cubic_sample(const RealtimeSampleBank& bank, float position) const noexcept {
        const float frames = static_cast<float>(bank.frameCount);
        const float bounded = clampf(position, 0.0F, frames - 1.0F);
        const std::int32_t center = static_cast<std::int32_t>(bounded);
        const float fraction = bounded - static_cast<float>(center);
        const std::uint32_t last = bank.frameCount - 1U;
        const std::uint32_t i0 = static_cast<std::uint32_t>(std::max<std::int32_t>(center - 1, 0));
        const std::uint32_t i1 = static_cast<std::uint32_t>(center);
        const std::uint32_t i2 = std::min(i1 + 1U, last);
        const std::uint32_t i3 = std::min(i1 + 2U, last);
        const float p0 = bank.samples[i0];
        const float p1 = bank.samples[i1];
        const float p2 = bank.samples[i2];
        const float p3 = bank.samples[i3];
        const float f2 = fraction * fraction;
        const float f3 = f2 * fraction;
        return 0.5F * (2.0F * p1 + (p2 - p0) * fraction +
                       (2.0F * p0 - 5.0F * p1 + 4.0F * p2 - p3) * f2 +
                       (3.0F * (p1 - p2) + p3 - p0) * f3);
    }

    // Phase 2: dedicated sampler generator. Reads the shared preset sample
    // bank (resident, cooked on the control thread). One-shot releases the
    // voice at the sample end; loop mode wraps inside [loopStart, loopEnd)
    // with an equal-power crossfade. Reverse plays backwards; the start
    // offset is measured back from the region end in reverse so the default
    // (offset 0) begins at the end of the sample.
    std::pair<float, float> render_sampler(Voice& v, std::size_t oscillatorIndex,
                                           float frequency, const ModulationValues& mod) noexcept {
        const RealtimeSampleBank& bank = parameters.sampleBank;
        const SamplerParameters& sampler = parameters.sampler;
        if (!sampler.enabled || !bank.enabled || bank.frameCount < 2U) return {};
        if (v.samplerFinished[oscillatorIndex]) return {};
        const float frameCount = static_cast<float>(bank.frameCount);
        const float bankRate = static_cast<float>(bank.sampleRate);
        const float durationSeconds = frameCount / bankRate;
        const bool reverse = sampler.direction == SamplerDirection::Reverse;
        const bool loop = sampler.playbackMode == SamplerPlaybackMode::Loop;

        float loopStart = clampf(sampler.loopStartSeconds * bankRate, 0.0F, frameCount);
        float loopEnd = clampf(sampler.loopEndSeconds * bankRate, 0.0F, frameCount);
        // Loop mode with a degenerate (empty or out-of-range) loop falls back to
        // the full sample so the voice keeps sounding instead of pinning a
        // single clamped frame.
        if (loop && loopEnd - loopStart < 1.0F) {
            loopStart = 0.0F;
            loopEnd = frameCount;
        }
        if (loopEnd < loopStart + 1.0F) loopEnd = std::min(loopStart + 1.0F, frameCount);
        if (loopStart > loopEnd - 1.0F) loopStart = std::max(loopEnd - 1.0F, 0.0F);
        const float loopLength = loopEnd - loopStart;
        const bool loopValid = loop && loopLength >= 1.0F;
        const float crossfade = clampf(sampler.loopCrossfadeSeconds * bankRate, 0.0F, loopLength * 0.5F);
        const float regionStart = loopValid ? loopStart : 0.0F;
        const float regionEnd = loopValid ? loopEnd : frameCount;
        const float regionLength = regionEnd - regionStart;

        float& position = v.samplerPositions[oscillatorIndex];
        if (!v.samplerPrimed[oscillatorIndex]) {
            const float startSeconds = clampf(
                sampler.startOffsetSeconds + mod.samplerStartPosition * durationSeconds,
                0.0F, durationSeconds);
            if (!reverse) {
                position = clampf(startSeconds * bankRate, regionStart, regionEnd - 1.0F);
            } else {
                const float offsetFrames = clampf(startSeconds * bankRate, 0.0F, regionLength - 1.0F);
                position = regionEnd - 1.0F - offsetFrames;
            }
            v.samplerPrimed[oscillatorIndex] = true;
        }

        float ratio = 1.0F;
        if (sampler.pitchTracking) {
            const float root = tuned_frequency(bank.rootNote, 0.0F);
            ratio = root > 0.0001F ? frequency / root : 1.0F;
        }
        const float step = ratio * bankRate / sampleRate;

        float output = sampler_cubic_sample(bank, position);
        if (loopValid && crossfade > 0.0F) {
            float phase = -1.0F;
            float alternatePosition = 0.0F;
            if (!reverse && position >= loopEnd - crossfade && position < loopEnd) {
                phase = (position - (loopEnd - crossfade)) / crossfade;
                alternatePosition = loopStart + (position - (loopEnd - crossfade));
            } else if (reverse && position >= loopStart && position < loopStart + crossfade) {
                phase = (loopStart + crossfade - position) / crossfade;
                alternatePosition = loopEnd - (loopStart + crossfade - position);
                alternatePosition = std::min(alternatePosition, loopEnd - 1.0e-4F);
            }
            if (phase >= 0.0F) {
                const float alternate = sampler_cubic_sample(bank, alternatePosition);
                const float primaryGain = fast_sin_phase(0.25F - phase * 0.25F);
                const float alternateGain = fast_sin_phase(phase * 0.25F);
                output = output * primaryGain + alternate * alternateGain;
            }
        }

        position += reverse ? -step : step;
        if (loopValid) {
            if (!reverse && position >= loopEnd) {
                position = loopStart + std::fmod(position - loopEnd, loopLength);
            } else if (reverse && position < loopStart) {
                position = std::min(loopEnd - std::fmod(loopStart - position, loopLength),
                                    loopEnd - 1.0e-4F);
            }
        } else if ((!reverse && position >= frameCount) || (reverse && position < 0.0F)) {
            v.samplerFinished[oscillatorIndex] = true;
            v.release();
            position = reverse ? 0.0F : frameCount - 1.0F;
        }

        const float mono = output * sampler.gain * 0.70710678F;
        return {mono, mono};
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
        if (!v.amp.active()) {
            v.kill();
            profilerVoicesRetired.fetch_add(1U, std::memory_order_relaxed);
            return {};
        }
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
            const float smoothedGain = smoothedOscGain[i].current;
            if (!osc.enabled || smoothedGain <= 0.0F) continue;

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
            // Perf: split semitones into a cacheable base (constant while pitch
            // bend / modulation are idle) and the tiny slow-drift term.
            const float baseSemitones = parameters.tuning.transposeSemitones +
                         parameters.tuning.fineCents * 0.01F +
                         parameters.tuning.analogDriftCents * staticDrift * 0.01F + osc.semitones +
                         osc.cents * 0.01F + bendSemitones + legacyModulation + mod.globalPitch + mod.pitch[i];
            const float driftSemitones = parameters.tuning.analogDriftCents * slowDrift * 0.01F;
            const float additionalSemitones = baseSemitones + driftSemitones;
            float note = static_cast<float>(v.note) + additionalSemitones;
            const float fmSource = source_sample(osc.frequencyModSource);
            if (osc.frequencyModMode == FrequencyModulationMode::Exponential)
                note += fmSource * clampf(osc.frequencyModAmount, -4.0F, 4.0F) * 24.0F;
            float increment;
            const bool useFreqCache = !parameters.microtuning.enabled &&
                                      osc.frequencyModMode != FrequencyModulationMode::Exponential;
            if (useFreqCache) {
                if (v.freqCacheNote != v.note ||
                    v.freqCacheRefHertz != parameters.tuning.referenceHertz ||
                    v.freqCacheSemitones[i] != baseSemitones) {
                    // Cache the clamped increment: saves a division per sample.
                    const float clamped = clampf(tuned_frequency(v.note, baseSemitones),
                                                 0.1F, sampleRate * 0.45F);
                    v.freqCache[i] = clamped / sampleRate;
                    v.freqCacheSemitones[i] = baseSemitones;
                    v.freqCacheNote = v.note;
                    v.freqCacheRefHertz = parameters.tuning.referenceHertz;
                }
                increment = v.freqCache[i];
                // exp2(d/12) ~= 1 + y + y^2/2 with y = d*ln2/12; |d| <= 0.0035
                // semitones here, so the truncation error is < 1e-12 relative.
                // (x*1.0 is bit-identical, so skipping when y==0 is safe.)
                const float y = driftSemitones * 0.057762265F;
                if (y != 0.0F) increment *= 1.0F + y + y * y * 0.5F;
                if (osc.frequencyModMode == FrequencyModulationMode::Linear) {
                    increment *= 1.0F + fmSource * clampf(osc.frequencyModAmount, -2.0F, 2.0F);
                    // Re-clamp: FM can push a clamped base out of range (rare).
                    increment = clampf(increment, 0.1F / sampleRate, 0.45F);
                }
            } else {
                float frequency0 = tuned_frequency(v.note, note - static_cast<float>(v.note));
                if (osc.frequencyModMode == FrequencyModulationMode::Linear)
                    frequency0 += fmSource * frequency0 * clampf(osc.frequencyModAmount, -2.0F, 2.0F);
                frequency0 = clampf(frequency0, 0.1F, sampleRate * 0.45F);
                increment = frequency0 / sampleRate;
            }
            // Reconstruct for the Sample/Sampler/Granular/Spectral/Physical
            // branches below (one multiply; the common analog waveforms use
            // `increment` directly).
            const float frequency = increment * sampleRate;

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
                        ? wavetable_sample(phase, clampf(osc.wavetablePosition + shape + mod.wavetablePosition, 0.0F, 1.0F), subIncrement)
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
            } else if (osc.waveform == OscillatorWaveform::Sampler) {
                // Phase 2: dedicated sampler generator (preset-level parameters).
                const auto samplerOut = render_sampler(v, i, frequency, mod);
                stereoLeft = samplerOut.first;
                stereoRight = samplerOut.second;
                sample = (stereoLeft + stereoRight) * 0.70710678F;
            } else if (osc.waveform == OscillatorWaveform::Granular) {
                // Phase 4: dedicated granular generator (SYN-014). Preset-level
                // parameters; the grain source is the resident sample bank.
                // An empty/disabled bank renders silence (counted as grain
                // misses inside the engine, never a crash). Each oscillator
                // owns its grain pool, and grain pitch tracks the voice's
                // played frequency (bend/tuning/semitones already folded in)
                // relative to the bank's recorded root note.
                auto& engine = v.granularEngines[i];
                engine.set_sample_rate(sampleRate);
                const RealtimeSampleBank& bank = parameters.sampleBank;
                const GranularSource source{bank.samples.data(),
                                            bank.enabled ? bank.frameCount : 0U,
                                            bank.sampleRate,
                                            bank.rootNote};
                const auto granularOut =
                    engine.render(source, parameters.granular, mod.granularPosition, frequency);
                forward_granular_counters(engine);
                stereoLeft = granularOut.first;
                stereoRight = granularOut.second;
                sample = (stereoLeft + stereoRight) * 0.70710678F;
            } else if (osc.waveform == OscillatorWaveform::Spectral) {
                // Phase 5: spectral resynthesis oscillator (SYN-015).
                // Preset-level parameters; the asset is a non-owning view of
                // the cooked spectral asset (Worker C/D own its lifetime). A
                // null/missing asset renders silence (counted inside the
                // engine, never a crash).
                auto& spec = v.spectralOscillator;
                spec.set_sample_rate(sampleRate);
                const SpectralAssetView asset =
                    parameters.spectralAsset != nullptr ? *parameters.spectralAsset
                                                        : SpectralAssetView{};
                const auto spectralOut = spec.render(asset, parameters.spectral, frequency);
                stereoLeft = spectralOut.first;
                stereoRight = spectralOut.second;
                sample = (stereoLeft + stereoRight) * 0.70710678F;
            } else if (osc.waveform == OscillatorWaveform::PhysicalModel) {
                sample = physical::process(v.physicalModels[i], osc, frequency, v.velocity,
                                           v.pressure, v.timbre, sampleRate);
                const float width = 0.18F + 0.22F * std::abs(osc.physicalPickupPosition - 0.5F);
                stereoLeft = sample * (0.70710678F + width * 0.25F);
                stereoRight = sample * (0.70710678F - width * 0.25F);
            } else if (osc.waveform == OscillatorWaveform::ModalResonator) {
                // Phase 2: modal resonator bank (even modes left, odd modes right).
                const auto resonatorStereo = modal_resonator::process(
                    v.modalResonators[i], osc.modalResonator, frequency, v.velocity, v.keyHeld,
                    parameters.sampleBank, sampleRate);
                stereoLeft = resonatorStereo.first * 0.70710678F;
                stereoRight = resonatorStereo.second * 0.70710678F;
                sample = (resonatorStereo.first + resonatorStereo.second) * 0.5F;
            } else {
                const float divergence = clampf(osc.stereoDivergence, 0.0F, 1.0F);
                if (divergence > 0.001F) {
                    // Phase 1: true stereo divergence — render L/R with slight
                    // detune and phase offset for width without chorus.
                    // Perf: the detune ratios depend only on the (preset-level)
                    // divergence, so cache them per voice/osc and recompute
                    // only on change; per sample this is then 2 multiplies.
                    if (v.divergenceCached[i] != divergence) {
                        v.divergenceCached[i] = divergence;
                        const float detuneCents = divergence * 8.0F; // up to 8 cents
                        v.divergenceRatioL[i] = std::exp2(detuneCents / 1200.0F);
                        v.divergenceRatioR[i] = std::exp2(-detuneCents / 1200.0F);
                        v.divergencePhaseOffset[i] = divergence * 0.02F; // up to 2% phase
                    }
                    const float incL = increment * v.divergenceRatioL[i];
                    const float incR = increment * v.divergenceRatioR[i];
                    float phaseL = v.phases[i];
                    float phaseR = wrap_phase(v.phases[i] + v.divergencePhaseOffset[i]);
                    auto auxL = v.auxiliaryPhases[i];
                    auto auxR = v.auxiliaryPhases[i];
                    std::uint32_t noiseL = v.noiseState[i];
                    std::uint32_t noiseR = v.noiseState[i] ^ 0x9E3779B9U;
                    const float sampleL = renderPhase(phaseL, incL, true, auxL, noiseL);
                    const float sampleR = renderPhase(phaseR, incR, true, auxR, noiseR);
                    // Advance the main phase by the average.
                    v.phases[i] = wrap_phase(v.phases[i] + increment);
                    v.auxiliaryPhases[i] = auxL;
                    v.noiseState[i] = noiseL;
                    sample = (sampleL + sampleR) * 0.5F;
                    stereoLeft = sampleL * 0.70710678F;
                    stereoRight = sampleR * 0.70710678F;
                } else {
                    sample = renderPhase(v.phases[i], increment, true, v.auxiliaryPhases[i], v.noiseState[i]);
                    stereoLeft = sample * 0.70710678F;
                    stereoRight = sample * 0.70710678F;
                }
            }
            std::uint8_t unisonVoices = parameters.unison.enabled
                ? std::clamp<std::uint8_t>(parameters.unison.voices, 1U, static_cast<std::uint8_t>(kSynthUnisonMax)) : 1U;
            if (osc.waveform == OscillatorWaveform::Noise || osc.waveform == OscillatorWaveform::SuperSaw ||
                osc.waveform == OscillatorWaveform::Sample || osc.waveform == OscillatorWaveform::Granular ||
                osc.waveform == OscillatorWaveform::PhysicalModel || osc.waveform == OscillatorWaveform::Sampler ||
                osc.waveform == OscillatorWaveform::ModalResonator || osc.waveform == OscillatorWaveform::Spectral)
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
                osc.waveform != OscillatorWaveform::PhysicalModel &&
                osc.waveform != OscillatorWaveform::Sampler &&
                osc.waveform != OscillatorWaveform::ModalResonator &&
                osc.waveform != OscillatorWaveform::Spectral) {
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
            const float oscillatorGain = smoothedOscGain[i].current * gainMod;
            sample *= oscillatorGain; stereoLeft *= oscillatorGain; stereoRight *= oscillatorGain;
            currentSamples[i] = clampf(sample, -8.0F, 8.0F);
            const float basePan = clampf(osc.pan + mod.voicePan, -1.0F, 1.0F);
            const float panLeft = std::sqrt(0.5F * (1.0F - basePan));
            const float panRight = std::sqrt(0.5F * (1.0F + basePan));
            const bool intrinsicStereo = osc.waveform == OscillatorWaveform::Sample ||
                                         osc.waveform == OscillatorWaveform::Granular ||
                                         osc.waveform == OscillatorWaveform::PhysicalModel ||
                                         osc.waveform == OscillatorWaveform::Sampler ||
                                         osc.waveform == OscillatorWaveform::ModalResonator ||
                                         osc.waveform == OscillatorWaveform::Spectral;
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

        if (parameters.harmonizer.enabled) {
            const float subLevel = clampf(parameters.harmonizer.subLevel, 0.0F, 1.0F);
            const float upLevel = clampf(parameters.harmonizer.upLevel, 0.0F, 1.0F);
            const float harmMix = clampf(parameters.harmonizer.mix, 0.0F, 1.0F);
            if ((subLevel > 0.0F || upLevel > 0.0F) && harmMix > 0.0F) {
                const float harmSemitones = parameters.tuning.transposeSemitones +
                    parameters.tuning.fineCents * 0.01F + bendSemitones + legacyModulation + mod.globalPitch;
                const float baseFreq = tuned_frequency(v.note, harmSemitones);
                v.harmonizerPhases[0] = wrap_phase(v.harmonizerPhases[0] + (baseFreq * 0.5F) / sampleRate);
                v.harmonizerPhases[1] = wrap_phase(v.harmonizerPhases[1] + (baseFreq * 2.0F) / sampleRate);
                const float harmSample = (fast_sin_phase(v.harmonizerPhases[0]) * subLevel +
                                          fast_sin_phase(v.harmonizerPhases[1]) * upLevel) *
                                         harmMix * 0.5F;
                left += harmSample;
                right += harmSample;
            }
        }

        const float pressureGain = 0.85F + 0.15F * std::max(v.pressure, channelPressure[v.channel]);
        const float gain = amp * v.velocity * pressureGain * clampf(1.0F + mod.voiceGain, 0.0F, 3.0F);
        left *= gain;
        right *= gain;
        if (parameters.filter.enabled) {
            const float timbreValue = mpe_master(v.channel) ? v.timbre : controller[74];
            const float cutoffCc = timbreValue > 0.0F ? std::exp2((timbreValue - 0.5F) * 8.0F) : 1.0F;
            const float keyTrackOctaves = (static_cast<float>(v.note) - 60.0F) / 12.0F * parameters.filter.keyTrack;
            // Phase 3: the generative conductor scales the base cutoff with
            // brightness (1.0 when the conductor is disabled: no-op).
            const float cutoff = std::exp2(smoothedFilterCutoffLog.current) * cutoffCc *
                                 conductorCutoffMultiplier *
                                 std::exp2(parameters.filter.envelopeAmountOctaves * filterEnv +
                                           keyTrackOctaves + mod.filterCutoff);
            const float resonance = clampf(smoothedFilterResonance.current + controller[71] * 0.5F +
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
            const float mix = clampf(parameters.distortion.mix, 0.0F, 1.0F);
            const float smoothedDrive = smoothedDistortionDrive.current;
            if (parameters.distortion.mode == DistortionMode::Fuzz) {
                const float gain = smoothedDrive * 4.0F;
                const float clippedL = clampf(left * gain, -0.85F, 1.0F);
                const float clippedR = clampf(right * gain, -0.85F, 1.0F);
                const float tone = 1.0F - std::exp(-kTwoPi * 6500.0F / sampleRate);
                fuzzToneL += tone * (clippedL - fuzzToneL);
                fuzzToneR += tone * (clippedR - fuzzToneR);
                left += (fuzzToneL * 0.9F - left) * mix;
                right += (fuzzToneR * 0.9F - right) * mix;
            } else if (parameters.distortion.mode == DistortionMode::SoftClip) {
                const float wetL = soft_clip(left * smoothedDrive);
                const float wetR = soft_clip(right * smoothedDrive);
                left += (wetL - left) * mix; right += (wetR - right) * mix;
            } else if (parameters.distortion.mode == DistortionMode::Foldback) {
                const float wetL = wavefold(left * smoothedDrive);
                const float wetR = wavefold(right * smoothedDrive);
                left += (wetL - left) * mix; right += (wetR - right) * mix;
            } else {
                const float wetL = fast_tanh(left * smoothedDrive);
                const float wetR = fast_tanh(right * smoothedDrive);
                left += (wetL - left) * mix; right += (wetR - right) * mix;
            }
        }
        if (parameters.bitcrusher.enabled) {
            const unsigned bits = std::min(16U, std::max(1U, static_cast<unsigned>(parameters.bitcrusher.bits)));
            const unsigned downsample = std::min(64U, std::max(1U, static_cast<unsigned>(parameters.bitcrusher.downsample)));
            const float steps = static_cast<float>(1U << (bits - 1U));
            if (++crusherCount >= downsample) {
                crusherCount = 0;
                crusherHoldL = std::floor(clampf(left, -1.0F, 1.0F) * steps + 0.5F) / steps;
                crusherHoldR = std::floor(clampf(right, -1.0F, 1.0F) * steps + 0.5F) / steps;
            }
            const float mix = clampf(parameters.bitcrusher.mix, 0.0F, 1.0F);
            left += (crusherHoldL - left) * mix; right += (crusherHoldR - right) * mix;
        }
        if (parameters.eq.enabled) {
            const float lowCoefficient = 1.0F - std::exp(-kTwoPi * 220.0F / sampleRate);
            const float highCoefficient = 1.0F - std::exp(-kTwoPi * 4200.0F / sampleRate);
            eqLowL += lowCoefficient * (left - eqLowL); eqLowR += lowCoefficient * (right - eqLowR);
            eqHighL += highCoefficient * (left - eqHighL); eqHighR += highCoefficient * (right - eqHighR);
            const float lowL = eqLowL; const float lowR = eqLowR;
            const float highL = left - eqHighL; const float highR = right - eqHighR;
            const float midL = left - lowL - highL; const float midR = right - lowR - highR;
            left = lowL * db_to_gain(smoothedEqLowDb.current) + midL * db_to_gain(smoothedEqMidDb.current) +
                   highL * db_to_gain(smoothedEqHighDb.current);
            right = lowR * db_to_gain(smoothedEqLowDb.current) + midR * db_to_gain(smoothedEqMidDb.current) +
                    highR * db_to_gain(smoothedEqHighDb.current);
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
        if (parameters.flanger.enabled) {
            flangerPhase = wrap_phase(flangerPhase + parameters.flanger.rateHertz / sampleRate);
            const float base = 1.0F * sampleRate / 1000.0F;
            const float depth = clampf(parameters.flanger.depthMilliseconds, 0.0F, 10.0F) * sampleRate / 1000.0F;
            const float delayL_ = base + depth * (0.5F + 0.5F * fast_sin_phase(flangerPhase));
            const float delayR_ = base + depth * (0.5F + 0.5F * fast_sin_phase(flangerPhase + 0.5F));
            const float feedback = clampf(smoothedFlangerFeedback.current, -0.92F, 0.92F);
            const float inL = left + flangerFeedbackL * feedback;
            const float inR = right + flangerFeedbackR * feedback;
            const float wetL = flangerL.read_fractional(delayL_);
            const float wetR = flangerR.read_fractional(delayR_);
            flangerFeedbackL = wetL; flangerFeedbackR = wetR;
            flangerL.push(inL); flangerR.push(inR);
            const float mix = clampf(parameters.flanger.mix, 0.0F, 1.0F);
            left += (wetL - left) * mix; right += (wetR - right) * mix;
        } else { flangerL.push(left); flangerR.push(right); flangerFeedbackL = 0.0F; flangerFeedbackR = 0.0F; }
        if (parameters.ensemble.enabled) {
            const bool both = parameters.ensemble.mode == EnsembleMode::Both;
            const bool modeII = parameters.ensemble.mode == EnsembleMode::II;
            const float rate = modeII ? 0.75F : 0.45F;
            ensemblePhase = wrap_phase(ensemblePhase + rate / sampleRate);
            const float base = 6.0F * sampleRate / 1000.0F;
            const float depth = (modeII ? 2.8F : 1.8F) * sampleRate / 1000.0F;
            const float depthII = 2.8F * sampleRate / 1000.0F;
            float wetL = 0.0F, wetR = 0.0F;
            for (int tap = 0; tap < 3; ++tap) {
                const float offset = static_cast<float>(tap) / 3.0F;
                const float mod = 0.5F + 0.5F * fast_sin_phase(ensemblePhase + offset);
                wetL += ensembleBufL.read_fractional(base + depth * mod);
                wetR += ensembleBufR.read_fractional(base + depth * mod);
                if (both) {
                    const float mod2 = 0.5F + 0.5F * fast_sin_phase(ensemblePhase * 1.65F + offset + 0.13F);
                    wetL += ensembleBufL.read_fractional(base + depthII * mod2);
                    wetR += ensembleBufR.read_fractional(base + depthII * mod2);
                }
            }
            const float taps = both ? 6.0F : 3.0F;
            wetL /= taps; wetR /= taps;
            ensembleBufL.push(left); ensembleBufR.push(right);
            const float mix = clampf(parameters.ensemble.mix, 0.0F, 1.0F);
            left += (wetL - left) * mix; right += (wetR - right) * mix;
        } else { ensembleBufL.push(left); ensembleBufR.push(right); }
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
            const float delaySamples = clampf(smoothedDelayTime.current, 0.01F, 1.95F) * sampleRate;
            const float delayedL = delayL.read_fractional(delaySamples);
            const float delayedR = delayR.read_fractional(delaySamples);
            const float feedback = clampf(smoothedDelayFeedback.current, 0.0F, 0.94F);
            delayL.push(left + (parameters.delay.pingPong ? delayedR : delayedL) * feedback);
            delayR.push(right + (parameters.delay.pingPong ? delayedL : delayedR) * feedback);
            const float mix = clampf(parameters.delay.mix, 0.0F, 1.0F);
            left += (delayedL - left) * mix; right += (delayedR - right) * mix;
        } else { delayL.push(left); delayR.push(right); }
        // Phase 2: diffusion delay. A recirculating delay whose wet path runs
        // through cascaded allpass stages; the recirculated signal is already
        // diffused, so repeats smear into a reverb-ish wash instead of staying
        // distinct. Allpass stages are unity-magnitude (|g| < 1), so the loop
        // is stable for feedback < 1.
        if (parameters.diffusionDelay.enabled) {
            const float delaySamples = clampf(smoothedDiffDelayTime.current, 0.01F, 1.95F) * sampleRate;
            const float apCoeff = clampf(parameters.diffusionDelay.diffusion, 0.0F, 1.0F) * 0.7F;
            float wetL = diffDelayL.read_fractional(delaySamples);
            float wetR = diffDelayR.read_fractional(delaySamples);
            for (auto& stage : diffApL) wetL = allpass_diffuse(stage, wetL, apCoeff);
            for (auto& stage : diffApR) wetR = allpass_diffuse(stage, wetR, apCoeff);
            const float feedback = clampf(smoothedDiffDelayFeedback.current, 0.0F, 0.94F);
            diffDelayL.push(left + wetL * feedback);
            diffDelayR.push(right + wetR * feedback);
            const float mix = clampf(parameters.diffusionDelay.mix, 0.0F, 1.0F);
            left += (wetL - left) * mix; right += (wetR - right) * mix;
        } else { diffDelayL.push(left); diffDelayR.push(right); }
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
            const float smoothedThreshold = smoothedCompThresholdDb.current;
            if (envelopeDb > smoothedThreshold)
                reductionDb = (smoothedThreshold +
                               (envelopeDb - smoothedThreshold) / std::max(1.0F, parameters.compressor.ratio)) - envelopeDb;
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
        profilerActiveVoices.store(count, std::memory_order_relaxed);
        std::uint32_t peak = profilerMaximumActiveVoices.load(std::memory_order_relaxed);
        while (count > peak &&
               !profilerMaximumActiveVoices.compare_exchange_weak(peak, count, std::memory_order_relaxed)) {}
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
    // An enabled bank with zero frames carries no sample data (e.g. after a
    // binary patch round-trip, which stores the enabled flag but not the
    // audio); every render path already treats frameCount < 2 as silence, so
    // it validates. A single frame is degenerate (nothing to interpolate
    // from) and is still rejected.
    if (enabled && frameCount == 1U) return fail("enabled sample bank requires at least two frames");
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
    // Phase 3: sequencer steps default to the musical lane defaults so an
    // old preset string (no seq.* keys) loads with a sensible program.
    for (std::size_t li = 0; li < kSequencerLaneCount; ++li) {
        const SequencerLane lane = static_cast<SequencerLane>(li);
        for (auto& step : result.sequencer.lanes[li].steps) step = default_sequencer_step(lane);
    }
    return result;
}

namespace {
// Clean starting point for factory presets: every voice element silenced, pitch
// reference exact, limiter on. Each preset below voices the played MIDI note as
// the perceived fundamental.
SynthPreset base_preset(const char* name) {
    SynthPreset preset;
    preset.name = name;
    for (auto& osc : preset.oscillators) osc.enabled = false;
    preset.ampEnvelope = {0.005F, 0.10F, 0.80F, 0.20F, EnvelopeCurve::Exponential};
    preset.filter.enabled = false;
    preset.tuning.analogDriftCents = 0.0F;
    preset.distortion.enabled = false;
    preset.eq.enabled = false;
    preset.chorus.enabled = false;
    preset.phaser.enabled = false;
    preset.delay.enabled = false;
    preset.reverb.enabled = false;
    preset.compressor.enabled = false;
    preset.limiter.enabled = true;
    preset.masterGain = 0.70F;
    preset.masterPan = 0.0F;
    return preset;
}

void enable_osc(SynthPreset& preset, std::size_t index, OscillatorWaveform wave,
                float semitones, float cents, float gain) {
    auto& osc = preset.oscillators[index];
    osc.enabled = true;
    osc.waveform = wave;
    osc.semitones = semitones;
    osc.cents = cents;
    osc.gain = gain;
}

void lowpass(SynthPreset& preset, float cutoffHertz, float resonance, float envOctaves,
             float envAttack, float envDecay, float envSustain, float envRelease) {
    preset.filter.enabled = true;
    preset.filter.topology = FilterTopology::MoogLadder;
    preset.filter.mode = FilterMode::LowPass;
    preset.filter.cutoffHertz = cutoffHertz;
    preset.filter.resonance = resonance;
    preset.filter.envelopeAmountOctaves = envOctaves;
    preset.filter.envelope = {envAttack, envDecay, envSustain, envRelease, EnvelopeCurve::Exponential};
}

SynthPreset make_clean_saw_lead() {
    auto preset = base_preset("Clean Saw Lead");
    enable_osc(preset, 0, OscillatorWaveform::Saw, 0.0F, 0.0F, 0.50F);
    preset.ampEnvelope = {0.005F, 0.10F, 0.90F, 0.15F, EnvelopeCurve::Exponential};
    lowpass(preset, 8000.0F, 0.10F, 0.5F, 0.01F, 0.20F, 0.50F, 0.20F);
    preset.chorus.enabled = true;
    preset.chorus.rateHertz = 0.35F;
    preset.chorus.depthMilliseconds = 3.5F;
    preset.chorus.mix = 0.18F;
    return preset;
}

SynthPreset make_deep_sub_bass() {
    auto preset = base_preset("Deep Sub Bass");
    enable_osc(preset, 0, OscillatorWaveform::Sine, 0.0F, 0.0F, 0.55F);
    enable_osc(preset, 1, OscillatorWaveform::Triangle, -12.0F, 0.0F, 0.22F);
    preset.ampEnvelope = {0.008F, 0.05F, 1.00F, 0.12F, EnvelopeCurve::Exponential};
    lowpass(preset, 1200.0F, 0.10F, 0.0F, 0.01F, 0.20F, 0.50F, 0.20F);
    return preset;
}

SynthPreset make_pluck() {
    auto preset = base_preset("Pluck");
    enable_osc(preset, 0, OscillatorWaveform::Saw, 0.0F, 0.0F, 0.45F);
    preset.ampEnvelope = {0.002F, 0.30F, 0.05F, 0.12F, EnvelopeCurve::Exponential};
    lowpass(preset, 900.0F, 0.25F, 5.0F, 0.002F, 0.28F, 0.0F, 0.10F);
    return preset;
}

SynthPreset make_warm_pad() {
    auto preset = base_preset("Warm Pad");
    enable_osc(preset, 0, OscillatorWaveform::Saw, 0.0F, -6.0F, 0.28F);
    enable_osc(preset, 1, OscillatorWaveform::Saw, 0.0F, 6.0F, 0.28F);
    preset.ampEnvelope = {0.90F, 0.50F, 0.85F, 1.20F, EnvelopeCurve::Exponential};
    lowpass(preset, 2200.0F, 0.10F, 0.5F, 0.40F, 0.60F, 0.60F, 0.80F);
    preset.chorus.enabled = true;
    preset.chorus.mix = 0.22F;
    preset.reverb.enabled = true;
    preset.reverb.roomSize = 0.70F;
    preset.reverb.mix = 0.25F;
    return preset;
}

SynthPreset make_chiptune_square() {
    auto preset = base_preset("Chiptune Square");
    enable_osc(preset, 0, OscillatorWaveform::Pulse, 0.0F, 0.0F, 0.40F);
    auto& osc = preset.oscillators[0];
    osc.pulseWidth = 0.50F;
    osc.pwmDepth = 0.25F;
    osc.pwmRateHertz = 0.35F;
    preset.ampEnvelope = {0.003F, 0.08F, 0.70F, 0.08F, EnvelopeCurve::Exponential};
    lowpass(preset, 12000.0F, 0.05F, 0.0F, 0.01F, 0.20F, 0.50F, 0.20F);
    return preset;
}

SynthPreset make_organ() {
    auto preset = base_preset("Organ");
    enable_osc(preset, 0, OscillatorWaveform::Sine, 0.0F, 0.0F, 0.38F);
    enable_osc(preset, 1, OscillatorWaveform::Sine, 12.0F, 0.0F, 0.22F);
    enable_osc(preset, 2, OscillatorWaveform::Sine, 19.0F, 0.0F, 0.12F);
    enable_osc(preset, 3, OscillatorWaveform::Sine, 24.0F, 0.0F, 0.16F);
    preset.ampEnvelope = {0.010F, 0.05F, 1.00F, 0.08F, EnvelopeCurve::Exponential};
    return preset;
}

SynthPreset make_brass_stab() {
    auto preset = base_preset("Brass Stab");
    enable_osc(preset, 0, OscillatorWaveform::Saw, 0.0F, 0.0F, 0.32F);
    enable_osc(preset, 1, OscillatorWaveform::Saw, 0.0F, 4.0F, 0.32F);
    preset.ampEnvelope = {0.060F, 0.15F, 0.75F, 0.20F, EnvelopeCurve::Exponential};
    lowpass(preset, 1500.0F, 0.15F, 3.0F, 0.04F, 0.20F, 0.60F, 0.25F);
    return preset;
}

SynthPreset make_glass_bell() {
    auto preset = base_preset("Glass Bell");
    enable_osc(preset, 0, OscillatorWaveform::Sine, 0.0F, 0.0F, 0.50F);
    enable_osc(preset, 1, OscillatorWaveform::Sine, 12.0F, 0.0F, 0.18F);
    enable_osc(preset, 2, OscillatorWaveform::Sine, 17.54F, 0.0F, 0.12F); // 2.76x inharmonic partial
    enable_osc(preset, 3, OscillatorWaveform::Sine, 24.0F, 0.0F, 0.08F);
    preset.ampEnvelope = {0.002F, 1.80F, 0.00F, 2.50F, EnvelopeCurve::Exponential};
    preset.reverb.enabled = true;
    preset.reverb.roomSize = 0.80F;
    preset.reverb.mix = 0.30F;
    return preset;
}

SynthPreset make_reese_bass() {
    auto preset = base_preset("Reese Bass");
    enable_osc(preset, 0, OscillatorWaveform::Saw, 0.0F, -8.0F, 0.34F);
    enable_osc(preset, 1, OscillatorWaveform::Saw, 0.0F, 8.0F, 0.34F);
    enable_osc(preset, 2, OscillatorWaveform::Sine, -12.0F, 0.0F, 0.12F);
    preset.ampEnvelope = {0.010F, 0.10F, 0.90F, 0.15F, EnvelopeCurve::Exponential};
    lowpass(preset, 850.0F, 0.20F, 1.0F, 0.02F, 0.25F, 0.60F, 0.25F);
    return preset;
}

SynthPreset make_e_keys() {
    auto preset = base_preset("E-Keys");
    enable_osc(preset, 0, OscillatorWaveform::Triangle, 0.0F, 0.0F, 0.40F);
    enable_osc(preset, 1, OscillatorWaveform::Sine, 12.0F, 0.0F, 0.18F);
    preset.ampEnvelope = {0.004F, 0.50F, 0.35F, 0.40F, EnvelopeCurve::Exponential};
    preset.chorus.enabled = true;
    preset.chorus.mix = 0.20F;
    return preset;
}

SynthPreset make_noise_sweep_fx() {
    auto preset = base_preset("Noise Sweep FX");
    enable_osc(preset, 0, OscillatorWaveform::Noise, 0.0F, 0.0F, 0.50F);
    preset.ampEnvelope = {0.050F, 0.50F, 0.00F, 0.40F, EnvelopeCurve::Exponential};
    lowpass(preset, 500.0F, 0.30F, 6.0F, 0.40F, 0.60F, 0.00F, 0.30F);
    preset.delay.enabled = true;
    return preset;
}

// Phase 2: sampler showcase. The factory preset bakes a small seamlessly
// loopable vocal-ish tone into the preset sample bank (12000 frames = exactly
// 110 cycles at 440 Hz, so the loop seam is click-free) and plays it back
// through the Sampler generator with pitch tracking. If the bank were ever
// empty the sampler would simply stay silent, but the baked sample keeps the
// factory preset self-contained and audible.
SynthPreset make_sampled_loop_vox() {
    auto preset = base_preset("Sampled Loop Vox");
    enable_osc(preset, 0, OscillatorWaveform::Sampler, 0.0F, 0.0F, 0.90F);
    constexpr std::uint32_t kFrames = 12000U;  // 110 cycles at 440 Hz / 48 kHz: seamless loop
    constexpr float kRate = 48000.0F;
    constexpr float kFrequency = 440.0F;
    constexpr float kHarmonics[8] = {1.0F, 0.55F, 0.38F, 0.26F, 0.18F, 0.12F, 0.08F, 0.05F};
    float peak = 0.0F;
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        float sample = 0.0F;
        const float phase = 2.0F * 3.14159265358979F * kFrequency * static_cast<float>(i) / kRate;
        for (int harmonic = 0; harmonic < 8; ++harmonic)
            sample += kHarmonics[harmonic] * std::sin(phase * static_cast<float>(harmonic + 1));
        peak = std::max(peak, std::abs(sample));
        preset.sampleBank.samples[i] = sample;
    }
    const float normalize = peak > 0.0F ? 0.75F / peak : 1.0F;
    for (std::uint32_t i = 0; i < kFrames; ++i) preset.sampleBank.samples[i] *= normalize;
    preset.sampleBank.name = "Loop Vox Ah";
    preset.sampleBank.enabled = true;
    preset.sampleBank.sampleRate = 48000U;
    preset.sampleBank.rootNote = 69;  // recorded at A440: MIDI 69 plays at concert pitch
    preset.sampleBank.frameCount = kFrames;
    preset.sampler.enabled = true;
    preset.sampler.playbackMode = SamplerPlaybackMode::Loop;
    preset.sampler.direction = SamplerDirection::Forward;
    preset.sampler.loopStartSeconds = 0.0F;
    preset.sampler.loopEndSeconds = static_cast<float>(kFrames) / kRate;
    preset.sampler.loopCrossfadeSeconds = 0.004F;
    preset.sampler.pitchTracking = true;
    preset.sampler.gain = 0.8F;
    preset.ampEnvelope = {0.008F, 0.30F, 0.70F, 0.35F, EnvelopeCurve::Exponential};
    preset.chorus.enabled = true;
    preset.chorus.mix = 0.15F;
    return preset;
}

// Phase 2: modal resonator showcase. A bank of damped modes struck by an
// impulse at note-on, following the played note (baseFrequency 0): a mallet
// with gently inharmonic upper partials.
SynthPreset make_modal_marimba() {
    auto preset = base_preset("Modal Marimba");
    enable_osc(preset, 0, OscillatorWaveform::ModalResonator, 0.0F, 0.0F, 0.85F);
    auto& mr = preset.oscillators[0].modalResonator;
    mr.excitation = ExcitationSource::Impulse;
    mr.modeCount = 8;
    mr.baseFrequency = 0.0F;  // follow the played note
    mr.damping = 1.1F;
    mr.inharmonicity = 0.03F;
    mr.brightness = 0.55F;
    mr.excitationLevel = 1.0F;
    constexpr float kRatios[8] = {1.0F, 2.01F, 2.98F, 4.16F, 5.43F, 6.79F, 8.21F, 9.65F};
    constexpr float kGains[8] = {1.0F, 0.55F, 0.38F, 0.24F, 0.15F, 0.10F, 0.06F, 0.04F};
    constexpr float kDecays[8] = {2.2F, 1.6F, 1.2F, 0.9F, 0.7F, 0.5F, 0.4F, 0.3F};
    for (int i = 0; i < 8; ++i) mr.modes[i] = ModalResonatorMode{kRatios[i], kDecays[i], kGains[i]};
    preset.ampEnvelope = {0.002F, 1.60F, 0.00F, 1.80F, EnvelopeCurve::Exponential};
    preset.reverb.enabled = true;
    preset.reverb.roomSize = 0.55F;
    preset.reverb.mix = 0.22F;
    return preset;
}

// Phase 2: diffusion delay showcase. A clean saw lead whose repeats smear
// through the diffusion allpass stages into a reverb-ish wash.
SynthPreset make_cloud_delay() {
    auto preset = base_preset("Cloud Delay");
    enable_osc(preset, 0, OscillatorWaveform::Saw, 0.0F, 0.0F, 0.45F);
    preset.ampEnvelope = {0.010F, 0.25F, 0.60F, 0.45F, EnvelopeCurve::Exponential};
    lowpass(preset, 5200.0F, 0.12F, 0.5F, 0.02F, 0.30F, 0.50F, 0.30F);
    preset.diffusionDelay.enabled = true;
    preset.diffusionDelay.timeSeconds = 0.45F;
    preset.diffusionDelay.feedback = 0.55F;
    preset.diffusionDelay.mix = 0.38F;
    preset.diffusionDelay.diffusion = 0.85F;
    preset.reverb.enabled = true;
    preset.reverb.roomSize = 0.70F;
    preset.reverb.mix = 0.15F;
    return preset;
}

// Phase 3 showcase: a warm pad whose generative sequencer (A minor pentatonic,
// uneven lane lengths for phasing polyrhythms) is steered live by the
// attractor conductor over a ~2.7-minute arc. Oscillators stay at unison pitch
// with no sub-oscillator stack so the played note remains the fundamental.
SynthPreset make_generative_attractor_pad() {
    auto preset = base_preset("Generative Attractor Pad");
    enable_osc(preset, 0, OscillatorWaveform::Saw, 0.0F, -6.0F, 0.26F);
    enable_osc(preset, 1, OscillatorWaveform::Saw, 0.0F, 6.0F, 0.26F);
    enable_osc(preset, 2, OscillatorWaveform::Triangle, 12.0F, 0.0F, 0.10F);
    preset.ampEnvelope = {0.90F, 0.50F, 0.85F, 1.20F, EnvelopeCurve::Exponential};
    lowpass(preset, 2200.0F, 0.10F, 0.5F, 0.40F, 0.60F, 0.60F, 0.80F);
    preset.chorus.enabled = true;
    preset.chorus.mix = 0.22F;
    preset.reverb.enabled = true;
    preset.reverb.roomSize = 0.70F;
    preset.reverb.mix = 0.25F;

    // Generative sequencer: uneven lane lengths (7/5/11/8/13/9/3) phase
    // against each other; A minor pentatonic around the played note.
    auto& seq = preset.sequencer;
    seq.enabled = true;
    seq.channel = 0;
    seq.scale = SequencerScale::PentatonicMinor;
    seq.rootNote = 69;  // A4: the preset test holds note 69, so the sequence reinforces it
    seq.octaveRange = 2;
    seq.randomSeed = 0xA771AC70U;
    const std::uint8_t lengths[kSequencerLaneCount] = {7, 5, 11, 8, 13, 9, 3};
    const float mutations[kSequencerLaneCount] = {0.30F, 0.20F, 0.15F, 0.25F, 0.20F, 0.35F, 0.15F};
    for (std::size_t li = 0; li < kSequencerLaneCount; ++li) {
        auto& lane = seq.lanes[li];
        lane.stepCount = lengths[li];
        lane.direction = SequencerDirection::Forward;
        lane.mutationAmount = mutations[li];
        lane.patternCycles = 4;
        // Start from the musical lane defaults so only authored steps
        // appear in the serialized preset text.
        const SequencerLane which = static_cast<SequencerLane>(li);
        for (auto& step : lane.steps) step = default_sequencer_step(which);
    }
    // Pitch: A minor pentatonic climb, capped at +10 semitones so sequence
    // notes stay clear of the octave-above test band.
    const float pitchSteps[7] = {0.0F, 3.0F, 5.0F, 7.0F, 10.0F, 7.0F, 5.0F};
    for (std::size_t i = 0; i < 7; ++i)
        seq.lanes[0].steps[i].value = pitchSteps[i];
    const float velocitySteps[5] = {0.90F, 0.70F, 0.95F, 0.60F, 0.85F};
    for (std::size_t i = 0; i < 5; ++i)
        seq.lanes[1].steps[i].value = velocitySteps[i];
    const float gateSteps[11] = {0.80F, 0.80F, 0.50F, 0.80F, 0.80F, 0.60F,
                                 0.80F, 0.80F, 0.50F, 0.80F, 0.80F};
    for (std::size_t i = 0; i < 11; ++i)
        seq.lanes[2].steps[i].value = gateSteps[i];
    const float timbreSteps[8] = {0.30F, 0.40F, 0.50F, 0.60F, 0.50F, 0.40F, 0.35F, 0.45F};
    for (std::size_t i = 0; i < 8; ++i)
        seq.lanes[3].steps[i].value = timbreSteps[i];
    for (std::size_t i = 0; i < 12; ++i)
        seq.lanes[4].steps[i].value = 1.0F;
    seq.lanes[4].steps[12].value = 0.60F;
    const float morphSteps[9] = {0.20F, 0.30F, 0.40F, 0.50F, 0.60F, 0.50F, 0.40F, 0.30F, 0.25F};
    for (std::size_t i = 0; i < 9; ++i)
        seq.lanes[5].steps[i].value = morphSteps[i];
    const float panSteps[3] = {-0.40F, 0.40F, 0.00F};
    for (std::size_t i = 0; i < 3; ++i)
        seq.lanes[6].steps[i].value = panSteps[i];

    // Genetics: moderate default mutation intensity, nothing locked.
    preset.genetics.mutationIntensity = 0.30F;
    preset.genetics.mutationSeed = 0xC0FFEE42ULL;
    preset.genetics.lockedGroups = 0;

    // Attractor: ~2.7-minute arc at 100 BPM (68 bars of 4/4).
    preset.attractor.enabled = true;
    preset.attractor.config.bpm = 100.0;
    preset.attractor.config.barsHome = 16.0;
    preset.attractor.config.barsRise = 16.0;
    preset.attractor.config.barsTension = 12.0;
    preset.attractor.config.barsPeak = 8.0;
    preset.attractor.config.barsFall = 16.0;
    preset.attractor.config.seed = 0x5EED1234ULL;

    // The conductor's morph wander steers this preset's live evolution.
    preset.morphEnabled = true;
    preset.morphAmount = 0.30F;
    return preset;
}

// Phase 4: granular showcase. Bakes a small seamlessly loopable tone into the
// preset sample bank (the same technique as "Sampled Loop Vox": 12000 frames
// is exactly 110 cycles at 440 Hz, so the loop seam is click-free) and plays
// it through the dedicated granular generator as a drifting cloud: slow
// overlapping Hann grains, wide random pan, a few reversed grains, and a
// lush reverb tail. Because the bank tone sits at concert A440, the preset
// test's pitch check (MIDI 69) still hears the played note as fundamental.
SynthPreset make_granular_cloud_drift() {
    auto preset = base_preset("Granular Cloud Drift");
    enable_osc(preset, 0, OscillatorWaveform::Granular, 0.0F, 0.0F, 0.90F);
    constexpr std::uint32_t kFrames = 12000U;  // 110 cycles at 440 Hz / 48 kHz: seamless loop
    constexpr float kRate = 48000.0F;
    constexpr float kFrequency = 440.0F;
    constexpr float kHarmonics[8] = {1.0F, 0.55F, 0.38F, 0.26F, 0.18F, 0.12F, 0.08F, 0.05F};
    float peak = 0.0F;
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        float sample = 0.0F;
        const float phase = 2.0F * 3.14159265358979F * kFrequency * static_cast<float>(i) / kRate;
        for (int harmonic = 0; harmonic < 8; ++harmonic)
            sample += kHarmonics[harmonic] * std::sin(phase * static_cast<float>(harmonic + 1));
        peak = std::max(peak, std::abs(sample));
        preset.sampleBank.samples[i] = sample;
    }
    const float normalize = peak > 0.0F ? 0.75F / peak : 1.0F;
    for (std::uint32_t i = 0; i < kFrames; ++i) preset.sampleBank.samples[i] *= normalize;
    preset.sampleBank.name = "Cloud Drift Tone";
    preset.sampleBank.enabled = true;
    preset.sampleBank.sampleRate = 48000U;
    preset.sampleBank.rootNote = 69;  // recorded at A440: grains play at concert pitch
    preset.sampleBank.frameCount = kFrames;

    auto& g = preset.granular;
    g.enabled = true;
    g.densityHz = 32.0F;              // overlapping cloud
    g.durationMs = 240.0F;            // long, slowly evolving grains
    g.pitchSemitones = 0.0F;
    g.position01 = 0.40F;
    g.positionJitter01 = 0.25F;        // spray around the read position
    g.panScatter01 = 0.90F;           // wide stereo drift
    g.gain = 0.70F;
    g.reverseProbability01 = 0.08F;    // occasional reversed grains
    g.envelopeShape = GranularEnvelopeShape::Hann;
    g.cloud01 = 0.70F;
    g.scatter01 = 0.20F;
    g.dust01 = 0.05F;
    g.freeze01 = 0.0F;
    g.freezePosition01 = 0.5F;
    g.smear01 = 0.30F;
    g.width01 = 0.90F;

    preset.ampEnvelope = {0.08F, 0.50F, 0.80F, 0.80F, EnvelopeCurve::Exponential};
    preset.reverb.enabled = true;
    preset.reverb.roomSize = 0.65F;
    preset.reverb.mix = 0.28F;
    return preset;
}

// Phase 5: spectral showcase. Bakes a glass-like harmonic tone into the
// preset sample bank (the same seamless-loop technique as "Sampled Loop Vox":
// 12000 frames is exactly 110 cycles at 440 Hz, so the loop seam is
// click-free) and voices it through the Sampler so the preset renders audibly
// today; the spectral block is armed with a musical resynthesis setting for
// that bank tone — gentle downward tilt for a glassy shimmer, a light blur,
// and a touch of inharmonicity. The spectral oscillator engine (worker B)
// will read preset.spectral from RealtimePreset when it lands. Because the
// bank tone sits at concert A440 with a dominant fundamental, the preset
// test's pitch check (MIDI 69) still hears the played note as fundamental.
SynthPreset make_spectral_glass_resynthesis() {
    auto preset = base_preset("Spectral Glass Resynthesis");
    enable_osc(preset, 0, OscillatorWaveform::Sampler, 0.0F, 0.0F, 0.90F);
    constexpr std::uint32_t kFrames = 12000U;  // 110 cycles at 440 Hz / 48 kHz: seamless loop
    constexpr float kRate = 48000.0F;
    constexpr float kFrequency = 440.0F;
    constexpr float kHarmonics[8] = {1.0F, 0.45F, 0.30F, 0.20F, 0.14F, 0.10F, 0.07F, 0.05F};
    float peak = 0.0F;
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        float sample = 0.0F;
        const float phase = 2.0F * 3.14159265358979F * kFrequency * static_cast<float>(i) / kRate;
        for (int harmonic = 0; harmonic < 8; ++harmonic)
            sample += kHarmonics[harmonic] * std::sin(phase * static_cast<float>(harmonic + 1));
        peak = std::max(peak, std::abs(sample));
        preset.sampleBank.samples[i] = sample;
    }
    const float normalize = peak > 0.0F ? 0.75F / peak : 1.0F;
    for (std::uint32_t i = 0; i < kFrames; ++i) preset.sampleBank.samples[i] *= normalize;
    preset.sampleBank.name = "Glass Source A440";
    preset.sampleBank.enabled = true;
    preset.sampleBank.sampleRate = 48000U;
    preset.sampleBank.rootNote = 69;  // recorded at A440: MIDI 69 plays at concert pitch
    preset.sampleBank.frameCount = kFrames;
    preset.sampler.enabled = true;
    preset.sampler.playbackMode = SamplerPlaybackMode::Loop;
    preset.sampler.direction = SamplerDirection::Forward;
    preset.sampler.loopStartSeconds = 0.0F;
    preset.sampler.loopEndSeconds = static_cast<float>(kFrames) / kRate;
    preset.sampler.loopCrossfadeSeconds = 0.004F;
    preset.sampler.pitchTracking = true;
    preset.sampler.gain = 0.8F;

    auto& s = preset.spectral;
    s.enabled = true;
    s.gain = 0.8F;
    s.freeze01 = 0.0F;            // live resynthesis; freeze holds the spectrum
    s.timeStretch = 1.0F;         // natural speed
    s.formantShiftSemitones = 0.0F;
    s.harmonicStretch = 1.0F;
    s.spectralTiltDbPerOct = -3.0F;  // gentle downward tilt: glassy shimmer
    s.partialThreshold01 = 0.15F;    // drop the quietest partials
    s.spectralBlur01 = 0.12F;        // light spectral blur
    s.frequencyQuantize01 = 0.0F;  // no frequency quantization
    s.inharmonicity01 = 0.03F;      // a breath of bell-like stretch
    s.spectralQuality = FilterQuality::Standard;

    preset.ampEnvelope = {0.03F, 0.40F, 0.75F, 0.90F, EnvelopeCurve::Exponential};
    preset.reverb.enabled = true;
    preset.reverb.roomSize = 0.70F;
    preset.reverb.mix = 0.30F;
    return preset;
}
} // namespace

std::vector<SynthPreset> SynthPreset::builtin_presets() {
    return {
        make_clean_saw_lead(),
        make_deep_sub_bass(),
        make_pluck(),
        make_warm_pad(),
        make_chiptune_square(),
        make_organ(),
        make_brass_stab(),
        make_glass_bell(),
        make_reese_bass(),
        make_e_keys(),
        make_noise_sweep_fx(),
        make_sampled_loop_vox(),
        make_modal_marimba(),
        make_cloud_delay(),
        make_generative_attractor_pad(),
        make_granular_cloud_drift(),
        make_spectral_glass_resynthesis(),
    };
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
            !in_range(osc.stereoDivergence, 0.0F, 1.0F) ||
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
            !in_range(osc.physicalVelocityToEmbouchure, 0.0F, 1.0F) ||
            // Phase 2: modal resonator
            osc.modalResonator.modeCount < 1U || osc.modalResonator.modeCount > kModalResonatorMaxModes ||
            !in_range(osc.modalResonator.baseFrequency, 0.0F, 20000.0F) ||
            !in_range(osc.modalResonator.damping, 0.01F, 8.0F) ||
            !in_range(osc.modalResonator.inharmonicity, 0.0F, 1.0F) ||
            !in_range(osc.modalResonator.brightness, 0.0F, 1.0F) ||
            !in_range(osc.modalResonator.excitationLevel, 0.0F, 4.0F) ||
            !in_range(osc.modalResonator.noiseBurstMilliseconds, 1.0F, 2000.0F) ||
            !in_range(osc.modalResonator.transientMilliseconds, 1.0F, 2000.0F))
            return fail("invalid oscillator parameters");
        for (std::size_t m = 0; m < osc.modalResonator.modes.size(); ++m) {
            const auto& resonatorMode = osc.modalResonator.modes[m];
            if (!in_range(resonatorMode.frequencyRatio, 0.01F, 64.0F) ||
                !in_range(resonatorMode.decaySeconds, 0.005F, 60.0F) ||
                !in_range(resonatorMode.gain, 0.0F, 4.0F))
                return fail("invalid oscillator parameters");
        }
    }
    // Phase 4: granular generator parameters (NaN/inf rejected by in_range's
    // finite() check, same convention as every other section).
    {
        const auto& g = granular;
        if (!in_range(g.densityHz, 0.0F, 4000.0F) || !in_range(g.durationMs, 1.0F, 10000.0F) ||
            !in_range(g.pitchSemitones, -96.0F, 96.0F) || !in_range(g.position01, 0.0F, 1.0F) ||
            !in_range(g.positionJitter01, 0.0F, 1.0F) || !in_range(g.panScatter01, 0.0F, 1.0F) ||
            !in_range(g.gain, 0.0F, 4.0F) || !in_range(g.reverseProbability01, 0.0F, 1.0F) ||
            !in_range(g.cloud01, 0.0F, 1.0F) || !in_range(g.scatter01, 0.0F, 1.0F) ||
            !in_range(g.dust01, 0.0F, 1.0F) || !in_range(g.freeze01, 0.0F, 1.0F) ||
            !in_range(g.freezePosition01, 0.0F, 1.0F) || !in_range(g.smear01, 0.0F, 1.0F) ||
            !in_range(g.width01, 0.0F, 1.0F) ||
            (g.envelopeShape != GranularEnvelopeShape::Hann &&
             g.envelopeShape != GranularEnvelopeShape::Triangle &&
             g.envelopeShape != GranularEnvelopeShape::ExponentialDecay &&
             g.envelopeShape != GranularEnvelopeShape::PlanckTaper) ||
            (g.granularQuality != FilterQuality::Eco && g.granularQuality != FilterQuality::Standard &&
             g.granularQuality != FilterQuality::High && g.granularQuality != FilterQuality::Offline))
            return fail("invalid granular parameters");
    }
    if (!in_range(filter.cutoffHertz, 18.0F, 24000.0F) || !in_range(filter.resonance, 0.0F, 1.0F) ||
        !in_range(filter.envelopeAmountOctaves, -12.0F, 12.0F) || !in_range(filter.keyTrack, -2.0F, 2.0F) ||
        !in_range(filter.drive, 0.05F, 24.0F) || !in_range(filter.bassCompensation, 0.0F, 1.0F) ||
        !in_range(filter.morph, 0.0F, 1.0F) || !in_range(filter.ms20HighPassCutoffHertz, 12.0F, 18000.0F) ||
        !in_range(filter.selfOscillation, 0.5F, 1.35F) ||
        (filter.oversampling != FilterOversampling::X1 && filter.oversampling != FilterOversampling::X2 &&
         filter.oversampling != FilterOversampling::X4 && filter.oversampling != FilterOversampling::Auto) ||
        !in_range(filter.comb.damping, 0.0F, 1.0F) || !in_range(filter.comb.mix, 0.0F, 1.0F) ||
        !in_range(filter.comb.feedbackScale, 0.0F, 1.5F) || !in_range(filter.formant.dryMix, 0.0F, 1.0F))
        return fail("invalid filter parameters");
    for (std::size_t i = 0; i < FormantParameters::kBandCount; ++i) {
        if (!in_range(filter.formant.frequencyHertz[i], 50.0F, 12000.0F) ||
            !in_range(filter.formant.gains[i], 0.0F, 2.0F))
            return fail("invalid formant parameters");
    }
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
        if (!in_range(slot.amount, -1.0F, 1.0F) || !in_range(slot.bias, -1.0F, 1.0F) ||
            !in_range(slot.smoothingMilliseconds, 0.0F, 2000.0F) ||
            static_cast<unsigned>(slot.source) > static_cast<unsigned>(ModulationSource::SeqPan) ||
            static_cast<unsigned>(slot.destination) > static_cast<unsigned>(ModulationDestination::GranularPosition))
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
    // Phase 2: sampler generator parameters.
    if (sampler.sampleIndex != 0U) return fail("invalid sampler sample index");
    if (!in_range(sampler.loopStartSeconds, 0.0F, 3600.0F) ||
        !in_range(sampler.loopEndSeconds, 0.0F, 3600.0F) ||
        !in_range(sampler.loopCrossfadeSeconds, 0.0F, 60.0F) ||
        !in_range(sampler.startOffsetSeconds, 0.0F, 3600.0F) ||
        !in_range(sampler.gain, 0.0F, 2.0F))
        return fail("invalid sampler parameters");
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
        !in_range(arpeggiator.externalTempoBpm, 20.0F, 400.0F) ||
        !in_range(arpeggiator.humanizeTiming, 0.0F, 1.0F) ||
        !in_range(arpeggiator.humanizeVelocity, 0.0F, 1.0F) ||
        !in_range(arpeggiator.phraseVelocityStart, 0.0F, 2.0F) ||
        !in_range(arpeggiator.phraseVelocityEnd, 0.0F, 2.0F))
        return fail("invalid arpeggiator parameters");
    for (const auto& step : arpeggiator.steps) {
        if (step.transpose < -48 || step.transpose > 48 || step.octaveOffset < -4 || step.octaveOffset > 4 ||
            !in_range(step.velocityScale, 0.0F, 2.0F) || !in_range(step.gateScale, 0.1F, 2.0F) ||
            !in_range(step.probability, 0.0F, 1.0F) || step.ratchets < 1U || step.ratchets > 8U ||
            step.conditionA < 1U || step.conditionA > 8U || step.conditionB < 1U || step.conditionB > 8U ||
            !in_range(step.macro1, -1.0F, 1.0F) || !in_range(step.macro2, -1.0F, 1.0F) ||
            !in_range(step.macro3, -1.0F, 1.0F) || !in_range(step.macro4, -1.0F, 1.0F))
            return fail("invalid arpeggiator step");
    }
    if (!in_range(distortion.drive, 0.05F, 32.0F) || !in_range(distortion.mix, 0.0F, 1.0F) ||
        static_cast<unsigned>(distortion.mode) > 3U)
        return fail("invalid distortion parameters");
    if (!in_range(bitcrusher.mix, 0.0F, 1.0F) || bitcrusher.bits < 1U || bitcrusher.bits > 16U ||
        bitcrusher.downsample < 1U || bitcrusher.downsample > 64U)
        return fail("invalid bitcrusher parameters");
    if (!in_range(harmonizer.subLevel, 0.0F, 1.0F) || !in_range(harmonizer.upLevel, 0.0F, 1.0F) ||
        !in_range(harmonizer.mix, 0.0F, 1.0F))
        return fail("invalid octave harmonizer parameters");
    if (!in_range(eq.lowGainDb, -24.0F, 24.0F) || !in_range(eq.midGainDb, -24.0F, 24.0F) ||
        !in_range(eq.highGainDb, -24.0F, 24.0F))
        return fail("invalid equalizer parameters");
    if (!in_range(chorus.rateHertz, 0.01F, 20.0F) || !in_range(chorus.depthMilliseconds, 0.0F, 30.0F) ||
        !in_range(chorus.mix, 0.0F, 1.0F))
        return fail("invalid chorus parameters");
    if (!in_range(flanger.rateHertz, 0.01F, 20.0F) || !in_range(flanger.depthMilliseconds, 0.0F, 10.0F) ||
        !in_range(flanger.feedback, -0.92F, 0.92F) || !in_range(flanger.mix, 0.0F, 1.0F))
        return fail("invalid flanger parameters");
    if (static_cast<unsigned>(ensemble.mode) > 2U || !in_range(ensemble.mix, 0.0F, 1.0F))
        return fail("invalid ensemble parameters");
    if (!in_range(phaser.rateHertz, 0.01F, 20.0F) || !in_range(phaser.depth, 0.0F, 1.0F) ||
        !in_range(phaser.feedback, -0.95F, 0.95F) || !in_range(phaser.mix, 0.0F, 1.0F))
        return fail("invalid phaser parameters");
    if (!in_range(delay.timeSeconds, 0.01F, 1.95F) || !in_range(delay.feedback, 0.0F, 0.94F) ||
        !in_range(delay.mix, 0.0F, 1.0F) || !in_range(delay.syncBeats, 0.03125F, 32.0F))
        return fail("invalid delay parameters");
    if (!in_range(diffusionDelay.timeSeconds, 0.01F, 1.95F) || !in_range(diffusionDelay.feedback, 0.0F, 0.94F) ||
        !in_range(diffusionDelay.mix, 0.0F, 1.0F) || !in_range(diffusionDelay.diffusion, 0.0F, 1.0F))
        return fail("invalid diffusion delay parameters");
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
        !in_range(pitchBendRangeSemitones, 0.0F, 48.0F) || !in_range(morphAmount, 0.0F, 1.0F))
        return fail("invalid master parameters");
    // Phase 3: generative sequencer / genetics / attractor ranges.
    {
        if (sequencer.channel > 15U || sequencer.rootNote > 127U || sequencer.octaveRange > 8U ||
            static_cast<unsigned>(sequencer.scale) > 5U)
            return fail("invalid sequencer globals");
        for (std::size_t li = 0; li < kSequencerLaneCount; ++li) {
            const auto& lane = sequencer.lanes[li];
            const SequencerLane which = static_cast<SequencerLane>(li);
            if (lane.stepCount < 1U || lane.stepCount > kSequencerMaxSteps ||
                static_cast<unsigned>(lane.direction) > 3U ||
                !in_range(lane.mutationAmount, 0.0F, 1.0F))
                return fail("invalid sequencer lane header");
            float valueLo = -1.0F, valueHi = 1.0F;
            switch (which) {
                case SequencerLane::Pitch: valueLo = -48.0F; valueHi = 48.0F; break;
                case SequencerLane::Velocity:
                case SequencerLane::Gate:
                case SequencerLane::Probability:
                case SequencerLane::Morph: valueLo = 0.0F; valueHi = 1.0F; break;
                case SequencerLane::Timbre:
                case SequencerLane::Pan: valueLo = -1.0F; valueHi = 1.0F; break;
                case SequencerLane::Count: break;
            }
            for (const auto& step : lane.steps) {
                if (!in_range(step.value, valueLo, valueHi) ||
                    !in_range(step.probability, 0.0F, 1.0F) ||
                    step.ratchets < 1U || step.ratchets > 8U ||
                    !in_range(step.microtiming, -1.0F, 1.0F) ||
                    !in_range(step.accent, 0.0F, 4.0F) ||
                    static_cast<unsigned>(step.condition) > 3U ||
                    step.conditionN < 1U || step.conditionN > 64U)
                    return fail("invalid sequencer step");
            }
        }
        if (!in_range(genetics.mutationIntensity, 0.0F, 1.0F))
            return fail("invalid genetics settings");
        const auto& attractorConfig = attractor.config;
        const auto finiteDouble = [](double v) { return std::isfinite(v); };
        if (!finiteDouble(attractorConfig.bpm) || attractorConfig.bpm < 20.0 ||
            attractorConfig.bpm > 400.0 || !finiteDouble(attractorConfig.barsHome) ||
            attractorConfig.barsHome < 0.0 || !finiteDouble(attractorConfig.barsRise) ||
            attractorConfig.barsRise < 0.0 || !finiteDouble(attractorConfig.barsTension) ||
            attractorConfig.barsTension < 0.0 || !finiteDouble(attractorConfig.barsPeak) ||
            attractorConfig.barsPeak < 0.0 || !finiteDouble(attractorConfig.barsFall) ||
            attractorConfig.barsFall < 0.0)
            return fail("invalid attractor config");
    }
    return true;
}

std::string SynthPreset::serialize() const {
    std::ostringstream out;
    out << "DVE_SYNTH_PRESET=5\nname=" << name << '\n'
        << "master.gain=" << masterGain << "\nmaster.pan=" << masterPan
        << "\nmaster.bend=" << pitchBendRangeSemitones << "\nmaster.midiThru=" << midiThru << '\n'
        << "master.morphEnabled=" << morphEnabled << "\nmaster.morphAmount=" << morphAmount << '\n'
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
        << "\nfilter.selfOscillation=" << filter.selfOscillation
        << "\nfilter.comb.damping=" << filter.comb.damping
        << "\nfilter.comb.mix=" << filter.comb.mix
        << "\nfilter.comb.feedbackScale=" << filter.comb.feedbackScale
        << "\nfilter.formant.dryMix=" << filter.formant.dryMix << '\n';
    for (std::size_t i = 0; i < FormantParameters::kBandCount; ++i)
        out << "filter.formant.freq" << i << "=" << filter.formant.frequencyHertz[i] << '\n'
            << "filter.formant.gain" << i << "=" << filter.formant.gains[i] << '\n';
    out << "chord.enabled=" << chord.enabled << "\nchord.type=" << chord_type_token(chord.type)
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
        << "\narp.externalTempo=" << arpeggiator.externalTempoBpm
        << "\narp.humanizeTiming=" << arpeggiator.humanizeTiming
        << "\narp.humanizeVelocity=" << arpeggiator.humanizeVelocity
        << "\narp.phraseVelStart=" << arpeggiator.phraseVelocityStart
        << "\narp.phraseVelEnd=" << arpeggiator.phraseVelocityEnd
        << "\narp.scale=" << chord_scale_token(arpeggiator.scale)
        << "\narp.scaleRoot=" << static_cast<unsigned>(arpeggiator.scaleRoot) << '\n';
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
            << prefix << "stereoDivergence=" << osc.stereoDivergence << '\n'
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
            << prefix << "physicalVelocityToEmbouchure=" << osc.physicalVelocityToEmbouchure << '\n'
            << prefix << "modalExcitation=" << modal_excitation_token(osc.modalResonator.excitation) << '\n'
            << prefix << "modalModeCount=" << static_cast<unsigned>(osc.modalResonator.modeCount) << '\n'
            << prefix << "modalBaseFrequency=" << osc.modalResonator.baseFrequency << '\n'
            << prefix << "modalDamping=" << osc.modalResonator.damping << '\n'
            << prefix << "modalInharmonicity=" << osc.modalResonator.inharmonicity << '\n'
            << prefix << "modalBrightness=" << osc.modalResonator.brightness << '\n'
            << prefix << "modalExcitationLevel=" << osc.modalResonator.excitationLevel << '\n'
            << prefix << "modalNoiseBurstMs=" << osc.modalResonator.noiseBurstMilliseconds << '\n'
            << prefix << "modalTransientMs=" << osc.modalResonator.transientMilliseconds << '\n';
        for (std::size_t m = 0; m < osc.modalResonator.modes.size(); ++m) {
            const auto& resonatorMode = osc.modalResonator.modes[m];
            out << prefix << "modalMode" << m << '=' << resonatorMode.frequencyRatio << ','
                << resonatorMode.decaySeconds << ',' << resonatorMode.gain << '\n';
        }
    }
    // Phase 4: granular generator (mirrors binary patch IDs 0x0800-0x0811).
    // Old files without these keys keep make_default() values (parse() starts
    // from the default preset and only overwrites recognized keys).
    out << "granular.enabled=" << granular.enabled << '\n'
        << "granular.densityHz=" << granular.densityHz << '\n'
        << "granular.durationMs=" << granular.durationMs << '\n'
        << "granular.pitchSemitones=" << granular.pitchSemitones << '\n'
        << "granular.position=" << granular.position01 << '\n'
        << "granular.positionJitter=" << granular.positionJitter01 << '\n'
        << "granular.panScatter=" << granular.panScatter01 << '\n'
        << "granular.gain=" << granular.gain << '\n'
        << "granular.reverseProbability=" << granular.reverseProbability01 << '\n'
        << "granular.envelopeShape=" << static_cast<unsigned>(granular.envelopeShape) << '\n'
        << "granular.cloud=" << granular.cloud01 << '\n'
        << "granular.scatter=" << granular.scatter01 << '\n'
        << "granular.dust=" << granular.dust01 << '\n'
        << "granular.freeze=" << granular.freeze01 << '\n'
        << "granular.freezePosition=" << granular.freezePosition01 << '\n'
        << "granular.smear=" << granular.smear01 << '\n'
        << "granular.width=" << granular.width01 << '\n'
        << "granular.quality=" << static_cast<unsigned>(granular.granularQuality) << '\n';
    for (std::size_t i = 0; i < arpeggiator.steps.size(); ++i) {
        const auto& step = arpeggiator.steps[i];
        const std::string prefix = "arp.step" + std::to_string(i) + ".";
        out << prefix << "enabled=" << step.enabled << '\n'
            << prefix << "condition=" << static_cast<unsigned>(step.condition) << '\n'
            << prefix << "conditionA=" << static_cast<unsigned>(step.conditionA) << '\n'
            << prefix << "conditionB=" << static_cast<unsigned>(step.conditionB) << '\n'
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
            << prefix << "bias=" << slot.bias << '\n'
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
    // Phase 2: sampler generator parameters.
    out << "sampler.enabled=" << sampler.enabled << '\n'
        << "sampler.sampleIndex=" << static_cast<unsigned>(sampler.sampleIndex) << '\n'
        << "sampler.mode=" << sampler_playback_mode_token(sampler.playbackMode) << '\n'
        << "sampler.direction=" << sampler_direction_token(sampler.direction) << '\n'
        << "sampler.loopStart=" << sampler.loopStartSeconds << '\n'
        << "sampler.loopEnd=" << sampler.loopEndSeconds << '\n'
        << "sampler.loopCrossfade=" << sampler.loopCrossfadeSeconds << '\n'
        << "sampler.pitchTracking=" << sampler.pitchTracking << '\n'
        << "sampler.startOffset=" << sampler.startOffsetSeconds << '\n'
        << "sampler.gain=" << sampler.gain << '\n';
    out << "distortion.enabled=" << distortion.enabled << "\ndistortion.drive=" << distortion.drive
        << "\ndistortion.mix=" << distortion.mix
        << "\ndistortion.mode=" << static_cast<unsigned>(distortion.mode) << '\n'
        << "bitcrusher.enabled=" << bitcrusher.enabled << "\nbitcrusher.bits=" << static_cast<unsigned>(bitcrusher.bits)
        << "\nbitcrusher.downsample=" << static_cast<unsigned>(bitcrusher.downsample)
        << "\nbitcrusher.mix=" << bitcrusher.mix << '\n'
        << "harmonizer.enabled=" << harmonizer.enabled << "\nharmonizer.subLevel=" << harmonizer.subLevel
        << "\nharmonizer.upLevel=" << harmonizer.upLevel << "\nharmonizer.mix=" << harmonizer.mix << '\n'
        << "eq.enabled=" << eq.enabled << "\neq.lowDb=" << eq.lowGainDb
        << "\neq.midDb=" << eq.midGainDb << "\neq.highDb=" << eq.highGainDb << '\n'
        << "chorus.enabled=" << chorus.enabled << "\nchorus.rate=" << chorus.rateHertz
        << "\nchorus.depthMs=" << chorus.depthMilliseconds << "\nchorus.mix=" << chorus.mix << '\n'
        << "flanger.enabled=" << flanger.enabled << "\nflanger.rate=" << flanger.rateHertz
        << "\nflanger.depthMs=" << flanger.depthMilliseconds << "\nflanger.feedback=" << flanger.feedback
        << "\nflanger.mix=" << flanger.mix << '\n'
        << "ensemble.enabled=" << ensemble.enabled << "\nensemble.mode=" << static_cast<unsigned>(ensemble.mode)
        << "\nensemble.mix=" << ensemble.mix << '\n'
        << "phaser.enabled=" << phaser.enabled << "\nphaser.rate=" << phaser.rateHertz
        << "\nphaser.depth=" << phaser.depth << "\nphaser.feedback=" << phaser.feedback
        << "\nphaser.mix=" << phaser.mix << '\n'
        << "delay.enabled=" << delay.enabled << "\ndelay.time=" << delay.timeSeconds
        << "\ndelay.feedback=" << delay.feedback << "\ndelay.mix=" << delay.mix
        << "\ndelay.pingPong=" << delay.pingPong
        << "\ndelay.tempoSync=" << delay.tempoSync << "\ndelay.syncBeats=" << delay.syncBeats << '\n'
        << "diffusionDelay.enabled=" << diffusionDelay.enabled
        << "\ndiffusionDelay.time=" << diffusionDelay.timeSeconds
        << "\ndiffusionDelay.feedback=" << diffusionDelay.feedback
        << "\ndiffusionDelay.mix=" << diffusionDelay.mix
        << "\ndiffusionDelay.diffusion=" << diffusionDelay.diffusion << '\n'
        << "reverb.enabled=" << reverb.enabled << "\nreverb.room=" << reverb.roomSize
        << "\nreverb.damping=" << reverb.damping << "\nreverb.width=" << reverb.width
        << "\nreverb.mix=" << reverb.mix << '\n'
        << "compressor.enabled=" << compressor.enabled << "\ncompressor.thresholdDb=" << compressor.thresholdDb
        << "\ncompressor.ratio=" << compressor.ratio << "\ncompressor.attackMs=" << compressor.attackMilliseconds
        << "\ncompressor.releaseMs=" << compressor.releaseMilliseconds << "\ncompressor.makeupDb=" << compressor.makeupDb << '\n'
        << "limiter.enabled=" << limiter.enabled << "\nlimiter.ceilingDb=" << limiter.ceilingDb
        << "\nlimiter.releaseMs=" << limiter.releaseMilliseconds << '\n';
    // Phase 3: generative sequencer / genetics / attractor state. Floats use
    // 9 significant digits so they parse back bit-exactly; steps equal to the
    // musical lane default are omitted (parse() fills them back in).
    {
        auto precise = [](float value) {
            std::ostringstream ss;
            ss << std::setprecision(9) << value;
            return ss.str();
        };
        out << "seq.enabled=" << sequencer.enabled << "\nseq.channel="
            << static_cast<unsigned>(sequencer.channel)
            << "\nseq.scale=" << static_cast<unsigned>(sequencer.scale)
            << "\nseq.root=" << static_cast<unsigned>(sequencer.rootNote)
            << "\nseq.octaves=" << static_cast<unsigned>(sequencer.octaveRange)
            << "\nseq.seed=" << sequencer.randomSeed << '\n';
        for (std::size_t li = 0; li < kSequencerLaneCount; ++li) {
            const auto& lane = sequencer.lanes[li];
            const std::string prefix = "seq.lane" + std::to_string(li) + ".";
            out << prefix << "steps=" << static_cast<unsigned>(lane.stepCount) << '\n'
                << prefix << "direction=" << static_cast<unsigned>(lane.direction) << '\n'
                << prefix << "mutation=" << precise(lane.mutationAmount) << '\n'
                << prefix << "cycles=" << static_cast<unsigned>(lane.patternCycles) << '\n';
            const SequencerLane which = static_cast<SequencerLane>(li);
            for (std::size_t si = 0; si < lane.steps.size(); ++si) {
                const SequencerStep& step = lane.steps[si];
                if (step == default_sequencer_step(which)) continue;
                const std::string sprefix = prefix + "step" + std::to_string(si) + ".";
                out << sprefix << "value=" << precise(step.value) << '\n'
                    << sprefix << "probability=" << precise(step.probability) << '\n'
                    << sprefix << "ratchets=" << static_cast<unsigned>(step.ratchets) << '\n'
                    << sprefix << "microtiming=" << precise(step.microtiming) << '\n'
                    << sprefix << "glide=" << step.glide << '\n'
                    << sprefix << "accent=" << precise(step.accent) << '\n'
                    << sprefix << "skip=" << step.skip << '\n'
                    << sprefix << "condition=" << static_cast<unsigned>(step.condition) << '\n'
                    << sprefix << "conditionN=" << static_cast<unsigned>(step.conditionN) << '\n';
            }
        }
        out << "gen.mutationIntensity=" << precise(genetics.mutationIntensity)
            << "\ngen.mutationSeed=" << genetics.mutationSeed
            << "\ngen.lockedGroups=" << static_cast<unsigned>(genetics.lockedGroups) << '\n'
            << "attr.enabled=" << attractor.enabled
            << "\nattr.bpm=" << std::setprecision(9) << attractor.config.bpm << '\n'
            << "attr.barsHome=" << attractor.config.barsHome
            << "\nattr.barsRise=" << attractor.config.barsRise
            << "\nattr.barsTension=" << attractor.config.barsTension
            << "\nattr.barsPeak=" << attractor.config.barsPeak
            << "\nattr.barsFall=" << attractor.config.barsFall
            << "\nattr.seed=" << attractor.config.seed << '\n';
    }
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
        else if (key == "master.morphEnabled") parsed = readBool(result.morphEnabled);
        else if (key == "master.morphAmount") parsed = readFloat(result.morphAmount);
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
        else if (key == "filter.oversampling") { unsigned v=0; parsed=parse_number<unsigned>(value,v) && (v==0U||v==1U||v==2U||v==4U); if(parsed) result.filter.oversampling=static_cast<FilterOversampling>(v); }
        else if (key == "filter.ms20HighPass") parsed = readFloat(result.filter.ms20HighPassCutoffHertz);
        else if (key == "filter.selfOscillation") parsed = readFloat(result.filter.selfOscillation);
        else if (key == "filter.comb.damping") parsed = readFloat(result.filter.comb.damping);
        else if (key == "filter.comb.mix") parsed = readFloat(result.filter.comb.mix);
        else if (key == "filter.comb.feedbackScale") parsed = readFloat(result.filter.comb.feedbackScale);
        else if (key == "filter.formant.dryMix") parsed = readFloat(result.filter.formant.dryMix);
        else if (key == "filter.formant.freq0") parsed = readFloat(result.filter.formant.frequencyHertz[0]);
        else if (key == "filter.formant.freq1") parsed = readFloat(result.filter.formant.frequencyHertz[1]);
        else if (key == "filter.formant.freq2") parsed = readFloat(result.filter.formant.frequencyHertz[2]);
        else if (key == "filter.formant.freq3") parsed = readFloat(result.filter.formant.frequencyHertz[3]);
        else if (key == "filter.formant.gain0") parsed = readFloat(result.filter.formant.gains[0]);
        else if (key == "filter.formant.gain1") parsed = readFloat(result.filter.formant.gains[1]);
        else if (key == "filter.formant.gain2") parsed = readFloat(result.filter.formant.gains[2]);
        else if (key == "filter.formant.gain3") parsed = readFloat(result.filter.formant.gains[3]);
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
        else if (key == "arp.humanizeTiming") parsed = readFloat(result.arpeggiator.humanizeTiming);
        else if (key == "arp.humanizeVelocity") parsed = readFloat(result.arpeggiator.humanizeVelocity);
        else if (key == "arp.phraseVelStart") parsed = readFloat(result.arpeggiator.phraseVelocityStart);
        else if (key == "arp.phraseVelEnd") parsed = readFloat(result.arpeggiator.phraseVelocityEnd);
        else if (key == "arp.scale") { const auto scale = parse_chord_scale(value); parsed = scale.has_value(); if (scale) result.arpeggiator.scale = *scale; }
        else if (key == "arp.scaleRoot") parsed = readUInt(result.arpeggiator.scaleRoot, 127U);
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
        else if (key == "sampler.enabled") parsed = readBool(result.sampler.enabled);
        else if (key == "sampler.sampleIndex") parsed = readUInt(result.sampler.sampleIndex, 0U);
        else if (key == "sampler.mode") { const auto mode = parse_sampler_playback_mode(value); parsed = mode.has_value(); if (mode) result.sampler.playbackMode = *mode; }
        else if (key == "sampler.direction") { const auto direction = parse_sampler_direction(value); parsed = direction.has_value(); if (direction) result.sampler.direction = *direction; }
        else if (key == "sampler.loopStart") parsed = readFloat(result.sampler.loopStartSeconds);
        else if (key == "sampler.loopEnd") parsed = readFloat(result.sampler.loopEndSeconds);
        else if (key == "sampler.loopCrossfade") parsed = readFloat(result.sampler.loopCrossfadeSeconds);
        else if (key == "sampler.pitchTracking") parsed = readBool(result.sampler.pitchTracking);
        else if (key == "sampler.startOffset") parsed = readFloat(result.sampler.startOffsetSeconds);
        else if (key == "sampler.gain") parsed = readFloat(result.sampler.gain);
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
            else if (field == "stereoDivergence") parsed = readFloat(osc.stereoDivergence);
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
            else if (field == "modalExcitation") { const auto v = parse_modal_excitation(value); parsed = v.has_value(); if (v) osc.modalResonator.excitation = *v; }
            else if (field == "modalModeCount") { unsigned v = 0; parsed = parse_number<unsigned>(value, v) && v >= 1U && v <= 32U; if (parsed) osc.modalResonator.modeCount = static_cast<std::uint8_t>(v); }
            else if (field == "modalBaseFrequency") parsed = readFloat(osc.modalResonator.baseFrequency);
            else if (field == "modalDamping") parsed = readFloat(osc.modalResonator.damping);
            else if (field == "modalInharmonicity") parsed = readFloat(osc.modalResonator.inharmonicity);
            else if (field == "modalBrightness") parsed = readFloat(osc.modalResonator.brightness);
            else if (field == "modalExcitationLevel") parsed = readFloat(osc.modalResonator.excitationLevel);
            else if (field == "modalNoiseBurstMs") parsed = readFloat(osc.modalResonator.noiseBurstMilliseconds);
            else if (field == "modalTransientMs") parsed = readFloat(osc.modalResonator.transientMilliseconds);
            else if (field.starts_with("modalMode")) {
                // "modalModeCount" is matched above; this branch handles "modalMode<m>=ratio,decay,gain".
                std::size_t m = 0;
                recognized = parse_number<std::size_t>(field.substr(9U), m) && m < kModalResonatorMaxModes;
                if (recognized) {
                    const std::size_t c1 = value.find(',');
                    const std::size_t c2 = c1 == std::string_view::npos ? c1 : value.find(',', c1 + 1U);
                    float ratio = 0.0F; float decay = 0.0F; float gain = 0.0F;
                    parsed = c1 != std::string_view::npos && c2 != std::string_view::npos &&
                             c2 + 1U < value.size() &&
                             parse_number<float>(value.substr(0, c1), ratio) &&
                             parse_number<float>(value.substr(c1 + 1U, c2 - c1 - 1U), decay) &&
                             parse_number<float>(value.substr(c2 + 1U), gain);
                    if (parsed) osc.modalResonator.modes[m] = ModalResonatorMode{ratio, decay, gain};
                }
            }
            else recognized = false;
        }
        if (!recognized && key.starts_with("granular.")) {
            // Phase 4: granular generator keys (mirrors binary IDs 0x0800-0x0811).
            // Old files without these keys keep the make_default() values that
            // parse() starts from.
            const std::string_view field = std::string_view(key).substr(9U);
            auto& g = result.granular;
            recognized = true;
            if (field == "enabled") parsed = readBool(g.enabled);
            else if (field == "densityHz") parsed = readFloat(g.densityHz);
            else if (field == "durationMs") parsed = readFloat(g.durationMs);
            else if (field == "pitchSemitones") parsed = readFloat(g.pitchSemitones);
            else if (field == "position") parsed = readFloat(g.position01);
            else if (field == "positionJitter") parsed = readFloat(g.positionJitter01);
            else if (field == "panScatter") parsed = readFloat(g.panScatter01);
            else if (field == "gain") parsed = readFloat(g.gain);
            else if (field == "reverseProbability") parsed = readFloat(g.reverseProbability01);
            else if (field == "envelopeShape") { unsigned v=0; parsed=parse_number<unsigned>(value,v) && v<=3U; if(parsed) g.envelopeShape=static_cast<GranularEnvelopeShape>(v); }
            else if (field == "cloud") parsed = readFloat(g.cloud01);
            else if (field == "scatter") parsed = readFloat(g.scatter01);
            else if (field == "dust") parsed = readFloat(g.dust01);
            else if (field == "freeze") parsed = readFloat(g.freeze01);
            else if (field == "freezePosition") parsed = readFloat(g.freezePosition01);
            else if (field == "smear") parsed = readFloat(g.smear01);
            else if (field == "width") parsed = readFloat(g.width01);
            else if (field == "quality") { unsigned v=0; parsed=parse_number<unsigned>(value,v) && v<=3U; if(parsed) g.granularQuality=static_cast<FilterQuality>(v); }
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
            else if (field == "condition") { unsigned v = 0; parsed = parse_number<unsigned>(value, v) && v <= 6U; if (parsed) step.condition = static_cast<ArpeggiatorCondition>(v); }
            else if (field == "conditionA") { unsigned v = 1; parsed = parse_number<unsigned>(value, v) && v >= 1U && v <= 8U; if (parsed) step.conditionA = static_cast<std::uint8_t>(v); }
            else if (field == "conditionB") { unsigned v = 2; parsed = parse_number<unsigned>(value, v) && v >= 1U && v <= 8U; if (parsed) step.conditionB = static_cast<std::uint8_t>(v); }
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
            else if (field == "bias") parsed = readFloat(slot.bias);
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
            else if (key == "distortion.mode") parsed = readUInt(result.distortion.mode, 3U);
            else if (key == "bitcrusher.enabled") parsed = readBool(result.bitcrusher.enabled);
            else if (key == "bitcrusher.bits") parsed = readUInt(result.bitcrusher.bits, 16U);
            else if (key == "bitcrusher.downsample") parsed = readUInt(result.bitcrusher.downsample, 64U);
            else if (key == "bitcrusher.mix") parsed = readFloat(result.bitcrusher.mix);
            else if (key == "harmonizer.enabled") parsed = readBool(result.harmonizer.enabled);
            else if (key == "harmonizer.subLevel") parsed = readFloat(result.harmonizer.subLevel);
            else if (key == "harmonizer.upLevel") parsed = readFloat(result.harmonizer.upLevel);
            else if (key == "harmonizer.mix") parsed = readFloat(result.harmonizer.mix);
            else if (key == "eq.enabled") parsed = readBool(result.eq.enabled);
            else if (key == "eq.lowDb") parsed = readFloat(result.eq.lowGainDb);
            else if (key == "eq.midDb") parsed = readFloat(result.eq.midGainDb);
            else if (key == "eq.highDb") parsed = readFloat(result.eq.highGainDb);
            else if (key == "chorus.enabled") parsed = readBool(result.chorus.enabled);
            else if (key == "chorus.rate") parsed = readFloat(result.chorus.rateHertz);
            else if (key == "chorus.depthMs") parsed = readFloat(result.chorus.depthMilliseconds);
            else if (key == "chorus.mix") parsed = readFloat(result.chorus.mix);
            else if (key == "flanger.enabled") parsed = readBool(result.flanger.enabled);
            else if (key == "flanger.rate") parsed = readFloat(result.flanger.rateHertz);
            else if (key == "flanger.depthMs") parsed = readFloat(result.flanger.depthMilliseconds);
            else if (key == "flanger.feedback") parsed = readFloat(result.flanger.feedback);
            else if (key == "flanger.mix") parsed = readFloat(result.flanger.mix);
            else if (key == "ensemble.enabled") parsed = readBool(result.ensemble.enabled);
            else if (key == "ensemble.mode") parsed = readUInt(result.ensemble.mode, 2U);
            else if (key == "ensemble.mix") parsed = readFloat(result.ensemble.mix);
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
            else if (key == "delay.tempoSync") parsed = readBool(result.delay.tempoSync);
            else if (key == "delay.syncBeats") parsed = readFloat(result.delay.syncBeats);
            else if (key == "diffusionDelay.enabled") parsed = readBool(result.diffusionDelay.enabled);
            else if (key == "diffusionDelay.time") parsed = readFloat(result.diffusionDelay.timeSeconds);
            else if (key == "diffusionDelay.feedback") parsed = readFloat(result.diffusionDelay.feedback);
            else if (key == "diffusionDelay.mix") parsed = readFloat(result.diffusionDelay.mix);
            else if (key == "diffusionDelay.diffusion") parsed = readFloat(result.diffusionDelay.diffusion);
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
        // Phase 3: generative sequencer / genetics / attractor state. All
        // keys are optional: old preset strings simply keep make_default()
        // values for anything absent.
        if (!recognized && key.starts_with("seq.")) {
            recognized = true;
            const std::string_view rest = std::string_view(key).substr(4U);
            if (rest == "enabled") parsed = readBool(result.sequencer.enabled);
            else if (rest == "channel") parsed = readUInt(result.sequencer.channel, 15U);
            else if (rest == "scale") {
                unsigned v = 0U;
                parsed = parse_number<unsigned>(value, v) && v <= 5U;
                if (parsed) result.sequencer.scale = static_cast<SequencerScale>(v);
            }
            else if (rest == "root") parsed = readUInt(result.sequencer.rootNote, 127U);
            else if (rest == "octaves") parsed = readUInt(result.sequencer.octaveRange, 8U);
            else if (rest == "seed") parsed = parse_number<std::uint32_t>(value, result.sequencer.randomSeed);
            else if (rest.starts_with("lane")) {
                const std::string_view afterLane = rest.substr(4U);
                const auto dot = afterLane.find('.');
                std::size_t li = 0U;
                if (dot == std::string_view::npos ||
                    !parse_number<std::size_t>(afterLane.substr(0, dot), li) ||
                    li >= kSequencerLaneCount) {
                    parsed = false;
                } else {
                    auto& lane = result.sequencer.lanes[li];
                    const std::string_view field = afterLane.substr(dot + 1U);
                    if (field == "steps") {
                        unsigned v = 0U;
                        parsed = parse_number<unsigned>(value, v) && v >= 1U && v <= 64U;
                        if (parsed) lane.stepCount = static_cast<std::uint8_t>(v);
                    }
                    else if (field == "direction") {
                        unsigned v = 0U;
                        parsed = parse_number<unsigned>(value, v) && v <= 3U;
                        if (parsed) lane.direction = static_cast<SequencerDirection>(v);
                    }
                    else if (field == "mutation") parsed = readFloat(lane.mutationAmount);
                    else if (field == "cycles") parsed = readUInt(lane.patternCycles, 255U);
                    else if (field.starts_with("step")) {
                        const std::string_view afterStep = field.substr(4U);
                        const auto dot2 = afterStep.find('.');
                        std::size_t si = 0U;
                        if (dot2 == std::string_view::npos ||
                            !parse_number<std::size_t>(afterStep.substr(0, dot2), si) ||
                            si >= kSequencerMaxSteps) {
                            parsed = false;
                        } else {
                            auto& step = lane.steps[si];
                            const std::string_view sub = afterStep.substr(dot2 + 1U);
                            if (sub == "value") parsed = readFloat(step.value);
                            else if (sub == "probability") parsed = readFloat(step.probability);
                            else if (sub == "ratchets") {
                                unsigned v = 0U;
                                parsed = parse_number<unsigned>(value, v) && v >= 1U && v <= 8U;
                                if (parsed) step.ratchets = static_cast<std::uint8_t>(v);
                            }
                            else if (sub == "microtiming") parsed = readFloat(step.microtiming);
                            else if (sub == "glide") parsed = readBool(step.glide);
                            else if (sub == "accent") parsed = readFloat(step.accent);
                            else if (sub == "skip") parsed = readBool(step.skip);
                            else if (sub == "condition") {
                                unsigned v = 0U;
                                parsed = parse_number<unsigned>(value, v) && v <= 3U;
                                if (parsed)
                                    step.condition = static_cast<SequencerStepCondition>(v);
                            }
                            else if (sub == "conditionN") {
                                unsigned v = 0U;
                                parsed = parse_number<unsigned>(value, v) && v >= 1U && v <= 64U;
                                if (parsed) step.conditionN = static_cast<std::uint8_t>(v);
                            }
                            else recognized = false;
                        }
                    }
                    else recognized = false;
                }
            }
            else recognized = false;
        }
        if (!recognized && key.starts_with("gen.")) {
            recognized = true;
            const std::string_view rest = std::string_view(key).substr(4U);
            if (rest == "mutationIntensity") parsed = readFloat(result.genetics.mutationIntensity);
            else if (rest == "mutationSeed")
                parsed = parse_number<std::uint64_t>(value, result.genetics.mutationSeed);
            else if (rest == "lockedGroups") parsed = readUInt(result.genetics.lockedGroups, 255U);
            else recognized = false;
        }
        if (!recognized && key.starts_with("attr.")) {
            recognized = true;
            const std::string_view rest = std::string_view(key).substr(5U);
            auto readDouble = [&](double& target) { return parse_number<double>(value, target); };
            if (rest == "enabled") parsed = readBool(result.attractor.enabled);
            else if (rest == "bpm") parsed = readDouble(result.attractor.config.bpm);
            else if (rest == "barsHome") parsed = readDouble(result.attractor.config.barsHome);
            else if (rest == "barsRise") parsed = readDouble(result.attractor.config.barsRise);
            else if (rest == "barsTension") parsed = readDouble(result.attractor.config.barsTension);
            else if (rest == "barsPeak") parsed = readDouble(result.attractor.config.barsPeak);
            else if (rest == "barsFall") parsed = readDouble(result.attractor.config.barsFall);
            else if (rest == "seed")
                parsed = parse_number<std::uint64_t>(value, result.attractor.config.seed);
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
        r.stereoDivergence=lerp(x.stereoDivergence,y.stereoDivergence);
    }
    auto morphEnvelope = [&](AdsrParameters& r, const AdsrParameters& x, const AdsrParameters& y) {
        // Phase 1: time parameters morph in log domain (perceptually uniform).
        auto logLerp = [t](float a, float b) {
            const float la = std::log(std::max(0.001F, a));
            const float lb = std::log(std::max(0.001F, b));
            return std::exp(la + (lb - la) * t);
        };
        r.attackSeconds=logLerp(x.attackSeconds,y.attackSeconds);
        r.decaySeconds=logLerp(x.decaySeconds,y.decaySeconds);
        r.sustainLevel=lerp(x.sustainLevel,y.sustainLevel);
        r.releaseSeconds=logLerp(x.releaseSeconds,y.releaseSeconds);
        r.delaySeconds=logLerp(x.delaySeconds,y.delaySeconds);
        r.holdSeconds=logLerp(x.holdSeconds,y.holdSeconds);
    };
    morphEnvelope(result.ampEnvelope,a.ampEnvelope,b.ampEnvelope);
    morphEnvelope(result.filter.envelope,a.filter.envelope,b.filter.envelope);
    result.filter.cutoffHertz=std::exp(lerp(std::log(std::max(18.0F,a.filter.cutoffHertz)),std::log(std::max(18.0F,b.filter.cutoffHertz))));
    result.filter.resonance=lerp(a.filter.resonance,b.filter.resonance); result.filter.envelopeAmountOctaves=lerp(a.filter.envelopeAmountOctaves,b.filter.envelopeAmountOctaves);
    result.filter.keyTrack=lerp(a.filter.keyTrack,b.filter.keyTrack); result.filter.drive=lerp(a.filter.drive,b.filter.drive);
    result.filter.bassCompensation=lerp(a.filter.bassCompensation,b.filter.bassCompensation); result.filter.morph=lerp(a.filter.morph,b.filter.morph);
    result.filter.ms20HighPassCutoffHertz=lerp(a.filter.ms20HighPassCutoffHertz,b.filter.ms20HighPassCutoffHertz);
    result.filter.selfOscillation=lerp(a.filter.selfOscillation,b.filter.selfOscillation);
    // Phase 2: morph the new comb/formant parameters too.
    result.filter.comb.damping=lerp(a.filter.comb.damping,b.filter.comb.damping);
    result.filter.comb.mix=lerp(a.filter.comb.mix,b.filter.comb.mix);
    result.filter.comb.feedbackScale=lerp(a.filter.comb.feedbackScale,b.filter.comb.feedbackScale);
    result.filter.formant.dryMix=lerp(a.filter.formant.dryMix,b.filter.formant.dryMix);
    for (std::size_t i=0;i<FormantParameters::kBandCount;++i) {
        result.filter.formant.frequencyHertz[i]=lerp(a.filter.formant.frequencyHertz[i],b.filter.formant.frequencyHertz[i]);
        result.filter.formant.gains[i]=lerp(a.filter.formant.gains[i],b.filter.formant.gains[i]);
    }
    result.tuning.referenceHertz=lerp(a.tuning.referenceHertz,b.tuning.referenceHertz); result.tuning.transposeSemitones=lerp(a.tuning.transposeSemitones,b.tuning.transposeSemitones);
    result.tuning.fineCents=lerp(a.tuning.fineCents,b.tuning.fineCents); result.tuning.analogDriftCents=lerp(a.tuning.analogDriftCents,b.tuning.analogDriftCents);
    for (std::size_t i=0;i<kSynthLfoCount;++i) {
        // Phase 1: LFO rate morphs in log domain.
        const float rateA = std::max(0.01F, a.lfos[i].rateHertz);
        const float rateB = std::max(0.01F, b.lfos[i].rateHertz);
        result.lfos[i].rateHertz=std::exp(std::log(rateA) + (std::log(rateB) - std::log(rateA)) * t);
        result.lfos[i].depth=lerp(a.lfos[i].depth,b.lfos[i].depth);
        result.lfos[i].phase=lerp(a.lfos[i].phase,b.lfos[i].phase);
        result.lfos[i].fadeInSeconds=lerp(a.lfos[i].fadeInSeconds,b.lfos[i].fadeInSeconds);
        result.lfos[i].beatsPerCycle=lerp(a.lfos[i].beatsPerCycle,b.lfos[i].beatsPerCycle);
    }
    for (std::size_t i=0;i<kSynthModulationSlotCount;++i) result.modulation[i].amount=lerp(a.modulation[i].amount,b.modulation[i].amount);
    for (std::size_t i=0;i<kSynthMacroCount;++i) result.macros.values[i]=lerp(a.macros.values[i],b.macros.values[i]);
    result.masterGain=lerp(a.masterGain,b.masterGain); result.masterPan=lerp(a.masterPan,b.masterPan); result.pitchBendRangeSemitones=lerp(a.pitchBendRangeSemitones,b.pitchBendRangeSemitones);
    result.distortion.drive=lerp(a.distortion.drive,b.distortion.drive); result.distortion.mix=lerp(a.distortion.mix,b.distortion.mix);
    result.eq.lowGainDb=lerp(a.eq.lowGainDb,b.eq.lowGainDb); result.eq.midGainDb=lerp(a.eq.midGainDb,b.eq.midGainDb); result.eq.highGainDb=lerp(a.eq.highGainDb,b.eq.highGainDb);
    result.chorus.rateHertz=lerp(a.chorus.rateHertz,b.chorus.rateHertz); result.chorus.depthMilliseconds=lerp(a.chorus.depthMilliseconds,b.chorus.depthMilliseconds); result.chorus.mix=lerp(a.chorus.mix,b.chorus.mix);
    result.phaser.rateHertz=lerp(a.phaser.rateHertz,b.phaser.rateHertz); result.phaser.depth=lerp(a.phaser.depth,b.phaser.depth); result.phaser.feedback=lerp(a.phaser.feedback,b.phaser.feedback); result.phaser.mix=lerp(a.phaser.mix,b.phaser.mix);
    result.delay.timeSeconds=std::exp(std::log(std::max(0.001F,a.delay.timeSeconds)) +
        (std::log(std::max(0.001F,b.delay.timeSeconds)) - std::log(std::max(0.001F,a.delay.timeSeconds))) * t);
    result.delay.feedback=lerp(a.delay.feedback,b.delay.feedback); result.delay.mix=lerp(a.delay.mix,b.delay.mix);
    result.delay.syncBeats=lerp(a.delay.syncBeats,b.delay.syncBeats);
    // Phase 2: diffusion delay morphs like the plain delay (log-domain time).
    result.diffusionDelay.timeSeconds=std::exp(std::log(std::max(0.001F,a.diffusionDelay.timeSeconds)) +
        (std::log(std::max(0.001F,b.diffusionDelay.timeSeconds)) - std::log(std::max(0.001F,a.diffusionDelay.timeSeconds))) * t);
    result.diffusionDelay.feedback=lerp(a.diffusionDelay.feedback,b.diffusionDelay.feedback);
    result.diffusionDelay.mix=lerp(a.diffusionDelay.mix,b.diffusionDelay.mix);
    result.diffusionDelay.diffusion=lerp(a.diffusionDelay.diffusion,b.diffusionDelay.diffusion);
    result.reverb.roomSize=lerp(a.reverb.roomSize,b.reverb.roomSize); result.reverb.damping=lerp(a.reverb.damping,b.reverb.damping); result.reverb.width=lerp(a.reverb.width,b.reverb.width); result.reverb.mix=lerp(a.reverb.mix,b.reverb.mix);
    result.wavetable.enabled = a.wavetable.enabled || b.wavetable.enabled;
    result.wavetable.frameCount = std::max(a.wavetable.frameCount,b.wavetable.frameCount);
    for (std::size_t i=0;i<result.wavetable.samples.size();++i) result.wavetable.samples[i]=lerp(a.wavetable.samples[i],b.wavetable.samples[i]);
    result.wavetable.contentHash = 0U;
    return result;
}

// Realtime-safe counterpart of morph_synth_presets(): interpolates two
// numeric RealtimePresets without touching strings or allocating, so the
// audio thread can apply the conductor's morph walk every block. Mirrors the
// UI-thread morph's field selection exactly, with one deliberate deviation:
// wavetable *content* snaps at t >= 0.5 instead of lerping, because lerped
// content would force a full HQ wavetable re-cook on every morph block.
// Wavetable *position* (the musically primary morph dimension) still
// interpolates smoothly via oscillators[].wavetablePosition.
RealtimePreset morph_realtime_presets(const RealtimePreset& a, const RealtimePreset& b,
                                      float amount) noexcept {
    const float t = clampf(amount, 0.0F, 1.0F);
    const bool chooseB = t >= 0.5F;
    auto lerp = [t](float x, float y) { return x + (y - x) * t; };
    auto logLerp = [t](float x, float y) {
        const float lx = std::log(std::max(0.001F, x));
        const float ly = std::log(std::max(0.001F, y));
        return std::exp(lx + (ly - lx) * t);
    };
    RealtimePreset result = chooseB ? b : a;
    for (std::size_t i = 0; i < result.oscillators.size(); ++i) {
        auto& r = result.oscillators[i]; const auto& x = a.oscillators[i]; const auto& y = b.oscillators[i];
        r.gain=lerp(x.gain,y.gain); r.pan=lerp(x.pan,y.pan); r.semitones=lerp(x.semitones,y.semitones);
        r.cents=lerp(x.cents,y.cents); r.pulseWidth=lerp(x.pulseWidth,y.pulseWidth); r.pwmDepth=lerp(x.pwmDepth,y.pwmDepth);
        r.pwmRateHertz=lerp(x.pwmRateHertz,y.pwmRateHertz); r.shape=lerp(x.shape,y.shape);
        r.phaseOffset=lerp(x.phaseOffset,y.phaseOffset); r.frequencyModAmount=lerp(x.frequencyModAmount,y.frequencyModAmount);
        r.ringModDepth=lerp(x.ringModDepth,y.ringModDepth); r.subOscillatorLevel=lerp(x.subOscillatorLevel,y.subOscillatorLevel);
        r.wavetablePosition=lerp(x.wavetablePosition,y.wavetablePosition);
        r.stereoDivergence=lerp(x.stereoDivergence,y.stereoDivergence);
    }
    auto morphEnvelope = [&](AdsrParameters& r, const AdsrParameters& x, const AdsrParameters& y) {
        r.attackSeconds=logLerp(x.attackSeconds,y.attackSeconds);
        r.decaySeconds=logLerp(x.decaySeconds,y.decaySeconds);
        r.sustainLevel=lerp(x.sustainLevel,y.sustainLevel);
        r.releaseSeconds=logLerp(x.releaseSeconds,y.releaseSeconds);
        r.delaySeconds=logLerp(x.delaySeconds,y.delaySeconds);
        r.holdSeconds=logLerp(x.holdSeconds,y.holdSeconds);
    };
    morphEnvelope(result.ampEnvelope,a.ampEnvelope,b.ampEnvelope);
    morphEnvelope(result.filter.envelope,a.filter.envelope,b.filter.envelope);
    result.filter.cutoffHertz=logLerp(a.filter.cutoffHertz,b.filter.cutoffHertz);
    result.filter.resonance=lerp(a.filter.resonance,b.filter.resonance);
    result.filter.envelopeAmountOctaves=lerp(a.filter.envelopeAmountOctaves,b.filter.envelopeAmountOctaves);
    result.filter.keyTrack=lerp(a.filter.keyTrack,b.filter.keyTrack);
    result.filter.drive=lerp(a.filter.drive,b.filter.drive);
    result.filter.bassCompensation=lerp(a.filter.bassCompensation,b.filter.bassCompensation);
    result.filter.morph=lerp(a.filter.morph,b.filter.morph);
    result.filter.ms20HighPassCutoffHertz=lerp(a.filter.ms20HighPassCutoffHertz,b.filter.ms20HighPassCutoffHertz);
    result.filter.selfOscillation=lerp(a.filter.selfOscillation,b.filter.selfOscillation);
    result.filter.comb.damping=lerp(a.filter.comb.damping,b.filter.comb.damping);
    result.filter.comb.mix=lerp(a.filter.comb.mix,b.filter.comb.mix);
    result.filter.comb.feedbackScale=lerp(a.filter.comb.feedbackScale,b.filter.comb.feedbackScale);
    result.filter.formant.dryMix=lerp(a.filter.formant.dryMix,b.filter.formant.dryMix);
    for (std::size_t i=0;i<FormantParameters::kBandCount;++i) {
        result.filter.formant.frequencyHertz[i]=lerp(a.filter.formant.frequencyHertz[i],b.filter.formant.frequencyHertz[i]);
        result.filter.formant.gains[i]=lerp(a.filter.formant.gains[i],b.filter.formant.gains[i]);
    }
    result.tuning.referenceHertz=lerp(a.tuning.referenceHertz,b.tuning.referenceHertz);
    result.tuning.transposeSemitones=lerp(a.tuning.transposeSemitones,b.tuning.transposeSemitones);
    result.tuning.fineCents=lerp(a.tuning.fineCents,b.tuning.fineCents);
    result.tuning.analogDriftCents=lerp(a.tuning.analogDriftCents,b.tuning.analogDriftCents);
    for (std::size_t i=0;i<kSynthLfoCount;++i) {
        const float rateA = std::max(0.01F, a.lfos[i].rateHertz);
        const float rateB = std::max(0.01F, b.lfos[i].rateHertz);
        result.lfos[i].rateHertz=std::exp(std::log(rateA) + (std::log(rateB) - std::log(rateA)) * t);
        result.lfos[i].depth=lerp(a.lfos[i].depth,b.lfos[i].depth);
        result.lfos[i].phase=lerp(a.lfos[i].phase,b.lfos[i].phase);
        result.lfos[i].fadeInSeconds=lerp(a.lfos[i].fadeInSeconds,b.lfos[i].fadeInSeconds);
        result.lfos[i].beatsPerCycle=lerp(a.lfos[i].beatsPerCycle,b.lfos[i].beatsPerCycle);
    }
    for (std::size_t i=0;i<kSynthModulationSlotCount;++i) result.modulation[i].amount=lerp(a.modulation[i].amount,b.modulation[i].amount);
    for (std::size_t i=0;i<kSynthMacroCount;++i) result.macroValues[i]=lerp(a.macroValues[i],b.macroValues[i]);
    result.masterGain=lerp(a.masterGain,b.masterGain);
    result.masterPan=lerp(a.masterPan,b.masterPan);
    result.pitchBendRangeSemitones=lerp(a.pitchBendRangeSemitones,b.pitchBendRangeSemitones);
    result.distortion.drive=lerp(a.distortion.drive,b.distortion.drive);
    result.distortion.mix=lerp(a.distortion.mix,b.distortion.mix);
    result.eq.lowGainDb=lerp(a.eq.lowGainDb,b.eq.lowGainDb);
    result.eq.midGainDb=lerp(a.eq.midGainDb,b.eq.midGainDb);
    result.eq.highGainDb=lerp(a.eq.highGainDb,b.eq.highGainDb);
    result.chorus.rateHertz=lerp(a.chorus.rateHertz,b.chorus.rateHertz);
    result.chorus.depthMilliseconds=lerp(a.chorus.depthMilliseconds,b.chorus.depthMilliseconds);
    result.chorus.mix=lerp(a.chorus.mix,b.chorus.mix);
    result.phaser.rateHertz=lerp(a.phaser.rateHertz,b.phaser.rateHertz);
    result.phaser.depth=lerp(a.phaser.depth,b.phaser.depth);
    result.phaser.feedback=lerp(a.phaser.feedback,b.phaser.feedback);
    result.phaser.mix=lerp(a.phaser.mix,b.phaser.mix);
    result.delay.timeSeconds=logLerp(a.delay.timeSeconds,b.delay.timeSeconds);
    result.delay.feedback=lerp(a.delay.feedback,b.delay.feedback);
    result.delay.mix=lerp(a.delay.mix,b.delay.mix);
    result.delay.syncBeats=lerp(a.delay.syncBeats,b.delay.syncBeats);
    result.diffusionDelay.timeSeconds=logLerp(a.diffusionDelay.timeSeconds,b.diffusionDelay.timeSeconds);
    result.diffusionDelay.feedback=lerp(a.diffusionDelay.feedback,b.diffusionDelay.feedback);
    result.diffusionDelay.mix=lerp(a.diffusionDelay.mix,b.diffusionDelay.mix);
    result.diffusionDelay.diffusion=lerp(a.diffusionDelay.diffusion,b.diffusionDelay.diffusion);
    result.reverb.roomSize=lerp(a.reverb.roomSize,b.reverb.roomSize);
    result.reverb.damping=lerp(a.reverb.damping,b.reverb.damping);
    result.reverb.width=lerp(a.reverb.width,b.reverb.width);
    result.reverb.mix=lerp(a.reverb.mix,b.reverb.mix);
    // Wavetable content snaps at the midpoint (see note above); the enable
    // flag ORs and the frame count takes the max, as in the UI morph.
    result.wavetable.enabled = a.wavetable.enabled || b.wavetable.enabled;
    result.wavetable.frameCount = std::max(a.wavetable.frameCount,b.wavetable.frameCount);
    result.wavetable.samples = chooseB ? b.wavetable.samples : a.wavetable.samples;
    // Re-derive the fields realtime_preset() computes from the morphed values.
    for (std::size_t i = 0; i < result.oscillators.size(); ++i) {
        const float pan = clampf(result.oscillators[i].pan, -1.0F, 1.0F);
        result.oscillatorPanLeft[i] = std::sqrt(0.5F * (1.0F - pan));
        result.oscillatorPanRight[i] = std::sqrt(0.5F * (1.0F + pan));
    }
    const float masterPan = clampf(result.masterPan, -1.0F, 1.0F);
    result.masterPanLeft = std::sqrt(0.5F * (1.0F - masterPan));
    result.masterPanRight = std::sqrt(0.5F * (1.0F + masterPan));
    result.activeModulationCount = 0;
    for (std::size_t slotIndex = 0; slotIndex < result.modulation.size(); ++slotIndex) {
        const ModulationSlot& slot = result.modulation[slotIndex];
        if (slot.enabled && slot.source != ModulationSource::Off &&
            slot.destination != ModulationDestination::Off) {
            result.activeModulation[result.activeModulationCount] = slot;
            result.activeModulationIndices[result.activeModulationCount] = static_cast<std::uint8_t>(slotIndex);
            ++result.activeModulationCount;
        }
    }
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
    // Pre-size the wavetable cook scratch on the constructing thread so the
    // first render-thread cook (inside adopt_preset) performs no allocation.
    impl_->wavetableCookScratch_.resize(kHQWavetableFrames * kHQWavetableSamples);
    impl_->wavetableCookSpectrum_.resize(kHQWavetableSamples);
    impl_->wavetableCookFiltered_.resize(kHQWavetableSamples);
    impl_->wavetableCookFrame_.resize(kHQWavetableSamples);
    PresetUpdate initial{};
    initial.base = realtime_preset(preset_);
    initial.sequencer = preset_.sequencer;
    initial.sequencerChanged = true;
    initial.attractor = preset_.attractor.config;
    initial.attractorEnabled = preset_.attractor.enabled;
    impl_->adopt_preset(initial, 0.0F, false);
    impl_->limiterEnvelope = 1.0F;
}
Synthesizer::~Synthesizer() { delete impl_; }

SynthPreset Synthesizer::preset() const {
    std::lock_guard<std::mutex> lock(presetMutex_);
    return preset_;
}

void Synthesizer::request_morph_amount(float amount) noexcept {
    rtMorphAmount_.store(clampf(amount, 0.0F, 1.0F), std::memory_order_relaxed);
}

std::shared_ptr<const Synthesizer::PendingConductorConfig>
Synthesizer::take_pending_conductor_config() noexcept {
    return pendingConductorConfig_.exchange(nullptr, std::memory_order_acq_rel);
}

void Synthesizer::set_preset(const SynthPreset& preset) {
    std::string error;
    if (!preset.validate(&error)) return;
    PresetUpdate update{};
    std::shared_ptr<PendingConductorConfig> pendingConductor;
    {
        std::lock_guard<std::mutex> lock(presetMutex_);
        // Phase 3: the preset owns the sequencer's authored config. The live
        // sequencer used to be reconfigured here on the UI thread while the
        // audio thread could be inside advance_sequencer(); the config is now
        // carried in the update and applied on the render thread inside
        // adopt_preset() (only when it actually changed, so reseeds are
        // avoided for identical configs).
        const bool sequencerChanged = !(preset.sequencer == preset_.sequencer);
        preset_ = preset;
        update.base = realtime_preset(preset_);
        update.hasMorphB = hasMorphPresetB_;
        if (hasMorphPresetB_) update.morphB = realtime_preset(morphPresetB_);
        update.morphEnabled = preset_.morphEnabled;
        update.morphAmount = preset_.morphAmount;
        update.sequencer = preset_.sequencer;
        update.sequencerChanged = sequencerChanged;
        // Phase 3: the preset's attractor settings used to be installed here
        // too (same race vs conductor.process()); now applied on the render
        // thread. configure() still only resets the phase machine when the
        // flag or config actually changed.
        update.attractor = preset_.attractor.config;
        update.attractorEnabled = preset_.attractor.enabled;
        rtMorphEnabled_.store(preset_.morphEnabled, std::memory_order_relaxed);
        rtMorphAmount_.store(clampf(preset_.morphAmount, 0.0F, 1.0F), std::memory_order_relaxed);
        // Publish the attractor config for the conductor as well, so
        // GenerativeConductor::process() uses the preset's config even when
        // render() hasn't run yet to drain the PresetUpdate queue (e.g. a test
        // driving the conductor directly). Lock-free single-producer handoff;
        // the audio thread picks it up in process(). The queued update above
        // still applies it on the render thread via adopt_preset().
        pendingConductor = std::make_shared<PendingConductorConfig>();
        pendingConductor->enabled = preset_.attractor.enabled;
        pendingConductor->config = preset_.attractor.config;
    }
    pendingConductorConfig_.store(std::move(pendingConductor), std::memory_order_release);
    if (!impl_->presetIn.push(update)) impl_->droppedPresets.fetch_add(1U, std::memory_order_relaxed);
}

void Synthesizer::set_morph_preset_b(const SynthPreset& presetB) {
    std::string error;
    if (!presetB.validate(&error)) return;
    SynthPreset current;
    {
        std::lock_guard<std::mutex> lock(presetMutex_);
        morphPresetB_ = presetB;
        hasMorphPresetB_ = true;
        current = preset_;
    }
    // Re-publish the current preset so the render thread caches the new B
    // endpoint. (Separate lock scope above: set_preset() takes the mutex.)
    set_preset(current);
}

void Synthesizer::clear_morph_preset_b() {
    SynthPreset current;
    {
        std::lock_guard<std::mutex> lock(presetMutex_);
        hasMorphPresetB_ = false;
        current = preset_;
    }
    set_preset(current);
}

bool Synthesizer::has_morph_preset_b() const {
    std::lock_guard<std::mutex> lock(presetMutex_);
    return hasMorphPresetB_;
}

void Synthesizer::set_morph_amount(float amount) {
    SynthPreset current;
    {
        std::lock_guard<std::mutex> lock(presetMutex_);
        preset_.morphAmount = clampf(amount, 0.0F, 1.0F);
        rtMorphAmount_.store(preset_.morphAmount, std::memory_order_relaxed);
        current = preset_;
    }
    set_preset(current);
}

// Phase 3: generative sequencer accessors.
Sequencer& Synthesizer::sequencer() noexcept { return impl_->sequencer; }
const Sequencer& Synthesizer::sequencer() const noexcept { return impl_->sequencer; }

// Phase 3: generative conductor accessors.
GenerativeConductor& Synthesizer::generative_conductor() noexcept { return impl_->conductor; }
const GenerativeConductor& Synthesizer::generative_conductor() const noexcept {
    return impl_->conductor;
}
void Synthesizer::set_generative_conductor_enabled(bool enabled) noexcept {
    impl_->conductor.set_enabled(enabled);
}
bool Synthesizer::generative_conductor_enabled() const noexcept {
    return impl_->conductor.enabled();
}
void Synthesizer::set_conductor_cutoff_multiplier(float multiplier) noexcept {
    impl_->conductorCutoffMultiplier =
        std::clamp(multiplier, 0.125F, 8.0F);  // +/-3 octaves, never zero/negative
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

std::uint64_t Synthesizer::wavetable_cook_count() const noexcept {
    return impl_->wavetableCookCount_.load(std::memory_order_relaxed);
}

void Synthesizer::reset_granular_profiler() noexcept {
    impl_->requestedGrains.store(0U, std::memory_order_relaxed);
    impl_->admittedGrains.store(0U, std::memory_order_relaxed);
    impl_->grainSteals.store(0U, std::memory_order_relaxed);
    impl_->grainMisses.store(0U, std::memory_order_relaxed);
    impl_->samplePageUnderruns.store(0U, std::memory_order_relaxed);
    impl_->maximumActiveGrains.store(impl_->activeGrainTelemetry.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
    impl_->sampleStreamCache.reset_metrics();
}

SynthProfiler Synthesizer::profiler() const noexcept {
    return {impl_->profilerRenderCalls.load(std::memory_order_relaxed),
            impl_->profilerSamplesRendered.load(std::memory_order_relaxed),
            impl_->profilerVoicesStarted.load(std::memory_order_relaxed),
            impl_->profilerVoicesStolen.load(std::memory_order_relaxed),
            impl_->profilerVoicesRetired.load(std::memory_order_relaxed),
            impl_->profilerOscillatorVoiceFrames.load(std::memory_order_relaxed),
            impl_->profilerFilterFrames.load(std::memory_order_relaxed),
            impl_->profilerFxFrames.load(std::memory_order_relaxed),
            impl_->profilerArpSteps.load(std::memory_order_relaxed),
            impl_->droppedMidi.load(std::memory_order_relaxed),
            impl_->profilerActiveVoices.load(std::memory_order_relaxed),
            impl_->profilerMaximumActiveVoices.load(std::memory_order_relaxed)};
}

void Synthesizer::reset_profiler() noexcept {
    impl_->profilerRenderCalls.store(0U, std::memory_order_relaxed);
    impl_->profilerSamplesRendered.store(0U, std::memory_order_relaxed);
    impl_->profilerVoicesStarted.store(0U, std::memory_order_relaxed);
    impl_->profilerVoicesStolen.store(0U, std::memory_order_relaxed);
    impl_->profilerVoicesRetired.store(0U, std::memory_order_relaxed);
    impl_->profilerOscillatorVoiceFrames.store(0U, std::memory_order_relaxed);
    impl_->profilerFilterFrames.store(0U, std::memory_order_relaxed);
    impl_->profilerFxFrames.store(0U, std::memory_order_relaxed);
    impl_->profilerArpSteps.store(0U, std::memory_order_relaxed);
    impl_->profilerActiveVoices.store(0U, std::memory_order_relaxed);
    impl_->profilerMaximumActiveVoices.store(impl_->profilerActiveVoices.load(std::memory_order_relaxed),
                                            std::memory_order_relaxed);
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
    const DenormalGuard denormalGuard{};
    impl_->adopt_pending_sample_map();
    // Snapshot the audio-thread morph request (the conductor walk below also
    // writes these atomics; the drain sees a consistent snapshot).
    const bool morphRequested = rtMorphEnabled_.load(std::memory_order_relaxed);
    const float morphRequestedAmount = rtMorphAmount_.load(std::memory_order_relaxed);
    PresetUpdate latest{};
    while (impl_->presetIn.pop(latest))
        impl_->adopt_preset(latest, morphRequestedAmount, morphRequested);
    impl_->advance_parameter_smoothing(frameCount);
    impl_->track_tempo_synced_delay();
    // Phase 3: step physics modulation bank.
    impl_->physicsBank.step(static_cast<float>(frameCount) / impl_->sampleRate);
    // Phase 3: generative conductor maps attractor state onto the live synth
    // (no-op unless enabled). Runs before the sequencer so the sequencer
    // advances with this block's mapped scale/density/mutation. The morph
    // walk publishes through request_morph_amount() (lock-free); the result
    // is applied to the cached numeric presets just below.
    impl_->conductor.process(*this, static_cast<double>(frameCount) / impl_->sampleRate);
    impl_->apply_morph_amount(rtMorphAmount_.load(std::memory_order_relaxed),
                              rtMorphEnabled_.load(std::memory_order_relaxed));
    // Phase 3: advance the generative sequencer once per block (no-op unless enabled).
    impl_->advance_sequencer(frameCount);
    if (impl_->transportRestartRequested.exchange(false, std::memory_order_acq_rel)) {
        impl_->clear_arp_held(true);
        impl_->renderFrame = impl_->transportRestartFrame.load(std::memory_order_relaxed);
        impl_->arpStepCounter = 0U;
        impl_->arpProgress = 0U;
        impl_->activeArpStep.store(0U, std::memory_order_relaxed);
        // Phase 3: restart the sequencer with the transport.
        impl_->sequencer.start();
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
    // Profiler stage activity, accumulated locally and published once per render call to keep
    // atomics out of the per-frame hot loop.
    std::uint64_t blockOscillatorVoiceFrames = 0;
    std::uint64_t blockFilterFrames = 0;
    std::uint64_t blockFxFrames = 0;
    const bool filterEnabled = impl_->parameters.filter.enabled;
    const bool anyFxEnabled = impl_->parameters.distortion.enabled || impl_->parameters.bitcrusher.enabled ||
        impl_->parameters.harmonizer.enabled || impl_->parameters.eq.enabled || impl_->parameters.chorus.enabled ||
        impl_->parameters.flanger.enabled || impl_->parameters.ensemble.enabled || impl_->parameters.phaser.enabled ||
        impl_->parameters.delay.enabled || impl_->parameters.diffusionDelay.enabled || impl_->parameters.reverb.enabled ||
        impl_->parameters.compressor.enabled || impl_->parameters.limiter.enabled;
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const std::uint64_t absoluteFrame = blockStart + frame;
        impl_->renderFrame = absoluteFrame;
        while (eventIndex < pendingCount && pending[eventIndex].sampleFrame <= absoluteFrame)
            impl_->handle_midi(pending[eventIndex++]);
        impl_->process_scheduled_events();
        impl_->advance_arpeggiator();
        float left = 0.0F; float right = 0.0F;
        std::uint32_t frameActiveVoices = 0;
        for (Voice& voice : impl_->voice) {
            if (voice.active) ++frameActiveVoices;
            const auto [voiceLeft, voiceRight] = impl_->render_voice(voice);
            left += voiceLeft; right += voiceRight;
        }
        blockOscillatorVoiceFrames += frameActiveVoices;
        if (filterEnabled && frameActiveVoices > 0U) ++blockFilterFrames;
        if (anyFxEnabled) ++blockFxFrames;
        const float channelVolume = impl_->controller[7] > 0.0F ? impl_->controller[7] : 1.0F;
        const float panController = impl_->controller[10];
        const float smoothedMaster = impl_->smoothedMasterGain.current;
        if (panController <= 0.0F) {
            left *= smoothedMaster * channelVolume * impl_->parameters.masterPanLeft;
            right *= smoothedMaster * channelVolume * impl_->parameters.masterPanRight;
        } else {
            const float masterPan = clampf(impl_->parameters.masterPan + panController * 2.0F - 1.0F, -1.0F, 1.0F);
            left *= smoothedMaster * channelVolume * std::sqrt(0.5F * (1.0F - masterPan));
            right *= smoothedMaster * channelVolume * std::sqrt(0.5F * (1.0F + masterPan));
        }
        impl_->effects(left, right);
        if (!finite(left)) left = 0.0F;
        if (!finite(right)) right = 0.0F;
        output[frame * 2U] = left;
        output[frame * 2U + 1U] = right;
        peakLeft = std::max(peakLeft, std::abs(left)); peakRight = std::max(peakRight, std::abs(right));
        squareLeft += static_cast<double>(left) * left; squareRight += static_cast<double>(right) * right;
    }
    // Phase 4: publish granular engine activity once per block (spawn/steal/
    // miss counters are forwarded per spawn event in the voice path above).
    std::uint32_t blockActiveGrains = 0U;
    for (const Voice& blockVoice : impl_->voice)
        for (const auto& engine : blockVoice.granularEngines)
            blockActiveGrains += engine.active_grain_count();
    impl_->activeGrainTelemetry.store(blockActiveGrains, std::memory_order_relaxed);
    std::uint32_t observedGrainMax = impl_->maximumActiveGrains.load(std::memory_order_relaxed);
    while (blockActiveGrains > observedGrainMax &&
           !impl_->maximumActiveGrains.compare_exchange_weak(observedGrainMax, blockActiveGrains,
                                                             std::memory_order_relaxed)) {
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
    impl_->profilerRenderCalls.fetch_add(1U, std::memory_order_relaxed);
    impl_->profilerSamplesRendered.fetch_add(frameCount, std::memory_order_relaxed);
    impl_->profilerOscillatorVoiceFrames.fetch_add(blockOscillatorVoiceFrames, std::memory_order_relaxed);
    impl_->profilerFilterFrames.fetch_add(blockFilterFrames, std::memory_order_relaxed);
    impl_->profilerFxFrames.fetch_add(blockFxFrames, std::memory_order_relaxed);
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
