#include "dve/polygon_asset.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
std::string require_value(int& index, int argc, char** argv, std::string_view option) {
    if (index + 1 >= argc) throw std::runtime_error("missing value for " + std::string(option));
    return argv[++index];
}
void usage() {
    std::cout << "dve_cook_mesh <source.gltf|source.glb|source.obj> --output <asset.dmesh> [options]\n"
                 "  --object-id <uint64>\n"
                 "  --no-textures\n"
                 "  --keep-missing-normals\n"
                 "  --no-tangents\n"
                 "  --non-strict\n";
}
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) { usage(); return 2; }
        const std::filesystem::path source = argv[1];
        std::filesystem::path output;
        dve::ModelImportOptions importOptions;
        dve::PolygonCookOptions options;
        for (int i = 2; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--output") output = require_value(i, argc, argv, arg);
            else if (arg == "--object-id") options.objectId = std::stoull(require_value(i, argc, argv, arg));
            else if (arg == "--no-textures") options.preserveTextures = false;
            else if (arg == "--keep-missing-normals") options.generateMissingNormals = false;
            else if (arg == "--no-tangents") options.generateTangents = false;
            else if (arg == "--non-strict") importOptions.strict = false;
            else throw std::runtime_error("unknown option: " + std::string(arg));
        }
        if (output.empty()) throw std::runtime_error("--output is required");
        std::vector<dve::ImportDiagnostic> diagnostics;
        dve::CookedPolygonAsset asset = dve::cook_polygon_model(source, importOptions, options, &diagnostics);
        for (const auto& diagnostic : diagnostics) {
            std::cerr << dve::to_string(diagnostic.severity) << " [" << diagnostic.code << "] "
                      << diagnostic.message << '\n';
        }
        std::string error;
        if (!dve::write_dmesh(output, asset, &error)) throw std::runtime_error(error);
        std::cout << "{\n"
                  << "  \"source\": \"" << source.string() << "\",\n"
                  << "  \"output\": \"" << output.string() << "\",\n"
                  << "  \"vertices\": " << asset.vertices.size() << ",\n"
                  << "  \"triangles\": " << asset.indices.size() / 3U << ",\n"
                  << "  \"submeshes\": " << asset.submeshes.size() << ",\n"
                  << "  \"materials\": " << asset.materials.size() << ",\n"
                  << "  \"textures\": " << asset.textures.size() << ",\n"
                  << "  \"content_hash\": " << asset.contentHash << "\n"
                  << "}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dve_cook_mesh: " << e.what() << '\n';
        return 2;
    }
}
