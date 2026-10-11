#pragma once

// Voxel selection (artist worklist ART-043): a set of voxel cells in one voxel object, and the
// edits that act on it. The editor's selection domain (M) decides whether Select and Move work on
// objects or on voxels; see NativeEditorController::set_selection_domain.
//
//   gestures (Voxel domain, Select or Move tool): click = the clicked face-connected piece,
//   drag = box (front-facing voxels; every voxel in the box with X-Ray Selection on),
//   Ctrl+Shift-drag = brush (the voxel brush size, [ ]); Shift adds, Ctrl removes.
//   edits: Delete, Extract to New Object, Copy/Cut/Paste (as a new object in place), Move with
//   the gizmo in whole voxels, Grow/Shrink/Invert/Select All; Paint, Remove and Flood Fill only
//   touch selected voxels while a selection exists.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dve/editor_command.hpp"
#include "dve/editor_viewport.hpp"

namespace dve::editor {

// The most voxels one selection may hold. A std::set cell costs ~50 bytes, and the highlight,
// pruning and the Move gizmo walk the set, so a bigger region is refused with a message (use
// Separate Islands or Slice for whole-object work).
inline constexpr std::size_t kMaxVoxelSelection = 500'000;

// Face-connected (6-neighbour) occupied cells reachable from `seed` (empty when seed is air).
// Stops once the piece holds more than `limit` cells (callers treat size() > limit as too big).
[[nodiscard]] std::set<Int3> voxel_connected_piece(const VoxelObject& voxels, Int3 seed,
                                                   std::size_t limit = static_cast<std::size_t>(-1));
// Every occupied cell.
[[nodiscard]] std::set<Int3> all_occupied_voxels(const VoxelObject& voxels);
// Grow: adds occupied face neighbours. Shrink: drops cells with a face neighbour outside the
// selection (its boundary). Invert: occupied cells not selected. Grow and Invert stop once the
// result holds more than `limit` cells.
[[nodiscard]] std::set<Int3> grow_voxel_selection(const VoxelObject& voxels, const std::set<Int3>& selection,
                                                  std::size_t limit = static_cast<std::size_t>(-1));
[[nodiscard]] std::set<Int3> shrink_voxel_selection(const std::set<Int3>& selection);
[[nodiscard]] std::set<Int3> invert_voxel_selection(const VoxelObject& voxels, const std::set<Int3>& selection,
                                                    std::size_t limit = static_cast<std::size_t>(-1));

// Cells of `object` whose centres project inside `rect`. Without `through`, only cells with an
// exposed face turned towards the camera count (what the artist can see on a solid object).
[[nodiscard]] std::set<Int3> voxels_in_screen_rect(const EditorObject& object, const EditorCamera& camera,
                                                   UiRect viewport, UiRect rect, bool through);
// Occupied cells in a cube of `radius` around `center` (radius 0 = the cell itself).
[[nodiscard]] std::set<Int3> voxels_in_cube(const VoxelObject& voxels, Int3 center, int radius);

// Builds (does not execute) one undoable command. Each returns null and sets *error when it
// cannot apply (nothing selected, locked object, ...).
[[nodiscard]] std::unique_ptr<IEditorCommand> build_delete_voxels_command(const EditorObject& object,
                                                                          const std::set<Int3>& cells,
                                                                          std::string* error = nullptr);
// Moves the cells by `offset` (in the object's own grid). Moved cells replace whatever is at the
// destination; the cells they leave become empty; anchors move with them.
[[nodiscard]] std::unique_ptr<IEditorCommand> build_move_voxels_command(const EditorObject& object,
                                                                        const std::set<Int3>& cells, Int3 offset,
                                                                        std::string* error = nullptr);

// A copy of selected cells, independent of the scene.
struct VoxelClipboard {
    std::string name;
    float voxelSizeMeters{0.1F};
    RigidTransform transform{};
    std::optional<EditorObjectId> parent;
    EditorObjectFlags flags{};
    std::vector<SparseVoxelStateEntry> cells;
    std::vector<Int3> anchors;
};
[[nodiscard]] VoxelClipboard copy_voxels(const EditorObject& object, const std::set<Int3>& cells);
// A new object holding the clipboard's cells at the place they were copied from.
[[nodiscard]] EditorObject make_object_from_voxels(EditorObjectId id, const VoxelClipboard& clipboard, std::string name);

struct ExtractVoxelsBuild {
    std::unique_ptr<CompoundCommand> command;
    EditorObjectId newObject{};
    std::string error;
};
// Moves the cells into a new sibling object (same transform and voxel size, so nothing moves).
// Same rules as Separate Islands / Slice's Separate: prefab instances and objects attached to a
// socket are refused; the new object keeps flags, tags, groups, layer and membership, not the
// import source, prefab link or other components.
[[nodiscard]] ExtractVoxelsBuild build_extract_voxels_command(EditorDocument& document, EditorObjectId id,
                                                              const std::set<Int3>& cells);

} // namespace dve::editor
