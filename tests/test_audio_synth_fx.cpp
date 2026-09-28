// Verification for the five new synthesizer effects:
// flanger, fuzz (distortion mode), octave harmonizer, Juno-style ensemble
// (triple chorus), and bitcrusher.
//
// Covers: validate() ranges, finite + level-safe renders, each effect audibly
// changes the output vs bypass, harmonizer pitch accuracy (exact octave
// factors via FFT peak), bitcrusher quantization levels, and
// serialize/parse round-trip plus old-format (v5 without new keys) compat.
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/audio/synthesizer.hpp"

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void fft_magnitudes(std::vector<double>& real, std::vector<double>& imag) {
    const std::size_t n = real.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / static_cast<double>(len);
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (std::size_t i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (std::size_t k = 0; k < len / 2; ++k) {
                const double ur = real[i + k], ui = imag[i + k];
                const double vr = real[i + k + len / 2] * cr - imag[i + k + len / 2] * ci;
                const double vi = real[i + k + len / 2] * ci + imag[i + k + len / 2] * cr;
                real[i + k] = ur + vr;
                imag[i + k] = ui + vi;
                real[i + k + len / 2] = ur - vr;
                imag[i + k + len / 2] = ui - vi;
                const double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

// Peak frequency (Hz) inside [loHz, hiHz] of the left channel.
double peak_frequency(const std::vector<float>& interleaved, double loHz, double hiHz,
                      double sampleRate) {
    const std::size_t frames = interleaved.size() / 2;
    const std::size_t start = static_cast<std::size_t>(sampleRate * 0.25);
    const std::size_t n = 1U << 15;
    std::vector<double> real(n, 0.0), imag(n, 0.0);
    for (std::size_t i = 0; i < n && start + i < frames; ++i) {
        const double w = 0.5 * (1.0 - std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(n)));
        real[i] = interleaved[(start + i) * 2] * w;
    }
    fft_magnitudes(real, imag);
    double best = 0.0, bestMag = 0.0;
    for (std::size_t k = 1; k < n / 2; ++k) {
        const double f = static_cast<double>(k) * sampleRate / static_cast<double>(n);
        if (f < loHz || f > hiHz) continue;
        const double mag = real[k] * real[k] + imag[k] * imag[k];
        if (mag > bestMag) { bestMag = mag; best = f; }
    }
    return best;
}

double band_energy(const std::vector<float>& interleaved, double loHz, double hiHz,
                   double sampleRate) {
    const std::size_t frames = interleaved.size() / 2;
    const std::size_t start = static_cast<std::size_t>(sampleRate * 0.25);
    const std::size_t n = 1U << 15;
    std::vector<double> real(n, 0.0), imag(n, 0.0);
    for (std::size_t i = 0; i < n && start + i < frames; ++i) {
        const double w = 0.5 * (1.0 - std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(n)));
        real[i] = interleaved[(start + i) * 2] * w;
    }
    fft_magnitudes(real, imag);
    double energy = 0.0;
    for (std::size_t k = 1; k < n / 2; ++k) {
        const double f = static_cast<double>(k) * sampleRate / static_cast<double>(n);
        if (f >= loHz && f <= hiHz) energy += real[k] * real[k] + imag[k] * imag[k];
    }
    return energy;
}

std::vector<float> render_preset(const dve::audio::SynthPreset& preset, std::uint8_t midiNote,
                                 double seconds = 1.0) {
    using namespace dve::audio;
    Synthesizer synth(48000);
    synth.set_preset(preset);
    require(synth.note_on(midiNote, 0.9F), "note-on failed");
    std::vector<float> audio(static_cast<std::size_t>(48000 * seconds) * 2, 0.0F);
    synth.render(audio);
    return audio;
}

float peak_abs(const std::vector<float>& audio) {
    float peak = 0.0F;
    for (float s : audio) {
        require(std::isfinite(s), "non-finite sample rendered");
        peak = std::max(peak, std::abs(s));
    }
    return peak;
}

float max_diff(const std::vector<float>& a, const std::vector<float>& b) {
    require(a.size() == b.size(), "render size mismatch");
    float d = 0.0F;
    for (std::size_t i = 0; i < a.size(); ++i) d = std::max(d, std::abs(a[i] - b[i]));
    return d;
}

// A clean single-sine test voice: everything else off.
dve::audio::SynthPreset sine_voice() {
    using namespace dve::audio;
    SynthPreset preset = SynthPreset::make_default();
    for (auto& osc : preset.oscillators) osc.enabled = false;
    auto& osc = preset.oscillators[0];
    osc.enabled = true;
    osc.waveform = OscillatorWaveform::Sine;
    osc.semitones = 0.0F;
    osc.cents = 0.0F;
    osc.gain = 0.5F;
    osc.pan = 0.0F;
    preset.filter.enabled = false;
    preset.distortion.enabled = false;
    preset.bitcrusher.enabled = false;
    preset.harmonizer.enabled = false;
    preset.eq.enabled = false;
    preset.chorus.enabled = false;
    preset.flanger.enabled = false;
    preset.ensemble.enabled = false;
    preset.phaser.enabled = false;
    preset.delay.enabled = false;
    preset.reverb.enabled = false;
    preset.compressor.enabled = false;
    preset.limiter.enabled = false;
    return preset;
}
} // namespace

int main() {
    using namespace dve::audio;
    try {
        // 1. validate() accepts cranked-but-legal settings for every new effect.
        {
            SynthPreset preset = SynthPreset::make_default();
            preset.distortion.enabled = true;
            preset.distortion.mode = DistortionMode::Fuzz;
            preset.distortion.drive = 12.0F;
            preset.bitcrusher.enabled = true;
            preset.bitcrusher.bits = 4;
            preset.bitcrusher.downsample = 8;
            preset.harmonizer.enabled = true;
            preset.flanger.enabled = true;
            preset.flanger.feedback = 0.8F;
            preset.ensemble.enabled = true;
            preset.ensemble.mode = EnsembleMode::Both;
            std::string error;
            require(preset.validate(&error), ("validate failed: " + error).c_str());
            std::cout << "validate: OK\n";
        }

        // 2. Each effect renders finite, non-silent, level-safe audio.
        {
            auto check = [](SynthPreset preset, const char* name) {
                const auto audio = render_preset(preset, 69);
                const float peak = peak_abs(audio);
                require(peak > 0.01F, "effect rendered (near) silence");
                require(peak <= 1.0F, "effect clipped past 0 dBFS");
                std::cout << "render " << name << ": peak " << peak << " OK\n";
            };
            SynthPreset base = sine_voice();
            SynthPreset p = base;
            p.flanger.enabled = true; p.flanger.feedback = 0.7F; p.flanger.mix = 0.6F;
            check(p, "flanger");
            p = base;
            p.distortion.enabled = true; p.distortion.mode = DistortionMode::Fuzz;
            p.distortion.drive = 6.0F; p.distortion.mix = 0.8F;
            check(p, "fuzz");
            p = base;
            p.bitcrusher.enabled = true; p.bitcrusher.bits = 6; p.bitcrusher.downsample = 4;
            check(p, "bitcrusher");
            p = base;
            p.harmonizer.enabled = true; p.harmonizer.subLevel = 0.6F; p.harmonizer.upLevel = 0.4F;
            check(p, "harmonizer");
            p = base;
            p.ensemble.enabled = true; p.ensemble.mode = EnsembleMode::Both;
            check(p, "ensemble");
        }

        // 3. Every new effect audibly changes the output vs bypass.
        {
            const SynthPreset base = sine_voice();
            auto differs = [&](SynthPreset preset, const char* name) {
                const float d = max_diff(render_preset(base, 69), render_preset(preset, 69));
                require(d > 0.01F, "effect did not change the output");
                std::cout << "audible " << name << ": max diff " << d << " OK\n";
            };
            SynthPreset p = base; p.flanger.enabled = true;
            differs(p, "flanger");
            p = base; p.distortion.enabled = true; p.distortion.mode = DistortionMode::Fuzz;
            differs(p, "fuzz");
            p = base; p.distortion.enabled = true; p.distortion.mode = DistortionMode::Classic;
            const float fuzzVsClassic = max_diff(render_preset(p, 69),
                []{ SynthPreset q = sine_voice(); q.distortion.enabled = true;
                    q.distortion.mode = DistortionMode::Fuzz; return render_preset(q, 69); }());
            require(fuzzVsClassic > 0.01F, "fuzz identical to classic distortion");
            std::cout << "fuzz vs classic: max diff " << fuzzVsClassic << " OK\n";
            p = base; p.bitcrusher.enabled = true;
            differs(p, "bitcrusher");
            p = base; p.harmonizer.enabled = true;
            differs(p, "harmonizer");
            p = base; p.ensemble.enabled = true;
            differs(p, "ensemble");
        }

        // 4. Harmonizer pitch accuracy: exact octave factors.
        {
            SynthPreset preset = sine_voice();
            preset.harmonizer.enabled = true;
            preset.harmonizer.subLevel = 1.0F;
            preset.harmonizer.upLevel = 0.0F;
            preset.harmonizer.mix = 1.0F;
            const auto sub = render_preset(preset, 69, 1.5);
            const double subPeak = peak_frequency(sub, 150.0, 300.0, 48000.0);
            require(std::abs(subPeak - 220.0) < 8.0, "sub octave not at 220 Hz");
            const double subEnergy = band_energy(sub, 200.0, 240.0, 48000.0);
            SynthPreset dry = sine_voice();
            const double drySubEnergy = band_energy(render_preset(dry, 69, 1.5), 200.0, 240.0, 48000.0);
            require(subEnergy > 10.0 * drySubEnergy, "sub voice too weak");
            std::cout << "harmonizer sub: peak " << subPeak << " Hz OK\n";

            preset.harmonizer.subLevel = 0.0F;
            preset.harmonizer.upLevel = 1.0F;
            const auto up = render_preset(preset, 69, 1.5);
            const double upPeak = peak_frequency(up, 800.0, 960.0, 48000.0);
            require(std::abs(upPeak - 880.0) < 10.0, "upper octave not at 880 Hz");
            std::cout << "harmonizer up: peak " << upPeak << " Hz OK\n";
        }

        // 5. Bitcrusher quantization: bits=1, mix=1 -> at most 3 distinct levels.
        {
            SynthPreset preset = sine_voice();
            preset.bitcrusher.enabled = true;
            preset.bitcrusher.bits = 1;
            preset.bitcrusher.downsample = 1;
            preset.bitcrusher.mix = 1.0F;
            const auto audio = render_preset(preset, 69);
            std::set<float> levels(audio.begin(), audio.end());
            require(levels.size() <= 3, "bits=1 did not quantize to <= 3 levels");
            std::cout << "bitcrusher bits=1: " << levels.size() << " levels OK\n";
        }

        // 6. Serialize/parse round-trip keeps every new field.
        {
            SynthPreset preset = SynthPreset::make_default();
            preset.distortion.mode = DistortionMode::Fuzz;
            preset.bitcrusher.enabled = true;
            preset.bitcrusher.bits = 4;
            preset.bitcrusher.downsample = 8;
            preset.bitcrusher.mix = 0.7F;
            preset.harmonizer.enabled = true;
            preset.harmonizer.subLevel = 0.7F;
            preset.harmonizer.upLevel = 0.5F;
            preset.harmonizer.mix = 0.6F;
            preset.flanger.enabled = true;
            preset.flanger.rateHertz = 0.5F;
            preset.flanger.depthMilliseconds = 4.0F;
            preset.flanger.feedback = -0.6F;
            preset.flanger.mix = 0.5F;
            preset.ensemble.enabled = true;
            preset.ensemble.mode = EnsembleMode::Both;
            preset.ensemble.mix = 0.55F;
            const std::string text = preset.serialize();
            std::string error;
            const auto parsed = SynthPreset::parse(text, &error);
            require(parsed.has_value(), ("parse failed: " + error).c_str());
            require(parsed->distortion.mode == DistortionMode::Fuzz, "distortion.mode lost");
            require(parsed->bitcrusher.enabled && parsed->bitcrusher.bits == 4 &&
                    parsed->bitcrusher.downsample == 8 &&
                    std::abs(parsed->bitcrusher.mix - 0.7F) < 1e-6F, "bitcrusher lost");
            require(parsed->harmonizer.enabled &&
                    std::abs(parsed->harmonizer.subLevel - 0.7F) < 1e-6F &&
                    std::abs(parsed->harmonizer.upLevel - 0.5F) < 1e-6F &&
                    std::abs(parsed->harmonizer.mix - 0.6F) < 1e-6F, "harmonizer lost");
            require(parsed->flanger.enabled &&
                    std::abs(parsed->flanger.rateHertz - 0.5F) < 1e-6F &&
                    std::abs(parsed->flanger.depthMilliseconds - 4.0F) < 1e-6F &&
                    std::abs(parsed->flanger.feedback - -0.6F) < 1e-6F &&
                    std::abs(parsed->flanger.mix - 0.5F) < 1e-6F, "flanger lost");
            require(parsed->ensemble.enabled && parsed->ensemble.mode == EnsembleMode::Both &&
                    std::abs(parsed->ensemble.mix - 0.55F) < 1e-6F, "ensemble lost");
            std::cout << "round-trip: OK\n";
        }

        // 7. Old-format preset (no new keys) still parses; new fields take defaults.
        {
            const std::string legacy = "DVE_SYNTH_PRESET=5\nname=Legacy\n";
            std::string error;
            const auto parsed = SynthPreset::parse(legacy, &error);
            require(parsed.has_value(), ("legacy parse failed: " + error).c_str());
            require(parsed->distortion.mode == DistortionMode::Classic, "legacy distortion.mode default wrong");
            require(!parsed->flanger.enabled && !parsed->bitcrusher.enabled &&
                    !parsed->harmonizer.enabled && !parsed->ensemble.enabled,
                    "legacy new-effect defaults wrong");
            require(parsed->ensemble.mode == EnsembleMode::I, "legacy ensemble.mode default wrong");
            std::cout << "legacy compat: OK\n";
        }

        std::cout << "ALL FX TESTS PASSED\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
