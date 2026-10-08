#pragma once
#include "dve/audio/audio_clock.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/audio/midi.hpp"
#include "dve/audio/sample_map.hpp"
#include "dve/audio/sequencer.hpp"
#include "dve/audio/attractor.hpp"
#include "dve/audio/granular.hpp"
#include "dve/audio/spectral.hpp"  // Phase 5: spectral resynthesis oscillator (SYN-015)
#include "dve/audio/synth_profiler.hpp"

namespace dve::audio {

// Forward declaration (full definition in dve/audio/generative_conductor.hpp;
// the .cpp includes it, so the header stays light).
class GenerativeConductor;

inline constexpr std::size_t kSynthOscillatorCount = 8;
inline constexpr std::size_t kSynthVoiceCount = 16;
inline constexpr std::size_t kChordIntervalCount = 8;
inline constexpr std::size_t kChordMemorySlotCount = 8;
inline constexpr std::size_t kArpeggiatorStepCount = 16;
inline constexpr std::size_t kSynthLfoCount = 2;
inline constexpr std::size_t kSynthModulationSlotCount = 16;
inline constexpr std::size_t kSynthMacroCount = 4;
inline constexpr std::size_t kSynthMidiLearnCount = 8;
inline constexpr std::size_t kWavetableFrameCount = 8;
inline constexpr std::size_t kWavetableSampleCount = 128;
inline constexpr std::size_t kWavetableMipCount = 5;
inline constexpr std::size_t kMicrotuningNoteCount = 128;
inline constexpr std::size_t kSynthUnisonMax = 4;
inline constexpr std::size_t kSynthSampleMaxFrames = 16384;
inline constexpr std::size_t kSynthGrainsPerOscillator = 8;
inline constexpr std::size_t kPhysicalModelMaxDelay = 4096;
inline constexpr std::size_t kPhysicalModelMaxModes = 16;
inline constexpr std::size_t kModalResonatorMaxModes = 32;
inline constexpr std::uint32_t kDefaultSynthSampleRate = 48000;

// Phase 2: auto oversampling policy thresholds (see effective_oversampling).
// Resonance above this upgrades an explicit X1 setting to X2.
inline constexpr float kAutoOversampleResonanceThreshold = 0.75F;
// Drive above this counts as "drive engaged" and upgrades X1 to X2.
inline constexpr float kAutoOversampleDriveThreshold = 1.5F;
// Resonance above this, combined with engaged drive, upgrades X1 to X4.
inline constexpr float kAutoOversampleExtremeResonanceThreshold = 0.95F;

enum class OscillatorWaveform : std::uint8_t {
    Sine,
    Saw,
    Square,
    Triangle,
    Pulse,
    Noise,
    SuperSaw,
    Organ,
    FoldedSine,
    Digital,
    Wavetable,
    Sample,
    Granular,
    PhysicalModel,
    Sampler,  // Phase 2: dedicated sampler generator (preset-level SamplerParameters)
    ModalResonator,  // Phase 2: bank of damped modal resonators (SYN-011b)
    Spectral,  // Phase 5: spectral resynthesis oscillator (SYN-015; appended, never renumbered)
};

// Phase 2: sampler playback mode and direction.
enum class SamplerPlaybackMode : std::uint8_t { OneShot, Loop };
enum class SamplerDirection : std::uint8_t { Forward, Reverse };

enum class PhysicalModelType : std::uint8_t {
    WaveguideString,
    ModalPlate,
    Hybrid,
};

enum class PhysicalMaterial : std::uint8_t {
    String,
    Metal,
    Brass,
    Glass,
    Wood,
    Membrane,
    Synthetic,
};

enum class PhysicalExcitation : std::uint8_t {
    Pluck,
    Strike,
    Bow,
    Blow,
};

// Yamaha VL1-style driver models for wind / brass
enum class PhysicalDriver : std::uint8_t {
    Disabled,   // used for string / plate / mallet
    Reed,       // clarinet, sax, oboe (single/double reed)
    Lip,        // trumpet, trombone, horn (lip reed)
    Jet,        // flute, recorder, organ flue
};

// Phase 2: what strikes the modal resonator bank at note-on.
enum class ExcitationSource : std::uint8_t {
    Impulse,        // one-shot displacement of every mode (classic modal synthesis)
    NoiseBurst,     // short burst of white noise injected through the mode inputs
    Oscillator,     // dedicated internal sawtooth exciter, continuous while the key is held
    SampleTransient,// attack portion of the preset's resident sample bank (falls back to noise)
};

struct ModalResonatorMode {
    float frequencyRatio{1.0F};  // partial ratio relative to the base frequency
    float decaySeconds{1.0F};     // per-mode decay time constant (envelope ~ e^(-t/decay))
    float gain{1.0F};             // per-mode gain before the brightness tilt
};

// Phase 2: modal resonator voice generator parameters (SYN-011b). Each mode is a
// damped 2-pole resonator; the output is the gain-weighted sum of the modes.
//
// Formulas (all defined here so UI and DSP agree):
//   stretched ratio: ratio' = ratio * sqrt(1 + inharmonicity * (ratio*ratio - 1))
//     inharmonicity is clamped to [0, 1]; ratio == 1 is untouched for any value.
//   effective decay: tau_m = clamp(mode.decaySeconds, 0.005, 60) * clamp(damping, 0.01, 8)
//     damping == 1 leaves decay times unchanged; < 1 shortens, > 1 lengthens.
//     A mode's envelope decays as e^(-t/tau_m); its -60 dB time is tau_m * ln(1000).
//   brightness tilt: gain_m = mode.gain * (m+1)^(2*(brightness - 0.5)), m = 0-based index
//     brightness is clamped to [0, 1]; 0.5 is flat, 0 darkens, 1 brightens.
//   baseFrequency == 0 means "follow the played note" (MIDI pitch is authoritative);
//     otherwise the bank is fixed at baseFrequency Hz.
struct ModalResonatorParameters {
    std::uint8_t modeCount{8};
    std::array<ModalResonatorMode, kModalResonatorMaxModes> modes{};
    float baseFrequency{};       // Hz; 0 = follow the played note
    float damping{1.0F};         // multiplier applied to every mode's decay time
    float inharmonicity{};       // partial stretch; 0 = harmonic
    float brightness{0.5F};      // gain tilt across mode index; 0.5 = flat
    ExcitationSource excitation{ExcitationSource::Impulse};
    float excitationLevel{1.0F}; // overall excitation energy
    float noiseBurstMilliseconds{40.0F};  // NoiseBurst window length
    float transientMilliseconds{60.0F};   // SampleTransient window length

    static ModalResonatorParameters make_default();
};

enum class FilterMode : std::uint8_t { LowPass, BandPass, HighPass, Notch };
// Phase 2: Comb and Formant appended at the end; existing values are never
// renumbered so serialized presets keep their meaning.
enum class FilterTopology : std::uint8_t { CleanStateVariable, MoogLadder, KorgMs20, OberheimSem, Comb, Formant };
enum class EnvelopeCurve : std::uint8_t { Linear, Exponential };
enum class VoiceStage : std::uint8_t { Idle, Delay, Attack, Hold, Decay, Sustain, Release };

enum class ArpeggiatorMode : std::uint8_t { Up, Down, UpDown, DownUp, Played, Random, Chord };
enum class ArpeggiatorDivision : std::uint8_t {
    Quarter,
    Eighth,
    EighthTriplet,
    Sixteenth,
    SixteenthTriplet,
    ThirtySecond,
    DottedEighth,
    DottedQuarter,
    SixtyFourth,
};


enum class LfoWaveform : std::uint8_t { Sine, Triangle, Saw, Square, SampleAndHold, SmoothRandom };
enum class ModulationCurve : std::uint8_t { Linear, Quadratic, Cubic };
enum class FrequencyModulationMode : std::uint8_t { Off, Linear, Exponential };
enum class FilterOversampling : std::uint8_t { X1 = 1, X2 = 2, X4 = 4, Auto = 0 };
enum class OscillatorQuality : std::uint8_t { Normal, High, Offline };
// FilterQuality lives in dve/audio/granular.hpp (shared with the granular engine).
enum class ModulationPolarity : std::uint8_t { Bipolar, Unipolar };
enum class MpeZoneMode : std::uint8_t { Off, Lower, Upper, Dual };
enum class ArpeggiatorCondition : std::uint8_t { Unconditional, Every2, Every3, Every4, FirstOf4, Fill, AB };
enum class StepAutomationCurve : std::uint8_t { Step, Linear, Smooth };
enum class ArpeggiatorClockSource : std::uint8_t { Internal, GameClock, MidiClock };
enum class ChordScale : std::uint8_t { Chromatic, Major, NaturalMinor, HarmonicMinor, Dorian, Mixolydian, Pentatonic };
enum class GrainWindow : std::uint8_t { Hann, Triangle, Tukey };

enum class ModulationSource : std::uint8_t {
    Off, Lfo1, Lfo2, AmpEnvelope, FilterEnvelope, Velocity, KeyTrack,
    ModWheel, Aftertouch, Random, Macro1, Macro2, Macro3, Macro4,
    Timbre, NotePitchBend, ReleaseVelocity,
    Spring, Pendulum, Orbiter, Lorenz,  // Phase 1: physics modulation
    SeqTimbre, SeqMorph, SeqPan  // Phase 3: generative sequencer lane currents (appended)
};

enum class ModulationDestination : std::uint8_t {
    Off, GlobalPitch, FilterCutoff, FilterResonance, FilterDrive, VoiceGain, VoicePan,
    Osc1Pitch, Osc2Pitch, Osc3Pitch, Osc4Pitch, Osc5Pitch, Osc6Pitch, Osc7Pitch, Osc8Pitch,
    Osc1Shape, Osc2Shape, Osc3Shape, Osc4Shape, Osc5Shape, Osc6Shape, Osc7Shape, Osc8Shape,
    Osc1PulseWidth, Osc2PulseWidth, Osc3PulseWidth, Osc4PulseWidth,
    Osc5PulseWidth, Osc6PulseWidth, Osc7PulseWidth, Osc8PulseWidth,
    Osc1Gain, Osc2Gain, Osc3Gain, Osc4Gain, Osc5Gain, Osc6Gain, Osc7Gain, Osc8Gain,
    WavetablePosition, MorphAmount,  // Phase 1: added
    SamplerStartPosition,  // Phase 2: added (sampler start offset, seconds)
    GranularPosition  // Phase 4: added (grain source position, 0..1 over the sample bank)
};

enum class ChordType : std::uint8_t {
    Custom,
    Major,
    Minor,
    Diminished,
    Augmented,
    Sus2,
    Sus4,
    Fifth,
    Major6,
    Minor6,
    Dominant7,
    Major7,
    Minor7,
    Diminished7,
    Add9,
    Minor9,
};

struct AdsrParameters {
    float attackSeconds{0.005F};
    float decaySeconds{0.16F};
    float sustainLevel{0.72F};
    float releaseSeconds{0.35F};
    EnvelopeCurve curve{EnvelopeCurve::Exponential};
    float delaySeconds{};
    float holdSeconds{};
};

struct OscillatorParameters {
    bool enabled{true};
    OscillatorWaveform waveform{OscillatorWaveform::Saw};
    float gain{0.125F};
    float pan{};
    float semitones{};
    float cents{};
    float pulseWidth{0.5F};
    float pwmDepth{0.0F};
    float pwmRateHertz{0.35F};
    float shape{0.5F};
    float phaseOffset{};
    bool keySync{true};
    std::int8_t hardSyncSource{-1};
    std::int8_t frequencyModSource{-1};
    FrequencyModulationMode frequencyModMode{FrequencyModulationMode::Off};
    float frequencyModAmount{};
    std::int8_t ringModSource{-1};
    float ringModDepth{};
    float subOscillatorLevel{};
    std::uint8_t subOscillatorOctaves{1};
    float wavetablePosition{};
    float stereoDivergence{};  // Phase 1: 0=mono, 1=full L/R divergence (detune + phase)

    // Resident sample/granular source controls. Positions are normalized to the shared
    // preset sample bank. Asset loading and resampling occur on the control thread.
    float sampleStart{};
    float sampleEnd{1.0F};
    float sampleLoopStart{};
    float sampleLoopEnd{1.0F};
    bool sampleLoop{};
    bool sampleReverse{};
    bool sampleOneShot{true};
    bool sampleKeyTrack{true};
    float sampleVelocityToGain{1.0F};
    float grainPosition{0.5F};
    float grainSizeMilliseconds{65.0F};
    float grainDensityHertz{18.0F};
    float grainSpray{0.12F};
    float grainPitchSemitones{};
    float grainStereoSpread{0.65F};
    float grainStereoMotion{};
    float grainEnvelopeCurve{1.0F};
    float grainReverseProbability{};
    float grainPitchRandomSemitones{};
    float grainPitchQuantizeSemitones{};
    float grainDensityVelocity{};
    float grainDensityTimbre{};
    bool grainFreeze{};
    GrainWindow grainWindow{GrainWindow::Hann};

    // Physical modeling (waveform == PhysicalModel)
    PhysicalModelType physicalModel{PhysicalModelType::Hybrid};
    PhysicalMaterial physicalMaterial{PhysicalMaterial::String};
    PhysicalExcitation physicalExcitation{PhysicalExcitation::Pluck};
    float physicalSizeMeters{0.65F};
    float physicalTension{0.72F};
    float physicalStiffness{0.18F};
    float physicalDamping{0.35F};
    float physicalBrightness{0.62F};
    float physicalExcitationPosition{0.28F};
    float physicalHardness{0.45F};
    float physicalPickupPosition{0.42F};
    float physicalBodyAmount{0.28F};
    float physicalContinuousAmount{0.0F};
    float physicalVelocityToHardness{0.55F};
    float physicalPressureToBow{0.85F};
    float physicalTimbreToBrightness{0.40F};
    std::uint8_t physicalModeCount{12};

    // VL1 / VL70-m style wind & brass
    PhysicalDriver physicalDriver{PhysicalDriver::Disabled};
    float physicalReedStiffness{0.45F};     // 0=soft reed, 1=hard
    float physicalEmbouchure{0.50F};        // lip/reed opening / pressure balance
    float physicalBreathNoise{0.18F};       // amount of turbulent breath noise
    float physicalThroatFreqHz{1800.0F};    // formant / throat resonance
    float physicalThroatQ{2.5F};            // formant resonance sharpness
    float physicalBoreTaper{0.0F};          // 0=cylindrical, 1=conical (sax-like)
    float physicalPressureToBreath{0.90F};  // aftertouch/pressure -> continuous breath
    float physicalVelocityToEmbouchure{0.35F};

    // Phase 2: modal resonator (waveform == ModalResonator)
    ModalResonatorParameters modalResonator = ModalResonatorParameters::make_default();
};

// Phase 2: parameters for the Comb filter topology. The comb spacing is set by
// the voice cutoff (comb frequency = cutoff Hz); resonance drives the feedback
// amount. Trivially copyable so RealtimePreset stays trivially copyable.
struct CombParameters {
    float damping{0.25F};       // 0..1: lowpass damping inside the feedback loop (higher = darker)
    float mix{1.0F};            // 0..1: dry/wet mix (1 = fully resonant comb)
    float feedbackScale{1.0F};  // 0..1.5: scales the resonance -> feedback mapping
};

// Phase 2: parameters for the Formant filter topology. A bank of parallel
// resonant bandpass biquads tuned to an open "ah" vowel (F1..F4 after
// Klatt/Stevens vocal-tract data); the voice cutoff sweeps the whole bank
// multiplicatively (cutoff 1000 Hz = authored frequencies), resonance sets
// the band Q. Trivially copyable so RealtimePreset stays trivially copyable.
struct FormantParameters {
    static constexpr std::size_t kBandCount{4};
    std::array<float, kBandCount> frequencyHertz{730.0F, 1090.0F, 2440.0F, 3500.0F};
    std::array<float, kBandCount> gains{1.0F, 0.75F, 0.45F, 0.30F};
    float dryMix{0.25F};        // 0..1: direct signal blended under the formant bank
};

struct FilterParameters {
    bool enabled{true};
    FilterTopology topology{FilterTopology::MoogLadder};
    FilterMode mode{FilterMode::LowPass};
    float cutoffHertz{9000.0F};
    float resonance{0.15F};
    float envelopeAmountOctaves{1.5F};
    float keyTrack{0.35F};
    float drive{1.0F};
    float bassCompensation{0.35F};
    float morph{0.0F};
    bool alternateRevision{};
    AdsrParameters envelope{0.01F, 0.20F, 0.25F, 0.30F, EnvelopeCurve::Exponential};
    FilterOversampling oversampling{FilterOversampling::X1};
    float ms20HighPassCutoffHertz{35.0F};
    float selfOscillation{0.85F};
    CombParameters comb{};
    FormantParameters formant{};
};

struct TuningParameters {
    float referenceHertz{440.0F};
    float transposeSemitones{};
    float fineCents{};
    float analogDriftCents{0.75F};
};

struct LfoParameters {
    bool enabled{};
    LfoWaveform waveform{LfoWaveform::Sine};
    float rateHertz{1.0F};
    float depth{1.0F};
    float phase{};
    float fadeInSeconds{};
    bool keySync{true};
    bool tempoSync{};
    float beatsPerCycle{1.0F};
};

struct ModulationSlot {
    bool enabled{};
    ModulationSource source{ModulationSource::Off};
    ModulationDestination destination{ModulationDestination::Off};
    float amount{};
    float bias{};  // Phase 0: constant offset added to the routed value (-1..1)
    ModulationCurve curve{ModulationCurve::Linear};
    ModulationPolarity polarity{ModulationPolarity::Bipolar};
    float smoothingMilliseconds{8.0F};
};



struct SynthSampleBank {
    std::string name{"No Sample"};
    bool enabled{};
    std::uint32_t sampleRate{kDefaultSynthSampleRate};
    std::uint8_t rootNote{60};
    std::uint32_t frameCount{};
    std::array<float, kSynthSampleMaxFrames> samples{};
    std::uint64_t contentHash{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

// Phase 2: sampler voice generator. Playback reads from the shared preset
// sample bank (SynthSampleBank) — the same resident buffer the Sample
// oscillator uses. Asset loading and resampling occur on the control thread;
// the render path only reads the cooked bank. loopStart/loopEnd are in
// seconds and clamped to the sample duration at render time.
struct SamplerParameters {
    bool enabled{true};
    std::uint8_t sampleIndex{};  // Selects within the shared preset sample bank (single sample today).
    SamplerPlaybackMode playbackMode{SamplerPlaybackMode::OneShot};
    SamplerDirection direction{SamplerDirection::Forward};
    float loopStartSeconds{};
    float loopEndSeconds{1.0F};
    float loopCrossfadeSeconds{0.005F};
    bool pitchTracking{true};
    float startOffsetSeconds{};
    float gain{0.8F};
};

struct MpeParameters {
    MpeZoneMode zoneMode{MpeZoneMode::Off};
    std::uint8_t lowerMasterChannel{0};
    std::uint8_t lowerMemberCount{15};
    std::uint8_t upperMasterChannel{15};
    std::uint8_t upperMemberCount{};
    float masterPitchBendRangeSemitones{2.0F};
    float memberPitchBendRangeSemitones{48.0F};
    std::uint8_t timbreController{74};
    bool masterSustainToMembers{true};
};

struct MicrotuningTable {
    bool enabled{};
    std::string name{"12-TET"};
    std::uint8_t referenceNote{69};
    float referenceHertz{440.0F};
    std::array<float, kMicrotuningNoteCount> centsOffset{};
    std::uint64_t contentHash{};

    static MicrotuningTable equal_temperament(float referenceHertz = 440.0F,
                                               std::uint8_t referenceNote = 69) noexcept;
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] float frequency(std::uint8_t note, float additionalSemitones = 0.0F) const noexcept;
};

struct UnisonParameters {
    bool enabled{};
    std::uint8_t voices{1};
    float detuneCents{9.0F};
    float stereoSpread{0.65F};
    float phaseSpread{0.17F};
    bool preserveLevel{true};
};

struct SynthPresetMetadata {
    std::string author{"DVE"};
    std::string category{"Synth"};
    std::string version{"1.27"};
    std::array<std::string, 8> tags{};
    std::uint8_t tagCount{};
    bool favorite{};
};

struct MacroControls {
    std::array<float, kSynthMacroCount> values{};
    std::array<std::string, kSynthMacroCount> names{"Macro 1", "Macro 2", "Macro 3", "Macro 4"};
};

struct MidiLearnMapping {
    bool enabled{};
    std::uint8_t controller{};
    std::uint8_t macroIndex{};
    float minimum{};
    float maximum{1.0F};
    bool inverted{};
};

struct WavetableBank {
    std::string name{"Basic Morph"};
    bool enabled{};
    std::uint8_t frameCount{4};
    std::array<float, kWavetableFrameCount * kWavetableSampleCount> samples{};
    std::uint64_t contentHash{};

    static WavetableBank make_default();
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};


struct ChordMemorySlot {
    std::string name{"User Chord"};
    std::array<std::int8_t, kChordIntervalCount> intervals{0, 4, 7, 12, 16, 19, 24, 28};
    std::uint8_t noteCount{3};
};

struct ChordDetection {
    bool matched{};
    ChordType type{ChordType::Custom};
    std::uint8_t rootPitchClass{};
    std::int8_t inversion{};
    float confidence{};
};

struct ChordParameters {
    bool enabled{};
    ChordType type{ChordType::Major};
    std::array<std::int8_t, kChordIntervalCount> customIntervals{0, 4, 7, 12, 16, 19, 24, 28};
    std::uint8_t noteCount{3};
    std::int8_t inversion{};
    std::uint8_t spreadOctaves{};
    float velocityScale{0.92F};
    ChordScale scale{ChordScale::Chromatic};
    std::uint8_t scaleRoot{};
    float strumMilliseconds{};
    bool useMemory{};
    std::uint8_t memorySlot{};
};

struct ArpeggiatorStep {
    bool enabled{true};
    ArpeggiatorCondition condition{ArpeggiatorCondition::Unconditional};
    std::uint8_t conditionA{1};
    std::uint8_t conditionB{2};
    StepAutomationCurve automationCurve{StepAutomationCurve::Step};
    bool accent{};
    bool slide{};
    std::int8_t transpose{};
    std::int8_t octaveOffset{};
    float velocityScale{1.0F};
    float gateScale{1.0F};
    float probability{1.0F};
    std::uint8_t ratchets{1};
    bool tie{};
    float macro1{-1.0F};
    float macro2{-1.0F};
    float macro3{-1.0F};
    float macro4{-1.0F};
};

struct ArpeggiatorParameters {
    bool enabled{};
    bool latch{};
    bool sendMidiOutput{true};
    bool retriggerEnvelopes{true};
    ArpeggiatorMode mode{ArpeggiatorMode::Up};
    ArpeggiatorDivision division{ArpeggiatorDivision::Sixteenth};
    float tempoBpm{120.0F};
    float gate{0.68F};
    float swing{};
    std::uint8_t octaveRange{1};
    std::uint8_t stepCount{8};
    std::uint32_t randomSeed{0x51A3D8E7U};
    ArpeggiatorClockSource clockSource{ArpeggiatorClockSource::Internal};
    float externalTempoBpm{120.0F};
    float humanizeTiming{};
    float humanizeVelocity{};
    ChordScale scale{ChordScale::Chromatic};
    std::uint8_t scaleRoot{};
    float phraseVelocityStart{1.0F};
    float phraseVelocityEnd{1.0F};
    std::array<ArpeggiatorStep, kArpeggiatorStepCount> steps{};
};

enum class DistortionMode : std::uint8_t {
    Classic = 0,   // tanh soft clip (the original drive curve)
    Fuzz = 1,      // asymmetric hard clip + tone filter
    // Phase 2: gentler rational soft clipper x/(1+|x|) — rounder knee and a
    // darker harmonic series than tanh at the same drive.
    SoftClip = 2,
    // Phase 2: triangle wavefolder — overdriven peaks fold back instead of
    // clipping, for hollow/metallic timbres.
    Foldback = 3
};
struct DistortionParameters { bool enabled{}; float drive{1.8F}; float mix{0.15F}; DistortionMode mode{DistortionMode::Classic}; };
struct BitcrusherParameters { bool enabled{}; std::uint8_t bits{12}; std::uint8_t downsample{4}; float mix{0.5F}; };
struct OctaveHarmonizerParameters { bool enabled{}; float subLevel{0.5F}; float upLevel{0.35F}; float mix{0.5F}; };
struct EqParameters { bool enabled{true}; float lowGainDb{}; float midGainDb{}; float highGainDb{}; };
struct ChorusParameters { bool enabled{true}; float rateHertz{0.32F}; float depthMilliseconds{4.0F}; float mix{0.16F}; };
struct FlangerParameters { bool enabled{}; float rateHertz{0.25F}; float depthMilliseconds{2.5F}; float feedback{0.45F}; float mix{0.35F}; };
enum class EnsembleMode : std::uint8_t { I = 0, II = 1, Both = 2 };
struct EnsembleParameters { bool enabled{}; EnsembleMode mode{EnsembleMode::I}; float mix{0.4F}; };
struct PhaserParameters { bool enabled{}; float rateHertz{0.18F}; float depth{0.65F}; float feedback{0.25F}; float mix{0.12F}; };
struct DelayParameters {
    bool enabled{true};
    float timeSeconds{0.31F};
    float feedback{0.28F};
    float mix{0.12F};
    bool pingPong{true};
    // Phase 2: tempo sync. When true, the effective delay time is
    // syncBeats * 60 / effectiveTempoBpm, where the tempo is resolved the
    // same way LFO tempo sync resolves it (arpeggiator.clockSource:
    // Internal -> arpeggiator.tempoBpm, GameClock -> set_game_clock_tempo()
    // value, MidiClock -> incoming MIDI clock tempo), clamped to the delay
    // line's 0.01..1.95 s range. timeSeconds applies when tempoSync is false.
    // syncBeats is beats per repeat: 1 = quarter note, 0.5 = eighth note,
    // 1.5 = dotted quarter, 0.75 = dotted eighth, etc.
    bool tempoSync{};
    float syncBeats{1.0F};
};
struct DiffusionDelayParameters {
    bool enabled{};
    float timeSeconds{0.31F};
    float feedback{0.35F};
    float mix{0.18F};
    // Diffusion amount 0..1. 0 gives distinct repeats (plain-echo-like);
    // higher values smear every repeat through cascaded allpass stages for a
    // reverb-ish wash. Note the diffusion stages sit in the wet path, so they
    // add ~60 ms of group delay on top of timeSeconds.
    float diffusion{0.65F};
};
struct ReverbParameters { bool enabled{true}; float roomSize{0.62F}; float damping{0.42F}; float width{0.85F}; float mix{0.18F}; };
struct CompressorParameters { bool enabled{true}; float thresholdDb{-12.0F}; float ratio{3.0F}; float attackMilliseconds{8.0F}; float releaseMilliseconds{90.0F}; float makeupDb{1.5F}; };
struct LimiterParameters { bool enabled{true}; float ceilingDb{-0.4F}; float releaseMilliseconds{45.0F}; };

// Phase 3 (SYN-013): patch-genetics authoring state, serialized with the
// preset. Drives the editor's Mutate/Breed controls; mutate_preset() takes
// these as explicit arguments, so the struct is a convenience carrier.
struct SynthGeneticsSettings {
    float mutationIntensity{0.25F};   // 0..1 editor mutate intensity
    std::uint64_t mutationSeed{0x12345678ULL};
    std::uint8_t lockedGroups{0};    // bitmask over GeneGroup (gene_group_bit)

    bool operator==(const SynthGeneticsSettings&) const = default;
};

// Phase 3: attractor/conductor authoring state, serialized with the preset.
// When enabled, Synthesizer::render() runs the GenerativeConductor, which
// advances the AttractorSequencer and maps its state onto the live synth.
struct SynthAttractorSettings {
    bool enabled{false};
    AttractorConfig config{};

    bool operator==(const SynthAttractorSettings&) const = default;
};

// Phase 5: SpectralParameters is the canonical preset-level spectral/
// resynthesis parameter block defined in dve/audio/spectral.hpp (the
// oscillator-native contract: the realtime engine reads these field names).
// The earlier duplicate definition here was removed at the Phase 5 merge;
// SynthPreset::spectral and RealtimePreset::spectral use that single type.

struct SynthPreset {
    std::string name{"DVE Eightfold Hybrid"};
    std::array<OscillatorParameters, kSynthOscillatorCount> oscillators{};
    AdsrParameters ampEnvelope{};
    FilterParameters filter{};
    TuningParameters tuning{};
    ChordParameters chord{};
    std::array<ChordMemorySlot, kChordMemorySlotCount> chordMemory{};
    ArpeggiatorParameters arpeggiator{};
    std::array<LfoParameters, kSynthLfoCount> lfos{};
    std::array<ModulationSlot, kSynthModulationSlotCount> modulation{};
    MacroControls macros{};
    std::array<MidiLearnMapping, kSynthMidiLearnCount> midiLearn{};
    WavetableBank wavetable{};
    SynthSampleBank sampleBank{};
    SamplerParameters sampler{};  // Phase 2: dedicated sampler generator
    GranularParameters granular{};  // Phase 4: dedicated granular generator (SYN-014)
    SpectralParameters spectral{};  // Phase 5: spectral/resynthesis oscillator (SYN-015)
    // Phase 5: non-owning view of the cooked spectral asset the Spectral
    // waveform resynthesizes. nullptr = silence. Lifetime is owned by the
    // caller (preset/serialization layer, wave-2 workers); the voice never
    // owns or frees it.
    const SpectralAssetView* spectralAsset{nullptr};
    MpeParameters mpe{};
    MicrotuningTable microtuning{};
    UnisonParameters unison{};
    OscillatorQuality oscillatorQuality{OscillatorQuality::Normal};
    FilterQuality filterQuality{FilterQuality::Standard};
    SynthPresetMetadata metadata{};
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
    float masterGain{0.72F};
    float masterPan{};
    float pitchBendRangeSemitones{2.0F};
    bool midiThru{};
    // Phase 1: A/B patch morphing. When enabled, the preset is interpolated
    // with morphPresetB by morphAmount (0=A, 1=B). morphAmount is realtime
    // controllable and a modulation destination.
    bool morphEnabled{false};
    float morphAmount{0.0F};
    // Note: morphPresetB is stored separately (not in serialized text) to
    // keep patch files small. Set via Synthesizer::set_morph_preset_b().
    // Phase 3: generative sequencer patch state (serializable authoring
    // config; applied to the live Sequencer by Synthesizer::set_preset).
    SequencerConfig sequencer{};
    // Phase 3: patch genetics authoring state (editor mutate/breed controls).
    SynthGeneticsSettings genetics{};
    // Phase 3: attractor/conductor authoring state.
    SynthAttractorSettings attractor{};
    static SynthPreset make_default();
    // Factory bank of musically voiced presets. Every pitched preset is voiced
    // so the played MIDI note is the perceived fundamental (no sub-oscillator
    // stack drowning the fundamental an octave down).
    static std::vector<SynthPreset> builtin_presets();
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] std::string serialize() const;
    static std::optional<SynthPreset> parse(std::string_view text, std::string* error = nullptr);
    bool save(const std::filesystem::path& path, std::string* error = nullptr) const;
    static std::optional<SynthPreset> load(const std::filesystem::path& path, std::string* error = nullptr);
};

struct SynthVoiceInfo {
    bool active{};
    std::uint8_t channel{};
    std::uint8_t note{};
    float velocity{};
    float envelope{};
    float pressure{};
    float timbre{};
    float pitchBendSemitones{};
    VoiceStage stage{VoiceStage::Idle};
    std::uint64_t age{};
};

struct SynthModulationInfo {
    bool enabled{};
    ModulationSource source{ModulationSource::Off};
    ModulationDestination destination{ModulationDestination::Off};
    ModulationPolarity polarity{ModulationPolarity::Bipolar};
    float amount{};
    float currentValue{};
};

struct SynthGranularProfiler {
    std::uint64_t requestedGrains{};
    std::uint64_t admittedGrains{};
    std::uint64_t grainSteals{};
    std::uint64_t grainMisses{};
    std::uint64_t pageUnderruns{};
    std::uint32_t activeGrains{};
    std::uint32_t maximumActiveGrains{};
    SampleStreamMetrics stream{};
};

struct SynthMeters {
    float peakLeft{};
    float peakRight{};
    float rmsLeft{};
    float rmsRight{};
    std::uint32_t activeVoices{};
    std::uint32_t heldArpeggiatorNotes{};
    std::uint32_t arpeggiatorStep{};
    std::uint64_t renderedFrames{};
    std::uint64_t droppedMidiMessages{};
};

// Polyphonic 8-oscillator / 16-voice instrument. All real-time state is fixed-capacity. The
// render path performs no heap allocation, file access, logging, or operating-system locking.
class Synthesizer {
public:
    explicit Synthesizer(std::uint32_t sampleRate = kDefaultSynthSampleRate);
    ~Synthesizer();

    Synthesizer(const Synthesizer&) = delete;
    Synthesizer& operator=(const Synthesizer&) = delete;

    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sampleRate_; }
    [[nodiscard]] std::uint64_t current_frame() const noexcept { return currentFrame_.load(std::memory_order_relaxed); }
    // UI thread: returns a copy of the authored preset under the preset lock.
    // (Copies are cheap here; the audio thread never touches this.)
    [[nodiscard]] SynthPreset preset() const;
    // UI thread only: validates, stores, and publishes the preset to the
    // render thread through a lock-free latest-wins mailbox (the newest
    // preset always wins; superseded ones are coalesced, never the newest).
    // Any changed HQ wavetable is cooked here, on the caller's thread, and
    // handed to the render thread atomically. Never call from the audio
    // thread (it copies strings, cooks, and may block on the preset mutex).
    void set_preset(const SynthPreset& preset);
    [[nodiscard]] bool set_sample_map(const SynthSampleMap& sampleMap,
                                      std::string* error = nullptr);
    void clear_sample_map();
    [[nodiscard]] std::uint64_t sample_map_generation() const noexcept;
    [[nodiscard]] bool publish_sample_stream_page(std::uint8_t sourceIndex,
                                                  std::uint32_t firstFrame,
                                                  std::span<const float> monoFrames) noexcept;
    [[nodiscard]] SynthGranularProfiler granular_profiler() const noexcept;
    void set_granular_runtime_quality(GranularRuntimeQuality quality) noexcept;
    [[nodiscard]] GranularRuntimeQuality granular_runtime_quality() const noexcept;
    // Test introspection: how many times the HQ wavetable was cooked (on the
    // set_preset caller's thread; should stay flat across morph walks /
    // repeated set_preset, and never advance inside render()).
    [[nodiscard]] std::uint64_t wavetable_cook_count() const noexcept;
    // Test introspection: presets superseded by a newer set_preset before the
    // render thread picked them up.
    [[nodiscard]] std::uint64_t coalesced_preset_count() const noexcept;
    void reset_granular_profiler() noexcept;
    [[nodiscard]] SynthProfiler profiler() const noexcept;
    void reset_profiler() noexcept;
    void all_notes_off(bool immediate = false) noexcept;

    // Post from game/editor/MIDI threads. sampleFrame==0 means the next render quantum.
    bool post_midi(MidiMessage message) noexcept;
    void publish_audio_clock(AudioClockAnchor anchor) noexcept { audioClock_.publish(anchor); }
    [[nodiscard]] const AudioClock& audio_clock() const noexcept { return audioClock_; }
    bool note_on(std::uint8_t note, float velocity = 1.0F, std::uint8_t channel = 0,
                 std::uint64_t sampleFrame = 0) noexcept;
    bool note_off(std::uint8_t note, float velocity = 0.0F, std::uint8_t channel = 0,
                  std::uint64_t sampleFrame = 0) noexcept;
    bool control_change(std::uint8_t controller, std::uint8_t value, std::uint8_t channel = 0,
                        std::uint64_t sampleFrame = 0) noexcept;
    bool pitch_bend(std::int16_t centeredValue, std::uint8_t channel = 0,
                    std::uint64_t sampleFrame = 0) noexcept;
    void set_game_clock_tempo(float bpm) noexcept;
    // Publish on pause, speed, seek or tempo changes. Beat phase is independent of tempo.
    bool set_game_music_clock(GameMusicClockAnchor anchor) noexcept;
    void set_arpeggiator_fill(bool enabled) noexcept;
    void restart_performance_transport(std::uint64_t sampleFrame = 0) noexcept;
    bool post_midi_clock(std::uint64_t sampleFrame = 0) noexcept;

    // Interleaved stereo float output. The call may be made with any frame count.
    void render(std::span<float> interleavedStereo) noexcept;
    void render(float* interleavedStereo, std::size_t frameCount) noexcept;

    [[nodiscard]] std::array<SynthVoiceInfo, kSynthVoiceCount> voices() const noexcept;
    [[nodiscard]] SynthMeters meters() const noexcept;
    [[nodiscard]] std::array<SynthModulationInfo, kSynthModulationSlotCount> modulation_activity() const noexcept;
    void set_morph_preset_b(const SynthPreset& presetB);
    void clear_morph_preset_b();
    [[nodiscard]] bool has_morph_preset_b() const;
    // UI thread only: sets the authored morph amount and re-publishes the
    // preset. The audio-thread morph walk uses request_morph_amount() instead.
    void set_morph_amount(float amount);
    // Realtime-safe: publishes a morph-amount request from the audio thread
    // (generative conductor walk). The render thread applies it to the cached
    // numeric presets — no string copies, no allocation, no locks.
    void request_morph_amount(float amount) noexcept;
    [[nodiscard]] bool morph_enabled_rt() const noexcept {
        return rtMorphEnabled_.load(std::memory_order_relaxed);
    }
    // Lock-free handoff for the conductor's attractor config, published by
    // set_preset() (UI thread) and drained by GenerativeConductor::process()
    // (audio/test thread). Lets the conductor use the preset's config even
    // when render() hasn't run yet to apply the queued PresetUpdate; the
    // render-thread path in adopt_preset() is unchanged. Returns null when
    // no new config was published since the last call.
    struct PendingConductorConfig {
        bool enabled = false;
        AttractorConfig config{};
    };
    [[nodiscard]] std::shared_ptr<const PendingConductorConfig> take_pending_conductor_config() noexcept;

    // Phase 3: generative step sequencer (SYN-012). Configure lanes via the
    // returned object; it advances once per render() block when enabled.
    [[nodiscard]] Sequencer& sequencer() noexcept;
    [[nodiscard]] const Sequencer& sequencer() const noexcept;

    // Phase 3: generative conductor (attractor -> live synth mapping).
    // Disabled by default; enabling is a no-op unless the loaded preset
    // carries attractor settings. When enabled, each render() block advances
    // the attractor and maps its state onto the sequencer (scale/root,
    // probability-lane scaling, mutation amount), the morph amount (smooth
    // seeded random walk) and the filter cutoff (brightness multiplier).
    [[nodiscard]] GenerativeConductor& generative_conductor() noexcept;
    [[nodiscard]] const GenerativeConductor& generative_conductor() const noexcept;
    void set_generative_conductor_enabled(bool enabled) noexcept;
    [[nodiscard]] bool generative_conductor_enabled() const noexcept;
    // Conductor-driven filter cutoff multiplier around the base cutoff
    // (1.0 = no change). Written by the conductor from the render thread.
    void set_conductor_cutoff_multiplier(float multiplier) noexcept;

    bool poll_midi_output(MidiMessage& message) noexcept;

private:
    AudioClock audioClock_;
    struct Impl;
    Impl* impl_{};
    std::uint32_t sampleRate_{};
    std::atomic<GranularRuntimeQuality> rtGranularQuality_{GranularRuntimeQuality::Inherit};
    SynthPreset preset_{};
    SynthPreset morphPresetB_{};
    bool hasMorphPresetB_{false};
    // Guards preset_, morphPresetB_, hasMorphPresetB_ against the audio
    // thread's realtime morph requests. Only the UI thread takes this lock;
    // the render path uses the lock-free presetIn queue and the atomics below.
    mutable std::mutex presetMutex_;
    // Audio-thread morph walk target, published by request_morph_amount().
    std::atomic<float> rtMorphAmount_{0.0F};
    // Mirrors (preset_.morphEnabled && hasMorphPresetB_) for the audio thread.
    std::atomic<bool> rtMorphEnabled_{false};
    std::atomic<std::uint64_t> currentFrame_{};
    std::atomic<std::uint64_t> nextMidiSequence_{1};
    std::atomic<std::uint64_t> writableFrame_{};
    // Pending conductor attractor config, published by set_preset() and
    // drained by GenerativeConductor::process(). Single-producer (UI thread)
    // / single-consumer (audio thread); the shared_ptr handoff keeps the
    // audio thread lock-free and race-free.
    std::atomic<std::shared_ptr<const PendingConductorConfig>> pendingConductorConfig_{nullptr};
};

[[nodiscard]] std::string_view oscillator_waveform_name(OscillatorWaveform waveform) noexcept;
[[nodiscard]] std::string_view modal_excitation_source_name(ExcitationSource source) noexcept;
[[nodiscard]] std::string_view filter_topology_name(FilterTopology topology) noexcept;
// Phase 2: auto oversampling policy for the voice filter. When
// FilterParameters::oversampling is Auto, the effective rate is X1 by default,
// upgraded to X2 when resonance exceeds kAutoOversampleResonanceThreshold or
// drive is engaged (drive > kAutoOversampleDriveThreshold), and to X4 when
// resonance exceeds kAutoOversampleExtremeResonanceThreshold with drive
// engaged. An explicit X1/X2/X4 setting is always honored exactly (so existing
// presets and the oversampling regression test are unaffected); Auto presets
// without a trigger condition render bit-identical to X1.
[[nodiscard]] FilterOversampling effective_oversampling(const FilterParameters& params) noexcept;
[[nodiscard]] std::string_view filter_mode_name(FilterMode mode) noexcept;
[[nodiscard]] std::string_view arpeggiator_mode_name(ArpeggiatorMode mode) noexcept;
[[nodiscard]] std::string_view arpeggiator_division_name(ArpeggiatorDivision division) noexcept;
[[nodiscard]] std::string_view chord_type_name(ChordType type) noexcept;
[[nodiscard]] ChordDetection detect_chord(std::span<const std::uint8_t> notes) noexcept;
[[nodiscard]] std::string_view lfo_waveform_name(LfoWaveform waveform) noexcept;
[[nodiscard]] std::string_view modulation_source_name(ModulationSource source) noexcept;
[[nodiscard]] std::string_view modulation_destination_name(ModulationDestination destination) noexcept;
[[nodiscard]] std::string_view sampler_playback_mode_name(SamplerPlaybackMode mode) noexcept;
[[nodiscard]] std::string_view sampler_direction_name(SamplerDirection direction) noexcept;

[[nodiscard]] std::optional<MicrotuningTable> import_scala_tuning(
    const std::filesystem::path& scalaPath,
    const std::optional<std::filesystem::path>& keyboardMapPath = std::nullopt,
    std::string* error = nullptr);
bool apply_midi_tuning_standard(std::span<const std::uint8_t> message,
                                MicrotuningTable& table, std::string* error = nullptr);

[[nodiscard]] std::optional<SynthSampleBank> import_synth_sample_from_audio(
    const std::filesystem::path& path, std::uint8_t rootNote = 60,
    std::string* error = nullptr);
[[nodiscard]] std::optional<WavetableBank> import_wavetable_from_audio(
    const std::filesystem::path& path, std::string* error = nullptr);
[[nodiscard]] std::optional<WavetableBank> import_wavetable_frames(
    std::span<const std::filesystem::path> paths, std::string* error = nullptr);
bool wavetable_remove_dc(WavetableBank& bank) noexcept;
bool wavetable_normalize(WavetableBank& bank) noexcept;
bool wavetable_align_phases(WavetableBank& bank) noexcept;
bool wavetable_draw_frame(WavetableBank& bank, std::size_t frame,
                          std::span<const float> samples, std::string* error = nullptr);
bool wavetable_spectral_morph(WavetableBank& bank, std::size_t frameA, std::size_t frameB,
                              std::size_t destinationFrame, float amount,
                              std::string* error = nullptr);
bool capture_chord_memory(SynthPreset& preset, std::size_t slot,
                          std::span<const std::uint8_t> notes, std::string_view name,
                          std::string* error = nullptr);
[[nodiscard]] std::vector<std::string> diff_synth_presets(const SynthPreset& a, const SynthPreset& b);

[[nodiscard]] SynthPreset morph_synth_presets(const SynthPreset& a, const SynthPreset& b, float amount);
bool export_arpeggiator_midi_file(const SynthPreset& preset, const std::filesystem::path& path,
                                  std::uint8_t rootNote = 60, std::string* error = nullptr);

struct SynthPresetEntry {
    std::filesystem::path path;
    std::string name;
    std::vector<std::string> tags;
    std::string author;
    std::string category;
    std::string version;
    bool favorite{};
};

class SynthPresetLibrary {
public:
    bool scan(const std::filesystem::path& directory, std::string* error = nullptr);
    [[nodiscard]] const std::vector<SynthPresetEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::vector<std::size_t> find_by_tag(std::string_view tag) const;
private:
    std::vector<SynthPresetEntry> entries_;
};

// Writes 32-bit IEEE float stereo WAV. Used by deterministic tests and useful for headless
// preset auditioning and regression renders.
bool write_float_wav(const std::filesystem::path& path, std::span<const float> interleavedStereo,
                     std::uint32_t sampleRate, std::string* error = nullptr);

} // namespace dve::audio
