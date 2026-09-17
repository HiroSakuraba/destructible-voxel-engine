#include "dve/audio/chiptune.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace dve::audio;

int g_failures = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

bool approx(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

float rms(const std::vector<float>& pcm, std::size_t startFrame, std::size_t frameCount) {
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t f = startFrame; f < startFrame + frameCount && f * 2 + 1 < pcm.size(); ++f) {
        const float s = pcm[f * 2];
        sum += static_cast<double>(s) * s;
        ++n;
    }
    return n ? static_cast<float>(std::sqrt(sum / static_cast<double>(n))) : 0.0F;
}

void test_note_math() {
    require(chip_note_to_midi(49) == 60, "note 49 maps to MIDI 60 (C-4)");
    require(chip_note_to_midi(kChipNoteNone) == -1, "note none maps to -1");
    require(chip_note_to_midi(kChipNoteOff) == -1, "note off maps to -1");
    require(approx(midi_note_to_frequency(69), 440.0F, 0.01F), "A4 is 440 Hz");
    require(approx(midi_note_to_frequency(60), 261.6256F, 0.1F), "C4 is ~261.63 Hz");
}

void test_serialize_roundtrip() {
    const ChipSong song = make_demo_chiptune_song();
    std::string error;
    require(song.validate(&error), ("demo song validates: " + error).c_str());

    const std::string text = song.serialize();
    ChipSong parsed;
    const bool ok = ChipSong::parse(text, parsed, &error);
    require(ok, ("demo song parses back: " + error).c_str());
    require(parsed.serialize() == text, "serialize is a fixed point under parse");
    require(song.content_hash() == parsed.content_hash(), "content hash stable across round trip");
    require(song.content_hash() == song.content_hash(), "content hash is repeatable");
    require(parsed.instruments[0].name == "lead", "instrument names survive round trip");
    require(approx(parsed.instruments[0].pan, -0.35F, 0.001F), "instrument pan survives round trip");
    require(parsed.instruments[0].dutyEnvelope.values.size() == 5, "duty envelope survives round trip");
    require(parsed.instruments[3].bitDepth == 8 && parsed.instruments[3].sampleHold == 2,
            "bit crusher settings survive round trip");

    // Structural checks.
    require(parsed.channelCount == 4, "channel count preserved");
    require(parsed.instruments.size() == 4, "instrument count preserved");
    require(parsed.patterns.size() == 1 && parsed.patterns[0].rowCount == 16, "pattern preserved");
    require(parsed.order.size() == 1 && parsed.order[0] == 0, "order preserved");
}

void test_parse_rejects_malformed() {
    ChipSong out;
    std::string error;
    require(!ChipSong::parse("not a chip file", out, &error), "rejects non-dvechip input");
    require(!ChipSong::parse("dvechip 1\nchannels 99\n", out, &error), "rejects invalid channel count");
    require(!ChipSong::parse("dvechip 1\nchannels 1\npattern 4\nrow 1:1:255:0:0:0\n", out, &error),
            "rejects a song without instruments/order");
}

void test_determinism() {
    const ChipSong song = make_demo_chiptune_song();
    std::string error;
    ChiptunePlayer a, b;
    require(a.load(song, &error) && b.load(song, &error), "both players load the demo song");

    const std::size_t frames = 48000; // one second
    std::vector<float> pcmA(frames * 2, 0.0F), pcmB(frames * 2, 0.0F);
    a.render(pcmA.data(), frames);
    b.render(pcmB.data(), frames);

    bool identical = true;
    for (std::size_t i = 0; i < pcmA.size(); ++i) {
        if (pcmA[i] != pcmB[i]) { identical = false; break; }
    }
    require(identical, "two independent renders of the same song are bit-identical");
}

void test_render_is_bounded_and_audible() {
    const ChipSong song = make_demo_chiptune_song();
    std::vector<float> pcm = render_chiptune_song(song, 1.0F);
    require(!pcm.empty(), "offline render produced samples");

    float maxAbs = 0.0F;
    bool finite = true;
    for (const float s : pcm) {
        if (!std::isfinite(s)) finite = false;
        maxAbs = std::max(maxAbs, std::fabs(s));
    }
    require(finite, "all samples are finite");
    require(maxAbs <= 1.0F, "all samples within [-1,1]");
    require(maxAbs > 0.05F, "render has real signal energy (not silent)");
}

void test_pulse_frequency_and_duty() {
    // A clean (non-band-limited) 25% pulse at A4 should cross zero ~880 times/second and have a
    // mean near -0.5 (high for 25% of the cycle, low for 75%).
    ChipInstrument inst;
    inst.name = "test_pulse";
    inst.wave = ChipWave::Pulse;
    inst.duty = 0.25F;
    inst.bandLimited = false;
    inst.gain = 1.0F;
    inst.volume.values = {1.0F};
    inst.volume.loopIndex = 0;

    ChiptuneVoice voice(48000);
    voice.set_instrument(inst);
    voice.note_on(69, 1.0F); // A4
    voice.tick();

    const std::size_t n = 48000;
    double sum = 0.0;
    int crossings = 0;
    float prev = voice.next_sample();
    sum += prev;
    for (std::size_t i = 1; i < n; ++i) {
        const float s = voice.next_sample();
        sum += s;
        if ((prev < 0.0F && s >= 0.0F) || (prev >= 0.0F && s < 0.0F)) ++crossings;
        prev = s;
    }
    const float mean = static_cast<float>(sum / static_cast<double>(n));
    require(crossings > 840 && crossings < 920, "A4 pulse crosses zero ~880 times per second");
    require(approx(mean, -0.5F, 0.05F), "25% duty pulse mean is near -0.5");
}

void test_noise_toggles() {
    ChipInstrument inst;
    inst.name = "test_noise";
    inst.wave = ChipWave::Noise;
    inst.noiseMode = ChipNoiseMode::Long;
    inst.gain = 1.0F;
    inst.volume.values = {1.0F};
    inst.volume.loopIndex = 0;

    ChiptuneVoice voice(48000);
    voice.set_instrument(inst);
    voice.note_on(72, 1.0F);
    voice.tick();

    bool sawPositive = false, sawNegative = false, bounded = true;
    for (std::size_t i = 0; i < 20000; ++i) {
        const float s = voice.next_sample();
        if (s > 0.1F) sawPositive = true;
        if (s < -0.1F) sawNegative = true;
        if (std::fabs(s) > 1.0F) bounded = false;
    }
    require(sawPositive && sawNegative, "noise channel produces both polarities");
    require(bounded, "noise channel stays within [-1,1]");
}

void test_envelope_decay() {
    // An instrument with a decaying loop envelope should yield decreasing RMS over ticks.
    ChipSong song;
    song.name = "decay";
    song.channelCount = 1;
    song.ticksPerRow = 4;
    song.ticksPerSecond = 60;
    song.masterGain = 1.0F;
    song.loop = false;

    ChipInstrument inst;
    inst.name = "decay";
    inst.wave = ChipWave::Pulse;
    inst.duty = 0.5F;
    inst.gain = 1.0F;
    inst.volume.values = {1.0F, 0.75F, 0.5F, 0.25F, 0.1F, 0.05F, 0.0F};
    inst.volume.loopIndex = 6; // hold at 0 once decayed
    song.instruments = {inst};

    ChipPattern pattern;
    pattern.rowCount = 8;
    pattern.cells.resize(8);
    pattern.at(0, 0, 1).note = 61; // C-5
    pattern.at(0, 0, 1).instrument = 1;
    song.patterns = {pattern};
    song.order = {0};

    std::string error;
    require(song.validate(&error), ("decay song validates: " + error).c_str());

    std::vector<float> pcm = render_chiptune_song(song, 1.0F);
    require(!pcm.empty(), "decay render produced samples");

    const float early = rms(pcm, 0, 1000);
    const float late = rms(pcm, 6000, 1000);
    require(early > late, "amplitude envelope decays over time");
    require(early > 0.01F, "attack window is audible");
}


ChipSong make_one_channel_song(const ChipInstrument& instrument, std::uint32_t sampleRate = 48000,
                               std::uint32_t ticksPerSecond = 60, std::uint32_t ticksPerRow = 6,
                               std::uint32_t rows = 1) {
    ChipSong song;
    song.name = "test";
    song.sampleRate = sampleRate;
    song.channelCount = 1;
    song.ticksPerSecond = ticksPerSecond;
    song.ticksPerRow = ticksPerRow;
    song.masterGain = 0.5F;
    song.loop = false;
    song.instruments = {instrument};
    ChipPattern pattern;
    pattern.rowCount = rows;
    pattern.cells.resize(rows);
    pattern.cells[0].note = 58; // A4
    pattern.cells[0].instrument = 1;
    song.patterns = {pattern};
    song.order = {0};
    return song;
}

void test_fractional_tick_scheduler() {
    ChipInstrument instrument;
    instrument.name = "timing";
    instrument.wave = ChipWave::Sine;
    instrument.volume.values = {1.0F};
    instrument.volume.loopIndex = 0;
    ChipSong song = make_one_channel_song(instrument, 44100, 59, 1, 59);

    std::string error;
    ChiptunePlayer player;
    require(player.load(song, &error), ("fractional-timing song loads: " + error).c_str());
    std::vector<float> almost(44099U * 2U);
    player.render(almost);
    require(!player.finished(), "59 Hz scheduler does not finish one sample early at 44.1 kHz");
    std::array<float, 2> finalFrame{};
    player.render(finalFrame);
    require(player.finished(), "fractional scheduler finishes exactly after 44,100 frames");
}

void test_stereo_pan() {
    ChipInstrument instrument;
    instrument.name = "left";
    instrument.wave = ChipWave::Sine;
    instrument.pan = -1.0F;
    instrument.volume.values = {1.0F};
    instrument.volume.loopIndex = 0;
    const ChipSong song = make_one_channel_song(instrument);
    std::vector<float> pcm = render_chiptune_song(song, 0.05F);
    double leftEnergy = 0.0;
    double rightEnergy = 0.0;
    for (std::size_t i = 0; i + 1 < pcm.size(); i += 2) {
        leftEnergy += std::fabs(pcm[i]);
        rightEnergy += std::fabs(pcm[i + 1]);
    }
    require(leftEnergy > 1.0, "hard-left voice produces left-channel energy");
    require(rightEnergy < 1.0e-6, "hard-left voice produces no right-channel energy");
}

void test_wavetable_and_crusher() {
    ChipInstrument instrument;
    instrument.name = "table";
    instrument.wave = ChipWave::Wavetable;
    instrument.wavetable = {0.0F, 1.0F, 0.0F, -1.0F};
    instrument.volume.values = {1.0F};
    instrument.volume.loopIndex = 0;
    instrument.bitDepth = 4;
    instrument.sampleHold = 4;
    instrument.lowPassHz = 6000.0F;
    instrument.highPassHz = 80.0F;
    std::string error;
    require(instrument.validate(&error), ("wavetable instrument validates: " + error).c_str());
    const std::vector<float> pcm = render_chiptune_song(make_one_channel_song(instrument), 0.05F, &error);
    require(!pcm.empty(), "wavetable instrument renders");
    bool finite = true;
    bool hasHeldSamples = false;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        finite = finite && std::isfinite(pcm[i]);
        if (i >= 2 && pcm[i] == pcm[i - 2]) hasHeldSamples = true;
    }
    require(finite, "wavetable/crusher output is finite");
    require(hasHeldSamples, "sample-hold crusher repeats output values");
}

void test_volume_slide_and_note_cut() {
    ChipInstrument instrument;
    instrument.name = "effects";
    instrument.wave = ChipWave::Pulse;
    instrument.volume.values = {1.0F};
    instrument.volume.loopIndex = 0;

    ChipSong slide = make_one_channel_song(instrument, 48000, 60, 8, 1);
    slide.patterns[0].cells[0].volume = 15;
    slide.patterns[0].cells[0].effect = ChipEffect::VolumeSlide;
    slide.patterns[0].cells[0].effectParam = 0x02; // down 2/15 per tick
    std::vector<float> slidePcm = render_chiptune_song(slide, 0.2F);
    require(rms(slidePcm, 0, 600) > rms(slidePcm, 5000, 600),
            "volume-slide effect reduces level over the row");

    ChipSong cut = make_one_channel_song(instrument, 48000, 60, 8, 1);
    cut.patterns[0].cells[0].effect = ChipEffect::NoteCut;
    cut.patterns[0].cells[0].effectParam = 2;
    std::vector<float> cutPcm = render_chiptune_song(cut, 0.2F);
    require(rms(cutPcm, 100, 400) > 0.01F, "note-cut sound is audible before cut tick");
    require(rms(cutPcm, 2000, 400) < 1.0e-6F, "note-cut effect silences voice after cut tick");
}

void test_retrigger_effect() {
    ChipInstrument instrument;
    instrument.name = "retrigger";
    instrument.wave = ChipWave::Pulse;
    instrument.volume.values = {1.0F, 0.0F};
    instrument.volume.loopIndex = 1;

    ChipSong song = make_one_channel_song(instrument, 48000, 60, 6, 1);
    song.patterns[0].cells[0].effect = ChipEffect::Retrigger;
    song.patterns[0].cells[0].effectParam = 2;
    const std::vector<float> pcm = render_chiptune_song(song, 0.12F);
    require(rms(pcm, 50, 500) > 0.01F, "retrigger voice is audible at row start");
    require(rms(pcm, 1650, 500) > 0.01F, "retrigger restarts the envelope on tick interval");
}

void test_sfx_presets_and_audio_asset_bridge() {
    const std::array<ChipSfxPreset, 8> presets{
        ChipSfxPreset::Coin, ChipSfxPreset::Jump, ChipSfxPreset::Laser, ChipSfxPreset::Explosion,
        ChipSfxPreset::Hit, ChipSfxPreset::PowerUp, ChipSfxPreset::UiConfirm, ChipSfxPreset::UiCancel};
    for (const ChipSfxPreset preset : presets) {
        ChipSfxRequest request;
        request.preset = preset;
        request.durationSeconds = 0.25F;
        std::string error;
        const DecodedAudioAsset asset = render_chiptune_sfx(request, &error);
        require(!asset.samples.empty(), ("SFX preset renders: " + error).c_str());
        require(asset.metadata.channels == 2 && asset.metadata.sampleRate == 48000,
                "SFX asset bridge emits stereo engine-rate metadata");
        require(asset.metadata.frameCount == asset.samples.size() / 2,
                "SFX asset metadata frame count matches PCM");
        require(asset.metadata.contentHash != 0, "SFX asset bridge computes content hash");
        require(asset.metadata.peakLinear > 0.01F && asset.metadata.peakLinear <= 1.0F,
                "SFX preset is audible and bounded");
    }
}

void test_validation_rejects_extended_invalid_values() {
    ChipInstrument instrument;
    instrument.name = "bad";
    instrument.pan = 2.0F;
    std::string error;
    require(!instrument.validate(&error), "rejects out-of-range pan");
    instrument.pan = 0.0F;
    instrument.wave = ChipWave::Wavetable;
    require(!instrument.validate(&error), "rejects wavetable oscillator without a table");

    ChipSong parsed;
    require(!ChipSong::parse("dvechip 2\n", parsed, &error), "rejects unsupported format version");
    require(!ChipSong::parse("dvechip 1\nsample_rate 999999999999999999\n", parsed, &error),
            "rejects overflowing numeric values");
}

} // namespace

int main() {
    test_note_math();
    test_serialize_roundtrip();
    test_parse_rejects_malformed();
    test_determinism();
    test_render_is_bounded_and_audible();
    test_pulse_frequency_and_duty();
    test_noise_toggles();
    test_envelope_decay();
    test_fractional_tick_scheduler();
    test_stereo_pan();
    test_wavetable_and_crusher();
    test_volume_slide_and_note_cut();
    test_retrigger_effect();
    test_sfx_presets_and_audio_asset_bridge();
    test_validation_rejects_extended_invalid_values();

    if (g_failures == 0) {
        std::cout << "chiptune: all tests passed\n";
        return 0;
    }
    std::cerr << "chiptune: " << g_failures << " test(s) failed\n";
    return 1;
}
