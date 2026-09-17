#include "dve/editor_native.hpp"
#include "dve/editor_workspace.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace dve;
using namespace dve::editor;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path make_temp_root() {
    const auto root = std::filesystem::temp_directory_path() / "dve_menu_command_center_v220";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "assets", error);
    std::filesystem::create_directories(root / "docs", error);
    if (error) throw std::runtime_error("could not create menu test root");
    std::ofstream(root / "assets" / "hero.dvesprite") << "DVE_SPRITE 1\n";
    std::ofstream(root / "docs" / "sprite_workflow.md") << "# Sprite workflow\n";
    return root;
}

void test_registry_visibility_search_and_state(const std::filesystem::path& root) {
    EditorMenuRegistry registry = EditorMenuRegistry::make_default();
    const auto compactTools = registry.menu("Tools", false);
    const auto advancedTools = registry.menu("Tools", true);
    require(advancedTools.size() > compactTools.size(), "advanced menu mode did not reveal additional tools");
    require(std::none_of(compactTools.begin(), compactTools.end(), [](const MenuAction& action) {
        return action.id == "render.gabor.quality_cinematic";
    }), "advanced rendering command leaked into compact menu mode");
    require(std::any_of(advancedTools.begin(), advancedTools.end(), [](const MenuAction& action) {
        return action.id == "render.gabor.quality_cinematic";
    }), "advanced rendering command is absent from advanced menu mode");
    require(std::none_of(advancedTools.begin(), advancedTools.end(), [](const MenuAction& action) {
        return action.id == "text3d.commit";
    }), "palette-only action appeared in a top-level menu");

    const auto fuzzy = registry.search("tile wrld", 8);
    require(!fuzzy.empty() && fuzzy.front().id == "sprite.tile_world",
            "multi-token fuzzy menu search did not find Tile World Editor");
    require(registry.set_enabled("edit.copy", false, "Select an object first."),
            "could not disable a command with a reason");
    const auto disabled = registry.search("copy", 32);
    const auto disabledCopy = std::find_if(disabled.begin(), disabled.end(), [](const MenuAction& action) {
        return action.id == "edit.copy";
    });
    require(disabledCopy != disabled.end() && !disabledCopy->enabled &&
            disabledCopy->disabledReason == "Select an object first.",
            "disabled command was not searchable with its explanation");

    EditorMenuUserState state;
    state.showAdvancedCommands = true;
    state.favoriteActionIds = {"sprite.tile_world", "sprite.diagnostics"};
    state.recentActionIds = {"file.save", "sprite.tile_world"};
    std::string error;
    const auto statePath = root / ".dve" / "user" / "menu_state.txt";
    require(state.save(statePath, &error), error.c_str());
    const auto loaded = EditorMenuUserState::load(statePath, &error);
    require(loaded.has_value(), error.c_str());
    require(loaded->showAdvancedCommands && loaded->favoriteActionIds == state.favoriteActionIds &&
            loaded->recentActionIds == state.recentActionIds,
            "menu user state did not round-trip exactly");
}

void test_command_center_providers_and_persistence(const std::filesystem::path& root) {
    NativeEditorController controller{EditorWorkspace{make_native_editor_demo_document()}};
    controller.configure_ai_assistant(root);
    EditorAssetScanReport scan;
    std::string error;
    require(controller.asset_database().scan(&scan, &error), error.c_str());
    const auto statePath = root / ".dve" / "user" / "controller_menu_state.txt";
    controller.configure_menu_state(statePath);

    require(controller.dispatch_action("help.command_palette"), "command center did not open");
    controller.text_input("/tile world");
    auto results = controller.command_palette_results(16);
    require(!results.empty() && results.front().kind == CommandPaletteResultKind::Panel &&
            results.front().id == "sprite.tile_world",
            "panel provider did not find Tile World Editor");
    controller.key_down("escape", false, false, false);

    require(controller.dispatch_action("help.command_palette"), "command center did not reopen");
    controller.text_input(":destructible wall");
    results = controller.command_palette_results(16);
    require(!results.empty() && results.front().kind == CommandPaletteResultKind::SceneObject,
            "scene-object provider did not find a named scene object");
    controller.key_down("enter", false, false, false);
    require(controller.workspace().selected_object().has_value(), "scene-object result did not select an object");

    require(controller.dispatch_action("help.command_palette"), "command center did not reopen for assets");
    controller.text_input("#hero");
    results = controller.command_palette_results(16);
    require(!results.empty() && results.front().kind == CommandPaletteResultKind::Asset,
            "asset provider did not find the sprite asset");
    controller.key_down("enter", false, false, false);
    require(controller.asset_browser_state().selectedId.has_value(), "asset result did not focus the Assets panel");

    require(controller.dispatch_action("help.command_palette"), "command center did not reopen for docs");
    controller.text_input("?sprite workflow");
    results = controller.command_palette_results(16);
    require(!results.empty() && results.front().kind == CommandPaletteResultKind::Documentation,
            "documentation provider did not find the markdown guide");
    controller.key_down("escape", false, false, false);

    controller.workspace().clear_selection();
    controller.refresh_menu_state();
    require(!controller.dispatch_action("edit.copy"), "disabled Copy command unexpectedly executed");
    require(controller.status().error && controller.status().text.find("Select") != std::string::npos,
            "disabled Copy command did not explain how to make it available");

    require(controller.dispatch_action("view.advanced_menus"), "advanced-menu toggle failed");
    require(controller.show_advanced_menus(), "advanced-menu state did not change");
    require(controller.dispatch_action("help.command_palette"), "command center did not reopen for favorites");
    controller.text_input("sprite diagnostics");
    results = controller.command_palette_results(16);
    require(!results.empty() && results.front().kind == CommandPaletteResultKind::Command,
            "command search did not find Sprite Diagnostics");
    controller.key_down("d", true, false, false);
    controller.key_down("escape", false, false, false);
    require(controller.command_is_favorite("sprite.diagnostics"), "Ctrl+D did not favorite the command");

    const auto persisted = EditorMenuUserState::load(statePath, &error);
    require(persisted.has_value(), error.c_str());
    require(persisted->showAdvancedCommands,
            "advanced-menu preference was not persisted");
    require(std::find(persisted->favoriteActionIds.begin(), persisted->favoriteActionIds.end(),
                      "sprite.diagnostics") != persisted->favoriteActionIds.end(),
            "favorite command was not persisted");

    NativeEditorController reloaded{EditorWorkspace{make_native_editor_demo_document()}};
    reloaded.configure_menu_state(statePath);
    require(reloaded.show_advanced_menus() && reloaded.command_is_favorite("sprite.diagnostics"),
            "a second editor session did not restore menu state");
}

} // namespace

int main() {
    try {
        const auto root = make_temp_root();
        test_registry_visibility_search_and_state(root);
        test_command_center_providers_and_persistence(root);
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::cout << "dve_menu_command_center_v220_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_menu_command_center_v220_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
