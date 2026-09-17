#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dve::editor {

struct EditorProject {
    std::string name{"Untitled Project"};
    std::filesystem::path root;
    std::vector<std::filesystem::path> recentScenes;

    [[nodiscard]] std::filesystem::path sources_dir() const { return root / "sources"; }
    [[nodiscard]] std::filesystem::path scenes_dir() const { return root / "scenes"; }
    [[nodiscard]] std::filesystem::path imports_dir() const { return root / "imports"; }
    [[nodiscard]] std::filesystem::path materials_dir() const { return root / "materials"; }
    [[nodiscard]] std::filesystem::path cooked_dir() const { return root / "cooked"; }
    [[nodiscard]] std::filesystem::path autosave_dir() const { return root / ".autosave"; }
    [[nodiscard]] bool contains(const std::filesystem::path& path) const;
    [[nodiscard]] bool initialize(std::string* error = nullptr) const;
    [[nodiscard]] bool save(std::string* error = nullptr) const;
    [[nodiscard]] static std::optional<EditorProject> load(const std::filesystem::path& file,
                                                            std::string* error = nullptr);
};

class EditorAutosaveManager {
public:
    explicit EditorAutosaveManager(std::filesystem::path recoveryRoot);
    [[nodiscard]] std::optional<std::filesystem::path> save_recovery(
        const class EditorDocument& document, std::string* error = nullptr) const;
    [[nodiscard]] std::vector<std::filesystem::path> recoveries() const;
private:
    std::filesystem::path root_;
};

} // namespace dve::editor
