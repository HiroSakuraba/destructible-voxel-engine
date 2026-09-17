#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "dve/editor_workspace.hpp"
#include "dve/text3d.hpp"

namespace dve::editor {

struct Text3DFontDependencyReport {
    std::filesystem::path requestedPath;
    std::filesystem::path resolvedPath;
    std::filesystem::path projectRelativePath;
    std::vector<std::filesystem::path> licenseFiles;
    bool exists{};
    bool insideProject{};
    bool supportedTrueType{};
    bool licenseVisible{};
    bool packageReady{};
    std::vector<std::string> diagnostics;
};

[[nodiscard]] Text3DFontDependencyReport inspect_text3d_font_dependency(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& fontPath);

[[nodiscard]] std::vector<Text3DFontDependencyReport> collect_text3d_font_dependencies(
    const EditorDocument& document,
    const std::filesystem::path& projectRoot);

struct Text3DFontPackagingResult {
    bool success{};
    std::vector<std::filesystem::path> copiedFiles;
    std::vector<std::string> warnings;
    std::string error;
};

// Copies each referenced source font and its visible license files into a package root while
// preserving project-relative paths. The cooked .dtext remains subset-only; this is an explicit
// dependency-collection step and never embeds the source font into the asset.
[[nodiscard]] Text3DFontPackagingResult package_text3d_font_dependencies(
    const EditorDocument& document,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& packageRoot,
    bool requireVisibleLicense = true);

struct EditorText3DPreview {
    std::optional<CookedText3DAsset> asset;
    Text3DFontDependencyReport dependency;
    std::vector<std::string> warnings;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return asset.has_value() && error.empty(); }
};

// Transactional authoring state shared by the native editor and headless tests. Field changes do
// not mutate the document. refresh() cooks a bounded preview, and commit() inserts or replaces the
// object through the normal undo stack.
class EditorText3DAuthoringSession {
public:
    bool begin_create(std::filesystem::path projectRoot, std::filesystem::path fontPath,
                      std::string textUtf8 = "3D Text", Text3DCookOptions options = {});
    bool begin_edit(std::filesystem::path projectRoot, const EditorObject& object);
    void cancel() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool editing_existing() const noexcept { return targetObject_.has_value(); }
    [[nodiscard]] std::optional<EditorObjectId> target_object() const noexcept { return targetObject_; }
    [[nodiscard]] const std::filesystem::path& project_root() const noexcept { return projectRoot_; }
    [[nodiscard]] const std::filesystem::path& font_path() const noexcept { return fontPath_; }
    [[nodiscard]] const std::filesystem::path& output_asset_path() const noexcept { return outputAssetPath_; }
    [[nodiscard]] const std::string& text() const noexcept { return textUtf8_; }
    [[nodiscard]] const Text3DCookOptions& options() const noexcept { return options_; }
    [[nodiscard]] const EditorText3DPreview& preview() const noexcept { return preview_; }

    void set_font_path(std::filesystem::path path);
    void set_output_asset_path(std::filesystem::path path);
    void set_text(std::string textUtf8);
    void set_style(Text3DStyle style);
    void set_face_material(std::uint32_t materialId, Float4 color);
    void set_side_material(std::uint32_t materialId, Float4 color);

    bool refresh();
    CommandResult commit(EditorWorkspace& workspace, std::string objectName = "3D Text",
                         RigidTransform transform = {});

private:
    static bool confined_path(const std::filesystem::path& root,
                              const std::filesystem::path& path,
                              std::filesystem::path& resolved,
                              std::filesystem::path& relative);
    bool active_{};
    std::optional<EditorObjectId> targetObject_;
    std::filesystem::path projectRoot_;
    std::filesystem::path fontPath_;
    std::filesystem::path outputAssetPath_;
    std::string textUtf8_;
    Text3DCookOptions options_{};
    Text3DObjectState before_{};
    EditorText3DPreview preview_{};
};

} // namespace dve::editor
