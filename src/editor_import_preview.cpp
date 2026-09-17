#include "dve/editor_import_preview.hpp"

#include <utility>
#include <stdexcept>

namespace dve::editor {

struct ModelImportPreviewService::SharedState {
    mutable std::mutex mutex;
    std::uint64_t generation{};
    std::optional<EditorTaskId> taskId;
    std::shared_ptr<CookedVoxelScene> scene;
    std::string error;
    std::uint64_t occupiedVoxels{};
    std::uint64_t occupiedBricks{};
};

ModelImportPreviewService::ModelImportPreviewService(EditorTaskManager& tasks)
    : tasks_(&tasks), shared_(std::make_shared<SharedState>()) {}

ModelImportPreviewService::~ModelImportPreviewService() { (void)cancel(); }

bool ModelImportPreviewService::request_preview(
    std::filesystem::path sourcePath,
    VoxelizeSettings settings,
    std::vector<ImportNodeChoice> nodes,
    std::string* error) {
    if (sourcePath.empty() || !std::filesystem::is_regular_file(sourcePath)) {
        if (error) *error = "preview source model does not exist";
        return false;
    }
    (void)cancel();
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(shared_->mutex);
        generation = ++shared_->generation;
        shared_->scene.reset();
        shared_->error.clear();
        shared_->occupiedVoxels = 0;
        shared_->occupiedBricks = 0;
        shared_->taskId.reset();
    }

    const std::shared_ptr<SharedState> state = shared_;
    std::string submitError;
    auto taskId = tasks_->submit("Cook model preview",
        [state, generation, sourcePath = std::move(sourcePath), settings,
         nodes = std::move(nodes)](EditorTaskContext& context) mutable {
            context.report(0.05F, "Importing model");
            ImportedModelResult imported = import_model(sourcePath);
            if (!imported.success) {
                std::string message = "model import failed";
                for (const ImportDiagnostic& diagnostic : imported.diagnostics) {
                    if (diagnostic.severity == ImportDiagnostic::Severity::Error) {
                        message = diagnostic.message;
                        break;
                    }
                }
                std::lock_guard lock(state->mutex);
                if (state->generation == generation) state->error = std::move(message);
                throw std::runtime_error("model import failed");
            }
            if (context.cancelled()) return;

            SceneCookSettings sceneSettings;
            sceneSettings.defaults = settings;
            sceneSettings.splitByNode = true;
            for (const ImportNodeChoice& choice : nodes) {
                NodeCookSettings nodeSettings;
                nodeSettings.ignore = !choice.included;
                nodeSettings.mode = choice.mode;
                nodeSettings.anchored = choice.anchored;
                nodeSettings.structural = choice.structural;
                nodeSettings.generateCollision = choice.generateCollision;
                sceneSettings.nodeOverrides[choice.name] = nodeSettings;
            }
            context.report(0.25F, "Voxelizing preview");
            auto cooked = std::make_shared<CookedVoxelScene>(
                voxelize_scene_objects(imported.scene, sceneSettings));
            if (!cooked->success) {
                std::lock_guard lock(state->mutex);
                if (state->generation == generation) state->error = "voxel preview failed";
                throw std::runtime_error("voxel preview failed");
            }
            if (context.cancelled()) return;

            context.report(0.90F, "Building preview statistics");
            std::uint64_t occupiedVoxels = 0;
            std::uint64_t occupiedBricks = 0;
            for (const CookedVoxelObject& object : cooked->objects) {
                occupiedVoxels += object.asset.object.occupied_voxel_count();
                occupiedBricks += object.asset.object.bricks().size();
            }
            if (context.cancelled()) return;
            {
                std::lock_guard lock(state->mutex);
                if (state->generation != generation) return;
                state->scene = std::move(cooked);
                state->occupiedVoxels = occupiedVoxels;
                state->occupiedBricks = occupiedBricks;
                state->error.clear();
            }
            context.report(1.0F, "Preview ready");
        }, &submitError);
    if (!taskId) {
        std::lock_guard lock(shared_->mutex);
        if (shared_->generation == generation) shared_->error = submitError;
        if (error) *error = submitError;
        return false;
    }
    bool accepted = false;
    {
        std::lock_guard lock(shared_->mutex);
        if (shared_->generation == generation) {
            shared_->taskId = *taskId;
            accepted = true;
        }
    }
    // A concurrent cancel can invalidate the generation after submit() succeeds but before
    // the task identifier is stored. Stop that now-stale worker instead of merely relying on
    // generation checking to discard its eventual result.
    if (!accepted && tasks_) (void)tasks_->cancel(*taskId);
    return true;
}

bool ModelImportPreviewService::cancel() {
    std::optional<EditorTaskId> task;
    {
        std::lock_guard lock(shared_->mutex);
        task = shared_->taskId;
        ++shared_->generation; // invalidate even during the submit/store-task-id window
        shared_->taskId.reset();
        shared_->scene.reset();
        shared_->error.clear();
        shared_->occupiedVoxels = 0;
        shared_->occupiedBricks = 0;
    }
    return task && tasks_ ? tasks_->cancel(*task) : false;
}

ImportPreviewSnapshot ModelImportPreviewService::snapshot() const {
    ImportPreviewSnapshot result;
    {
        std::lock_guard lock(shared_->mutex);
        result.generation = shared_->generation;
        result.taskId = shared_->taskId;
        result.scene = shared_->scene;
        result.ready = static_cast<bool>(shared_->scene) && shared_->scene->success;
        result.objectCount = shared_->scene ? shared_->scene->objects.size() : 0;
        result.occupiedVoxels = shared_->occupiedVoxels;
        result.occupiedBricks = shared_->occupiedBricks;
        result.error = shared_->error;
    }
    if (result.taskId && tasks_) {
        if (const auto task = tasks_->snapshot(*result.taskId)) {
            result.taskState = task->state;
            result.progress = task->progress;
            result.phase = task->phase;
            if (result.error.empty()) result.error = task->error;
        }
    }
    return result;
}

bool ModelImportPreviewService::publish_current(
    const std::filesystem::path& manifestPath,
    std::string* error) const {
    std::shared_ptr<const CookedVoxelScene> scene;
    {
        std::lock_guard lock(shared_->mutex);
        scene = shared_->scene;
    }
    if (!scene || !scene->success) {
        if (error) *error = "no completed import preview is available";
        return false;
    }
    return write_dvox_scene_package(manifestPath, *scene, error);
}

} // namespace dve::editor
