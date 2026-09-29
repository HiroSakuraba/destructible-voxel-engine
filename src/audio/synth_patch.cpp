// Compiled synth patch format implementation.
#include "dve/audio/synth_patch.hpp"

#include <cstring>

#include "dve/audio/synthesizer.hpp"

namespace dve::audio {
namespace {

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
    }
    return crc ^ 0xFFFFFFFFU;
}

class Writer {
public:
    std::vector<std::uint8_t> bytes;
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u16(std::uint16_t v) { u8(static_cast<std::uint8_t>(v)); u8(static_cast<std::uint8_t>(v >> 8)); }
    void u32(std::uint32_t v) { for (int s = 0; s < 32; s += 8) u8(static_cast<std::uint8_t>(v >> s)); }
    void f32(float v) { std::uint32_t u; std::memcpy(&u, &v, 4); u32(u); }
    void entry(SynthPatchParam id, float v) {
        u16(static_cast<std::uint16_t>(id)); u8(0); f32(v);
    }
    void entry(SynthPatchParam id, std::uint32_t v) {
        u16(static_cast<std::uint16_t>(id)); u8(1); u32(v);
    }
    void entry(SynthPatchParam id, bool v) {
        u16(static_cast<std::uint16_t>(id)); u8(2); u8(v ? 1 : 0);
    }
    void entry(SynthPatchParam id, const std::string& v) {
        u16(static_cast<std::uint16_t>(id)); u8(3);
        u16(static_cast<std::uint16_t>(std::min<std::size_t>(v.size(), 0xFFFF)));
        bytes.insert(bytes.end(), v.begin(), v.begin() + std::min<std::size_t>(v.size(), 0xFFFF));
    }
};

class Reader {
public:
    const std::uint8_t* p;
    std::size_t n;
    bool ok = true;
    std::uint8_t u8() { if (n < 1) { ok = false; return 0; } std::uint8_t v = *p; ++p; --n; return v; }
    std::uint16_t u16() { std::uint16_t a = u8(); std::uint16_t b = u8(); return a | (b << 8); }
    std::uint32_t u32() { std::uint32_t v = 0; for (int s = 0; s < 32; s += 8) v |= static_cast<std::uint32_t>(u8()) << s; return v; }
    float f32() { std::uint32_t u = u32(); float v; std::memcpy(&v, &u, 4); return v; }
};

} // namespace

SynthPatchProgram compile_patch(const SynthPreset& preset) {
    Writer w;
    w.u32(kSynthPatchMagic);
    w.u16(kSynthPatchFormatVersion);
    w.u16(0); // flags
    const std::size_t countPos = w.bytes.size();
    w.u32(0); // entry count placeholder

    std::uint32_t count = 0;
    auto bump = [&] { ++count; };

    w.entry(SynthPatchParam::Name, preset.name); bump();
    w.entry(SynthPatchParam::MasterGain, preset.masterGain); bump();
    w.entry(SynthPatchParam::MasterPan, preset.masterPan); bump();
    w.entry(SynthPatchParam::PitchBendRange, preset.pitchBendRangeSemitones); bump();
    w.entry(SynthPatchParam::MidiThru, preset.midiThru); bump();
    w.entry(SynthPatchParam::ReferenceHertz, preset.tuning.referenceHertz); bump();
    w.entry(SynthPatchParam::TransposeSemitones, static_cast<std::uint32_t>(preset.tuning.transposeSemitones)); bump();
    w.entry(SynthPatchParam::FineCents, preset.tuning.fineCents); bump();
    w.entry(SynthPatchParam::AnalogDriftCents, preset.tuning.analogDriftCents); bump();

    w.entry(SynthPatchParam::AmpAttack, preset.ampEnvelope.attackSeconds); bump();
    w.entry(SynthPatchParam::AmpDecay, preset.ampEnvelope.decaySeconds); bump();
    w.entry(SynthPatchParam::AmpSustain, preset.ampEnvelope.sustainLevel); bump();
    w.entry(SynthPatchParam::AmpRelease, preset.ampEnvelope.releaseSeconds); bump();
    w.entry(SynthPatchParam::AmpDelay, preset.ampEnvelope.delaySeconds); bump();
    w.entry(SynthPatchParam::AmpHold, preset.ampEnvelope.holdSeconds); bump();

    w.entry(SynthPatchParam::FilterEnabled, preset.filter.enabled); bump();
    w.entry(SynthPatchParam::FilterCutoff, preset.filter.cutoffHertz); bump();
    w.entry(SynthPatchParam::FilterResonance, preset.filter.resonance); bump();
    w.entry(SynthPatchParam::FilterEnvAmount, preset.filter.envelopeAmountOctaves); bump();
    w.entry(SynthPatchParam::FilterKeyTrack, preset.filter.keyTrack); bump();
    w.entry(SynthPatchParam::FilterDrive, preset.filter.drive); bump();

    w.entry(SynthPatchParam::ChordEnabled, preset.chord.enabled); bump();
    w.entry(SynthPatchParam::ChordStrumMs, preset.chord.strumMilliseconds); bump();
    w.entry(SynthPatchParam::ChordVelocityScale, preset.chord.velocityScale); bump();

    w.entry(SynthPatchParam::ArpEnabled, preset.arpeggiator.enabled); bump();
    w.entry(SynthPatchParam::ArpMode, static_cast<std::uint32_t>(preset.arpeggiator.mode)); bump();
    w.entry(SynthPatchParam::ArpDivision, static_cast<std::uint32_t>(preset.arpeggiator.division)); bump();
    w.entry(SynthPatchParam::ArpTempoBpm, preset.arpeggiator.tempoBpm); bump();
    w.entry(SynthPatchParam::ArpGate, preset.arpeggiator.gate); bump();
    w.entry(SynthPatchParam::ArpSwing, preset.arpeggiator.swing); bump();
    w.entry(SynthPatchParam::ArpOctaveRange, static_cast<std::uint32_t>(preset.arpeggiator.octaveRange)); bump();
    w.entry(SynthPatchParam::ArpStepCount, static_cast<std::uint32_t>(preset.arpeggiator.stepCount)); bump();
    w.entry(SynthPatchParam::ArpHumanizeTiming, preset.arpeggiator.humanizeTiming); bump();
    w.entry(SynthPatchParam::ArpHumanizeVelocity, preset.arpeggiator.humanizeVelocity); bump();
    w.entry(SynthPatchParam::ArpPhraseVelStart, preset.arpeggiator.phraseVelocityStart); bump();
    w.entry(SynthPatchParam::ArpPhraseVelEnd, preset.arpeggiator.phraseVelocityEnd); bump();
    w.entry(SynthPatchParam::ArpLatch, preset.arpeggiator.latch); bump();
    w.entry(SynthPatchParam::ArpRetrigger, preset.arpeggiator.retriggerEnvelopes); bump();

    // Effects
    w.entry(SynthPatchParam::FxDistortionOn, preset.distortion.enabled); bump();
    w.entry(SynthPatchParam::FxDistortionDrive, preset.distortion.drive); bump();
    w.entry(SynthPatchParam::FxDistortionMix, preset.distortion.mix); bump();
    w.entry(SynthPatchParam::FxDistortionMode, static_cast<std::uint32_t>(preset.distortion.mode)); bump();
    w.entry(SynthPatchParam::FxBitcrusherOn, preset.bitcrusher.enabled); bump();
    w.entry(SynthPatchParam::FxBitcrusherBits, static_cast<std::uint32_t>(preset.bitcrusher.bits)); bump();
    w.entry(SynthPatchParam::FxBitcrusherDownsample, static_cast<std::uint32_t>(preset.bitcrusher.downsample)); bump();
    w.entry(SynthPatchParam::FxBitcrusherMix, preset.bitcrusher.mix); bump();
    w.entry(SynthPatchParam::FxHarmonizerOn, preset.harmonizer.enabled); bump();
    w.entry(SynthPatchParam::FxHarmonizerSub, preset.harmonizer.subLevel); bump();
    w.entry(SynthPatchParam::FxHarmonizerUp, preset.harmonizer.upLevel); bump();
    w.entry(SynthPatchParam::FxHarmonizerMix, preset.harmonizer.mix); bump();
    w.entry(SynthPatchParam::FxEqOn, preset.eq.enabled); bump();
    w.entry(SynthPatchParam::FxEqLow, preset.eq.lowGainDb); bump();
    w.entry(SynthPatchParam::FxEqMid, preset.eq.midGainDb); bump();
    w.entry(SynthPatchParam::FxEqHigh, preset.eq.highGainDb); bump();
    w.entry(SynthPatchParam::FxChorusOn, preset.chorus.enabled); bump();
    w.entry(SynthPatchParam::FxChorusRate, preset.chorus.rateHertz); bump();
    w.entry(SynthPatchParam::FxChorusDepth, preset.chorus.depthMilliseconds); bump();
    w.entry(SynthPatchParam::FxChorusMix, preset.chorus.mix); bump();
    w.entry(SynthPatchParam::FxFlangerOn, preset.flanger.enabled); bump();
    w.entry(SynthPatchParam::FxFlangerRate, preset.flanger.rateHertz); bump();
    w.entry(SynthPatchParam::FxFlangerDepth, preset.flanger.depthMilliseconds); bump();
    w.entry(SynthPatchParam::FxFlangerFeedback, preset.flanger.feedback); bump();
    w.entry(SynthPatchParam::FxFlangerMix, preset.flanger.mix); bump();
    w.entry(SynthPatchParam::FxEnsembleOn, preset.ensemble.enabled); bump();
    w.entry(SynthPatchParam::FxEnsembleMode, static_cast<std::uint32_t>(preset.ensemble.mode)); bump();
    w.entry(SynthPatchParam::FxEnsembleMix, preset.ensemble.mix); bump();
    w.entry(SynthPatchParam::FxPhaserOn, preset.phaser.enabled); bump();
    w.entry(SynthPatchParam::FxPhaserRate, preset.phaser.rateHertz); bump();
    w.entry(SynthPatchParam::FxPhaserDepth, preset.phaser.depth); bump();
    w.entry(SynthPatchParam::FxPhaserFeedback, preset.phaser.feedback); bump();
    w.entry(SynthPatchParam::FxPhaserMix, preset.phaser.mix); bump();
    w.entry(SynthPatchParam::FxDelayOn, preset.delay.enabled); bump();
    w.entry(SynthPatchParam::FxDelayTime, preset.delay.timeSeconds); bump();
    w.entry(SynthPatchParam::FxDelayFeedback, preset.delay.feedback); bump();
    w.entry(SynthPatchParam::FxDelayMix, preset.delay.mix); bump();
    w.entry(SynthPatchParam::FxDelayPingPong, preset.delay.pingPong); bump();
    w.entry(SynthPatchParam::FxReverbOn, preset.reverb.enabled); bump();
    w.entry(SynthPatchParam::FxReverbRoom, preset.reverb.roomSize); bump();
    w.entry(SynthPatchParam::FxReverbDamping, preset.reverb.damping); bump();
    w.entry(SynthPatchParam::FxReverbWidth, preset.reverb.width); bump();
    w.entry(SynthPatchParam::FxReverbMix, preset.reverb.mix); bump();
    w.entry(SynthPatchParam::FxCompressorOn, preset.compressor.enabled); bump();
    w.entry(SynthPatchParam::FxCompThreshold, preset.compressor.thresholdDb); bump();
    w.entry(SynthPatchParam::FxCompRatio, preset.compressor.ratio); bump();
    w.entry(SynthPatchParam::FxCompAttack, preset.compressor.attackMilliseconds); bump();
    w.entry(SynthPatchParam::FxCompRelease, preset.compressor.releaseMilliseconds); bump();
    w.entry(SynthPatchParam::FxCompMakeup, preset.compressor.makeupDb); bump();
    w.entry(SynthPatchParam::FxLimiterOn, preset.limiter.enabled); bump();
    w.entry(SynthPatchParam::FxLimiterCeiling, preset.limiter.ceilingDb); bump();
    w.entry(SynthPatchParam::FxLimiterRelease, preset.limiter.releaseMilliseconds); bump();

    // Granular generator (Phase 4)
    const auto& granular = preset.granular;
    w.entry(SynthPatchParam::GranularEnabled, granular.enabled); bump();
    w.entry(SynthPatchParam::GranularDensityHz, granular.densityHz); bump();
    w.entry(SynthPatchParam::GranularDurationMs, granular.durationMs); bump();
    w.entry(SynthPatchParam::GranularPitchSemitones, granular.pitchSemitones); bump();
    w.entry(SynthPatchParam::GranularPosition, granular.position01); bump();
    w.entry(SynthPatchParam::GranularPositionJitter, granular.positionJitter01); bump();
    w.entry(SynthPatchParam::GranularPanScatter, granular.panScatter01); bump();
    w.entry(SynthPatchParam::GranularGain, granular.gain); bump();
    w.entry(SynthPatchParam::GranularReverseProbability, granular.reverseProbability01); bump();
    w.entry(SynthPatchParam::GranularEnvelopeShape,
             static_cast<std::uint32_t>(granular.envelopeShape)); bump();
    w.entry(SynthPatchParam::GranularCloud, granular.cloud01); bump();
    w.entry(SynthPatchParam::GranularScatter, granular.scatter01); bump();
    w.entry(SynthPatchParam::GranularDust, granular.dust01); bump();
    w.entry(SynthPatchParam::GranularFreeze, granular.freeze01); bump();
    w.entry(SynthPatchParam::GranularFreezePosition, granular.freezePosition01); bump();
    w.entry(SynthPatchParam::GranularSmear, granular.smear01); bump();
    w.entry(SynthPatchParam::GranularWidth, granular.width01); bump();
    w.entry(SynthPatchParam::GranularQuality,
             static_cast<std::uint32_t>(granular.granularQuality)); bump();

    // Spectral/resynthesis oscillator (Phase 5). Unified on the
    // oscillator-native SpectralParameters (spectral.hpp) at the Phase 5
    // merge; 0x0901/0x090B are reserved and not written.
    const auto& spectral = preset.spectral;
    w.entry(SynthPatchParam::SpectralEnabled, spectral.enabled); bump();
    w.entry(SynthPatchParam::SpectralGain, spectral.gain); bump();
    w.entry(SynthPatchParam::SpectralFreeze, spectral.freeze01); bump();
    w.entry(SynthPatchParam::SpectralStretch, spectral.timeStretch); bump();
    w.entry(SynthPatchParam::SpectralFormant, spectral.formantShiftSemitones); bump();
    w.entry(SynthPatchParam::SpectralHarmonicStretch, spectral.harmonicStretch); bump();
    w.entry(SynthPatchParam::SpectralTilt, spectral.spectralTiltDbPerOct); bump();
    w.entry(SynthPatchParam::SpectralThreshold, spectral.partialThreshold01); bump();
    w.entry(SynthPatchParam::SpectralBlur, spectral.spectralBlur01); bump();
    w.entry(SynthPatchParam::SpectralQuantize, spectral.frequencyQuantize01); bump();
    w.entry(SynthPatchParam::SpectralInharmonicity, spectral.inharmonicity01); bump();
    w.entry(SynthPatchParam::SpectralPhaseRandom, spectral.phaseRandom01); bump();
    w.entry(SynthPatchParam::SpectralStereoSpread, spectral.stereoSpread01); bump();
    w.entry(SynthPatchParam::SpectralQuality,
             static_cast<std::uint32_t>(spectral.spectralQuality)); bump();

    // Patch entry count
    w.bytes[countPos] = static_cast<std::uint8_t>(count);
    w.bytes[countPos + 1] = static_cast<std::uint8_t>(count >> 8);
    w.bytes[countPos + 2] = static_cast<std::uint8_t>(count >> 16);
    w.bytes[countPos + 3] = static_cast<std::uint8_t>(count >> 24);

    const std::uint32_t crc = crc32(w.bytes.data(), w.bytes.size());
    w.u32(crc);

    SynthPatchProgram program;
    program.bytes = std::move(w.bytes);
    return program;
}

std::optional<SynthPreset> load_patch_program(const std::uint8_t* data, std::size_t size,
                                              std::string* error) {
    auto fail = [&](std::string message) -> std::optional<SynthPreset> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    if (size < 14) return fail("patch program too small");
    const std::uint32_t storedCrc = static_cast<std::uint32_t>(data[size - 4]) |
        (static_cast<std::uint32_t>(data[size - 3]) << 8) |
        (static_cast<std::uint32_t>(data[size - 2]) << 16) |
        (static_cast<std::uint32_t>(data[size - 1]) << 24);
    if (crc32(data, size - 4) != storedCrc) return fail("patch program checksum mismatch");

    Reader r{data, size - 4};
    if (r.u32() != kSynthPatchMagic) return fail("not a DVE synth patch");
    const std::uint16_t version = r.u16();
    if (version != kSynthPatchFormatVersion) return fail("unsupported patch version");
    r.u16(); // flags
    const std::uint32_t entryCount = r.u32();

    SynthPreset preset = SynthPreset::make_default();
    auto readF = [&](float& target) { target = r.f32(); };
    auto readU = [&](auto& target) {
        using T = std::remove_reference_t<decltype(target)>;
        target = static_cast<T>(r.u32());
    };
    auto readB = [&](bool& target) { target = r.u8() != 0; };

    for (std::uint32_t i = 0; i < entryCount && r.ok; ++i) {
        const auto id = static_cast<SynthPatchParam>(r.u16());
        const std::uint8_t type = r.u8();
        switch (id) {
            case SynthPatchParam::Name:
                if (type == 3) { const std::uint16_t len = r.u16(); preset.name.assign(reinterpret_cast<const char*>(r.p), r.ok && r.n >= len ? len : 0); if (r.ok && r.n >= len) { r.p += len; r.n -= len; } }
                else r.ok = false;
                break;
            case SynthPatchParam::MasterGain: if (type == 0) readF(preset.masterGain); else r.ok = false; break;
            case SynthPatchParam::MasterPan: if (type == 0) readF(preset.masterPan); else r.ok = false; break;
            case SynthPatchParam::PitchBendRange: if (type == 0) readF(preset.pitchBendRangeSemitones); else r.ok = false; break;
            case SynthPatchParam::MidiThru: if (type == 2) readB(preset.midiThru); else r.ok = false; break;
            case SynthPatchParam::ReferenceHertz: if (type == 0) readF(preset.tuning.referenceHertz); else r.ok = false; break;
            case SynthPatchParam::TransposeSemitones: if (type == 1) { std::uint32_t v = r.u32(); preset.tuning.transposeSemitones = static_cast<std::int8_t>(v); } else r.ok = false; break;
            case SynthPatchParam::FineCents: if (type == 0) readF(preset.tuning.fineCents); else r.ok = false; break;
            case SynthPatchParam::AnalogDriftCents: if (type == 0) readF(preset.tuning.analogDriftCents); else r.ok = false; break;
            case SynthPatchParam::AmpAttack: if (type == 0) readF(preset.ampEnvelope.attackSeconds); else r.ok = false; break;
            case SynthPatchParam::AmpDecay: if (type == 0) readF(preset.ampEnvelope.decaySeconds); else r.ok = false; break;
            case SynthPatchParam::AmpSustain: if (type == 0) readF(preset.ampEnvelope.sustainLevel); else r.ok = false; break;
            case SynthPatchParam::AmpRelease: if (type == 0) readF(preset.ampEnvelope.releaseSeconds); else r.ok = false; break;
            case SynthPatchParam::AmpDelay: if (type == 0) readF(preset.ampEnvelope.delaySeconds); else r.ok = false; break;
            case SynthPatchParam::AmpHold: if (type == 0) readF(preset.ampEnvelope.holdSeconds); else r.ok = false; break;
            case SynthPatchParam::FilterEnabled: if (type == 2) readB(preset.filter.enabled); else r.ok = false; break;
            case SynthPatchParam::FilterCutoff: if (type == 0) readF(preset.filter.cutoffHertz); else r.ok = false; break;
            case SynthPatchParam::FilterResonance: if (type == 0) readF(preset.filter.resonance); else r.ok = false; break;
            case SynthPatchParam::FilterEnvAmount: if (type == 0) readF(preset.filter.envelopeAmountOctaves); else r.ok = false; break;
            case SynthPatchParam::FilterKeyTrack: if (type == 0) readF(preset.filter.keyTrack); else r.ok = false; break;
            case SynthPatchParam::FilterDrive: if (type == 0) readF(preset.filter.drive); else r.ok = false; break;
            case SynthPatchParam::ChordEnabled: if (type == 2) readB(preset.chord.enabled); else r.ok = false; break;
            case SynthPatchParam::ChordStrumMs: if (type == 0) readF(preset.chord.strumMilliseconds); else r.ok = false; break;
            case SynthPatchParam::ChordVelocityScale: if (type == 0) readF(preset.chord.velocityScale); else r.ok = false; break;
            case SynthPatchParam::ArpEnabled: if (type == 2) readB(preset.arpeggiator.enabled); else r.ok = false; break;
            case SynthPatchParam::ArpMode: if (type == 1) readU(preset.arpeggiator.mode); else r.ok = false; break;
            case SynthPatchParam::ArpDivision: if (type == 1) readU(preset.arpeggiator.division); else r.ok = false; break;
            case SynthPatchParam::ArpTempoBpm: if (type == 0) readF(preset.arpeggiator.tempoBpm); else r.ok = false; break;
            case SynthPatchParam::ArpGate: if (type == 0) readF(preset.arpeggiator.gate); else r.ok = false; break;
            case SynthPatchParam::ArpSwing: if (type == 0) readF(preset.arpeggiator.swing); else r.ok = false; break;
            case SynthPatchParam::ArpOctaveRange: if (type == 1) readU(preset.arpeggiator.octaveRange); else r.ok = false; break;
            case SynthPatchParam::ArpStepCount: if (type == 1) readU(preset.arpeggiator.stepCount); else r.ok = false; break;
            case SynthPatchParam::ArpHumanizeTiming: if (type == 0) readF(preset.arpeggiator.humanizeTiming); else r.ok = false; break;
            case SynthPatchParam::ArpHumanizeVelocity: if (type == 0) readF(preset.arpeggiator.humanizeVelocity); else r.ok = false; break;
            case SynthPatchParam::ArpPhraseVelStart: if (type == 0) readF(preset.arpeggiator.phraseVelocityStart); else r.ok = false; break;
            case SynthPatchParam::ArpPhraseVelEnd: if (type == 0) readF(preset.arpeggiator.phraseVelocityEnd); else r.ok = false; break;
            case SynthPatchParam::ArpLatch: if (type == 2) readB(preset.arpeggiator.latch); else r.ok = false; break;
            case SynthPatchParam::ArpRetrigger: if (type == 2) readB(preset.arpeggiator.retriggerEnvelopes); else r.ok = false; break;
            case SynthPatchParam::FxDistortionOn: if (type == 2) readB(preset.distortion.enabled); else r.ok = false; break;
            case SynthPatchParam::FxDistortionDrive: if (type == 0) readF(preset.distortion.drive); else r.ok = false; break;
            case SynthPatchParam::FxDistortionMix: if (type == 0) readF(preset.distortion.mix); else r.ok = false; break;
            case SynthPatchParam::FxDistortionMode: if (type == 1) readU(preset.distortion.mode); else r.ok = false; break;
            case SynthPatchParam::FxBitcrusherOn: if (type == 2) readB(preset.bitcrusher.enabled); else r.ok = false; break;
            case SynthPatchParam::FxBitcrusherBits: if (type == 1) readU(preset.bitcrusher.bits); else r.ok = false; break;
            case SynthPatchParam::FxBitcrusherDownsample: if (type == 1) readU(preset.bitcrusher.downsample); else r.ok = false; break;
            case SynthPatchParam::FxBitcrusherMix: if (type == 0) readF(preset.bitcrusher.mix); else r.ok = false; break;
            case SynthPatchParam::FxHarmonizerOn: if (type == 2) readB(preset.harmonizer.enabled); else r.ok = false; break;
            case SynthPatchParam::FxHarmonizerSub: if (type == 0) readF(preset.harmonizer.subLevel); else r.ok = false; break;
            case SynthPatchParam::FxHarmonizerUp: if (type == 0) readF(preset.harmonizer.upLevel); else r.ok = false; break;
            case SynthPatchParam::FxHarmonizerMix: if (type == 0) readF(preset.harmonizer.mix); else r.ok = false; break;
            case SynthPatchParam::FxEqOn: if (type == 2) readB(preset.eq.enabled); else r.ok = false; break;
            case SynthPatchParam::FxEqLow: if (type == 0) readF(preset.eq.lowGainDb); else r.ok = false; break;
            case SynthPatchParam::FxEqMid: if (type == 0) readF(preset.eq.midGainDb); else r.ok = false; break;
            case SynthPatchParam::FxEqHigh: if (type == 0) readF(preset.eq.highGainDb); else r.ok = false; break;
            case SynthPatchParam::FxChorusOn: if (type == 2) readB(preset.chorus.enabled); else r.ok = false; break;
            case SynthPatchParam::FxChorusRate: if (type == 0) readF(preset.chorus.rateHertz); else r.ok = false; break;
            case SynthPatchParam::FxChorusDepth: if (type == 0) readF(preset.chorus.depthMilliseconds); else r.ok = false; break;
            case SynthPatchParam::FxChorusMix: if (type == 0) readF(preset.chorus.mix); else r.ok = false; break;
            case SynthPatchParam::FxFlangerOn: if (type == 2) readB(preset.flanger.enabled); else r.ok = false; break;
            case SynthPatchParam::FxFlangerRate: if (type == 0) readF(preset.flanger.rateHertz); else r.ok = false; break;
            case SynthPatchParam::FxFlangerDepth: if (type == 0) readF(preset.flanger.depthMilliseconds); else r.ok = false; break;
            case SynthPatchParam::FxFlangerFeedback: if (type == 0) readF(preset.flanger.feedback); else r.ok = false; break;
            case SynthPatchParam::FxFlangerMix: if (type == 0) readF(preset.flanger.mix); else r.ok = false; break;
            case SynthPatchParam::FxEnsembleOn: if (type == 2) readB(preset.ensemble.enabled); else r.ok = false; break;
            case SynthPatchParam::FxEnsembleMode: if (type == 1) readU(preset.ensemble.mode); else r.ok = false; break;
            case SynthPatchParam::FxEnsembleMix: if (type == 0) readF(preset.ensemble.mix); else r.ok = false; break;
            case SynthPatchParam::FxPhaserOn: if (type == 2) readB(preset.phaser.enabled); else r.ok = false; break;
            case SynthPatchParam::FxPhaserRate: if (type == 0) readF(preset.phaser.rateHertz); else r.ok = false; break;
            case SynthPatchParam::FxPhaserDepth: if (type == 0) readF(preset.phaser.depth); else r.ok = false; break;
            case SynthPatchParam::FxPhaserFeedback: if (type == 0) readF(preset.phaser.feedback); else r.ok = false; break;
            case SynthPatchParam::FxPhaserMix: if (type == 0) readF(preset.phaser.mix); else r.ok = false; break;
            case SynthPatchParam::FxDelayOn: if (type == 2) readB(preset.delay.enabled); else r.ok = false; break;
            case SynthPatchParam::FxDelayTime: if (type == 0) readF(preset.delay.timeSeconds); else r.ok = false; break;
            case SynthPatchParam::FxDelayFeedback: if (type == 0) readF(preset.delay.feedback); else r.ok = false; break;
            case SynthPatchParam::FxDelayMix: if (type == 0) readF(preset.delay.mix); else r.ok = false; break;
            case SynthPatchParam::FxDelayPingPong: if (type == 2) readB(preset.delay.pingPong); else r.ok = false; break;
            case SynthPatchParam::FxReverbOn: if (type == 2) readB(preset.reverb.enabled); else r.ok = false; break;
            case SynthPatchParam::FxReverbRoom: if (type == 0) readF(preset.reverb.roomSize); else r.ok = false; break;
            case SynthPatchParam::FxReverbDamping: if (type == 0) readF(preset.reverb.damping); else r.ok = false; break;
            case SynthPatchParam::FxReverbWidth: if (type == 0) readF(preset.reverb.width); else r.ok = false; break;
            case SynthPatchParam::FxReverbMix: if (type == 0) readF(preset.reverb.mix); else r.ok = false; break;
            case SynthPatchParam::FxCompressorOn: if (type == 2) readB(preset.compressor.enabled); else r.ok = false; break;
            case SynthPatchParam::FxCompThreshold: if (type == 0) readF(preset.compressor.thresholdDb); else r.ok = false; break;
            case SynthPatchParam::FxCompRatio: if (type == 0) readF(preset.compressor.ratio); else r.ok = false; break;
            case SynthPatchParam::FxCompAttack: if (type == 0) readF(preset.compressor.attackMilliseconds); else r.ok = false; break;
            case SynthPatchParam::FxCompRelease: if (type == 0) readF(preset.compressor.releaseMilliseconds); else r.ok = false; break;
            case SynthPatchParam::FxCompMakeup: if (type == 0) readF(preset.compressor.makeupDb); else r.ok = false; break;
            case SynthPatchParam::FxLimiterOn: if (type == 2) readB(preset.limiter.enabled); else r.ok = false; break;
            case SynthPatchParam::FxLimiterCeiling: if (type == 0) readF(preset.limiter.ceilingDb); else r.ok = false; break;
            case SynthPatchParam::FxLimiterRelease: if (type == 0) readF(preset.limiter.releaseMilliseconds); else r.ok = false; break;
            case SynthPatchParam::GranularEnabled: if (type == 2) readB(preset.granular.enabled); else r.ok = false; break;
            case SynthPatchParam::GranularDensityHz: if (type == 0) readF(preset.granular.densityHz); else r.ok = false; break;
            case SynthPatchParam::GranularDurationMs: if (type == 0) readF(preset.granular.durationMs); else r.ok = false; break;
            case SynthPatchParam::GranularPitchSemitones: if (type == 0) readF(preset.granular.pitchSemitones); else r.ok = false; break;
            case SynthPatchParam::GranularPosition: if (type == 0) readF(preset.granular.position01); else r.ok = false; break;
            case SynthPatchParam::GranularPositionJitter: if (type == 0) readF(preset.granular.positionJitter01); else r.ok = false; break;
            case SynthPatchParam::GranularPanScatter: if (type == 0) readF(preset.granular.panScatter01); else r.ok = false; break;
            case SynthPatchParam::GranularGain: if (type == 0) readF(preset.granular.gain); else r.ok = false; break;
            case SynthPatchParam::GranularReverseProbability: if (type == 0) readF(preset.granular.reverseProbability01); else r.ok = false; break;
            case SynthPatchParam::GranularEnvelopeShape: {
                if (type != 1) { r.ok = false; break; }
                const std::uint32_t v = r.u32();
                // Clamp so a corrupt/out-of-range value can't form an invalid enum.
                preset.granular.envelopeShape =
                    static_cast<GranularEnvelopeShape>(v <= 3U ? v : 3U);
                break;
            }
            case SynthPatchParam::GranularCloud: if (type == 0) readF(preset.granular.cloud01); else r.ok = false; break;
            case SynthPatchParam::GranularScatter: if (type == 0) readF(preset.granular.scatter01); else r.ok = false; break;
            case SynthPatchParam::GranularDust: if (type == 0) readF(preset.granular.dust01); else r.ok = false; break;
            case SynthPatchParam::GranularFreeze: if (type == 0) readF(preset.granular.freeze01); else r.ok = false; break;
            case SynthPatchParam::GranularFreezePosition: if (type == 0) readF(preset.granular.freezePosition01); else r.ok = false; break;
            case SynthPatchParam::GranularSmear: if (type == 0) readF(preset.granular.smear01); else r.ok = false; break;
            case SynthPatchParam::GranularWidth: if (type == 0) readF(preset.granular.width01); else r.ok = false; break;
            case SynthPatchParam::GranularQuality: {
                if (type != 1) { r.ok = false; break; }
                const std::uint32_t v = r.u32();
                // Clamp so a corrupt/out-of-range value can't form an invalid enum.
                preset.granular.granularQuality =
                    static_cast<FilterQuality>(v <= 3U ? v : 3U);
                break;
            }
            case SynthPatchParam::SpectralEnabled: if (type == 2) readB(preset.spectral.enabled); else r.ok = false; break;
            case SynthPatchParam::SpectralAsset: if (type == 1) { (void)r.u32(); } else r.ok = false; break;  // reserved
            case SynthPatchParam::SpectralGain: if (type == 0) readF(preset.spectral.gain); else r.ok = false; break;
            case SynthPatchParam::SpectralFreeze:
                // Merged format is float 0..1; accept the pre-merge bool too.
                if (type == 0) readF(preset.spectral.freeze01);
                else if (type == 2) { bool b = false; readB(b); preset.spectral.freeze01 = b ? 1.0F : 0.0F; }
                else r.ok = false;
                break;
            case SynthPatchParam::SpectralStretch: if (type == 0) readF(preset.spectral.timeStretch); else r.ok = false; break;
            case SynthPatchParam::SpectralFormant: if (type == 0) readF(preset.spectral.formantShiftSemitones); else r.ok = false; break;
            case SynthPatchParam::SpectralHarmonicStretch: if (type == 0) readF(preset.spectral.harmonicStretch); else r.ok = false; break;
            case SynthPatchParam::SpectralTilt: if (type == 0) readF(preset.spectral.spectralTiltDbPerOct); else r.ok = false; break;
            case SynthPatchParam::SpectralThreshold: if (type == 0) readF(preset.spectral.partialThreshold01); else r.ok = false; break;
            case SynthPatchParam::SpectralBlur: if (type == 0) readF(preset.spectral.spectralBlur01); else r.ok = false; break;
            case SynthPatchParam::SpectralQuantize: if (type == 0) readF(preset.spectral.frequencyQuantize01); else r.ok = false; break;
            case SynthPatchParam::SpectralInharmonicity: if (type == 0) readF(preset.spectral.inharmonicity01); else r.ok = false; break;
            case SynthPatchParam::SpectralPhaseRandom: if (type == 0) readF(preset.spectral.phaseRandom01); else r.ok = false; break;
            case SynthPatchParam::SpectralStereoSpread: if (type == 0) readF(preset.spectral.stereoSpread01); else r.ok = false; break;
            case SynthPatchParam::SpectralSeed: if (type == 1) { (void)r.u32(); } else r.ok = false; break;  // reserved
            case SynthPatchParam::SpectralQuality: {
                if (type != 1) { r.ok = false; break; }
                const std::uint32_t v = r.u32();
                // Clamp so a corrupt/out-of-range value can't form an invalid enum.
                preset.spectral.spectralQuality =
                    static_cast<FilterQuality>(v <= 3U ? v : 3U);
                break;
            }
            default:
                // Unknown parameter ID: skip it so newer patches load on older builds.
                if (type == 0) r.f32();
                else if (type == 1) r.u32();
                else if (type == 2) r.u8();
                else if (type == 3) { const std::uint16_t len = r.u16(); if (r.ok && r.n >= len) { r.p += len; r.n -= len; } else r.ok = false; }
                else r.ok = false;
                break;
        }
    }
    if (!r.ok) return fail("patch program truncated or corrupt");
    std::string validation;
    if (!preset.validate(&validation)) return fail(validation);
    return preset;
}

} // namespace dve::audio
