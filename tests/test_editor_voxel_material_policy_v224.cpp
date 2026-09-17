#include "dve/editor_native.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
        const MenuAction* action = controller.workspace().menus().find("render.voxel_material_policy");
        require(action != nullptr && action->menu == "Tools" && action->section == "Voxel Materials",
                "voxel material policy menu action is missing or misplaced");

        const VoxelMaterialPolicyConfig defaults = controller.voxel_material_policy_config();
        require(defaults.mode == VoxelMaterialMode::Hybrid &&
                defaults.platform == VoxelMaterialPlatformProfile::HighEndDesktop &&
                defaults.maximumPaletteSlots == 4U && defaults.retainBakedFallback,
                "default editor voxel material policy is incorrect");

        require(controller.dispatch_action("render.voxel_material_policy"),
                "voxel material policy action failed");
        require(controller.settings_panel().open &&
                controller.settings_panel().selectedCategory == "Voxel",
                "voxel material policy action did not open project voxel settings");
        controller.close_settings(false);

        std::string error;
        require(controller.workspace().settings().set(
                    SettingScope::Project, "voxel.material_mode", std::string("baked"), &error), error);
        require(controller.workspace().settings().set(
                    SettingScope::Project, "voxel.material_platform", std::string("mobile"), &error), error);
        require(controller.workspace().settings().set(
                    SettingScope::Project, "voxel.material_palette_slots", std::int64_t{2}, &error), error);
        require(controller.workspace().settings().set(
                    SettingScope::Project, "voxel.material_runtime_switching", false, &error), error);
        controller.apply_settings_to_runtime();
        const VoxelMaterialPolicyConfig changed = controller.voxel_material_policy_config();
        require(changed.mode == VoxelMaterialMode::BakedProperties &&
                changed.platform == VoxelMaterialPlatformProfile::Mobile &&
                changed.maximumPaletteSlots == 2U && !changed.enableFourWayBlending &&
                !changed.allowRuntimeSwitching,
                "editor settings did not map into the runtime voxel material policy");

        const SettingDefinition* mode = controller.workspace().settings().find("voxel.material_mode");
        const SettingDefinition* fallback = controller.workspace().settings().find("voxel.material_retain_baked");
        require(mode != nullptr && fallback != nullptr && mode->category == "Voxel",
                "voxel material settings were not registered");

        std::cout << "editor voxel material policy v2.24 tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
