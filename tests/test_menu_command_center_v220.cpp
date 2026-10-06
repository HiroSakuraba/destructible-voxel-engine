#include "dve/editor_native.hpp"
#include "dve/editor_workspace.hpp"

#include <set>
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

void test_menu_cache_updates() {
    EditorMenuRegistry registry;
    MenuAction first{"test.first", "Test", "Same"};
    first.section = "First";
    first.order = 10;
    MenuAction second{"test.second", "Test", "Same"};
    second.section = "First";
    second.order = 5;
    require(registry.add(first) && registry.add(second), "cache test setup failed");
    const auto compact = registry.menu("Test");
    require(compact.size() == 2 && compact.front().id == second.id, "cached section order changed");
    require(registry.search("same", 1).front().id == first.id, "equal search scores lost registration order");
    require(registry.search("same", 8).size() == 2, "cached query was truncated by its previous limit");
    require(registry.set_enabled(first.id, false, "Disabled"), "could not disable cached action");
    require(registry.search("same", 1).front().id == second.id, "cached ranking ignored enable state");
    require(registry.menu("Test")[1].disabledReason == "Disabled", "cached menu retained stale action state");
    require(registry.set_shortcut(second.id, "Ctrl+Unique"), "could not change cached shortcut");
    require(registry.search("ctrl+unique", 8).front().id == second.id, "search index ignored shortcut change");
    require(registry.set_checked(second.id, true), "could not check cached action");
    require(registry.menu("Test").front().checked, "cached menu ignored checked state");

    MenuAction advanced{"test.advanced", "Test", "Advanced Unique"};
    advanced.visibility = MenuVisibility::Advanced;
    require(registry.add(advanced), "could not add an action after caching");
    require(registry.menu("Test").size() == 2 && registry.menu("Test", true).size() == 3,
            "adding an action left stale menu visibility");
    require(registry.search("advanced unique", 8).front().id == advanced.id,
            "adding an action left stale search fields");
    require(registry.search("same", 0).empty(), "zero-limit search returned results");

    // Legacy callers may keep and mutate a pointer after a cache has been warmed.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    MenuAction* escaped = registry.find(second.id);
#pragma GCC diagnostic pop
    require(escaped != nullptr, "legacy mutable lookup failed");
    (void)registry.search("same", 8);
    (void)registry.menu("Test");
    escaped->id = "test.renamed";
    escaped->label = "Changed Label";
    escaped->menu = "Other";
    require(std::as_const(registry).find("test.renamed") == escaped &&
            std::as_const(registry).find(second.id) == nullptr, "mutable ID edit left stale lookup index");
    require(registry.search("changed label", 8).front().id == "test.renamed",
            "escaped pointer left stale search results");
    require(registry.menu("Other").size() == 1 && registry.menu("Test").size() == 1,
            "escaped pointer left stale menu membership");
    escaped->keywords.push_back("afterwarm");
    require(registry.search("afterwarm", 8).front().id == "test.renamed",
            "later pointer edit left stale search fields");
    escaped->visibility = MenuVisibility::PaletteOnly;
    require(registry.menu("Other").empty() && registry.menu("Other", true).empty(),
            "later pointer edit left stale menu visibility");
    escaped->visibility = MenuVisibility::Primary;
    escaped->menu = "Test";
    escaped->order = -1000;
    require(registry.menu("Test").front().id == "test.renamed", "later pointer edit left stale menu order");

    // After a pointer escapes, menus are ordered per request rather than from the
    // cache; the order must match the cached one exactly for every menu and mode.
    EditorMenuRegistry cached = EditorMenuRegistry::make_default();
    EditorMenuRegistry exposed = EditorMenuRegistry::make_default();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    require(exposed.find(exposed.actions().front().id) != nullptr, "could not expose a default action");
#pragma GCC diagnostic pop
    std::set<std::string> menuNames;
    for (const MenuAction& action : cached.actions()) menuNames.insert(action.menu);
    for (const std::string& name : menuNames) {
        for (const bool includeAdvanced : {false, true}) {
            const auto a = cached.menu(name, includeAdvanced);
            const auto b = exposed.menu(name, includeAdvanced);
            require(a.size() == b.size(), "uncached menu changed size");
            for (std::size_t i = 0; i < a.size(); ++i)
                require(a[i].id == b[i].id, "uncached menu order differs from the cached order");
        }
    }
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
        test_menu_cache_updates();
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
