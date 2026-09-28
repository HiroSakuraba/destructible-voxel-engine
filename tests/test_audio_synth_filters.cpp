// Tests for Phase 2 filter work: Comb + Formant topologies and the auto
// oversampling policy (SYN-010 filter gaps).
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/audio/synthesizer.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void disable_effects(SynthPreset& preset) {
    preset.distortion.enabled = false; preset.eq.enabled = false; preset.chorus.enabled = false;
    preset.phaser.enabled = false; preset.delay.enabled = false; preset.reverb.enabled = false;
    preset.compressor.enabled = false; preset.limiter.enabled = true;
}

// Single-oscillator preset with the filter fully under test control:
// no envelope/keytrack/modulation influence on cutoff or resonance.
SynthPreset filter_test_preset(OscillatorWaveform wave, float gain) {
    SynthPreset preset = SynthPreset::make_default();
    for (auto& oscillator : preset.oscillators) oscillator.enabled = false;
    preset.oscillators[0].enabled = true;
    preset.oscillators[0].waveform = wave;
    preset.oscillators[0].gain = gain;
    preset.oscillators[0].subOscillatorLevel = 0.0F;
    preset.oscillators[0].ringModDepth = 0.0F;
    preset.ampEnvelope = {0.001F, 0.005F, 1.0F, 0.05F, EnvelopeCurve::Linear};
    preset.filter.enabled = true;
    preset.filter.envelope = {0.001F, 0.01F, 1.0F, 0.05F, EnvelopeCurve::Linear};
    preset.filter.envelopeAmountOctaves = 0.0F;
    preset.filter.keyTrack = 0.0F;
    preset.filter.drive = 1.0F;
    preset.unison.enabled = false;
    preset.masterGain = 0.5F;
    preset.masterPan = 0.0F;
    preset.tuning.analogDriftCents = 0.0F;
    disable_effects(preset);
    return preset;
}

std::vector<float> render_mono_left(const SynthPreset& preset, std::uint8_t note, std::size_t frames) {
    Synthesizer synth(48000);
    synth.set_preset(preset);
    require(synth.note_on(note, 0.82F), "note-on failed");
    std::vector<float> stereo(frames * 2U);
    synth.render(stereo);
    std::vector<float> mono(frames);
    for (std::size_t i = 0; i < frames; ++i) mono[i] = stereo[2U * i];
    require(std::all_of(mono.begin(), mono.end(),
                        [](float value) { return std::isfinite(value); }),
            "render contained non-finite samples");
    return mono;
}

void fft_radix2(std::vector<std::complex<float>>& data) {
    const std::size_t n = data.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.0F * 3.14159265F / static_cast<float>(len);
        const std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0F, 0.0F);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = data[i + k];
                const std::complex<float> v = data[i + k + len / 2] * w;
                data[i + k] = u + v;
                data[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

// Average spectral magnitude over [f0, f1] Hz of a windowed mono signal.
double band_magnitude(const std::vector<float>& mono, std::size_t skip,
                      std::size_t fftSize, double sampleRate, double f0, double f1) {
    std::vector<std::complex<float>> data(fftSize);
    for (std::size_t i = 0; i < fftSize; ++i) {
        const float window = 0.5F - 0.5F * std::cos(2.0F * 3.14159265F *
                             static_cast<float>(i) / static_cast<float>(fftSize));
        data[i] = std::complex<float>(mono[skip + i] * window, 0.0F);
    }
    fft_radix2(data);
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t k = 0; k < fftSize / 2; ++k) {
        const double freq = static_cast<double>(k) * sampleRate / static_cast<double>(fftSize);
        if (freq >= f0 && freq <= f1) { sum += std::abs(data[k]); ++count; }
    }
    require(count > 0, "empty analysis band");
    return sum / static_cast<double>(count);
}

void test_topology_names() {
    CHECK(filter_topology_name(FilterTopology::Comb) == "Comb");
    CHECK(filter_topology_name(FilterTopology::Formant) == "Formant");
    // Existing names unchanged (serialization stability).
    CHECK(filter_topology_name(FilterTopology::CleanStateVariable) == "Clean SVF");
    CHECK(filter_topology_name(FilterTopology::MoogLadder) == "Moog Ladder");
    CHECK(filter_topology_name(FilterTopology::KorgMs20) == "Korg MS-20");
    CHECK(filter_topology_name(FilterTopology::OberheimSem) == "Oberheim SEM");
    // Enum values not renumbered.
    CHECK(static_cast<std::uint8_t>(FilterTopology::CleanStateVariable) == 0);
    CHECK(static_cast<std::uint8_t>(FilterTopology::MoogLadder) == 1);
    CHECK(static_cast<std::uint8_t>(FilterTopology::KorgMs20) == 2);
    CHECK(static_cast<std::uint8_t>(FilterTopology::OberheimSem) == 3);
    CHECK(static_cast<std::uint8_t>(FilterTopology::Comb) == 4);
    CHECK(static_cast<std::uint8_t>(FilterTopology::Formant) == 5);
}

void test_oversampling_policy() {
    // Explicit X1/X2/X4 settings are always honored exactly (the pre-existing
    // regression test renders explicit X1/X2/X4 and requires them distinct).
    FilterParameters p;  // defaults: resonance 0.15, drive 1.0, oversampling X1
    CHECK(effective_oversampling(p) == FilterOversampling::X1);
    p.resonance = 0.99F; p.drive = 5.0F;  // extreme triggers: explicit X1 stays X1
    CHECK(effective_oversampling(p) == FilterOversampling::X1);
    p.oversampling = FilterOversampling::X2;
    CHECK(effective_oversampling(p) == FilterOversampling::X2);
    p.oversampling = FilterOversampling::X4;
    p.resonance = 0.1F; p.drive = 1.0F;
    CHECK(effective_oversampling(p) == FilterOversampling::X4);

    // Auto applies the policy: X1 baseline, X2 on high resonance or engaged
    // drive, X4 on extreme resonance with drive engaged.
    p.oversampling = FilterOversampling::Auto;
    p.resonance = 0.15F; p.drive = 1.0F;
    CHECK(effective_oversampling(p) == FilterOversampling::X1);
    p.resonance = 0.75F;  // exactly at threshold: no upgrade
    CHECK(effective_oversampling(p) == FilterOversampling::X1);
    p.resonance = 0.76F;  // above threshold: X2
    CHECK(effective_oversampling(p) == FilterOversampling::X2);
    p.resonance = 0.15F; p.drive = 1.5F;  // drive at threshold: no upgrade
    CHECK(effective_oversampling(p) == FilterOversampling::X1);
    p.drive = 1.6F;  // drive engaged: X2
    CHECK(effective_oversampling(p) == FilterOversampling::X2);
    p.resonance = 0.97F; p.drive = 1.0F;  // extreme resonance alone: X2, not X4
    CHECK(effective_oversampling(p) == FilterOversampling::X2);
    p.resonance = 0.97F; p.drive = 2.0F;  // extreme resonance + drive: X4
    CHECK(effective_oversampling(p) == FilterOversampling::X4);

    // Enum stability: existing serialized values keep their numeric codes.
    CHECK(static_cast<std::uint8_t>(FilterOversampling::X1) == 1);
    CHECK(static_cast<std::uint8_t>(FilterOversampling::X2) == 2);
    CHECK(static_cast<std::uint8_t>(FilterOversampling::X4) == 4);
}

void test_comb_spectrum() {
    // White noise through the comb: feedback comb has peaks at multiples of
    // the comb frequency and notches halfway between.
    SynthPreset preset = filter_test_preset(OscillatorWaveform::Noise, 0.5F);
    preset.filter.topology = FilterTopology::Comb;
    preset.filter.cutoffHertz = 500.0F;   // comb fundamental 500 Hz
    preset.filter.resonance = 0.7F;       // feedback = 0.7 * 0.85 = 0.595
    preset.filter.comb.damping = 0.0F;
    preset.filter.comb.mix = 1.0F;
    preset.filter.comb.feedbackScale = 1.0F;

    const std::vector<float> mono = render_mono_left(preset, 60, 131072);
    constexpr std::size_t kSkip = 16384;
    constexpr std::size_t kFft = 65536;
    const double peak = (band_magnitude(mono, kSkip, kFft, 48000.0, 485.0, 515.0) +
                         band_magnitude(mono, kSkip, kFft, 48000.0, 985.0, 1015.0) +
                         band_magnitude(mono, kSkip, kFft, 48000.0, 1485.0, 1515.0)) / 3.0;
    const double notch = (band_magnitude(mono, kSkip, kFft, 48000.0, 235.0, 265.0) +
                          band_magnitude(mono, kSkip, kFft, 48000.0, 735.0, 765.0) +
                          band_magnitude(mono, kSkip, kFft, 48000.0, 1235.0, 1265.0)) / 3.0;
    const double ratio = peak / notch;
    std::printf("comb peak/notch magnitude ratio: %.2f (theory ~3.9)\n", ratio);
    // Theory for feedback g=0.595: (1+g)/(1-g) = 3.94. Threshold well below.
    CHECK(ratio > 2.5);
}

void test_formant_spectrum() {
    // Saw (dense harmonics) through the formant bank at cutoff 1000 Hz, so the
    // authored "ah" formants (730/1090/2440/3500) sit exactly on their targets.
    SynthPreset preset = filter_test_preset(OscillatorWaveform::Saw, 0.4F);
    preset.filter.topology = FilterTopology::Formant;
    preset.filter.cutoffHertz = 1000.0F;
    preset.filter.resonance = 0.5F;  // Q = 0.7 + 0.5*8 = 4.7

    const std::vector<float> mono = render_mono_left(preset, 45, 65536);
    constexpr std::size_t kSkip = 8192;
    constexpr std::size_t kFft = 32768;
    const double formant = (band_magnitude(mono, kSkip, kFft, 48000.0, 690.0, 770.0) +
                            band_magnitude(mono, kSkip, kFft, 48000.0, 1050.0, 1130.0) +
                            band_magnitude(mono, kSkip, kFft, 48000.0, 2400.0, 2480.0) +
                            band_magnitude(mono, kSkip, kFft, 48000.0, 3460.0, 3540.0)) / 4.0;
    const double valley = (band_magnitude(mono, kSkip, kFft, 48000.0, 1550.0, 1750.0) +
                           band_magnitude(mono, kSkip, kFft, 48000.0, 1900.0, 2100.0) +
                           band_magnitude(mono, kSkip, kFft, 48000.0, 4100.0, 4300.0)) / 3.0;
    const double ratio = formant / valley;
    std::printf("formant/valley magnitude ratio: %.2f\n", ratio);
    CHECK(ratio > 1.8);
}

void test_auto_oversample_render_smoke() {
    // Extreme resonance + drive engages the X4 auto policy on a Comb voice;
    // the render must stay finite and non-silent.
    SynthPreset preset = filter_test_preset(OscillatorWaveform::Saw, 0.4F);
    preset.filter.topology = FilterTopology::Comb;
    preset.filter.cutoffHertz = 800.0F;
    preset.filter.resonance = 0.97F;
    preset.filter.drive = 3.0F;
    preset.filter.oversampling = FilterOversampling::Auto;
    CHECK(effective_oversampling(preset.filter) == FilterOversampling::X4);
    const std::vector<float> mono = render_mono_left(preset, 60, 32768);
    double energy = 0.0;
    for (std::size_t i = 8192; i < mono.size(); ++i)
        energy += static_cast<double>(mono[i]) * mono[i];
    std::printf("auto-X4 comb render energy: %.6f\n", energy);
    CHECK(energy > 1.0e-6);
}

void test_serialization_round_trip() {
    std::string error;
    SynthPreset preset = SynthPreset::make_default();
    preset.filter.topology = FilterTopology::Comb;
    preset.filter.oversampling = FilterOversampling::X4;
    preset.filter.comb.damping = 0.4F;
    preset.filter.comb.mix = 0.7F;
    preset.filter.comb.feedbackScale = 0.9F;
    preset.filter.formant.frequencyHertz = {500.0F, 900.0F, 2200.0F, 3200.0F};
    preset.filter.formant.gains = {0.9F, 0.6F, 0.4F, 0.2F};
    preset.filter.formant.dryMix = 0.1F;
    require(preset.validate(&error), error.c_str());

    const std::string text = preset.serialize();
    auto parsed = SynthPreset::parse(text, &error);
    require(parsed.has_value(), error.c_str());
    CHECK(parsed->filter.topology == FilterTopology::Comb);
    CHECK(parsed->filter.oversampling == FilterOversampling::X4);
    CHECK(std::abs(parsed->filter.comb.damping - 0.4F) < 1.0e-6F);
    CHECK(std::abs(parsed->filter.comb.mix - 0.7F) < 1.0e-6F);
    CHECK(std::abs(parsed->filter.comb.feedbackScale - 0.9F) < 1.0e-6F);
    for (std::size_t i = 0; i < FormantParameters::kBandCount; ++i) {
        CHECK(std::abs(parsed->filter.formant.frequencyHertz[i] -
                       preset.filter.formant.frequencyHertz[i]) < 1.0e-3F);
        CHECK(std::abs(parsed->filter.formant.gains[i] -
                       preset.filter.formant.gains[i]) < 1.0e-6F);
    }
    CHECK(std::abs(parsed->filter.formant.dryMix - 0.1F) < 1.0e-6F);

    // Formant topology token round-trips too.
    SynthPreset formantPreset = SynthPreset::make_default();
    formantPreset.filter.topology = FilterTopology::Formant;
    auto parsedFormant = SynthPreset::parse(formantPreset.serialize(), &error);
    require(parsedFormant.has_value(), error.c_str());
    CHECK(parsedFormant->filter.topology == FilterTopology::Formant);

    // Old presets are untouched: default topology/oversampling parse back and
    // the policy leaves them at X1.
    SynthPreset old = SynthPreset::make_default();
    auto parsedOld = SynthPreset::parse(old.serialize(), &error);
    require(parsedOld.has_value(), error.c_str());
    CHECK(parsedOld->filter.topology == FilterTopology::MoogLadder);
    CHECK(parsedOld->filter.oversampling == FilterOversampling::X1);
    CHECK(effective_oversampling(parsedOld->filter) == FilterOversampling::X1);

    // Validation rejects out-of-range new parameters.
    SynthPreset bad = SynthPreset::make_default();
    bad.filter.formant.frequencyHertz[0] = 5.0F;
    CHECK(!bad.validate(&error));
    bad = SynthPreset::make_default();
    bad.filter.comb.mix = 1.5F;
    CHECK(!bad.validate(&error));
}

}  // namespace

int main() {
    try {
        test_topology_names();
        test_oversampling_policy();
        test_comb_spectrum();
        test_formant_spectrum();
        test_auto_oversample_render_smoke();
        test_serialization_round_trip();
    } catch (const std::exception& e) {
        std::printf("EXCEPTION: %s\n", e.what());
        return 1;
    }
    if (g_failures == 0) std::printf("all filter tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
