#pragma once

// Editor workflow for authored voxel Booleans (ART-060).
//
//   1. Select two or more voxel objects. The active (primary) selection is the target "A"; every
//      other selected object is an operand "B" (in ascending object-id order).
//   2. Tools > Voxel > Boolean Union / Difference / Intersection (or the command palette) opens a
//      non-destructive preview session. Nothing in the document changes while previewing, so
//      Esc / Cancel restores the scene exactly (ART-113).
//   3. Commit applies the result to the target and the operand output policy to the operands as
//      ONE undoable command (ART-068): Undo restores the target voxels, anchors and the operands;
//      Redo reapplies both.
//
// The core computation lives in dve/voxel_boolean.hpp and is independent of the editor.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "dve/editor_command.hpp"
#include "dve/voxel_boolean.hpp"

namespace dve::editor {

class EditorWorkspace;

// What happens to the operand objects on commit. Hide (default) keeps them in the scene,
// invisible, so play, export and picking ignore them but they can be shown again to re-run the
// operation; Delete removes them; Keep leaves them untouched.
enum class VoxelBooleanOperandPolicy : std::uint8_t { Hide, Delete, Keep };

[[nodiscard]] std::string_view voxel_boolean_operand_policy_name(VoxelBooleanOperandPolicy policy) noexcept;

struct VoxelBooleanSelection {
    bool valid{};
    // Why the command is unavailable, phrased as what to select or change (ART-114).
    std::string reason;
    EditorObjectId target{};
    std::vector<EditorObjectId> operands;
};

// Validates the current selection for a Boolean. `primary` is the active selection; when it is
// absent or not in `selection`, the lowest selected id is the target.
[[nodiscard]] VoxelBooleanSelection evaluate_voxel_boolean_selection(
    const EditorDocument& document, const std::set<EditorObjectId>& selection,
    std::optional<EditorObjectId> primary,
    VoxelBooleanOperandPolicy policy = VoxelBooleanOperandPolicy::Hide);

// Applies a precomputed Boolean change list (and anchor set) to one voxel object. Execute and
// undo both verify that every voxel still holds the expected material first, so a stale command
// fails without touching the object.
class ApplyVoxelBooleanCommand final : public IEditorCommand {
public:
    ApplyVoxelBooleanCommand(EditorObjectId target, std::vector<VoxelBooleanChange> changes,
                             std::vector<Int3> anchorsBefore, std::vector<Int3> anchorsAfter,
                             std::string label);
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
    [[nodiscard]] std::size_t change_count() const noexcept { return changes_.size(); }

private:
    CommandResult apply(EditorDocument&, bool forward);
    EditorObjectId target_{};
    std::vector<VoxelBooleanChange> changes_;
    std::vector<Int3> anchorsBefore_;
    std::vector<Int3> anchorsAfter_;
    std::string label_;
};

struct VoxelBooleanCommitReport {
    EditorObjectId target{};
    std::vector<EditorObjectId> operands;
    VoxelBooleanOperation operation{VoxelBooleanOperation::Union};
    VoxelBooleanOperandPolicy policy{VoxelBooleanOperandPolicy::Hide};
    VoxelBooleanStats stats;
    std::size_t collisionBoxes{};   // merged box proxy of the committed target
    bool collisionEnabled{true}; // false: target has collision off, so no proxy was built
};

class EditorVoxelBooleanSession {
public:
    // Starts a preview. Fails (returns false, sets *error) when the selection is invalid. A
    // started session may still have a result that cannot be committed (empty result, no
    // overlap, ...): the preview then explains why.
    bool begin(const EditorDocument& document, const VoxelBooleanSelection& selection,
               VoxelBooleanOperation operation, VoxelBooleanOperandPolicy policy,
               std::string* error = nullptr);
    [[nodiscard]] bool active() const noexcept { return active_; }
    void set_operation(const EditorDocument& document, VoxelBooleanOperation operation);
    void set_operand_policy(VoxelBooleanOperandPolicy policy) noexcept { policy_ = policy; }
    [[nodiscard]] VoxelBooleanOperation operation() const noexcept { return operation_; }
    [[nodiscard]] VoxelBooleanOperandPolicy operand_policy() const noexcept { return policy_; }
    [[nodiscard]] VoxelBooleanOverlapMaterial overlap_material() const noexcept { return overlapMaterial_; }
    void set_overlap_material(const EditorDocument& document, VoxelBooleanOverlapMaterial material);
    [[nodiscard]] EditorObjectId target() const noexcept { return target_; }
    [[nodiscard]] const std::vector<EditorObjectId>& operands() const noexcept { return operands_; }
    [[nodiscard]] const VoxelBooleanResult& result() const noexcept { return result_; }
    // Recomputes when any participating object changed since the preview was computed (an edit
    // from another tool, the live MCP host, ...). Returns false and cancels the session when a
    // participant was removed or stopped being a voxel object.
    bool refresh(const EditorDocument& document);
    [[nodiscard]] std::uint64_t participants_fingerprint(const EditorDocument& document) const noexcept;
    [[nodiscard]] bool can_commit() const noexcept { return active_ && result_.can_commit(); }
    // Why commit is unavailable (empty when it is available).
    [[nodiscard]] std::string commit_blocked_reason() const;
    // Executes exactly one command on the workspace's command stack, selects the target and
    // ends the session. On failure the document is unchanged (compound rollback) and the
    // session stays open so the user can adjust or cancel.
    CommandResult commit(EditorWorkspace& workspace);
    void cancel() noexcept;
    // Human-readable preview lines for the viewport panel and status bar.
    [[nodiscard]] std::vector<std::string> describe(const EditorDocument& document) const;
    [[nodiscard]] const std::optional<VoxelBooleanCommitReport>& last_commit() const noexcept { return lastCommit_; }

private:
    void recompute(const EditorDocument& document);
    bool active_{};
    EditorObjectId target_{};
    std::vector<EditorObjectId> operands_;
    VoxelBooleanOperation operation_{VoxelBooleanOperation::Union};
    VoxelBooleanOperandPolicy policy_{VoxelBooleanOperandPolicy::Hide};
    VoxelBooleanOverlapMaterial overlapMaterial_{VoxelBooleanOverlapMaterial::KeepPrimary};
    VoxelBooleanResult result_;
    std::uint64_t fingerprint_{};
    std::optional<VoxelBooleanCommitReport> lastCommit_;
};

} // namespace dve::editor
