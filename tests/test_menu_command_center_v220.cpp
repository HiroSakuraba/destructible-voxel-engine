#include "dve/editor_native.hpp"
#include "dve/editor_workspace.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <utility>

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
    MenuAction* escaped = registry.find(second.id);
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
    // Keep mutating the retained pointer after warming both menu modes. Every
    // read must reflect membership, visibility, section, and order immediately.
    escaped->visibility = MenuVisibility::Advanced;
    require(registry.menu("Other").empty() && registry.menu("Other", true).size() == 1,
            "mutable fallback ignored visibility edits");
    escaped->menu = "Test";
    escaped->section = "First";
    escaped->order = -5;
    escaped->visibility = MenuVisibility::Primary;
    require(registry.menu("Test").front().id == "test.renamed",
            "mutable fallback ignored order or membership edits");
    escaped->visibility = MenuVisibility::PaletteOnly;
    require(registry.menu("Test", true).size() == 2,
            "mutable fallback exposed a palette-only action");
    escaped->keywords.push_back("afterwarm");
    require(registry.search("afterwarm", 8).front().id == "test.renamed",
            "later pointer edit left stale search fields");
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

// Palette results are cached and reused across pointer moves and frames, and every
// input they are derived from invalidates them.
void test_command_palette_cache() {
    NativeEditorController controller{EditorWorkspace{make_native_editor_demo_document()}};
    controller.resize(1600, 900);
    require(controller.dispatch_action("help.command_palette"), "command center did not open");
    controller.text_input("wall");
    const auto first = controller.command_palette_results(64);
    const auto rebuilds = controller.command_palette_rebuild_count();
    for (int i = 0; i < 5; ++i) {
        controller.pointer_move(400 + i * 7, 300, 0);
        (void)controller.command_palette_results(64);
    }
    require(controller.command_palette_rebuild_count() == rebuilds, "unchanged palette was rebuilt");
    require(controller.command_palette_results(64).size() == first.size(), "cached palette changed size");

    const auto has_object = [&](std::string_view name) {
        const auto results = controller.command_palette_results(64);
        return std::any_of(results.begin(), results.end(), [&](const CommandPaletteResult& result) {
            return result.kind == CommandPaletteResultKind::SceneObject && result.label == name;
        });
    };
    // Scene objects: adding and renaming are reflected.
    EditorObject added(424242, "Wall Panel Probe");
    controller.workspace().document().add_object(std::move(added));
    require(has_object("Wall Panel Probe"), "cached palette missed a new object");
    controller.workspace().document().find_object(424242)->name = "Wall Renamed Probe";
    require(has_object("Wall Renamed Probe") && !has_object("Wall Panel Probe"), "cached palette missed a rename");

    // Query and limit.
    auto before = controller.command_palette_rebuild_count();
    (void)controller.command_palette_results(3);
    require(controller.command_palette_rebuild_count() == before + 1U, "a different limit reused the cache");
    controller.text_input(" s");
    (void)controller.command_palette_results(3);
    require(controller.command_palette_rebuild_count() == before + 2U, "a query edit reused the cache");

    controller.key_down("escape", false, false, false);
    require(controller.dispatch_action("help.command_palette"), "command center did not reopen");
    controller.text_input("sprite diagnostics");
    auto results = controller.command_palette_results(16);
    require(!results.empty() && results.front().id == "sprite.diagnostics", "command not found");
    // Favorites.
    controller.key_down("d", true, false, false);
    require(controller.command_is_favorite("sprite.diagnostics"), "Ctrl+D did not favorite the command");
    results = controller.command_palette_results(16);
    require(std::any_of(results.begin(), results.end(), [](const CommandPaletteResult& result) {
                return result.id == "sprite.diagnostics" && result.favorite;
            }), "cached palette missed a favorite change");
    // Menu state: disabling a command shows up in the cached results.
    require(controller.workspace().menus().set_enabled("sprite.diagnostics", false, "Probe reason"), "disable failed");
    results = controller.command_palette_results(16);
    require(!results.empty(), "results vanished after disabling a command");
    const auto diagnostics = std::find_if(results.begin(), results.end(), [](const CommandPaletteResult& result) {
        return result.id == "sprite.diagnostics";
    });
    require(diagnostics != results.end() && !diagnostics->enabled && diagnostics->disabledReason == "Probe reason",
            "cached palette kept a stale enabled state");

    // The asset provider is keyed on the database revision, which every mutating call
    // (including the mutable find) advances.
    EditorAssetDatabase database;
    auto revision = database.revision();
    const auto assetRoot = std::filesystem::temp_directory_path() / "dve_palette_asset_revision";
    std::filesystem::remove_all(assetRoot);
    std::filesystem::create_directories(assetRoot / "assets");
    { std::ofstream(assetRoot / "assets" / "probe.png") << "png"; }
    database.set_project_root(assetRoot);
    require(database.revision() != revision, "set_project_root did not advance the asset revision");
    revision = database.revision();
    (void)database.find("missing");
    require(database.revision() != revision, "mutable asset find did not advance the revision");
    revision = database.revision();
    (void)std::as_const(database).find("missing");
    (void)database.records();
    require(database.revision() == revision, "const asset reads advanced the revision");
    // The editor scans with options (thumbnails deferred); that must advance it too.
    EditorAssetScanOptions scanOptions;
    scanOptions.generateThumbnails = false;
    EditorAssetScanReport scanReport;
    std::string scanError;
    require(database.scan(&scanReport, &scanError, scanOptions), ("probe asset scan failed: " + scanError).c_str());
    require(database.revision() != revision, "an editor-style scan did not advance the asset revision");
    std::filesystem::remove_all(assetRoot);
    std::cout << "command palette cache: OK\n";
}

int main() {
    try {
        const auto root = make_temp_root();
        test_registry_visibility_search_and_state(root);
        test_menu_cache_updates();
        test_command_center_providers_and_persistence(root);
        test_command_palette_cache();
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::cout << "dve_menu_command_center_v220_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_menu_command_center_v220_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
