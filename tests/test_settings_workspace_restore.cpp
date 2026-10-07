#include "dve/editor_native.hpp"
#include <png.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace dve;
using namespace dve::editor;
namespace fs = std::filesystem;
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
struct TempProjects {
    fs::path root = fs::temp_directory_path() / ("dve_workspace_restore_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempProjects() { fs::create_directories(root); }
    ~TempProjects() { std::error_code ec; fs::remove_all(root, ec); }
};
void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary); out << text;
    require(out.good(), "fixture write failed");
}
std::string read(const fs::path& path) { std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
void set_restore(NativeEditorController& controller, bool enabled) {
    std::string error;
    require(controller.workspace().settings().set(SettingScope::Session, "editor.restore_workspace", enabled, &error), error);
}
void parser_and_references(const fs::path& root) {
    std::string error;
    require(!EditorWorkspaceState::parse("DVE_WORKSPACE 2\n", &error), "future version accepted");
    require(!EditorWorkspaceState::parse(std::string(32769, 'x'), &error), "oversized record accepted");
    const auto state = EditorWorkspaceState::parse(
        "DVE_WORKSPACE 1\npanel 999 0\npanel 4 7\npanel 2 0\nbottom_tab 99\n"
        "camera 0 0 0 0 0 0 0 1 0 0 12\nselected_asset \"../outside.lua\"\n"
        "open_sprite \"/outside.dvesprite\"\nfuture_key ignored\n", &error);
    require(state && state->panels.size() == 1 && !state->panels.at(PanelId::SceneHierarchy), "valid entry lost");
    require(!state->camera && !state->bottomTab && state->selectedAsset.empty() && state->openSprite.empty(), "invalid entries accepted");
    const auto invalidCamera = EditorWorkspaceState::parse(
        "DVE_WORKSPACE 1\ncamera 0 0 0 0 1 0 0 1 0 0 12\n");
    require(invalidCamera && !invalidCamera->camera, "parallel view/up accepted");
    write(root / "assets/file.txt", "native data");
    require(resolve_workspace_asset(root, "assets/file.txt").has_value(), "valid asset rejected");
    require(!resolve_workspace_asset(root, "../assets/file.txt") && !resolve_workspace_asset(root, root / "assets/file.txt") &&
        !resolve_workspace_asset(root, "assets") && !resolve_workspace_asset(root, "missing"), "invalid reference resolved");
    std::error_code ec;
    fs::create_symlink(root.parent_path(), root / "outside", ec);
    if (!ec) require(resolve_workspace_asset(root, "outside/" + root.filename().string() + "/assets/file.txt").has_value(),
        "symlink inside project should still resolve");
    write(root.parent_path() / "outside.txt", "outside");
    if (!ec) require(!resolve_workspace_asset(root, "outside/outside.txt"), "symlink escaped project");
    const auto path = root / ".dve/user/parser.txt";
    require(state->save(path, &error), error);
    require(EditorWorkspaceState{}.save(path, &error), "record replacement failed: " + error);
    require(EditorWorkspaceState::load(path, &error).has_value(), "saved record unreadable");
    write(path, std::string(32769, 'x'));
    require(!EditorWorkspaceState::load(path, &error), "bounded file load failed");
}
void make_sprite(const fs::path& root) {
    fs::create_directories(root / "assets");
    png_image image{}; image.version = PNG_IMAGE_VERSION; image.width = 2; image.height = 2; image.format = PNG_FORMAT_RGBA;
    const unsigned char pixels[]{255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
    require(png_image_write_to_file(&image, (root / "assets/image.png").string().c_str(), 0, pixels, 0, nullptr) != 0, "PNG fixture failed");
    auto sprite = make_sprite_authoring_asset("hero", "assets/image.png", 2, 2);
    std::string error;
    require(write_dvesprite(root / "assets/hero.dvesprite", sprite, &error), error);
}
void lifecycle(const fs::path& root, const fs::path& other) {
    make_sprite(root); fs::create_directories(other);
    auto first = std::make_unique<NativeEditorController>();
    const auto defaults = first->camera();
    first->configure_ai_assistant(root);
    require(first->workspace_state_path() == fs::canonical(root) / ".dve/user/workspace.txt", "wrong per-project path");
    first->workspace().set_panel_visible(PanelId::SceneHierarchy, false);
    first->workspace().set_panel_visible(PanelId::Inspector, false);
    require(first->dispatch_action("window.toggle_assets"), "assets tab did not activate");
    first->camera().position = {4, 5, 6}; first->camera().target = {1, 2, 3};
    first->camera().projection = EditorProjection::Orthographic; first->camera().orthographicHeight = 19;
    auto& db = first->asset_database(); std::string error;
    require(db.scan(nullptr, &error) && db.save(&error), error);
    const auto* asset = db.find_path("assets/hero.dvesprite"); require(asset, "sprite not indexed");
    const auto imageId = db.find_path("assets/image.png")->id;
    first->asset_browser_state().selectedId = asset->id;
    require(first->sprite_authoring_panel().open_asset(root / "assets/hero.dvesprite", root, &error), error);
    require(first->save_workspace_state(&error), error);
    const auto original = read(first->workspace_state_path());
    auto restored = std::make_unique<NativeEditorController>(); restored->configure_ai_assistant(root);
    require(!restored->workspace().panel_visible(PanelId::SceneHierarchy) && !restored->workspace().panel_visible(PanelId::Inspector), "panels not restored");
    require(restored->layout().hierarchy.width == 0 && restored->layout().inspector.width == 0, "restored panels not applied to layout");
    require(restored->bottom_tab() == BottomPanelTab::Assets && restored->camera().position.x == 4 &&
        restored->camera().target.z == 3 && restored->camera().projection == EditorProjection::Orthographic &&
        restored->camera().orthographicHeight == 19, "viewport/tab not restored");
    require(restored->asset_browser_state().selectedId == first->asset_browser_state().selectedId, "asset selection not restored");
    require(restored->sprite_authoring_panel().open() && !restored->sprite_authoring_panel().workspace().session().dirty(), "saved sprite not restored cleanly");
    require(!restored->workspace().document().dirty(), "restoration dirtied scene");
    restored->configure_ai_assistant(root);
    require(restored->camera().position.x == 4, "same-project AI reconfiguration reset state");
    restored->configure_ai_assistant(other);
    require(restored->workspace().panel_visible(PanelId::SceneHierarchy) && restored->bottom_tab() == BottomPanelTab::Console &&
        restored->camera().position.x == defaults.position.x && !restored->sprite_authoring_panel().open(), "project state leaked into new project");
    auto disabled = std::make_unique<NativeEditorController>(); set_restore(*disabled, false); disabled->configure_ai_assistant(root);
    require(disabled->workspace().panel_visible(PanelId::SceneHierarchy) && disabled->bottom_tab() == BottomPanelTab::Console &&
        !disabled->sprite_authoring_panel().open(), "disabled setting restored workspace");
    require(disabled->save_workspace_state(&error) && read(first->workspace_state_path()) == original, "disabled mode overwrote record");
    // Scene dirty state protects quit. Cancel writes nothing; accepted quit saves.
    first->workspace().document().mark_dirty();
    first->request_quit(); require(first->pending_quit_confirmation() && !first->quit_requested(), "dirty quit bypassed confirmation");
    first->cancel_quit(); require(read(first->workspace_state_path()) == original, "canceled quit wrote workspace");
    first->camera().position.x = 8; first->request_quit(); first->confirm_quit();
    require(first->quit_requested() && read(first->workspace_state_path()) != original, "accepted quit did not save");
    require(!fs::exists(root / ".dve/user/workspace.txt.tmp"), "temporary record left behind");
    first.reset(); restored.reset(); disabled.reset();
    // Persisted settings are resolved before project activation, not only Session.
    auto settings = EditorSettingsRegistry::make_default();
    require(settings.set(SettingScope::User, "editor.restore_workspace", false, &error), error);
    const auto userPath = other / "settings.txt";
    require(settings.save_scope_file(SettingScope::User, userPath, &error), error);
    auto persistedOff = std::make_unique<NativeEditorController>(EditorWorkspace{}, userPath);
    persistedOff->configure_ai_assistant(root);
    require(persistedOff->bottom_tab() == BottomPanelTab::Console && !persistedOff->sprite_authoring_panel().open(), "persisted Off ignored");
    persistedOff.reset();
    // A missing index is scanned in the background. It cannot overwrite a new
    // user selection when its result arrives.
    fs::remove(root / ".dve/asset_index.dve");
    auto scanning = std::make_unique<NativeEditorController>(); scanning->configure_ai_assistant(root);
    require(scanning->finish_asset_scan(), "restoration scan did not finish");
    const auto* indexed = scanning->asset_database().find_path("assets/hero.dvesprite");
    require(indexed && scanning->asset_browser_state().selectedId == indexed->id, "missing-index selection not restored");
    scanning.reset(); fs::remove(root / ".dve/asset_index.dve");
    scanning = std::make_unique<NativeEditorController>(); scanning->configure_ai_assistant(root);
    scanning->asset_browser_state().selectedId = imageId;
    require(scanning->finish_asset_scan(), "restoration scan did not finish");
    require(scanning->asset_browser_state().selectedId == imageId, "late restore replaced a new user selection");
    scanning.reset();
    // Saving dirty sprite edits does not serialize those contents; an explicit
    // asset Save makes the on-disk document eligible again.
    auto edited = std::make_unique<NativeEditorController>(); edited->configure_ai_assistant(root);
    auto changed = edited->sprite_authoring_panel().workspace().session().asset(); changed.name = "edited";
    require(edited->sprite_authoring_panel().workspace().session().replace_asset(changed, "edit", &error), error);
    require(edited->save_workspace_state(&error), error);
    require(EditorWorkspaceState::load(edited->workspace_state_path())->openSprite.empty(), "dirty sprite was persisted");
    require(edited->sprite_authoring_panel().save(&error) && edited->save_workspace_state(&error), error);
    require(!EditorWorkspaceState::load(edited->workspace_state_path())->openSprite.empty(), "explicitly saved sprite was omitted");
    require(edited->dispatch_action("physics.play"), "Play did not start");
    const auto editorPosition = edited->camera().position;
    edited->camera().position = {90, 90, 90};
    require(edited->save_workspace_state(&error), error);
    require(EditorWorkspaceState::load(edited->workspace_state_path())->camera->position.x == editorPosition.x, "gameplay camera leaked into workspace");
    require(edited->dispatch_action("physics.stop"), "Play did not stop");
    edited.reset();
    // Deleted files and unsupported executable assets do not reopen or block panels.
    fs::remove(root / "assets/hero.dvesprite");
    write(root / ".dve/user/workspace.txt", "DVE_WORKSPACE 1\npanel 2 0\nselected_asset \"assets/hero.dvesprite\"\nopen_sprite \"scripts/main.lua\"\n");
    write(root / "scripts/main.lua", "error('must never run from workspace restore')");
    auto stale = std::make_unique<NativeEditorController>(); stale->configure_ai_assistant(root);
    require(!stale->workspace().panel_visible(PanelId::SceneHierarchy) && !stale->asset_browser_state().selectedId &&
        !stale->sprite_authoring_panel().open() && !stale->play_session().active(), "stale/script restore affected scene");
    write(root / ".dve/user/workspace.txt", "DVE_WORKSPACE 42\npanel 2 0\n");
    auto invalid = std::make_unique<NativeEditorController>(); invalid->configure_ai_assistant(root);
    require(invalid->workspace().panel_visible(PanelId::SceneHierarchy), "future version changed defaults");
    invalid.reset(); stale.reset();
    fs::remove(root / ".dve/user/workspace.txt");
    fs::create_directory(root / ".dve/user/workspace.txt");
    auto failedSave = std::make_unique<NativeEditorController>(); failedSave->configure_ai_assistant(root);
    failedSave->request_quit();
    require(failedSave->quit_requested() && fs::is_directory(failedSave->workspace_state_path()) &&
        !fs::exists(root / ".dve/user/workspace.txt.tmp"), "failed save blocked quit or destroyed destination");
}
} // namespace
int main() {
    try { TempProjects temp; const auto project = temp.root / "project"; fs::create_directories(project);
        parser_and_references(project); lifecycle(project, temp.root / "other");
        std::cout << "dve_settings_workspace_restore_tests: PASS\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
