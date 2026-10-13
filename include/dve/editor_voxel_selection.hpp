#pragma once

// Voxel selection (artist worklist ART-043): a set of voxel cells in one voxel object, and the
// edits that act on it. The editor's selection domain (M) decides whether Select and Move work on
// objects or on voxels; see NativeEditorController::set_selection_domain.
//
//   gestures (Voxel domain, Select or Move tool): click = the clicked face-connected piece,
//   left-to-right drag = window (whole cells), right-to-left = crossing (touching cells).
//   Shift adds, Ctrl toggles, Ctrl+Shift intersects; X-Ray includes buried cells.
//   Selection Brush is an explicit mode (the voxel brush size, [ ]); drag adds, Ctrl removes.
//   Alt subtracts from boxes when not bound to navigation.
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
#include "dve/editor_materials.hpp"

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

// Window: whole projected cells inside rect; crossing: the clipped cell silhouette touches it.
// Without through, require an exposed camera-facing face (not scene-occlusion testing).
// Stops collecting after kMaxVoxelSelection + 1 cells, so callers can refuse an oversized box.
[[nodiscard]] std::set<Int3> voxels_in_screen_rect(const EditorObject& object, const EditorCamera& camera,
                                                   UiRect viewport, UiRect rect, bool through, bool crossing = true,
                                                   const EditorDocument* document = nullptr,
                                                   const std::set<EditorObjectId>* onlyObjects = nullptr,
                                                   std::string* error = nullptr);
// Visibility is established by first-hit scene rays, including self-occlusion. Samples are
// bounded; narrow visible slivers can be missed, but occluded samples never select a cell.
inline constexpr std::size_t kMaxVoxelSelectionRays = 100'000;
[[nodiscard]] bool voxel_cell_visible(const EditorDocument& document, const EditorObject& object, Int3 cell,
    const EditorCamera& camera, UiRect viewport, const std::set<EditorObjectId>* onlyObjects,
    std::size_t& rayBudget);
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

enum class VoxelRegionEdit : std::uint8_t { Extrude, RotateQuarterTurn, ScaleInteger };
struct VoxelRegionSettings {
    int axis{};   // 0 = object-grid X, 1 = Y, 2 = Z
    int amount{1}; // Extrude: signed 1..32 layers; Rotate: +1/-1 quarter turn; Scale: 2..8
    std::optional<Int3> rotationPivot{}; // Cell-centre pivot; absent = nearest bounds-centre cell
};
struct VoxelRegionBuild {
    std::unique_ptr<IEditorCommand> command;
    std::set<Int3> selection;
    std::optional<Int3> rotationPivot;
    std::string error;
};
// Atomic edits, capped at kMaxVoxelSelection output cells. Occupied unselected destinations,
// stale selection, invalid settings and coordinate overflow are refused without edits.
// Extrude extends the directional frontier of each selected column, retaining the originals.
// Rotate uses a cell-centre pivot nearest the selected bounds centre (ties toward minimum).
// Scale expands every cell to an amount^3 block anchored at the selected minimum corner.
// Materials follow cells; selected anchors rotate or expand; extrusions create no new anchors.
[[nodiscard]] VoxelRegionBuild build_voxel_region_command(const EditorObject& object,
    const std::set<Int3>& cells, VoxelRegionEdit edit, VoxelRegionSettings settings);

inline constexpr std::uint64_t kMaxConversionWorkingCells = 2'000'000;
inline constexpr std::size_t kMaxConversionFaces = 250'000;
struct GeometryConversionSettings {
    float voxelSizeMeters{0.1F}; // Source-local units; the object's existing scale is retained
    VoxelizationMode mode{VoxelizationMode::Solid};
};
struct GeometryConversionBuild {
    std::optional<EditorObject> object;
    std::set<Int3> selectedCells; // Region preview output, unused by conversions
    std::string error;
    std::string note;
};
[[nodiscard]] GeometryConversionBuild polygon_to_voxels(const EditorObject& source,
    EditorObjectId outputId, GeometryConversionSettings settings, const std::vector<MaterialId>& paletteSlots);
[[nodiscard]] GeometryConversionBuild voxels_to_polygon(const EditorObject& source,
    EditorObjectId outputId, const EditorMaterialLibrary& materials);

// A copy of selected cells, independent of the scene.
struct VoxelClipboard {
    std::string name;
    float voxelSizeMeters{0.1F};
    Float3 voxelScale{1,1,1};
    RigidTransform transform{};
    std::optional<EditorObjectId> parent;
    EditorObjectFlags flags{};
    std::vector<SparseVoxelStateEntry> cells;
    std::map<MaterialId,VoxelMaterialDefinition> materials;
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
