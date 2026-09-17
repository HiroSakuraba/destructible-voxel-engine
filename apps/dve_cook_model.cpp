#include "dve/asset_cooker.hpp"
#include "dve/dvox.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::string require_value(int& index, int argc, char** argv, std::string_view option) {
    if (index + 1 >= argc) throw std::runtime_error("missing value for " + std::string(option));
    return argv[++index];
}

[[nodiscard]] dve::VoxelizationMode parse_mode(std::string_view value) {
    if (value == "solid") return dve::VoxelizationMode::Solid;
    if (value == "shell") return dve::VoxelizationMode::Shell;
    if (value == "surface") return dve::VoxelizationMode::SurfaceOnly;
    throw std::runtime_error("mode must be solid, shell, or surface");
}

void print_usage() {
    std::cout <<
        "dve_cook_model <source.gltf|source.glb|source.obj> --output <asset.dvox|scene.dvoxscene.json> [options]\n"
        "Options:\n"
        "  --report <report.html>\n"
        "  --settings <asset.dve-import.json>\n"
        "  --split-nodes\n"
        "  --mode solid|shell|surface\n"
        "  --voxel-size <meters>\n"
        "  --shell-thickness <voxels>\n"
        "  --supersample 1|2|4\n"
        "  --coverage <0..1>\n"
        "  --palette-limit <2..256>\n"
        "  --max-working-voxels <count>\n"
        "  --object-id <uint64>\n"
        "  --no-preserve-thin\n"
        "  --non-strict\n";
}

[[nodiscard]] bool has_errors(const std::vector<dve::ImportDiagnostic>& diagnostics) {
    bool result = false;
    for (const dve::ImportDiagnostic& diagnostic : diagnostics) {
        std::cerr << dve::to_string(diagnostic.severity) << " [" << diagnostic.code << "] "
                  << diagnostic.message << '\n';
        result = result || diagnostic.severity == dve::ImportDiagnostic::Severity::Error;
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            print_usage();
            return 2;
        }
        const std::filesystem::path source = argv[1];
        std::filesystem::path outputPath;
        std::filesystem::path reportPath;
        std::filesystem::path settingsPath;
        dve::ModelImportOptions importOptions;
        dve::SceneCookSettings sceneSettings;
        sceneSettings.splitByNode = false;
        bool splitNodes = false;

        for (int index = 2; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--settings") settingsPath = require_value(index, argc, argv, argument);
        }
        if (!settingsPath.empty()) {
            std::string settingsError;
            if (!dve::apply_scene_cook_settings_json(settingsPath, sceneSettings, &settingsError)) {
                throw std::runtime_error("settings load failed: " + settingsError);
            }
            splitNodes = sceneSettings.splitByNode;
        }

        dve::VoxelizeSettings& voxelSettings = sceneSettings.defaults;
        for (int index = 2; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--output") outputPath = require_value(index, argc, argv, argument);
            else if (argument == "--report") reportPath = require_value(index, argc, argv, argument);
            else if (argument == "--settings") (void)require_value(index, argc, argv, argument);
            else if (argument == "--split-nodes") splitNodes = true;
            else if (argument == "--mode") voxelSettings.mode = parse_mode(require_value(index, argc, argv, argument));
            else if (argument == "--voxel-size") voxelSettings.voxelSizeMeters = std::stof(require_value(index, argc, argv, argument));
            else if (argument == "--shell-thickness") voxelSettings.shellThicknessVoxels = static_cast<std::uint32_t>(
                std::stoul(require_value(index, argc, argv, argument)));
            else if (argument == "--supersample") voxelSettings.supersampleFactor = static_cast<std::uint32_t>(
                std::stoul(require_value(index, argc, argv, argument)));
            else if (argument == "--coverage") voxelSettings.downsampleCoverageThreshold = std::stof(
                require_value(index, argc, argv, argument));
            else if (argument == "--palette-limit") voxelSettings.maximumPaletteMaterials = static_cast<std::uint32_t>(
                std::stoul(require_value(index, argc, argv, argument)));
            else if (argument == "--max-working-voxels") voxelSettings.maximumWorkingVoxels = std::stoull(
                require_value(index, argc, argv, argument));
            else if (argument == "--object-id") voxelSettings.objectId = std::stoull(require_value(index, argc, argv, argument));
            else if (argument == "--no-preserve-thin") voxelSettings.preserveThinSurface = false;
            else if (argument == "--non-strict") importOptions.strict = false;
            else if (argument == "--help" || argument == "-h") {
                print_usage();
                return 0;
            } else {
                throw std::runtime_error("unknown argument: " + std::string(argument));
            }
        }
        if (outputPath.empty()) throw std::runtime_error("--output is required");
        if (!(voxelSettings.downsampleCoverageThreshold > 0.0F && voxelSettings.downsampleCoverageThreshold <= 1.0F)) {
            throw std::runtime_error("--coverage must be in (0, 1]");
        }
        if (voxelSettings.maximumPaletteMaterials < 2 || voxelSettings.maximumPaletteMaterials > 256) {
            throw std::runtime_error("--palette-limit must be in [2, 256]");
        }

        sceneSettings.splitByNode = splitNodes;
        std::string error;
        if (splitNodes) {
            dve::CookedVoxelScene scene = dve::cook_model_scene(source, importOptions, sceneSettings);
            if (has_errors(scene.diagnostics)) return 1;
            if (!dve::write_dvox_scene_package(outputPath, scene, &error)) {
                throw std::runtime_error("DVOX scene write failed: " + error);
            }
            std::uint64_t voxels = 0;
            std::uint64_t bricks = 0;
            for (const auto& object : scene.objects) {
                voxels += object.asset.stats.outputVoxels;
                bricks += object.asset.stats.outputBricks;
            }
            std::cout << "{\n"
                      << "  \"source\": \"" << source.string() << "\",\n"
                      << "  \"output\": \"" << outputPath.string() << "\",\n"
                      << "  \"objects\": " << scene.objects.size() << ",\n"
                      << "  \"output_voxels\": " << voxels << ",\n"
                      << "  \"output_bricks\": " << bricks << "\n"
                      << "}\n";
            return 0;
        }

        dve::CookedVoxelAsset asset = dve::cook_model(source, importOptions, voxelSettings);
        if (has_errors(asset.diagnostics)) return 1;
        if (!dve::write_dvox(outputPath, asset, {}, &error)) {
            throw std::runtime_error("DVOX write failed: " + error);
        }
        if (!reportPath.empty() && !dve::write_import_report_html(reportPath, source.string(), asset, &error)) {
            throw std::runtime_error("report write failed: " + error);
        }

        std::cout << "{\n"
                  << "  \"source\": \"" << source.string() << "\",\n"
                  << "  \"output\": \"" << outputPath.string() << "\",\n"
                  << "  \"mode\": \"" << dve::to_string(voxelSettings.mode) << "\",\n"
                  << "  \"voxel_size_meters\": " << asset.voxelSizeMeters << ",\n"
                  << "  \"source_triangles\": " << asset.stats.sourceTriangles << ",\n"
                  << "  \"decoded_images\": " << asset.stats.decodedImages << ",\n"
                  << "  \"palette_materials\": " << asset.stats.paletteMaterials << ",\n"
                  << "  \"output_voxels\": " << asset.stats.outputVoxels << ",\n"
                  << "  \"output_bricks\": " << asset.stats.outputBricks << ",\n"
                  << "  \"state_hash\": " << asset.object.state_hash() << "\n"
                  << "}\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_cook_model: " << exception.what() << '\n';
        return 2;
    }
}
