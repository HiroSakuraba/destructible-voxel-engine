// dve_export_scene: offline editor-scene export (packaging Phase 4, decision D2).
//
//   dve_export_scene <scene.dvescene> <out.dvoxscene.json>
//                    [--materials <library.dvematerials>] [--objects-dir <dir>]
//                    [--name <scene name>] [--strict] [--quiet]
//
// Loads a DVE_EDITOR_SCENE document with the editor library and writes the shipped runtime
// format (DVOXSCENE v1 JSON + one .dvox per object), so dve_player never links dve_editor.
// Warnings list what DVOXSCENE v1 cannot represent (see dve/editor_scene_export.hpp);
// --strict turns them into errors. Exit codes: 0 ok, 1 export failed, 2 usage error.
#include "dve/editor_document.hpp"
#include "dve/editor_materials.hpp"
#include "dve/editor_scene_export.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int usage(const char* message = nullptr) {
    if (message) std::cerr << "dve_export_scene: " << message << '\n';
    std::cerr << "usage: dve_export_scene <scene.dvescene> <out.dvoxscene.json> "
                 "[--materials <file.dvematerials>] [--objects-dir <dir>] [--name <name>] "
                 "[--strict] [--quiet]\n";
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path input;
    std::filesystem::path output;
    std::filesystem::path materialsPath;
    dve::editor::EditorSceneExportOptions options;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&](std::string_view name) -> const char* {
            if (i + 1 >= argc) return nullptr;
            (void)name;
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else if (arg == "--materials") {
            const char* v = value(arg);
            if (!v) return usage("--materials needs a file");
            materialsPath = v;
        } else if (arg == "--objects-dir") {
            const char* v = value(arg);
            if (!v) return usage("--objects-dir needs a folder");
            options.objectDirectory = v;
        } else if (arg == "--name") {
            const char* v = value(arg);
            if (!v) return usage("--name needs a value");
            options.sceneName = v;
        } else if (arg == "--strict") {
            options.strict = true;
        } else if (arg == "--quiet") {
            quiet = true;
        } else if (!arg.empty() && arg.front() == '-') {
            return usage(("unknown option " + std::string(arg)).c_str());
        } else if (input.empty()) {
            input = arg;
        } else if (output.empty()) {
            output = arg;
        } else {
            return usage("too many arguments");
        }
    }
    if (input.empty() || output.empty()) return usage("an input scene and an output manifest are required");

    std::string error;
    const auto document = dve::editor::EditorDocument::load(input, &error);
    if (!document) {
        std::cerr << "dve_export_scene: could not load " << input.generic_string() << ": " << error << '\n';
        return 1;
    }
    dve::editor::EditorMaterialLibrary materials = dve::editor::EditorMaterialLibrary::make_default();
    if (!materialsPath.empty()) {
        auto loaded = dve::editor::EditorMaterialLibrary::load(materialsPath, &error);
        if (!loaded) {
            std::cerr << "dve_export_scene: could not load materials " << materialsPath.generic_string()
                      << ": " << error << '\n';
            return 1;
        }
        materials = std::move(*loaded);
    }

    const dve::editor::EditorSceneExportResult result =
        dve::editor::export_editor_scene(*document, materials, output, options);
    for (const std::string& warning : result.warnings) {
        std::cerr << "dve_export_scene: warning: " << warning << '\n';
    }
    if (!result.success) {
        std::cerr << "dve_export_scene: export failed: " << result.error << '\n';
        return 1;
    }
    if (!quiet) {
        std::uint64_t voxels = 0;
        for (const auto& object : result.objects) voxels += object.voxelCount;
        std::cout << "dve_export_scene: wrote " << output.generic_string() << " objects="
                  << result.objects.size() << " skipped=" << result.skippedObjects << " voxels=" << voxels
                  << " warnings=" << result.warnings.size() << '\n';
    }
    return 0;
}
