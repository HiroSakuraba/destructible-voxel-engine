#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "dve/cpu_hair.hpp"

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: dve_cook_hair <input-sisir.ply> <output.dvehair> [scale]\n";
        return 2;
    }
    dve::SisirPlyImportOptions options;
    if (argc == 4) {
        char* end = nullptr;
        options.scale = std::strtof(argv[3], &end);
        if (end == argv[3] || *end != '\0') {
            std::cerr << "invalid scale\n";
            return 2;
        }
    }
    const dve::HairAssetReadResult imported = dve::read_sisir_hair_ply(argv[1], options);
    if (!imported) {
        std::cerr << "hair import failed: " << imported.error << '\n';
        return 1;
    }
    std::string error;
    if (!dve::write_dvehair(argv[2], imported.asset, &error)) {
        std::cerr << "hair cook failed: " << error << '\n';
        return 1;
    }
    std::cout << "cooked " << imported.asset.guides.size() << " guides and "
              << imported.asset.point_count() << " points to "
              << std::filesystem::path(argv[2]).string() << '\n';
    return 0;
}
