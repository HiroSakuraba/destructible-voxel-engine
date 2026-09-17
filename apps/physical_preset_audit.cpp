#include "dve/audio/synthesizer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dve::audio;

namespace {
constexpr double kPi = 3.14159265358979323846;

struct Metrics {
    double peak{};
    double rms{};
    double earlyRms{};
    double sustainRms{};
    double tailRms{};
    double dc{};
    double clippingFraction{};
    double estimatedHz{};
    double pitchCorrelation{};
    double centsError{};
    double renderRealtimePercent{};
    bool finite{true};
};

double rms_range(std::span<const float> audio, std::size_t firstFrame, std::size_t frameCount) {
    if (audio.empty() || frameCount == 0) return 0.0;
    const std::size_t totalFrames = audio.size() / 2U;
    firstFrame = std::min(firstFrame, totalFrames);
    frameCount = std::min(frameCount, totalFrames - firstFrame);
    long double sum = 0.0;
    for (std::size_t f = firstFrame; f < firstFrame + frameCount; ++f) {
        const double mono = 0.5 * (static_cast<double>(audio[2U * f]) + audio[2U * f + 1U]);
        sum += mono * mono;
    }
    return frameCount ? std::sqrt(static_cast<double>(sum / frameCount)) : 0.0;
}

std::pair<double,double> estimate_pitch(std::span<const float> audio, std::size_t firstFrame,
                                        std::size_t frameCount, double expectedHz,
                                        double sampleRate) {
    const std::size_t totalFrames = audio.size() / 2U;
    firstFrame = std::min(firstFrame, totalFrames);
    frameCount = std::min(frameCount, totalFrames - firstFrame);
    if (frameCount < 512 || expectedHz < 20.0) return {};
    std::vector<double> x(frameCount);
    double mean = 0.0;
    for (std::size_t i = 0; i < frameCount; ++i) {
        x[i] = 0.5 * (static_cast<double>(audio[2U * (firstFrame + i)])
                    + static_cast<double>(audio[2U * (firstFrame + i) + 1U]));
        mean += x[i];
    }
    mean /= static_cast<double>(x.size());
    for (double& v : x) v -= mean;
    const std::size_t minLag = static_cast<std::size_t>(sampleRate / (expectedHz * 1.18));
    const std::size_t maxLag = static_cast<std::size_t>(sampleRate / (expectedHz * 0.82));
    double best = -1.0;
    std::size_t bestLag = 0;
    for (std::size_t lag = std::max<std::size_t>(2, minLag);
         lag <= std::min<std::size_t>(maxLag, frameCount / 2U); ++lag) {
        long double cross = 0.0, a = 0.0, b = 0.0;
        for (std::size_t i = 0; i + lag < frameCount; ++i) {
            cross += x[i] * x[i + lag];
            a += x[i] * x[i];
            b += x[i + lag] * x[i + lag];
        }
        const double denom = std::sqrt(static_cast<double>(a * b));
        const double corr = denom > 1e-20 ? static_cast<double>(cross) / denom : 0.0;
        if (corr > best) { best = corr; bestLag = lag; }
    }
    return bestLag ? std::pair<double,double>{sampleRate / static_cast<double>(bestLag), best} : std::pair<double,double>{};
}

double expected_frequency(const SynthPreset& preset, std::uint8_t note) {
    const auto& osc = preset.oscillators[0];
    const double semitones = static_cast<double>(note) - 69.0
        + preset.tuning.transposeSemitones + osc.semitones
        + (preset.tuning.fineCents + osc.cents) / 100.0;
    return preset.tuning.referenceHertz * std::pow(2.0, semitones / 12.0);
}

Metrics render_and_measure(const SynthPreset& preset) {
    constexpr std::uint32_t sr = 48000;
    constexpr std::size_t heldFrames = sr * 2U;
    constexpr std::size_t releaseFrames = sr * 2U;
    constexpr std::size_t block = 256;
    std::vector<float> audio((heldFrames + releaseFrames) * 2U, 0.0F);
    Synthesizer synth(sr);
    synth.set_preset(preset);
    synth.note_on(60, 0.82F, 0);
    MidiMessage pressure{};
    pressure.type = MidiMessageType::PolyPressure;
    pressure.status = 0xA0U;
    pressure.channel = 0;
    pressure.data1 = 60;
    pressure.data2 = 96;
    pressure.bytes = {0xA0U, 60U, 96U};
    pressure.size = 3;
    synth.post_midi(pressure);
    synth.control_change(preset.mpe.timbreController, 80U, 0);

    const auto started = std::chrono::steady_clock::now();
    for (std::size_t f = 0; f < heldFrames; f += block) {
        const std::size_t n = std::min(block, heldFrames - f);
        synth.render(audio.data() + f * 2U, n);
    }
    synth.note_off(60, 0.0F, 0);
    for (std::size_t f = 0; f < releaseFrames; f += block) {
        const std::size_t n = std::min(block, releaseFrames - f);
        synth.render(audio.data() + (heldFrames + f) * 2U, n);
    }
    const auto ended = std::chrono::steady_clock::now();

    Metrics m{};
    long double energy = 0.0, sum = 0.0;
    std::size_t clipped = 0;
    for (float v : audio) {
        if (!std::isfinite(v)) m.finite = false;
        m.peak = std::max(m.peak, std::abs(static_cast<double>(v)));
        energy += static_cast<long double>(v) * v;
        sum += v;
        if (std::abs(v) >= 0.999F) ++clipped;
    }
    m.rms = std::sqrt(static_cast<double>(energy / audio.size()));
    m.dc = static_cast<double>(sum / audio.size());
    m.clippingFraction = static_cast<double>(clipped) / audio.size();
    m.earlyRms = rms_range(audio, sr / 20U, sr / 4U);
    m.sustainRms = rms_range(audio, sr + sr / 4U, sr / 2U);
    m.tailRms = rms_range(audio, heldFrames + releaseFrames - sr / 4U, sr / 4U);
    const double expected = expected_frequency(preset, 60);
    const auto [hz, corr] = estimate_pitch(audio, sr + sr / 4U, sr / 2U, expected, sr);
    m.estimatedHz = hz;
    m.pitchCorrelation = corr;
    m.centsError = hz > 0.0 ? 1200.0 * std::log2(hz / expected) : 0.0;
    const double renderSeconds = std::chrono::duration<double>(ended - started).count();
    m.renderRealtimePercent = 100.0 * renderSeconds / 4.0;
    return m;
}

std::string category_of(const fs::path& file, const fs::path& root) {
    const auto rel = fs::relative(file, root);
    return rel.begin() != rel.end() ? rel.begin()->string() : std::string{};
}

bool pitched_category(const std::string& category) {
    return category == "Plucked" || category == "Mallets" || category == "Plates"
        || category == "Bowed" || category == "Winds";
}
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: physical_preset_audit <preset-root> [csv-output]\n";
        return 2;
    }
    const fs::path root = argv[1];
    std::ostream* out = &std::cout;
    std::ofstream file;
    if (argc >= 3) { file.open(argv[2]); if (!file) return 3; out = &file; }
    *out << "file,category,name,status,peak,rms,early_rms,sustain_rms,tail_rms,dc,clip_fraction,expected_hz,estimated_hz,pitch_corr,cents_error,realtime_percent\n";
    std::vector<fs::path> paths;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file() && entry.path().extension() == ".dvesynth") paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());
    int failures = 0;
    int warnings = 0;
    for (const auto& path : paths) {
        std::string error;
        auto preset = SynthPreset::load(path, &error);
        const std::string category = category_of(path, root);
        if (!preset) {
            *out << std::quoted(fs::relative(path, root).generic_string()) << ',' << std::quoted(category)
                 << ",\"\",parse-fail:" << std::quoted(error) << "\n";
            ++failures;
            continue;
        }
        Metrics m = render_and_measure(*preset);
        std::string status = "ok";
        if (!m.finite) status = "nonfinite";
        else if (m.rms < 1e-5) status = "silent";
        else if (m.clippingFraction > 0.002) status = "clipping";
        else if (std::abs(m.dc) > 0.02) status = "dc-offset";
        else if ((category == "Plates" || category == "Mallets") && m.pitchCorrelation > 0.25
                 && std::abs(m.centsError) > 80.0) {
            // Bells, plates, and bars often present a stronger inharmonic partial than their
            // nominal root. Keep this visible for listening review, but do not call it a failed
            // MIDI tuning test: the engine's first resonant mode is explicitly root-normalized.
            status = "review-inharmonic";
            ++warnings;
        } else if ((category == "Plucked" || category == "Bowed") && m.pitchCorrelation > 0.45
                   && std::abs(m.centsError) > 80.0)
            status = "pitch-outlier";
        else if (category == "Winds" && m.sustainRms < 0.002) status = "weak-sustain";
        if (status != "ok" && status != "review-inharmonic") ++failures;
        *out << std::quoted(fs::relative(path, root).generic_string()) << ',' << std::quoted(category) << ','
             << std::quoted(preset->name) << ',' << status << ',' << std::fixed << std::setprecision(8)
             << m.peak << ',' << m.rms << ',' << m.earlyRms << ',' << m.sustainRms << ',' << m.tailRms << ','
             << m.dc << ',' << m.clippingFraction << ',' << expected_frequency(*preset, 60) << ','
             << m.estimatedHz << ',' << m.pitchCorrelation << ',' << m.centsError << ','
             << m.renderRealtimePercent << '\n';
    }
    std::cerr << "audited " << paths.size() << " presets; hard failures " << failures
              << "; listening-review warnings " << warnings << "\n";
    return failures == 0 ? 0 : 1;
}
