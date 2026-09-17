#pragma once

#include <memory>
#include <optional>
#include <string>

#include "dve/editor_diagnostics.hpp"
#include "dve/editor_document.hpp"
#include "dve/editor_materials.hpp"
#include "dve/editor_tasks.hpp"

namespace dve::editor {

struct ObjectDiagnosticsSnapshot {
    std::uint64_t generation{};
    std::optional<EditorTaskId> taskId;
    EditorTaskState taskState{EditorTaskState::Queued};
    float progress{};
    std::string phase;
    bool ready{};
    std::optional<EditorObjectDiagnostics> diagnostics;
    std::string error;
};

// Moves the connectivity/mass/inertia/collision-proxy analysis in editor_diagnostics.hpp onto
// a background EditorTaskManager worker, with stale-result rejection (v1.14 item 7).
//
// analyze_editor_object() is exact and CPU-bound: for a large authored object it walks every
// occupied voxel more than once (occupancy set, surface test, connected-component flood
// fill), which is exactly the "expensive diagnostics" the inspector panel should not compute
// on the thread handling input. Running it directly against the live EditorDocument from a
// worker thread would be a data race the moment the user edits while analysis is in flight,
// so request() takes a deep, owned clone of the target object (via clone_voxel_object(), the
// same helper EditorWorkspace's own pre-simulation checkpoint uses) before handing it to the
// worker. That clone happens synchronously on the caller's thread and is a bulk copy, not a
// flood fill; the actual analysis after it runs entirely off-thread.
//
// Selection-level diagnostics (analyze_editor_selection) are not covered here. They would
// follow the identical clone-then-analyze shape, just cloning every selected object instead
// of one; left as a direct extension rather than built speculatively.
class EditorObjectDiagnosticsService {
public:
    explicit EditorObjectDiagnosticsService(EditorTaskManager& tasks);
    ~EditorObjectDiagnosticsService();
    EditorObjectDiagnosticsService(const EditorObjectDiagnosticsService&) = delete;
    EditorObjectDiagnosticsService& operator=(const EditorObjectDiagnosticsService&) = delete;

    // Clones `object` and schedules background analysis against that clone using `materials`
    // (materials are small and copied by value into the task; no aliasing concern there).
    // Any previous in-flight request is cancelled first and its result, if it lands late, is
    // discarded by the generation check rather than overwriting a newer request's result.
    // Returns false only if `object.voxels` is null or the task queue rejects submission
    // (queue full); check *error in that case.
    [[nodiscard]] bool request(
        const EditorObject& object,
        const EditorMaterialLibrary& materials,
        std::string* error = nullptr);

    [[nodiscard]] bool cancel();
    [[nodiscard]] ObjectDiagnosticsSnapshot snapshot() const;

private:
    struct SharedState;
    EditorTaskManager* tasks_{};
    std::shared_ptr<SharedState> shared_;
};

} // namespace dve::editor
