// Sounds scheduled to start partway through a mixer chunk.
//
// The mixer renders in chunks of up to 1,024 frames and used to decide which voices are mixed
// (physical) only at the start of each chunk. A voice whose start frame fell inside the chunk
// was left out of that decision, yet the render loop still advanced its read position from the
// start frame on, without mixing it. The sound lost its first samples, and a sound shorter than
// the rest of the chunk was never heard. Voices are now reclassified at each scheduled start.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/audio/mixer.hpp"

namespace {
using namespace dve::audio;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint32_t kRate = 48000U;
constexpr std::size_t kChunk = 1024U;   // the mixer's render quantum
constexpr std::size_t kClickFrames = 64U;

ResidentSampleDesc constant(float value, std::size_t frames, const char* name) {
    ResidentSampleDesc sample;
    sample.name = name;
    sample.sampleRate = kRate;
    sample.channels = 1;
    sample.samples.assign(frames, value);
    return sample;
}

struct Heard {
    std::size_t first{};   // first output frame with sound, relative to the render start
    double energy{};       // sum of |left| over the 256 frames from that onset
};

// The bus processing leaves a long, faint tail after the click, so energy is measured over a
// fixed window from the onset; a window up to the end of the render would shrink with the offset.
Heard listen(const std::vector<float>& audio) {
    const std::size_t frames = audio.size() / 2U;
    Heard heard{frames, 0.0};
    for (std::size_t frame = 0; frame < frames; ++frame)
        if (std::fabs(audio[frame * 2U]) > 1.0e-6F) { heard.first = frame; break; }
    for (std::size_t frame = heard.first; frame < std::min(frames, heard.first + 256U); ++frame)
        heard.energy += std::fabs(audio[frame * 2U]);
    return heard;
}

// Plays a 64-frame click `offset` frames after the start of a render, with `background` silent
// looping voices of lower priority already occupying voice slots.
Heard play_click_at(std::size_t offset, std::size_t background = 0U,
                    AudioPriority priority = AudioPriority::Normal) {
    AudioMixer mixer(kRate);
    std::string error;
    const SampleId click = mixer.register_resident_sample(constant(0.5F, kClickFrames, "click"), &error);
    const SampleId silence = mixer.register_resident_sample(constant(0.0F, 512U, "silence"), &error);
    require(click && silence, "could not register test samples: " + error);

    PlaySampleDesc quiet;
    quiet.sample = silence;
    quiet.loop = true;
    quiet.spatialized = false;
    quiet.priority = AudioPriority::Background;
    for (std::size_t i = 0; i < background; ++i) require(static_cast<bool>(mixer.play_sample(quiet)), "queue full");

    // One chunk first, so the click's start frame is never 0 (which means "now").
    std::vector<float> warmup(kChunk * 2U);
    mixer.render(warmup);
    const std::uint64_t base = mixer.current_frame();

    PlaySampleDesc play;
    play.sample = click;
    play.spatialized = false;
    play.priority = priority;
    play.sampleFrame = base + offset;
    require(static_cast<bool>(mixer.play_sample(play)), "could not schedule the click");

    std::vector<float> audio(4U * kChunk * 2U);
    mixer.render(audio);
    return listen(audio);
}

void test_start_offsets() {
    const Heard reference = play_click_at(0U);
    require(reference.energy > 1.0, "the reference click is silent");
    for (const std::size_t offset : {std::size_t{1}, std::size_t{256}, std::size_t{900}, kChunk - 1U, kChunk,
                                     kChunk + 300U, 2U * kChunk + 1000U}) {
        const Heard heard = play_click_at(offset);
        require(heard.first == reference.first + offset,
                "click scheduled at offset " + std::to_string(offset) + " first sounded at " +
                    std::to_string(heard.first) + ", expected " + std::to_string(reference.first + offset));
        require(std::fabs(heard.energy - reference.energy) <= 0.001 * reference.energy,
                "click scheduled at offset " + std::to_string(offset) + " lost samples (energy " +
                    std::to_string(heard.energy) + " vs " + std::to_string(reference.energy) + ")");
    }
    std::printf("scheduled starts inside and across chunks: OK\n");
}

// All physical slots are taken by lower-priority voices: a higher-priority voice that starts
// mid-chunk must take a slot from its start frame, not wait for (or miss) the next chunk.
void test_start_under_voice_pressure() {
    const Heard reference = play_click_at(0U, kMaxPhysicalSampleVoices + 12U, AudioPriority::Critical);
    require(reference.energy > 1.0, "the reference click is silent under voice pressure");
    const Heard heard = play_click_at(300U, kMaxPhysicalSampleVoices + 12U, AudioPriority::Critical);
    require(heard.first == reference.first + 300U, "click under voice pressure started late");
    require(std::fabs(heard.energy - reference.energy) <= 0.001 * reference.energy,
            "click under voice pressure lost samples");
    std::printf("scheduled start under voice pressure: OK\n");
}

} // namespace

int main() {
    try {
        test_start_offsets();
        test_start_under_voice_pressure();
        std::printf("audio mixer scheduled start tests passed\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "dve_audio_mixer_start_tests: FAIL: %s\n", error.what());
        return 1;
    }
}
