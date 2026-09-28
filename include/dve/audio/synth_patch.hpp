// Compiled synth patch format (Phase 0 of the DVE Advanced Synthesizer plan).
//
// The text .dvesynth format is the human-readable authoring format. This header
// defines the compiled runtime representation: a compact binary blob with
// resolved integer parameter IDs and no string lookups in the load path.
//
// Binary layout (little-endian):
//   magic[4]      = "DVSP" (DVE Synth Patch)
//   version u16   = kSynthPatchFormatVersion
//   flags u16     = reserved (0)
//   entry_count u32
//   entries: { param_id u16, value_type u8, value... }
//     value_type 0 = float  (4 bytes)
//     value_type 1 = uint32 (4 bytes)
//     value_type 2 = bool   (1 byte, 0/1)
//     value_type 3 = string (u16 length + bytes, no NUL)
//   crc32 u32     = checksum of everything before it
//
// Parameter IDs are stable across versions. New parameters get new IDs;
// removed parameters keep their IDs reserved. The loader skips unknown IDs
// so newer patches degrade gracefully on older builds.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dve/audio/synthesizer.hpp"

namespace dve::audio {

inline constexpr std::uint16_t kSynthPatchFormatVersion = 1;
inline constexpr std::uint32_t kSynthPatchMagic = 0x50535644U; // "DVSP" little-endian

// Parameter IDs. Ranges:
//   0x0000-0x00FF  master / tuning
//   0x0100-0x01FF  oscillators
//   0x0200-0x02FF  envelopes / filter
//   0x0300-0x03FF  chord / arp
//   0x0400-0x04FF  LFO / modulation / macros
//   0x0500-0x05FF  effects
//   0x0600-0x06FF  MPE / microtuning / unison / quality
//   0x0700-0x07FF  metadata
//   0x0800-0x08FF  granular generator (Phase 4)
enum class SynthPatchParam : std::uint16_t {
    // Master / tuning
    Name = 0x0001,
    MasterGain = 0x0002,
    MasterPan = 0x0003,
    PitchBendRange = 0x0004,
    MidiThru = 0x0005,
    ReferenceHertz = 0x0010,
    TransposeSemitones = 0x0011,
    FineCents = 0x0012,
    AnalogDriftCents = 0x0013,
    OscillatorQuality = 0x0014,
    FilterQuality = 0x0015,
    // Envelopes / filter (per-oscillator params use indexed IDs below)
    AmpAttack = 0x0200,
    AmpDecay = 0x0201,
    AmpSustain = 0x0202,
    AmpRelease = 0x0203,
    AmpCurve = 0x0204,
    AmpDelay = 0x0205,
    AmpHold = 0x0206,
    FilterEnabled = 0x0210,
    FilterTopology = 0x0211,
    FilterMode = 0x0212,
    FilterCutoff = 0x0213,
    FilterResonance = 0x0214,
    FilterEnvAmount = 0x0215,
    FilterKeyTrack = 0x0216,
    FilterDrive = 0x0217,
    // Chord / arp
    ChordEnabled = 0x0300,
    ChordType = 0x0301,
    ChordInversion = 0x0302,
    ChordSpread = 0x0303,
    ChordScale = 0x0304,
    ChordScaleRoot = 0x0305,
    ChordStrumMs = 0x0306,
    ChordVelocityScale = 0x0307,
    ArpEnabled = 0x0310,
    ArpMode = 0x0311,
    ArpDivision = 0x0312,
    ArpTempoBpm = 0x0313,
    ArpGate = 0x0314,
    ArpSwing = 0x0315,
    ArpOctaveRange = 0x0316,
    ArpStepCount = 0x0317,
    ArpSeed = 0x0318,
    ArpClock = 0x0319,
    ArpExternalTempo = 0x031A,
    ArpHumanizeTiming = 0x031B,
    ArpHumanizeVelocity = 0x031C,
    ArpScale = 0x031D,
    ArpScaleRoot = 0x031E,
    ArpPhraseVelStart = 0x031F,
    ArpPhraseVelEnd = 0x0320,
    ArpLatch = 0x0321,
    ArpRetrigger = 0x0322,
    // Effects (enabled flag + key params; full detail lives in text format)
    FxDistortionOn = 0x0500,
    FxDistortionDrive = 0x0501,
    FxDistortionMix = 0x0502,
    FxDistortionMode = 0x0503,
    FxBitcrusherOn = 0x0504,
    FxBitcrusherBits = 0x0505,
    FxBitcrusherDownsample = 0x0506,
    FxBitcrusherMix = 0x0507,
    FxHarmonizerOn = 0x0508,
    FxHarmonizerSub = 0x0509,
    FxHarmonizerUp = 0x050A,
    FxHarmonizerMix = 0x050B,
    FxEqOn = 0x050C,
    FxEqLow = 0x050D,
    FxEqMid = 0x050E,
    FxEqHigh = 0x050F,
    FxChorusOn = 0x0510,
    FxChorusRate = 0x0511,
    FxChorusDepth = 0x0512,
    FxChorusMix = 0x0513,
    FxFlangerOn = 0x0514,
    FxFlangerRate = 0x0515,
    FxFlangerDepth = 0x0516,
    FxFlangerFeedback = 0x0517,
    FxFlangerMix = 0x0518,
    FxEnsembleOn = 0x0519,
    FxEnsembleMode = 0x051A,
    FxEnsembleMix = 0x051B,
    FxPhaserOn = 0x051C,
    FxPhaserRate = 0x051D,
    FxPhaserDepth = 0x051E,
    FxPhaserFeedback = 0x051F,
    FxPhaserMix = 0x0520,
    FxDelayOn = 0x0521,
    FxDelayTime = 0x0522,
    FxDelayFeedback = 0x0523,
    FxDelayMix = 0x0524,
    FxDelayPingPong = 0x0525,
    FxReverbOn = 0x0526,
    FxReverbRoom = 0x0527,
    FxReverbDamping = 0x0528,
    FxReverbWidth = 0x0529,
    FxReverbMix = 0x052A,
    FxCompressorOn = 0x052B,
    FxCompThreshold = 0x052C,
    FxCompRatio = 0x052D,
    FxCompAttack = 0x052E,
    FxCompRelease = 0x052F,
    FxCompMakeup = 0x0530,
    FxLimiterOn = 0x0531,
    FxLimiterCeiling = 0x0532,
    FxLimiterRelease = 0x0533,
    // Granular generator (Phase 4). Appended at the enum end so older builds
    // skip these IDs gracefully (the loader's default case skips unknown IDs).
    GranularEnabled = 0x0800,
    GranularDensityHz = 0x0801,
    GranularDurationMs = 0x0802,
    GranularPitchSemitones = 0x0803,
    GranularPosition = 0x0804,
    GranularPositionJitter = 0x0805,
    GranularPanScatter = 0x0806,
    GranularGain = 0x0807,
    GranularReverseProbability = 0x0808,
    GranularEnvelopeShape = 0x0809,
    GranularCloud = 0x080A,
    GranularScatter = 0x080B,
    GranularDust = 0x080C,
    GranularFreeze = 0x080D,
    GranularFreezePosition = 0x080E,
    GranularSmear = 0x080F,
    GranularWidth = 0x0810,
    GranularQuality = 0x0811,
};

struct SynthPatchProgram {
    std::vector<std::uint8_t> bytes;
    [[nodiscard]] bool valid() const noexcept { return !bytes.empty(); }
};

// Compiles a preset into the binary runtime patch. The result is
// self-contained and versioned; loading it performs no string lookups.
[[nodiscard]] SynthPatchProgram compile_patch(const SynthPreset& preset);

// Loads a compiled patch back into a preset. Returns nullopt (with *error
// set when non-null) if the magic, version, or checksum is bad. Unknown
// parameter IDs are skipped so newer patches load on older builds.
[[nodiscard]] std::optional<SynthPreset> load_patch_program(
    const std::uint8_t* data, std::size_t size, std::string* error = nullptr);

inline std::optional<SynthPreset> load_patch_program(
    const SynthPatchProgram& program, std::string* error = nullptr) {
    return load_patch_program(program.bytes.data(), program.bytes.size(), error);
}

} // namespace dve::audio
