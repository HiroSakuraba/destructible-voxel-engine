// Verification for SynthPreset::builtin_presets():
// names unique, presets validate, renders are finite and level-safe, and every
// pitched preset keeps the played MIDI note as the perceived fundamental
// (the octave-flat voicing bug found in the default preset must not recur).
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

// In-place radix-2 FFT (magnitudes only). Used for an honest broadband
// pitch check: detuned presets spread the fundamental across beating
// sidebands, so we sum spectral regions instead of picking single bins.
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

double band_energy_fft(const std::vector<float>& interleaved, double loHz, double hiHz,
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

std::vector<float> render_preset(const dve::audio::SynthPreset& preset, std::uint8_t midiNote) {
    using namespace dve::audio;
    Synthesizer synth(48000);
    synth.set_preset(preset);
    require(synth.note_on(midiNote, 0.9F), "note-on failed");
    std::vector<float> audio(48000 * 2, 0.0F); // 1 second, stereo
    synth.render(audio);
    return audio;
}
} // namespace

int main() {
    using namespace dve::audio;
    try {
        const auto presets = SynthPreset::builtin_presets();
        require(presets.size() >= 8, "expected at least 8 factory presets");

        std::set<std::string> names;
        for (const auto& preset : presets) {
            require(!preset.name.empty(), "preset with empty name");
            require(names.insert(preset.name).second, "duplicate preset name");
            std::string error;
            require(preset.validate(&error), ("preset failed validation: " + error).c_str());
        }
        std::cout << "names+validate: " << presets.size() << " presets OK\n";

        // "Noise Sweep FX" is the only deliberately non-pitched preset.
        const std::set<std::string> nonPitched{"Noise Sweep FX"};
        for (const auto& preset : presets) {
            const auto audio = render_preset(preset, 69);
            float peak = 0.0F;
            for (float s : audio) {
                require(std::isfinite(s), "non-finite sample rendered");
                peak = std::max(peak, std::abs(s));
            }
            require(peak > 0.01F, "preset rendered (near) silence");
            require(peak <= 1.0F, "preset clipped past 0 dBFS");
            if (nonPitched.count(preset.name) != 0U) {
                std::cout << preset.name << ": peak=" << peak << " (non-pitched, skipped pitch check)\n";
                continue;
            }
            // (comment above describes the broadband region check)
            // Broadband region comparison: detuned presets spread the
            // fundamental across beating sidebands, so sum whole regions.
            const double eSub = band_energy_fft(audio, 180.0, 260.0, 48000.0);
            const double eFund = band_energy_fft(audio, 400.0, 480.0, 48000.0);
            const double eOct = band_energy_fft(audio, 840.0, 920.0, 48000.0);
            require(eFund > 1.5 * eSub && eFund > eOct,
                    ("fundamental region not dominant in " + preset.name).c_str());
            std::cout << preset.name << ": peak=" << peak << " fundamental dominant OK\n";

            // Preset save/load round-trip keeps the voice intact.
            const std::string text = preset.serialize();
            require(!text.empty(), "serialize produced empty text");
            auto parsed = SynthPreset::parse(text, nullptr);
            require(parsed.has_value(), "parse of serialized preset failed");
            require(parsed->name == preset.name, "round-trip changed preset name");
        }
        // The default preset must obey the same pitch rule: the octave-flat
        // voicing bug was found in make_default(), so it is checked directly.
        {
            const auto defAudio = render_preset(SynthPreset::make_default(), 69);
            for (float s : defAudio) require(std::isfinite(s), "default preset non-finite");
            const double eSub = band_energy_fft(defAudio, 180.0, 260.0, 48000.0);
            const double eFund = band_energy_fft(defAudio, 400.0, 480.0, 48000.0);
            const double eOct = band_energy_fft(defAudio, 840.0, 920.0, 48000.0);
            require(eFund > 1.5 * eSub && eFund > eOct,
                    "fundamental region not dominant in default preset");
            std::cout << "default preset: fundamental dominant OK\n";
        }
        std::cout << "preset render/pitch/round-trip: ALL PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
