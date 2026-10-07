#include "dve/editor_native.hpp"

namespace dve::editor {
namespace {
std::filesystem::path project_reference(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code ec;
    const auto reference = std::filesystem::relative(path, root, ec);
    if (ec || !resolve_workspace_asset(root, reference)) return {};
    return reference;
}

bool unsaved_sprite(const EditorSpriteAuthoringPanel& panel) {
    if (!panel.open()) return false;
    if (panel.has_palette_document() && panel.palette_session().dirty()) return true;
    if (!panel.workspace().session().dirty()) return false;
    // The panel's save_asset route writes a source recipe without updating the
    // session's saved hash. Compare the saved content before classifying it as
    // unsaved, so an ordinary explicit Save remains eligible for restoration.
    if (panel.asset_path().empty()) return true;
    const auto saved = read_dvesprite(panel.asset_path());
    return !saved || saved.asset.contentHash != panel.workspace().session().asset().contentHash;
}

// Only saved native sprite documents are eligible for automatic opening. Verify
// their linked image/palette before invoking the ordinary authoring loader. Never
// dispatch generic asset.open (which includes other authoring/import routes).
bool restorable_sprite(const std::filesystem::path& root, const std::filesystem::path& path) {
    if (path.extension() != ".dvesprite") return false;
    const auto read = read_dvesprite(path);
    if (!read || read.asset.textureWidth > 4096U || read.asset.textureHeight > 4096U) return false;
    const auto dependency = [&](const std::string& text, bool texture) {
        const std::filesystem::path reference(text);
        // Match the authoring loader's project-first, asset-relative resolution.
        std::error_code ec;
        std::filesystem::path relative;
        if (reference.is_absolute()) relative = project_reference(root, reference);
        else {
            if (reference.has_root_name()) return false;
            for (const auto& part : reference) if (part == "..") return false;
            const bool projectCandidateExists = std::filesystem::exists(root / reference, ec);
            relative = projectCandidateExists ? reference : (path.parent_path() / reference).lexically_relative(root);
        }
        const auto resolved = resolve_workspace_asset(root, relative);
        if (!resolved) return false;
        const auto bytes = std::filesystem::file_size(*resolved, ec);
        if (ec || bytes > (texture ? 64ULL : 16ULL)*1024ULL*1024ULL) return false;
        const auto extension = resolved->extension();
        return texture ? (extension == ".png" || extension == ".jpg" || extension == ".jpeg")
                       : extension == ".dvepalette";
    };
    return dependency(read.asset.textureAsset, true) &&
        (read.asset.paletteAsset.empty() || dependency(read.asset.paletteAsset, false));
}
} // namespace

std::filesystem::path NativeEditorController::workspace_state_path() const {
    return workspaceProjectConfigured_ ? projectRoot_ / ".dve/user/workspace.txt" : std::filesystem::path{};
}

EditorWorkspaceState NativeEditorController::capture_workspace_state() const {
    EditorWorkspaceState state;
    for (unsigned id = 0; id <= static_cast<unsigned>(PanelId::Assistant); ++id)
        state.panels.emplace(static_cast<PanelId>(id), workspace_.panel_visible(static_cast<PanelId>(id)));
    state.bottomTab = static_cast<unsigned>(bottomTab_);
    state.camera = playSession_.active() ? prePlayEditorCamera_ : camera_;
    if (assetBrowserState_.selectedId) {
        if (const auto* asset = assetDatabase_.find(*assetBrowserState_.selectedId))
            if (resolve_workspace_asset(projectRoot_, asset->relativePath)) state.selectedAsset = asset->relativePath;
    } else if (resolve_workspace_asset(projectRoot_, pendingWorkspaceAsset_)) state.selectedAsset = pendingWorkspaceAsset_;
    if (spriteAuthoringPanel_.open() && !unsaved_sprite(spriteAuthoringPanel_)) {
        state.openSprite = project_reference(projectRoot_, spriteAuthoringPanel_.asset_path());
        if (!state.openSprite.empty() && !restorable_sprite(projectRoot_, projectRoot_ / state.openSprite))
            state.openSprite.clear();
    }
    return state;
}

bool NativeEditorController::save_workspace_state(std::string* error) {
    if (error) error->clear();
    if (!workspaceProjectConfigured_ || !std::get<bool>(workspace_.settings().value("editor.restore_workspace"))) return true;
    return capture_workspace_state().save(workspace_state_path(), error);
}

void NativeEditorController::save_workspace_state_on_quit() {
    std::string error;
    if (!save_workspace_state(&error)) workspace_.log().add(EditorLogLevel::Warning, "Workspace save: " + error);
}

void NativeEditorController::restore_workspace_asset_selection() {
    if (pendingWorkspaceAsset_.empty()) return;
    if (!assetBrowserState_.selectedId && resolve_workspace_asset(projectRoot_, pendingWorkspaceAsset_)) {
        if (const auto* asset = assetDatabase_.find_path(pendingWorkspaceAsset_)) assetBrowserState_.selectedId = asset->id;
    }
    pendingWorkspaceAsset_.clear();
}

void NativeEditorController::restore_workspace_state() {
    EditorWorkspaceState state = startupWorkspaceState_;
    if (std::get<bool>(workspace_.settings().value("editor.restore_workspace"))) {
        std::error_code ec;
        if (std::filesystem::exists(workspace_state_path(), ec)) {
            std::string error;
            if (const auto saved = EditorWorkspaceState::load(workspace_state_path(), &error)) {
                for (const auto& [id, visible] : saved->panels) state.panels.insert_or_assign(id, visible);
                if (saved->bottomTab) state.bottomTab = saved->bottomTab;
                if (saved->camera) state.camera = saved->camera;
                state.selectedAsset = saved->selectedAsset;
                state.openSprite = saved->openSprite;
            } else workspace_.log().add(EditorLogLevel::Warning, "Workspace restore: " + error);
        }
    }
    for (const auto& [id, visible] : state.panels) workspace_.set_panel_visible(id, visible);
    if (state.bottomTab) bottomTab_ = static_cast<BottomPanelTab>(*state.bottomTab);
    if (state.camera && !playSession_.active()) {
        // Restore navigation only; projection tuning/lens/clip settings keep their
        // current values from preferences. Do not alter authored camera rigs.
        camera_.position = state.camera->position;
        camera_.target = state.camera->target;
        camera_.worldUp = state.camera->worldUp;
        camera_.projection = state.camera->projection;
        camera_.orthographicHeight = state.camera->orthographicHeight;
    }
    clear_navigation_input();
    assetBrowserState_ = {};
    pendingWorkspaceAsset_ = state.selectedAsset;
    if (!pendingWorkspaceAsset_.empty()) {
        if (assetDatabase_.find_path(pendingWorkspaceAsset_)) restore_workspace_asset_selection();
        else if (resolve_workspace_asset(projectRoot_, pendingWorkspaceAsset_)) (void)start_asset_scan(false);
        else pendingWorkspaceAsset_.clear();
    }
    const bool dirtySprite = unsaved_sprite(spriteAuthoringPanel_);
    if (!dirtySprite) {
        spriteAuthoringPanel_.close();
        const auto sprite = resolve_workspace_asset(projectRoot_, state.openSprite);
        if (sprite && restorable_sprite(projectRoot_, *sprite)) {
            std::string error;
            if (!spriteAuthoringPanel_.open_asset(*sprite, projectRoot_, &error))
                workspace_.log().add(EditorLogLevel::Warning, "Workspace sprite restore: " + error);
        } else if (!state.openSprite.empty())
            workspace_.log().add(EditorLogLevel::Warning, "Workspace sprite skipped: missing or unsupported project asset");
    }
    if (bottomTab_ == BottomPanelTab::Assets && assetDatabase_.records().empty() && !assetScanInFlight_)
        (void)start_asset_scan(false);
    recompute_layout();
    refresh_menu_state();
}

} // namespace dve::editor
