// dve_export_scene: offline editor-scene export (packaging Phase 4, decision D2).
//
//   dve_export_scene <scene.dvescene> <out.dvoxscene.json>
//                    [--materials <library.dvematerials>] [--objects-dir <dir>]
//                    [--name <scene name>] [--project-root <dir>] [--gabor-opacity <0..1>]
//                    [--strict] [--quiet]
//
// Loads a DVE_EDITOR_SCENE document with the editor library and writes the shipped runtime
// format (DVOXSCENE v1 JSON + one .dvox per object), so dve_player never links dve_editor.
// .dmesh objects are copied, 3D text and Gabor volumes are baked to voxels, components and
// attachments go into the per-object extensions block (see dve/editor_scene_export.hpp).
// Warnings list what the runtime format cannot represent; --strict turns them into errors. Exit codes: 0 ok, 1 export failed, 2 usage error.
#include "dve/editor_document.hpp"
#include "dve/editor_materials.hpp"
#include "dve/editor_scene_export.hpp"
#include "dve/editor_runtime_settings.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int usage(const char* message = nullptr) {
    if (message) std::cerr << "dve_export_scene: " << message << '\n';
    std::cerr << "usage: dve_export_scene <scene.dvescene> <out.dvoxscene.json> "
                 "[--materials <file.dvematerials>] [--objects-dir <dir>] [--name <name>] "
                 "[--project-root <dir>] [--game-settings-output <file>] [--gabor-opacity <0..1>] [--strict] [--quiet]\n";
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path input;
    std::filesystem::path output;
    std::filesystem::path materialsPath;
    std::filesystem::path gameSettingsOutput;
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
        } else if (arg == "--project-root") {
            const char* v = value(arg);
            if (!v) return usage("--project-root needs a folder");
            options.projectRoot = v;
        } else if (arg == "--game-settings-output") {
            const char* v=value(arg);
            if(!v)return usage("--game-settings-output needs a file");
            gameSettingsOutput=v;
        } else if (arg == "--gabor-opacity") {
            const char* v = value(arg);
            if (!v) return usage("--gabor-opacity needs a value");
            char* end = nullptr;
            const float threshold = std::strtof(v, &end);
            if (end == v || *end != '\0' || !(threshold > 0.0F && threshold < 1.0F))
                return usage("--gabor-opacity must be a number in (0, 1)");
            options.gaborOpacityThreshold = threshold;
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
    dve::ui::GameSettings gameSettings;
    if(!gameSettingsOutput.empty()){
        if(options.projectRoot.empty())return usage("--game-settings-output requires --project-root");
        if(std::filesystem::weakly_canonical(gameSettingsOutput)==std::filesystem::weakly_canonical(output))
            return usage("scene output and game-settings output must be different files");
        // Preserve unrelated game defaults when updating an existing file.
        if(std::filesystem::exists(gameSettingsOutput)){
            std::ifstream in(gameSettingsOutput,std::ios::binary);
            std::string text(64U*1024U+1U,'\0');in.read(text.data(),static_cast<std::streamsize>(text.size()));
            text.resize(static_cast<std::size_t>(in.gcount()));
            const auto existing=in.bad() || text.size()>64U*1024U ? std::nullopt : dve::ui::GameSettings::parse(text,&error);
            if(!existing){std::cerr<<"dve_export_scene: cannot read existing game settings: "<<error<<'\n';return 1;}
            gameSettings=*existing;
        }
        auto settings=dve::editor::EditorSettingsRegistry::make_default();
        const auto preferences=options.projectRoot/".dve/project/editor_settings.txt";
        if(std::filesystem::exists(preferences) && !settings.load_scope_file(dve::editor::SettingScope::Project,preferences,&error)){
            std::cerr<<"dve_export_scene: cannot load project settings: "<<error<<'\n';return 1;
        }
        gameSettings=dve::editor::polygon_game_settings(settings,std::move(gameSettings));
    }
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
    if (!quiet) {
        for (const std::string& line : result.notes) std::cout << "dve_export_scene: note: " << line << '\n';
    }
    if (!result.success) {
        std::cerr << "dve_export_scene: export failed: " << result.error << '\n';
        return 1;
    }
    if(!gameSettingsOutput.empty() && !gameSettings.save(gameSettingsOutput,&error)){
        std::cerr<<"dve_export_scene: cannot export game settings: "<<error<<'\n';return 1;
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
