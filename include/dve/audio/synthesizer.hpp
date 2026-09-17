#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/audio/midi.hpp"
#include "dve/audio/sample_map.hpp"

namespace dve::audio {

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
inline constexpr std::uint32_t kDefaultSynthSampleRate = 48000;

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
};

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

enum class FilterMode : std::uint8_t { LowPass, BandPass, HighPass, Notch };
enum class FilterTopology : std::uint8_t { CleanStateVariable, MoogLadder, KorgMs20, OberheimSem };
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
};


enum class LfoWaveform : std::uint8_t { Sine, Triangle, Saw, Square, SampleAndHold, SmoothRandom };
enum class ModulationCurve : std::uint8_t { Linear, Quadratic, Cubic };
enum class FrequencyModulationMode : std::uint8_t { Off, Linear, Exponential };
enum class FilterOversampling : std::uint8_t { X1 = 1, X2 = 2, X4 = 4 };
enum class OscillatorQuality : std::uint8_t { Normal, High, Offline };
enum class FilterQuality : std::uint8_t { Eco, Standard, High, Offline };
enum class ModulationPolarity : std::uint8_t { Bipolar, Unipolar };
enum class MpeZoneMode : std::uint8_t { Off, Lower, Upper, Dual };
enum class ArpeggiatorCondition : std::uint8_t { Unconditional, Every2, Every3, Every4, FirstOf4, Fill };
enum class StepAutomationCurve : std::uint8_t { Step, Linear, Smooth };
enum class ArpeggiatorClockSource : std::uint8_t { Internal, GameClock, MidiClock };
enum class ChordScale : std::uint8_t { Chromatic, Major, NaturalMinor, HarmonicMinor, Dorian, Mixolydian, Pentatonic };
enum class GrainWindow : std::uint8_t { Hann, Triangle, Tukey };

enum class ModulationSource : std::uint8_t {
    Off, Lfo1, Lfo2, AmpEnvelope, FilterEnvelope, Velocity, KeyTrack,
    ModWheel, Aftertouch, Random, Macro1, Macro2, Macro3, Macro4
};

enum class ModulationDestination : std::uint8_t {
    Off, GlobalPitch, FilterCutoff, FilterResonance, FilterDrive, VoiceGain, VoicePan,
    Osc1Pitch, Osc2Pitch, Osc3Pitch, Osc4Pitch, Osc5Pitch, Osc6Pitch, Osc7Pitch, Osc8Pitch,
    Osc1Shape, Osc2Shape, Osc3Shape, Osc4Shape, Osc5Shape, Osc6Shape, Osc7Shape, Osc8Shape,
    Osc1PulseWidth, Osc2PulseWidth, Osc3PulseWidth, Osc4PulseWidth,
    Osc5PulseWidth, Osc6PulseWidth, Osc7PulseWidth, Osc8PulseWidth,
    Osc1Gain, Osc2Gain, Osc3Gain, Osc4Gain, Osc5Gain, Osc6Gain, Osc7Gain, Osc8Gain
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
    std::array<ArpeggiatorStep, kArpeggiatorStepCount> steps{};
};

struct DistortionParameters { bool enabled{}; float drive{1.8F}; float mix{0.15F}; };
struct EqParameters { bool enabled{true}; float lowGainDb{}; float midGainDb{}; float highGainDb{}; };
struct ChorusParameters { bool enabled{true}; float rateHertz{0.32F}; float depthMilliseconds{4.0F}; float mix{0.16F}; };
struct PhaserParameters { bool enabled{}; float rateHertz{0.18F}; float depth{0.65F}; float feedback{0.25F}; float mix{0.12F}; };
struct DelayParameters { bool enabled{true}; float timeSeconds{0.31F}; float feedback{0.28F}; float mix{0.12F}; bool pingPong{true}; };
struct ReverbParameters { bool enabled{true}; float roomSize{0.62F}; float damping{0.42F}; float width{0.85F}; float mix{0.18F}; };
struct CompressorParameters { bool enabled{true}; float thresholdDb{-12.0F}; float ratio{3.0F}; float attackMilliseconds{8.0F}; float releaseMilliseconds{90.0F}; float makeupDb{1.5F}; };
struct LimiterParameters { bool enabled{true}; float ceilingDb{-0.4F}; float releaseMilliseconds{45.0F}; };

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
    MpeParameters mpe{};
    MicrotuningTable microtuning{};
    UnisonParameters unison{};
    OscillatorQuality oscillatorQuality{OscillatorQuality::Normal};
    FilterQuality filterQuality{FilterQuality::Standard};
    SynthPresetMetadata metadata{};
    DistortionParameters distortion{};
    EqParameters eq{};
    ChorusParameters chorus{};
    PhaserParameters phaser{};
    DelayParameters delay{};
    ReverbParameters reverb{};
    CompressorParameters compressor{};
    LimiterParameters limiter{};
    float masterGain{0.72F};
    float masterPan{};
    float pitchBendRangeSemitones{2.0F};
    bool midiThru{};

    static SynthPreset make_default();
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
    [[nodiscard]] const SynthPreset& preset() const noexcept { return preset_; }
    void set_preset(const SynthPreset& preset);
    [[nodiscard]] bool set_sample_map(const SynthSampleMap& sampleMap,
                                      std::string* error = nullptr);
    void clear_sample_map();
    [[nodiscard]] std::uint64_t sample_map_generation() const noexcept;
    [[nodiscard]] bool publish_sample_stream_page(std::uint8_t sourceIndex,
                                                  std::uint32_t firstFrame,
                                                  std::span<const float> monoFrames) noexcept;
    [[nodiscard]] SynthGranularProfiler granular_profiler() const noexcept;
    void reset_granular_profiler() noexcept;
    void all_notes_off(bool immediate = false) noexcept;

    // Post from game/editor/MIDI threads. sampleFrame==0 means the next render quantum.
    bool post_midi(MidiMessage message) noexcept;
    bool note_on(std::uint8_t note, float velocity = 1.0F, std::uint8_t channel = 0,
                 std::uint64_t sampleFrame = 0) noexcept;
    bool note_off(std::uint8_t note, float velocity = 0.0F, std::uint8_t channel = 0,
                  std::uint64_t sampleFrame = 0) noexcept;
    bool control_change(std::uint8_t controller, std::uint8_t value, std::uint8_t channel = 0,
                        std::uint64_t sampleFrame = 0) noexcept;
    bool pitch_bend(std::int16_t centeredValue, std::uint8_t channel = 0,
                    std::uint64_t sampleFrame = 0) noexcept;
    void set_game_clock_tempo(float bpm) noexcept;
    void set_arpeggiator_fill(bool enabled) noexcept;
    void restart_performance_transport(std::uint64_t sampleFrame = 0) noexcept;
    bool post_midi_clock(std::uint64_t sampleFrame = 0) noexcept;

    // Interleaved stereo float output. The call may be made with any frame count.
    void render(std::span<float> interleavedStereo) noexcept;
    void render(float* interleavedStereo, std::size_t frameCount) noexcept;

    [[nodiscard]] std::array<SynthVoiceInfo, kSynthVoiceCount> voices() const noexcept;
    [[nodiscard]] SynthMeters meters() const noexcept;
    [[nodiscard]] std::array<SynthModulationInfo, kSynthModulationSlotCount> modulation_activity() const noexcept;
    bool poll_midi_output(MidiMessage& message) noexcept;

private:
    struct Impl;
    Impl* impl_{};
    std::uint32_t sampleRate_{};
    SynthPreset preset_{};
    std::atomic<std::uint64_t> currentFrame_{};
};

[[nodiscard]] std::string_view oscillator_waveform_name(OscillatorWaveform waveform) noexcept;
[[nodiscard]] std::string_view filter_topology_name(FilterTopology topology) noexcept;
[[nodiscard]] std::string_view filter_mode_name(FilterMode mode) noexcept;
[[nodiscard]] std::string_view arpeggiator_mode_name(ArpeggiatorMode mode) noexcept;
[[nodiscard]] std::string_view arpeggiator_division_name(ArpeggiatorDivision division) noexcept;
[[nodiscard]] std::string_view chord_type_name(ChordType type) noexcept;
[[nodiscard]] ChordDetection detect_chord(std::span<const std::uint8_t> notes) noexcept;
[[nodiscard]] std::string_view lfo_waveform_name(LfoWaveform waveform) noexcept;
[[nodiscard]] std::string_view modulation_source_name(ModulationSource source) noexcept;
[[nodiscard]] std::string_view modulation_destination_name(ModulationDestination destination) noexcept;

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
