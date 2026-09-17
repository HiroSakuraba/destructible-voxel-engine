#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "dve/editor_document.hpp"
#include "dve/editor_materials.hpp"
#include "dve/physics_jolt_backend.hpp"

namespace dve::editor {

// Bridges EditorWorkspace's Simulate mode to a real Jolt Physics world (v1.14 work package,
// item 9: "Execute Simulate mode through Jolt Physics v5.6.0 and restore the pre-simulation
// checkpoint on exit"). This class owns only the running Jolt world and the per-object
// bookkeeping needed to write simulated transforms back into an EditorDocument; it does not
// snapshot or restore the document itself. EditorWorkspace::set_mode() already does that
// (clone_editor_document() on entry, restore_pre_simulation() on exit), and this class is
// meant to be started only after EditorWorkspace has taken that snapshot, and stopped before
// (or as part of) returning to EditorMode::Edit.
//
// Object eligibility, matching the convention already established for the runtime scene
// loader's dynamic/static rigid-body construction (src/runtime_scene.cpp):
//   - flags.collisionEnabled == false, or no occupied voxels: no body at all.
//   - flags.anchored == true: a static body (never moves; not written back after stepping).
//   - otherwise: a dynamic body. flags.structural selects RigidBodyCollisionClass::Full vs
//     DebrisNoSelf, matching the runtime scene's own structural/decorative split.
//   - flags.locked is deliberately NOT consulted here: it is an edit-protection concept
//     (blocks command-stack mutation), orthogonal to whether an object participates in
//     physics.
//
// An object whose collision/mass data cannot be turned into a valid rigid body (empty
// collision proxy, non-finite mass properties, proxy over the box budget) is skipped rather
// than failing the whole start() call: see skipped_object_count().
class EditorJoltSimulation {
public:
    EditorJoltSimulation();
    explicit EditorJoltSimulation(const JoltWorldConfig& config);
    ~EditorJoltSimulation();
    EditorJoltSimulation(const EditorJoltSimulation&) = delete;
    EditorJoltSimulation& operator=(const EditorJoltSimulation&) = delete;

    // Builds one Jolt body per eligible object in `document` and enters the running state.
    // Fails without creating any body if `document` does not validate. Safe to call only
    // when not already running(); call stop() first to restart.
    [[nodiscard]] bool start(
        const EditorDocument& document,
        const EditorMaterialLibrary& materials,
        std::string* error = nullptr);

    // Advances the simulation by one fixed step and writes each simulated dynamic object's
    // resulting world transform back into the matching object in `document`. Objects that
    // were removed from `document` since start() (or whose id no longer resolves) are
    // silently skipped; this class does not assume the document is immutable while running,
    // only that object identities are stable. Returns false and does nothing if not running.
    bool step(EditorDocument& document, float fixedDeltaSeconds);

    // Releases the Jolt world and all bodies. Does not touch any EditorDocument; the caller
    // (normally EditorWorkspace::restore_pre_simulation) is responsible for restoring
    // pre-simulation document state. Safe to call when not running().
    void stop();

    [[nodiscard]] bool running() const noexcept { return world_ != nullptr; }
    [[nodiscard]] std::size_t simulated_dynamic_object_count() const noexcept { return dynamicObjects_.size(); }
    [[nodiscard]] std::size_t simulated_static_object_count() const noexcept { return staticHandles_.size(); }
    [[nodiscard]] std::size_t skipped_object_count() const noexcept { return skippedObjectCount_; }
    [[nodiscard]] JoltPhysicsTelemetry telemetry() const;

    // std::nullopt if `id` was never simulated (skipped, static, or unrecognized); otherwise
    // whether that dynamic body is currently active/awake (true) or sleeping (false).
    [[nodiscard]] std::optional<bool> object_awake(EditorObjectId id) const;

private:
    struct SimulatedDynamicObject {
        EditorObjectId objectId{};
        RigidBodyHandle handle{kInvalidRigidBodyHandle};
        // Object-local (unrotated), meters: offset from the object's origin/pivot to its
        // physical center of mass. Needed every step to convert Jolt's center-of-mass-frame
        // transform back into the object's own origin-frame transform on write-back.
        Float3 localCenterOfMassMeters{};
    };

    std::unique_ptr<JoltRigidBodyWorld> world_;
    std::vector<SimulatedDynamicObject> dynamicObjects_;
    std::vector<RigidBodyHandle> staticHandles_;
    std::size_t skippedObjectCount_{};
    JoltWorldConfig config_{};
};

} // namespace dve::editor
