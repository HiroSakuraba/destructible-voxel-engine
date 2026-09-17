#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/editor_document.hpp"
#include "dve/editor_workspace.hpp"

namespace dve::editor {

enum class ImportWizardPage : std::uint8_t {
    Source, ObjectStructure, VoxelSettings, Preview, Problems, CookAndSave, Complete
};

struct ImportNodeChoice {
    std::string name;
    std::string path;
    bool included{true};
    bool anchored{};
    bool structural{true};
    bool generateCollision{true};
    VoxelizationMode mode{VoxelizationMode::Solid};
};

struct ImportWorkflowEstimate {
    std::uint64_t triangles{};
    std::uint64_t predictedWorkingVoxels{};
    std::uint64_t predictedBytes{};
    std::size_t nodes{};
};

class ModelImportWorkflow {
public:
    void reset();
    [[nodiscard]] bool choose_source(const std::filesystem::path& path,
                                     std::string* error = nullptr);
    [[nodiscard]] bool advance(std::string* error = nullptr);
    [[nodiscard]] bool back() noexcept;
    [[nodiscard]] ImportWizardPage page() const noexcept { return page_; }
    [[nodiscard]] const ImportedModelResult& imported() const noexcept { return imported_; }
    [[nodiscard]] std::vector<ImportNodeChoice>& nodes() noexcept { return nodes_; }
    [[nodiscard]] VoxelizeSettings& settings() noexcept { return settings_; }
    [[nodiscard]] const ImportWorkflowEstimate& estimate() const noexcept { return estimate_; }
    [[nodiscard]] const std::vector<EditorProblem>& problems() const noexcept { return problems_; }
    [[nodiscard]] const std::filesystem::path& source_path() const noexcept { return sourcePath_; }

    [[nodiscard]] bool cook(const std::filesystem::path& manifestPath,
                            std::string* error = nullptr);
    [[nodiscard]] const CookedVoxelScene& cooked_scene() const noexcept { return cooked_; }
private:
    void build_node_choices();
    void build_problems();
    void update_estimate();
    std::filesystem::path sourcePath_;
    ImportWizardPage page_{ImportWizardPage::Source};
    ImportedModelResult imported_{};
    VoxelizeSettings settings_{};
    std::vector<ImportNodeChoice> nodes_;
    ImportWorkflowEstimate estimate_{};
    std::vector<EditorProblem> problems_;
    CookedVoxelScene cooked_{};
};

} // namespace dve::editor
