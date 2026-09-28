// Phase 2 synthesizer FX verification: tempo-synced delay, diffusion delay,
// and extended waveshaper (distortion mode) coverage.
//
// Covers:
//  1. Tempo-synced delay: rendered echo spacing equals syncBeats*60/bpm for
//     the internal clock and the game-clock source; manual timeSeconds is
//     unaffected when tempoSync is off; computed times clamp to the 1.95 s
//     delay-line maximum.
//  2. Diffusion delay: output differs from the plain delay at matched
//     time/feedback/mix; echoes decay over time with no runaway feedback;
//     the diffusion amount audibly changes the smear.
//  3. Waveshaper: SoftClip and Foldback render finite non-silent audio and
//     differ from Classic, Fuzz, and each other; validate() accepts modes 2-3
//     and rejects mode 4; serialize/parse round-trips every new parameter and
//     stays compatible with preset text that predates the new keys.
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/audio/synthesizer.hpp"

namespace {
constexpr double kSampleRate = 48000.0;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// A clean single-sine test voice: everything else off. Short percussive
// envelope so echoes are measurable as distinct repeats.
dve::audio::SynthPreset blip_voice() {
    using namespace dve::audio;
    SynthPreset preset = SynthPreset::make_default();
    for (auto& osc : preset.oscillators) osc.enabled = false;
    auto& osc = preset.oscillators[0];
    osc.enabled = true;
    osc.waveform = OscillatorWaveform::Sine;
    osc.gain = 0.5F;
    osc.pan = 0.0F;
    preset.ampEnvelope = AdsrParameters{0.001F, 0.03F, 0.0F, 0.05F, EnvelopeCurve::Exponential};
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
    preset.diffusionDelay.enabled = false;
    preset.reverb.enabled = false;
    preset.compressor.enabled = false;
    preset.limiter.enabled = false;
    return preset;
}

// Sustained voice for the waveshaper tests (default amp envelope sustains).
dve::audio::SynthPreset sustained_voice() {
    using namespace dve::audio;
    SynthPreset preset = blip_voice();
    preset.ampEnvelope = AdsrParameters{0.005F, 0.16F, 0.72F, 0.35F, EnvelopeCurve::Exponential};
    return preset;
}

std::vector<float> render_blip(const dve::audio::SynthPreset& preset, double seconds) {
    using namespace dve::audio;
    Synthesizer synth(48000);
    synth.set_preset(preset);
    require(synth.note_on(69, 0.9F), "note-on failed");
    std::vector<float> audio(static_cast<std::size_t>(kSampleRate * seconds) * 2, 0.0F);
    synth.render(audio);
    for (float s : audio) require(std::isfinite(s), "non-finite sample rendered");
    return audio;
}

float peak_abs(const std::vector<float>& audio) {
    float peak = 0.0F;
    for (float s : audio) peak = std::max(peak, std::abs(s));
    return peak;
}

float peak_abs_window(const std::vector<float>& audio, double fromSeconds, double toSeconds) {
    const std::size_t frames = audio.size() / 2;
    const std::size_t from = static_cast<std::size_t>(fromSeconds * kSampleRate);
    const std::size_t to = std::min(frames, static_cast<std::size_t>(toSeconds * kSampleRate));
    float peak = 0.0F;
    for (std::size_t i = from; i < to; ++i)
        peak = std::max(peak, std::abs(audio[i * 2]));
    return peak;
}

float max_diff(const std::vector<float>& a, const std::vector<float>& b) {
    require(a.size() == b.size(), "render size mismatch");
    float d = 0.0F;
    for (std::size_t i = 0; i < a.size(); ++i) d = std::max(d, std::abs(a[i] - b[i]));
    return d;
}

// Echo spacings (seconds) between successive repeats on the left channel.
// Uses a 4 ms block-maximum envelope with a refractory period so the 440 Hz
// carrier inside each echo cannot register as separate peaks. Callers must
// render wet-only (delay mix = 1) so the dry blip is absent.
std::vector<double> echo_spacings(const std::vector<float>& interleaved, double refractorySeconds,
                                  float threshold) {
    const std::size_t frames = interleaved.size() / 2;
    const std::size_t block = static_cast<std::size_t>(kSampleRate * 0.004);
    const std::size_t blocks = frames / block;
    std::vector<float> envelope(blocks, 0.0F);
    for (std::size_t b = 0; b < blocks; ++b) {
        float peak = 0.0F;
        for (std::size_t i = b * block; i < (b + 1) * block; ++i)
            peak = std::max(peak, std::abs(interleaved[i * 2]));
        envelope[b] = peak;
    }
    const std::size_t refractory = static_cast<std::size_t>(refractorySeconds * kSampleRate / block);
    std::vector<double> peaks;
    for (std::size_t b = 0; b < blocks; ++b) {
        if (envelope[b] < threshold) continue;
        bool isMax = true;
        const std::size_t lo = b > refractory ? b - refractory : 0;
        const std::size_t hi = std::min(blocks, b + refractory + 1);
        for (std::size_t k = lo; k < hi; ++k) {
            if (k != b && envelope[k] > envelope[b]) { isMax = false; break; }
        }
        if (isMax) peaks.push_back(static_cast<double>(b * block) / kSampleRate);
    }
    std::vector<double> spacings;
    for (std::size_t i = 1; i < peaks.size(); ++i) spacings.push_back(peaks[i] - peaks[i - 1]);
    return spacings;
}

void require_spacings(const std::vector<double>& spacings, double expected, double tolerance,
                      const char* what) {
    require(spacings.size() >= 2, "too few echoes detected for spacing measurement");
    for (double spacing : spacings) {
        if (std::abs(spacing - expected) > tolerance) {
            throw std::runtime_error(std::string(what) + ": echo spacing " +
                                     std::to_string(spacing) + "s, expected " +
                                     std::to_string(expected) + "s");
        }
    }
}
} // namespace

int main() {
    using namespace dve::audio;
    try {
        // 1. Tempo-synced delay, internal clock: quarter note at 120 bpm = 0.5 s.
        {
            SynthPreset preset = blip_voice();
            preset.delay.enabled = true;
            preset.delay.tempoSync = true;
            preset.delay.syncBeats = 1.0F;
            preset.delay.feedback = 0.5F;
            preset.delay.mix = 1.0F;
            preset.delay.pingPong = false;
            preset.arpeggiator.clockSource = ArpeggiatorClockSource::Internal;
            preset.arpeggiator.tempoBpm = 120.0F;
            const auto audio = render_blip(preset, 3.0);
            require_spacings(echo_spacings(audio, 0.3, 0.02F), 0.5, 0.01,
                             "tempo-synced delay (internal clock)");
            std::cout << "tempo sync internal (1 beat @ 120bpm = 0.5s): OK\n";
        }

        // 2. Tempo-synced delay, game-clock source: eighth note at 100 bpm = 0.3 s.
        // externalTempoBpm is the preset's game-clock tempo, so no call ordering
        // pitfalls with set_game_clock_tempo().
        {
            SynthPreset preset = blip_voice();
            preset.delay.enabled = true;
            preset.delay.tempoSync = true;
            preset.delay.syncBeats = 0.5F;
            preset.delay.feedback = 0.5F;
            preset.delay.mix = 1.0F;
            preset.delay.pingPong = false;
            preset.arpeggiator.clockSource = ArpeggiatorClockSource::GameClock;
            preset.arpeggiator.externalTempoBpm = 100.0F;
            const auto audio = render_blip(preset, 3.0);
            require_spacings(echo_spacings(audio, 0.2, 0.02F), 0.3, 0.01,
                             "tempo-synced delay (game clock)");
            std::cout << "tempo sync game clock (0.5 beat @ 100bpm = 0.3s): OK\n";
        }

        // 3. Manual mode unaffected: tempoSync off, timeSeconds rules even when
        // syncBeats would imply something else.
        {
            SynthPreset preset = blip_voice();
            preset.delay.enabled = true;
            preset.delay.tempoSync = false;
            preset.delay.timeSeconds = 0.31F;
            preset.delay.syncBeats = 2.0F; // must be ignored while tempoSync is off
            preset.delay.feedback = 0.5F;
            preset.delay.mix = 1.0F;
            preset.delay.pingPong = false;
            preset.arpeggiator.clockSource = ArpeggiatorClockSource::Internal;
            preset.arpeggiator.tempoBpm = 120.0F;
            const auto audio = render_blip(preset, 3.0);
            require_spacings(echo_spacings(audio, 0.2, 0.02F), 0.31, 0.01, "manual delay time");
            std::cout << "manual delay (tempoSync off, 0.31s): OK\n";
        }

        // 4. Computed times clamp to the delay line maximum (1.95 s):
        // 4 beats at 20 bpm = 12 s -> clamped.
        {
            SynthPreset preset = blip_voice();
            preset.delay.enabled = true;
            preset.delay.tempoSync = true;
            preset.delay.syncBeats = 4.0F;
            preset.delay.feedback = 0.5F;
            preset.delay.mix = 1.0F;
            preset.delay.pingPong = false;
            preset.arpeggiator.clockSource = ArpeggiatorClockSource::Internal;
            preset.arpeggiator.tempoBpm = 20.0F;
            const auto audio = render_blip(preset, 6.5);
            require_spacings(echo_spacings(audio, 1.0, 0.02F), 1.95, 0.01,
                             "tempo-synced delay clamp");
            std::cout << "tempo sync clamp (4 beats @ 20bpm -> 1.95s): OK\n";
        }

        // 5. Diffusion delay differs from the plain delay at matched
        // time/feedback/mix.
        {
            SynthPreset plain = blip_voice();
            plain.delay.enabled = true;
            plain.delay.tempoSync = false;
            plain.delay.timeSeconds = 0.31F;
            plain.delay.feedback = 0.6F;
            plain.delay.mix = 1.0F;
            plain.delay.pingPong = false;
            SynthPreset diffused = blip_voice();
            diffused.diffusionDelay.enabled = true;
            diffused.diffusionDelay.timeSeconds = 0.31F;
            diffused.diffusionDelay.feedback = 0.6F;
            diffused.diffusionDelay.mix = 1.0F;
            diffused.diffusionDelay.diffusion = 0.8F;
            const auto plainAudio = render_blip(plain, 3.0);
            const auto diffusedAudio = render_blip(diffused, 3.0);
            const float difference = max_diff(plainAudio, diffusedAudio);
            require(difference > 0.05F, "diffusion delay output matches plain delay");
            std::cout << "diffusion vs plain delay: max diff " << difference << " OK\n";
        }

        // 6. Diffusion delay decays over time with no runaway feedback.
        {
            SynthPreset preset = blip_voice();
            preset.diffusionDelay.enabled = true;
            preset.diffusionDelay.timeSeconds = 0.31F;
            preset.diffusionDelay.feedback = 0.6F;
            preset.diffusionDelay.mix = 1.0F;
            preset.diffusionDelay.diffusion = 1.0F;
            const auto audio = render_blip(preset, 4.0);
            const float peak = peak_abs(audio);
            require(peak <= 1.0F, "diffusion delay exceeded 0 dBFS");
            const float head = peak_abs_window(audio, 0.0, 1.0);
            const float tail = peak_abs_window(audio, 3.0, 4.0);
            require(tail < head * 0.35F, "diffusion delay tail did not decay");
            std::cout << "diffusion decay: head " << head << " tail " << tail << " OK\n";
        }

        // 7. The diffusion amount audibly changes the smear.
        {
            auto render_diffusion = [](float diffusion) {
                SynthPreset preset = blip_voice();
                preset.diffusionDelay.enabled = true;
                preset.diffusionDelay.timeSeconds = 0.31F;
                preset.diffusionDelay.feedback = 0.6F;
                preset.diffusionDelay.mix = 1.0F;
                preset.diffusionDelay.diffusion = diffusion;
                return render_blip(preset, 3.0);
            };
            const float difference = max_diff(render_diffusion(0.0F), render_diffusion(1.0F));
            require(difference > 0.01F, "diffusion amount did not change the output");
            std::cout << "diffusion amount: max diff " << difference << " OK\n";
        }

        // 8. New waveshaper curves render finite, non-silent, level-safe audio
        // and differ from Classic, Fuzz, and each other.
        {
            auto render_mode = [](DistortionMode mode) {
                SynthPreset preset = sustained_voice();
                preset.distortion.enabled = true;
                preset.distortion.mode = mode;
                preset.distortion.drive = 4.0F;
                preset.distortion.mix = 1.0F;
                Synthesizer synth(48000);
                synth.set_preset(preset);
                require(synth.note_on(69, 0.9F), "note-on failed");
                std::vector<float> audio(static_cast<std::size_t>(kSampleRate * 1.0) * 2, 0.0F);
                synth.render(audio);
                for (float s : audio) require(std::isfinite(s), "non-finite sample rendered");
                return audio;
            };
            const auto classic = render_mode(DistortionMode::Classic);
            const auto fuzz = render_mode(DistortionMode::Fuzz);
            const auto softClip = render_mode(DistortionMode::SoftClip);
            const auto foldback = render_mode(DistortionMode::Foldback);
            struct Named { const char* name; const std::vector<float>* audio; };
            const Named named[] = {{"softclip", &softClip}, {"foldback", &foldback}};
            for (const Named& n : named) {
                const float peak = peak_abs(*n.audio);
                require(peak > 0.05F, "waveshaper mode rendered (near) silence");
                require(peak <= 1.0F, "waveshaper mode exceeded 0 dBFS");
            }
            require(max_diff(softClip, classic) > 0.01F, "softclip identical to classic");
            require(max_diff(foldback, classic) > 0.01F, "foldback identical to classic");
            require(max_diff(foldback, fuzz) > 0.01F, "foldback identical to fuzz");
            require(max_diff(softClip, foldback) > 0.01F, "softclip identical to foldback");
            std::cout << "waveshaper curves (SoftClip/Foldback): OK\n";
        }

        // 9. validate() accepts the new modes and delay fields, rejects
        // out-of-range values.
        {
            SynthPreset preset = SynthPreset::make_default();
            std::string error;
            preset.distortion.mode = DistortionMode::SoftClip;
            preset.delay.tempoSync = true;
            preset.delay.syncBeats = 1.5F;
            preset.diffusionDelay.enabled = true;
            preset.diffusionDelay.diffusion = 0.9F;
            require(preset.validate(&error), ("validate failed: " + error).c_str());
            preset.distortion.mode = DistortionMode::Foldback;
            require(preset.validate(&error), ("validate failed: " + error).c_str());
            preset.distortion.mode = static_cast<DistortionMode>(4);
            require(!preset.validate(&error), "validate accepted distortion mode 4");
            preset.distortion.mode = DistortionMode::Classic;
            preset.delay.syncBeats = 64.0F;
            require(!preset.validate(&error), "validate accepted syncBeats 64");
            preset.delay.syncBeats = 1.0F;
            preset.diffusionDelay.diffusion = 1.5F;
            require(!preset.validate(&error), "validate accepted diffusion 1.5");
            std::cout << "validate: OK\n";
        }

        // 10. Serialize/parse round-trips every new parameter.
        {
            SynthPreset preset = SynthPreset::make_default();
            preset.delay.tempoSync = true;
            preset.delay.syncBeats = 1.5F;
            preset.diffusionDelay.enabled = true;
            preset.diffusionDelay.timeSeconds = 0.42F;
            preset.diffusionDelay.feedback = 0.55F;
            preset.diffusionDelay.mix = 0.33F;
            preset.diffusionDelay.diffusion = 0.8F;
            preset.distortion.mode = DistortionMode::Foldback;
            std::string error;
            const std::string text = preset.serialize();
            auto parsed = SynthPreset::parse(text, &error);
            require(parsed.has_value(), ("parse failed: " + error).c_str());
            require(parsed->delay.tempoSync, "delay.tempoSync lost in round-trip");
            require(parsed->delay.syncBeats == 1.5F, "delay.syncBeats lost in round-trip");
            require(parsed->diffusionDelay.enabled, "diffusionDelay.enabled lost in round-trip");
            require(parsed->diffusionDelay.timeSeconds == 0.42F, "diffusionDelay.time lost");
            require(parsed->diffusionDelay.feedback == 0.55F, "diffusionDelay.feedback lost");
            require(parsed->diffusionDelay.mix == 0.33F, "diffusionDelay.mix lost");
            require(parsed->diffusionDelay.diffusion == 0.8F, "diffusionDelay.diffusion lost");
            require(parsed->distortion.mode == DistortionMode::Foldback,
                    "distortion.mode lost in round-trip");
            std::cout << "serialize/parse round-trip: OK\n";
        }

        // 11. Preset text predating the new keys still parses with defaults.
        {
            const std::string text = SynthPreset::make_default().serialize();
            std::string legacy;
            for (std::size_t pos = 0; pos < text.size();) {
                const std::size_t end = text.find('\n', pos);
                const std::string line = text.substr(pos, end == std::string::npos ? end : end - pos);
                if (line.rfind("delay.tempoSync=", 0) != 0 && line.rfind("delay.syncBeats=", 0) != 0 &&
                    line.rfind("diffusionDelay.", 0) != 0) {
                    legacy += line;
                    legacy += '\n';
                }
                if (end == std::string::npos) break;
                pos = end + 1;
            }
            std::string error;
            auto parsed = SynthPreset::parse(legacy, &error);
            require(parsed.has_value(), ("legacy parse failed: " + error).c_str());
            require(!parsed->delay.tempoSync, "legacy delay.tempoSync default wrong");
            require(parsed->delay.syncBeats == 1.0F, "legacy delay.syncBeats default wrong");
            require(!parsed->diffusionDelay.enabled, "legacy diffusionDelay.enabled default wrong");
            require(parsed->distortion.mode == DistortionMode::Classic,
                    "legacy distortion.mode default wrong");
            std::cout << "legacy preset compat: OK\n";
        }

        std::cout << "phase2 fx tests: ALL PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
