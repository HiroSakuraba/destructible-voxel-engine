#pragma once

// Join and Separate for voxel objects (artist worklist ART-061 Separate Islands, ART-062 Join).
//
// Separate Islands: each selected voxel object is split into its face-connected pieces
// ("islands"). The largest island stays in the original object (same id, children, components);
// every other island becomes a new sibling object with the same transform and voxel size, so
// nothing moves in the world. One undo step for the whole selection. Like Slice's Separate,
// prefab instances and attached objects are refused; an object that would split into more than
// kMaxSeparateIslands pieces is refused too.
//
// Join: merges the geometry of the other selected voxel objects ("operands") into the active
// one ("target"). Unlike Group, the result is one voxel object. Operands are removed; their child
// objects move under the target. Where pieces overlap, the target's material wins. Operands with
// components (other than Tags/Layer/Groups membership) or prefab links are refused, since Join
// would delete them; so are hidden participants and locked children of an operand.
//   - Operands on the target's grid (same voxel size, rotation a multiple of 90 degrees, whole-
//     voxel offset) are copied voxel for voxel.
//   - Any other operand must be resampled into the target's grid. The preview names each
//     mismatch and the join waits for a choice every time: R resamples, G groups the objects
//     instead (no geometry change), Tab makes the next object the target.
// Nothing changes while previewing; Enter commits one undo step.

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dve/editor_command.hpp"
#include "dve/voxel_boolean.hpp"

namespace dve::editor {

class EditorWorkspace;

// Face-connected (6-neighbour) pieces of a voxel object, largest first. Ties go to the piece
// holding the smallest voxel coordinate, so the order is deterministic.
[[nodiscard]] std::vector<std::vector<SparseVoxelStateEntry>> find_voxel_islands(const VoxelObject& voxels);

inline constexpr std::size_t kMaxSeparateVoxels = 2'000'000;
// At most this many pieces (the original plus new objects) per object: thousands of stray single
// voxels would otherwise flood the outliner with objects in one click.
inline constexpr std::size_t kMaxSeparateIslands = 256;

// Why `id` cannot be separated (empty when it can; "already one piece" is a reason too).
[[nodiscard]] std::string separate_islands_problem(const EditorDocument& document, EditorObjectId id);

struct SeparateIslandsBuild {
    std::unique_ptr<CompoundCommand> command;  // null when nothing can be separated
    std::vector<EditorObjectId> newObjects;    // ids of the new pieces
    std::vector<EditorObjectId> newObjectSources;  // the original each new piece came from (parallel)
    std::size_t separatedObjects{};            // objects that had more than one piece
    std::vector<std::string> skipped;          // one line per selected object left alone, and why
};

// Builds (does not execute) the command separating every object in `ids` that has more than one
// island. Ids are allocated from `document`.
[[nodiscard]] SeparateIslandsBuild build_separate_islands_command(EditorDocument& document,
                                                                  const std::vector<EditorObjectId>& ids);

struct VoxelJoinOperand {
    EditorObjectId id{};
    std::uint64_t voxels{};
    std::string mismatch;  // why it must be resampled; empty when it is on the target's grid
};

class EditorVoxelJoinSession {
public:
    // Why the selection cannot be joined (empty when it can). `primary` is the active selection.
    [[nodiscard]] static std::string selection_problem(const EditorDocument& document,
                                                       const std::set<EditorObjectId>& selection,
                                                       std::optional<EditorObjectId> primary);
    bool begin(const EditorDocument& document, const std::set<EditorObjectId>& selection,
               std::optional<EditorObjectId> primary, std::string* error = nullptr);
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] EditorObjectId target() const noexcept { return target_; }
    [[nodiscard]] const std::vector<VoxelJoinOperand>& operands() const noexcept { return operands_; }
    [[nodiscard]] const VoxelBooleanResult& result() const noexcept { return result_; }
    [[nodiscard]] bool has_mismatch() const noexcept;
    // True until the user chose to resample, when any operand is off the target's grid.
    [[nodiscard]] bool needs_choice() const noexcept { return has_mismatch() && !resampleChosen_; }
    void choose_resample() noexcept { resampleChosen_ = true; }
    [[nodiscard]] bool resample_chosen() const noexcept { return resampleChosen_; }
    // Makes the next participant (by id) the target; the resample choice is asked again.
    void cycle_target(const EditorDocument& document);
    // Recomputes if a participant changed; cancels and returns false if one was removed.
    bool refresh(const EditorDocument& document);
    [[nodiscard]] std::string blocked_reason(const EditorDocument& document) const;
    [[nodiscard]] std::vector<std::string> describe(const EditorDocument& document) const;
    [[nodiscard]] std::vector<EditorObjectId> participants() const;
    CommandResult commit(EditorWorkspace& workspace);
    void cancel() noexcept;

private:
    void recompute(const EditorDocument& document);
    [[nodiscard]] std::uint64_t fingerprint(const EditorDocument& document) const noexcept;
    bool active_{};
    bool resampleChosen_{};
    EditorObjectId target_{};
    std::vector<VoxelJoinOperand> operands_;
    VoxelBooleanResult result_;
    std::uint64_t fingerprint_{};
};

} // namespace dve::editor
