#include "dve/audio/synthesizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dve::audio;

namespace {
struct ShowcaseItem {
    const char* relativePath;
    std::uint8_t note;
    double holdSeconds;
    double releaseSeconds;
};

constexpr std::uint32_t kSampleRate = 48000U;
constexpr std::size_t kBlock = 256U;

std::vector<float> render_item(const SynthPreset& preset, const ShowcaseItem& item) {
    const std::size_t held = static_cast<std::size_t>(item.holdSeconds * kSampleRate);
    const std::size_t released = static_cast<std::size_t>(item.releaseSeconds * kSampleRate);
    std::vector<float> output((held + released) * 2U, 0.0F);
    Synthesizer synth{kSampleRate};
    synth.set_preset(preset);
    synth.note_on(item.note, 0.84F, 0U);

    MidiMessage pressure{};
    pressure.type = MidiMessageType::PolyPressure;
    pressure.status = 0xA0U;
    pressure.channel = 0U;
    pressure.data1 = item.note;
    pressure.data2 = 100U;
    pressure.bytes = {0xA0U, item.note, 100U};
    pressure.size = 3U;
    synth.post_midi(pressure);
    synth.control_change(preset.mpe.timbreController, 82U, 0U);

    for (std::size_t f = 0; f < held; f += kBlock) {
        const std::size_t n = std::min(kBlock, held - f);
        synth.render(output.data() + f * 2U, n);
    }
    synth.note_off(item.note, 0.0F, 0U);
    for (std::size_t f = 0; f < released; f += kBlock) {
        const std::size_t n = std::min(kBlock, released - f);
        synth.render(output.data() + (held + f) * 2U, n);
    }

    float peak = 0.0F;
    for (float sample : output) peak = std::max(peak, std::abs(sample));
    const float gain = peak > 1.0e-5F ? std::min(12.0F, 0.55F / peak) : 1.0F;
    const std::size_t fadeFrames = std::min<std::size_t>(480U, output.size() / 4U);
    const std::size_t frames = output.size() / 2U;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float fade = 1.0F;
        if (frame < fadeFrames) fade *= static_cast<float>(frame) / static_cast<float>(fadeFrames);
        if (frames - frame <= fadeFrames)
            fade *= static_cast<float>(frames - frame) / static_cast<float>(fadeFrames);
        output[2U * frame] *= gain * fade;
        output[2U * frame + 1U] *= gain * fade;
    }
    return output;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: dve_physical_preset_showcase <preset-root> <output.wav> <cues.txt>\n";
        return 2;
    }
    const fs::path root = argv[1];
    const fs::path wav = argv[2];
    const fs::path cues = argv[3];
    const std::vector<ShowcaseItem> items{
        {"Plucked/Steel_String_Guitar.dvesynth", 52U, 1.20, 0.80},
        {"Mallets/Marimba.dvesynth", 60U, 0.22, 1.35},
        {"Plates/Bronze_Bell.dvesynth", 60U, 0.18, 1.55},
        {"Bowed/Bowed_Violin.dvesynth", 67U, 1.25, 0.65},
        {"Winds/Flute.dvesynth", 72U, 1.25, 0.60},
        {"Winds/Clarinet.dvesynth", 60U, 1.25, 0.60},
        {"Winds/Trumpet.dvesynth", 60U, 1.25, 0.60},
        {"Nature/Owl_Hoot.dvesynth", 55U, 1.10, 0.55},
        {"Nature/Wind_Through_Trees.dvesynth", 48U, 1.25, 0.55},
        {"Nature/Glass_Shatter_Impact.dvesynth", 72U, 0.10, 1.25},
        {"Mechanical/Diesel_Truck_Idle.dvesynth", 36U, 1.25, 0.55},
        {"Mechanical/Servo_Motor_Whine.dvesynth", 69U, 1.15, 0.55},
        {"Game/Crystal_Shard.dvesynth", 76U, 0.12, 1.25},
        {"Game/Heavy_Concrete_Plate.dvesynth", 43U, 0.12, 1.25},
    };

    std::vector<float> mix;
    std::ofstream cueFile(cues);
    if (!cueFile) {
        std::cerr << "could not open cue output\n";
        return 3;
    }
    cueFile << "DVE physical-model preset showcase (segments peak-normalized for audition)\n";
    const std::size_t gapFrames = kSampleRate / 5U;
    for (const auto& item : items) {
        std::string error;
        const fs::path path = root / item.relativePath;
        auto preset = SynthPreset::load(path, &error);
        if (!preset) {
            std::cerr << path << ": " << error << '\n';
            return 4;
        }
        const double start = static_cast<double>(mix.size() / 2U) / kSampleRate;
        auto segment = render_item(*preset, item);
        mix.insert(mix.end(), segment.begin(), segment.end());
        mix.insert(mix.end(), gapFrames * 2U, 0.0F);
        const double end = static_cast<double>(mix.size() / 2U - gapFrames) / kSampleRate;
        cueFile << start << " - " << end << "  " << preset->name << "  [" << item.relativePath << "]\n";
    }
    fs::create_directories(wav.parent_path());
    std::string error;
    if (!write_float_wav(wav, mix, kSampleRate, &error)) {
        std::cerr << error << '\n';
        return 5;
    }
    std::cout << "wrote " << wav << " with " << items.size() << " presets\n";
    return 0;
}
