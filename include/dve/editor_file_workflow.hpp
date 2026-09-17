#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dve::editor {

enum class EditorFileIntent {
    OpenProject,
    OpenScene,
    OpenUiAsset,
    OpenSpriteAsset,
    OpenDeformableAsset,
    ImportModel,
    ImportTexture,
    OpenMaterialLibrary,
    OpenImportRecipe,
    AddVoxelAsset,
    AddScenePackage,
    Unsupported,
};

struct EditorFileRoute {
    std::filesystem::path path;
    EditorFileIntent intent{EditorFileIntent::Unsupported};
    std::string actionLabel;
    bool supported{};
};

[[nodiscard]] EditorFileRoute classify_editor_file(const std::filesystem::path& path);
[[nodiscard]] std::vector<EditorFileRoute> route_editor_file_drop(
    const std::vector<std::filesystem::path>& paths);

struct RecentProjectEntry {
    std::filesystem::path projectFile;
    std::string displayName;
};

class RecentProjectRegistry {
public:
    explicit RecentProjectRegistry(std::size_t capacity = 12);

    void record(std::filesystem::path projectFile, std::string displayName = {});
    bool remove(const std::filesystem::path& projectFile);
    std::size_t remove_missing();
    void clear() noexcept { entries_.clear(); }

    [[nodiscard]] const std::vector<RecentProjectEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool save(const std::filesystem::path& path, std::string* error = nullptr) const;
    [[nodiscard]] static std::optional<RecentProjectRegistry> load(
        const std::filesystem::path& path,
        std::size_t capacity = 12,
        std::string* error = nullptr);

private:
    [[nodiscard]] static std::filesystem::path normalized_path(
        const std::filesystem::path& path);

    std::size_t capacity_{12};
    std::vector<RecentProjectEntry> entries_;
};

} // namespace dve::editor
