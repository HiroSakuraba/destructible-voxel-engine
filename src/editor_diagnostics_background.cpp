#include "dve/editor_diagnostics_background.hpp"

#include <utility>

namespace dve::editor {

struct EditorObjectDiagnosticsService::SharedState {
    mutable std::mutex mutex;
    std::uint64_t generation{};
    std::optional<EditorTaskId> taskId;
    std::optional<EditorObjectDiagnostics> diagnostics;
    std::string error;
};

EditorObjectDiagnosticsService::EditorObjectDiagnosticsService(EditorTaskManager& tasks)
    : tasks_(&tasks), shared_(std::make_shared<SharedState>()) {}

EditorObjectDiagnosticsService::~EditorObjectDiagnosticsService() { (void)cancel(); }

bool EditorObjectDiagnosticsService::request(
    const EditorObject& object, const EditorMaterialLibrary& materials, std::string* error) {
    if (!object.voxels) {
        if (error) *error = "object has no voxel data";
        return false;
    }
    (void)cancel();

    // Deep, owned clone: analysis runs on the worker thread against this copy, never against
    // the live document, so an edit arriving while analysis is in flight cannot race with it.
    // Wrapped in shared_ptr (rather than captured by value) because EditorObject is move-only
    // (it owns a std::unique_ptr<VoxelObject>), and std::function requires its target to be
    // copy-constructible even though EditorTaskManager only ever calls it once.
    auto clone = std::make_shared<EditorObject>(clone_editor_object(object));

    std::uint64_t generation = 0;
    {
        std::lock_guard lock(shared_->mutex);
        generation = ++shared_->generation;
        shared_->diagnostics.reset();
        shared_->error.clear();
        shared_->taskId.reset();
    }

    const std::shared_ptr<SharedState> state = shared_;
    std::string submitError;
    auto taskId = tasks_->submit(
        "Analyze object diagnostics",
        [state, generation, clone, materials](EditorTaskContext& context) {
            context.report(0.10F, "Analyzing structure");
            if (context.cancelled()) return;
            EditorObjectDiagnostics result = analyze_editor_object(*clone, materials);
            if (context.cancelled()) return;
            {
                std::lock_guard lock(state->mutex);
                if (state->generation != generation) return; // superseded; discard
                state->diagnostics = std::move(result);
                state->error.clear();
            }
            context.report(1.0F, "Diagnostics ready");
        },
        &submitError);

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

bool EditorObjectDiagnosticsService::cancel() {
    std::optional<EditorTaskId> task;
    {
        std::lock_guard lock(shared_->mutex);
        task = shared_->taskId;
        ++shared_->generation; // always invalidate work, including the submit/store-task-id window
        shared_->taskId.reset();
        shared_->diagnostics.reset(); // also clears an already-published result: the worker
                                       // may have finished and written back before this call
                                       // took the lock, and cancellation must still mean
                                       // "nothing ready" regardless of that race.
        shared_->error.clear();
    }
    return task && tasks_ ? tasks_->cancel(*task) : false;
}

ObjectDiagnosticsSnapshot EditorObjectDiagnosticsService::snapshot() const {
    ObjectDiagnosticsSnapshot result;
    {
        std::lock_guard lock(shared_->mutex);
        result.generation = shared_->generation;
        result.taskId = shared_->taskId;
        result.diagnostics = shared_->diagnostics;
        result.ready = shared_->diagnostics.has_value();
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

} // namespace dve::editor
