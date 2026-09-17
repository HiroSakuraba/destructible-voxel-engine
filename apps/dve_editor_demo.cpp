#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "dve/editor_import_workflow.hpp"
#include "dve/editor_project.hpp"
#include "dve/editor_tools.hpp"
#include "dve/editor_workspace.hpp"
#include "dve/game_ui.hpp"

int main(int argc, char** argv) {
    using namespace dve;
    using namespace dve::editor;
    try {
        const std::filesystem::path output = argc > 1 ? argv[1] :
            std::filesystem::temp_directory_path() / "dve_editor_demo";
        std::error_code ec;
        std::filesystem::remove_all(output, ec);
        EditorProject project{"DVE Editor Demo", output};
        std::string error;
        if (!project.initialize(&error) || !project.save(&error)) throw std::runtime_error(error);

        EditorDocument document("Editor Demo");
        EditorObject object(1001, "Demo Structure");
        object.flags.anchored = true;
        object.voxels->fill_brick({0,0,0}, 1);
        document.add_object(std::move(object));
        EditorWorkspace workspace(std::move(document));
        BrushSettings remove;
        remove.operation = VoxelToolOperation::Remove;
        remove.shape = BrushShape::Sphere;
        remove.radius = 2;
        remove.strokeId = 1;
        auto command = make_brush_command(workspace.document(), 1001, {4,4,4}, remove);
        if (!workspace.commands().execute(workspace.document(), std::move(command)).success)
            throw std::runtime_error("voxel tool command failed");
        auto anchors = make_anchor_brush_command(workspace.document(), 1001, {0,0,0}, 1, true);
        if (!workspace.commands().execute(workspace.document(), std::move(anchors)).success)
            throw std::runtime_error("anchor tool command failed");

        const auto scenePath = project.scenes_dir() / "editor_demo.dvescene";
        const auto saved = workspace.document().save_transactional(scenePath);
        if (!saved.success) throw std::runtime_error(saved.error);
        EditorAutosaveManager autosave(project.autosave_dir());
        if (!autosave.save_recovery(workspace.document(), &error)) throw std::runtime_error(error);

        dve::ui::GameSettings settings;
        settings.reducedMotion = true;
        std::ofstream settingsOut(output / "game_settings.txt");
        settingsOut << settings.serialize();

        std::cout << "dve_editor_demo: PASS\n"
                  << "scene=" << scenePath << "\n"
                  << "objects=" << workspace.document().objects().size()
                  << " voxels=" << workspace.document().find_object(1001)->voxels->occupied_voxel_count()
                  << " anchors=" << workspace.document().find_object(1001)->anchors.size() << "\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_editor_demo: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
