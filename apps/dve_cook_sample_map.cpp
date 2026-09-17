#include "dve/audio/sample_map.hpp"
#include "dve/audio/sample_map_recipe.hpp"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: dve_cook_sample_map <recipe.txt> <output.dvesamplemap>\n";
        return 2;
    }
    const std::filesystem::path recipePath(argv[1]);
    const std::filesystem::path outputPath(argv[2]);
    std::string error;
    if (!dve::audio::cook_sample_map_recipe(recipePath, outputPath, &error)) {
        std::cerr << "sample-map cook failed: " << error << '\n';
        return 1;
    }
    dve::audio::SynthSampleMap map;
    if (!dve::audio::load_sample_map(outputPath, map, &error)) {
        std::cerr << "sample-map verification failed: " << error << '\n';
        return 1;
    }
    std::cout << "cooked " << map.zoneCount << " zones / " << map.sourceCount
              << " sources to " << outputPath.string() << " hash=" << map.contentHash
              << " resident_frames=" << map.residentFrameCount << '\n';
    return 0;
}
