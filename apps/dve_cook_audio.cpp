#include "dve/audio/audio_asset.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: dve_cook_audio <input media: wav/flac/ogg/mp3/mp4/m4a/aac/opus/...> <output.dvesample> "
                     "[--stream] [--normalize] [--rate N]\n";
        return 2;
    }
    dve::audio::AudioImportOptions options;
    for (int i = 3; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--stream") options.storagePolicy = dve::audio::AudioStoragePolicy::Streamed;
        else if (argument == "--normalize") options.normalize = true;
        else if (argument == "--rate" && i + 1 < argc) {
            options.targetSampleRate = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            return 2;
        }
    }
    std::string error;
    auto asset = dve::audio::import_audio_file(argv[1], options, &error);
    if (!asset) { std::cerr << "import failed: " << error << '\n'; return 1; }
    if (!dve::audio::write_cooked_audio_asset(argv[2], *asset, &error)) {
        std::cerr << "cook failed: " << error << '\n';
        return 1;
    }
    const auto& metadata = asset->metadata;
    std::cout << "cooked " << metadata.name << ": " << static_cast<unsigned>(metadata.channels) << "ch, "
              << metadata.sampleRate << " Hz, " << metadata.frameCount << " frames, peak "
              << metadata.peakLinear << ", approx loudness " << metadata.approximateLoudnessDbfs
              << " dBFS, policy "
              << (metadata.storagePolicy == dve::audio::AudioStoragePolicy::Streamed ? "streamed" : "resident")
              << '\n';
    return 0;
}
