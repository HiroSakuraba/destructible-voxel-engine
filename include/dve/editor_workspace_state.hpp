#pragma once

#include "dve/editor_viewport.hpp"
#include "dve/editor_workspace.hpp"

namespace dve::editor {

// UI state only: never scene contents, undo history, settings, or executable assets.
// Missing/invalid individual entries retain the controller's startup defaults.
struct EditorWorkspaceState {
    std::map<PanelId, bool> panels;
    std::optional<unsigned> bottomTab;
    std::optional<EditorCamera> camera;
    std::filesystem::path selectedAsset;
    std::filesystem::path openSprite;

    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static std::optional<EditorWorkspaceState> parse(
        std::string_view text, std::string* error = nullptr);
    [[nodiscard]] bool save(const std::filesystem::path& path, std::string* error = nullptr) const;
    [[nodiscard]] static std::optional<EditorWorkspaceState> load(
        const std::filesystem::path& path, std::string* error = nullptr);
};

// A reference must name an existing regular file inside this project, including
// after symlink resolution. Empty, absolute and parent-traversing paths are rejected.
[[nodiscard]] std::optional<std::filesystem::path> resolve_workspace_asset(
    const std::filesystem::path& projectRoot, const std::filesystem::path& reference);

} // namespace dve::editor
