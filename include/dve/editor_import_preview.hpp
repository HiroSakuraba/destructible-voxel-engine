#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "dve/editor_import_workflow.hpp"
#include "dve/editor_tasks.hpp"

namespace dve::editor {

struct ImportPreviewSnapshot {
    std::uint64_t generation{};
    std::optional<EditorTaskId> taskId;
    EditorTaskState taskState{EditorTaskState::Queued};
    float progress{};
    std::string phase;
    bool ready{};
    std::uint64_t occupiedVoxels{};
    std::uint64_t occupiedBricks{};
    std::size_t objectCount{};
    std::string error;
    std::shared_ptr<const CookedVoxelScene> scene;
};

class ModelImportPreviewService {
public:
    explicit ModelImportPreviewService(EditorTaskManager& tasks);
    ~ModelImportPreviewService();
    ModelImportPreviewService(const ModelImportPreviewService&) = delete;
    ModelImportPreviewService& operator=(const ModelImportPreviewService&) = delete;

    [[nodiscard]] bool request_preview(
        std::filesystem::path sourcePath,
        VoxelizeSettings settings,
        std::vector<ImportNodeChoice> nodes,
        std::string* error = nullptr);
    [[nodiscard]] bool cancel();
    [[nodiscard]] ImportPreviewSnapshot snapshot() const;
    [[nodiscard]] bool publish_current(
        const std::filesystem::path& manifestPath,
        std::string* error = nullptr) const;

private:
    struct SharedState;
    EditorTaskManager* tasks_{};
    std::shared_ptr<SharedState> shared_;
};

} // namespace dve::editor
