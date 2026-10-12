#include "dve/editor_voxel_selection.hpp"
#include "dve/voxel_metric.hpp"

#include <algorithm>
#include <map>

#include "dve/editor_voxel_boolean.hpp"

namespace dve::editor {
namespace {

constexpr Int3 kFaces[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

Int3 add(Int3 a, Int3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

template <typename Fn>
void for_each_occupied(const VoxelObject& voxels, Fn&& fn) {
    for (const auto& [key, brick] : voxels.bricks())
        brick.occupancy().for_each_set([&](std::uint16_t index) {
            fn(global_from_local(key, local_from_index_unchecked(index)), brick.material(index));
        });
}

std::string object_problem(const EditorObject& object) {
    if (!object.voxels || object.text3d || object.gaborVolume) return "Voxel selection works on voxel objects";
    if (object.flags.locked) return "Unlock " + object.name + " first";
    return {};
}

} // namespace

std::set<Int3> voxel_connected_piece(const VoxelObject& voxels, Int3 seed, std::size_t limit) {
    std::set<Int3> piece;
    if (!voxels.occupied_at(seed)) return piece;
    std::vector<Int3> stack{seed};
    piece.insert(seed);
    while (!stack.empty() && piece.size() <= limit) {
        const Int3 at = stack.back();
        stack.pop_back();
        for (const Int3 face : kFaces) {
            const Int3 next = add(at, face);
            if (piece.contains(next) || !voxels.occupied_at(next)) continue;
            piece.insert(next);
            if (piece.size() > limit) break;
            stack.push_back(next);
        }
    }
    return piece;
}

std::set<Int3> all_occupied_voxels(const VoxelObject& voxels) {
    std::set<Int3> cells;
    for_each_occupied(voxels, [&](Int3 cell, MaterialId) { cells.insert(cell); });
    return cells;
}

std::set<Int3> grow_voxel_selection(const VoxelObject& voxels, const std::set<Int3>& selection, std::size_t limit) {
    std::set<Int3> grown = selection;
    for (const Int3 cell : selection) {
        if (grown.size() > limit) break;
        for (const Int3 face : kFaces) {
            const Int3 next = add(cell, face);
            if (voxels.occupied_at(next)) grown.insert(next);
        }
    }
    return grown;
}

std::set<Int3> shrink_voxel_selection(const std::set<Int3>& selection) {
    std::set<Int3> shrunk;
    for (const Int3 cell : selection) {
        bool interior = true;
        for (const Int3 face : kFaces) interior = interior && selection.contains(add(cell, face));
        if (interior) shrunk.insert(cell);
    }
    return shrunk;
}

std::set<Int3> invert_voxel_selection(const VoxelObject& voxels, const std::set<Int3>& selection, std::size_t limit) {
    std::set<Int3> inverted;
    for_each_occupied(voxels, [&](Int3 cell, MaterialId) {
        if (inverted.size() <= limit && !selection.contains(cell)) inverted.insert(cell);
    });
    return inverted;
}

std::set<Int3> voxels_in_screen_rect(const EditorObject& object, const EditorCamera& camera, UiRect viewport, UiRect rect,
                                     bool through) {
    std::set<Int3> cells;
    if (!object.voxels) return cells;
    const float size = object.voxelSizeMeters;
    for_each_occupied(*object.voxels, [&](Int3 cell, MaterialId) {
        const Float3 centre = voxel_to_world(object.transform,multiply(object.voxelScale,size),
            {float(cell.x)+0.5F,float(cell.y)+0.5F,float(cell.z)+0.5F});
        const ScreenPoint point = project_world_to_screen(camera, viewport, centre);
        if (!point.visible || !rect.contains(static_cast<int>(point.x), static_cast<int>(point.y))) return;
        if (!through) {
            const Float3 toCamera = subtract(camera.position, centre);
            bool facing = false;
            for (const Int3 face : kFaces) {
                if (object.voxels->occupied_at(add(cell, face))) continue;
                const Float3 normal = rotate(object.transform.rotation,
                    {static_cast<float>(face.x), static_cast<float>(face.y), static_cast<float>(face.z)});
                if (dot(normal, toCamera) > 0.0F) { facing = true; break; }
            }
            if (!facing) return;
        }
        cells.insert(cell);
    });
    return cells;
}

std::set<Int3> voxels_in_cube(const VoxelObject& voxels, Int3 center, int radius) {
    std::set<Int3> cells;
    const int r = std::max(0, radius);
    for (int x = -r; x <= r; ++x)
        for (int y = -r; y <= r; ++y)
            for (int z = -r; z <= r; ++z) {
                const Int3 cell{center.x + x, center.y + y, center.z + z};
                if (voxels.occupied_at(cell)) cells.insert(cell);
            }
    return cells;
}

std::unique_ptr<IEditorCommand> build_delete_voxels_command(const EditorObject& object, const std::set<Int3>& cells,
                                                            std::string* error) {
    const auto fail = [&](std::string message) -> std::unique_ptr<IEditorCommand> {
        if (error) *error = std::move(message);
        return nullptr;
    };
    if (std::string problem = object_problem(object); !problem.empty()) return fail(problem);
    std::vector<VoxelBooleanChange> changes;
    for (const Int3 cell : cells) {
        const MaterialId material = object.voxels->material_at(cell);
        if (material != kAirMaterial) changes.push_back({cell, material, kAirMaterial});
    }
    if (changes.empty()) return fail("No voxels selected");
    const std::vector<Int3> before(object.anchors.begin(), object.anchors.end());
    std::vector<Int3> after;
    for (const Int3 anchor : object.anchors) if (!cells.contains(anchor)) after.push_back(anchor);
    return std::make_unique<ApplyVoxelBooleanCommand>(object.id, std::move(changes), before, std::move(after),
                                                      "Delete voxels");
}

std::unique_ptr<IEditorCommand> build_move_voxels_command(const EditorObject& object, const std::set<Int3>& cells,
                                                          Int3 offset, std::string* error) {
    const auto fail = [&](std::string message) -> std::unique_ptr<IEditorCommand> {
        if (error) *error = std::move(message);
        return nullptr;
    };
    if (std::string problem = object_problem(object); !problem.empty()) return fail(problem);
    if (offset.x == 0 && offset.y == 0 && offset.z == 0) return fail("The selection did not move");
    // Final state of every cell touched: sources empty first, then destinations take the moved
    // material (overwriting what was there).
    std::map<Int3, MaterialId> final;
    std::map<Int3, MaterialId> moved;
    for (const Int3 cell : cells) {
        const MaterialId material = object.voxels->material_at(cell);
        if (material == kAirMaterial) continue;
        final[cell] = kAirMaterial;
        moved[add(cell, offset)] = material;
    }
    if (moved.empty()) return fail("No voxels selected");
    for (const auto& [cell, material] : moved) final[cell] = material;
    std::vector<VoxelBooleanChange> changes;
    for (const auto& [cell, after] : final) {
        const MaterialId before = object.voxels->material_at(cell);
        if (before != after) changes.push_back({cell, before, after});
    }
    const std::vector<Int3> anchorsBefore(object.anchors.begin(), object.anchors.end());
    std::set<Int3> anchorsAfter;
    for (const Int3 anchor : object.anchors) {
        if (cells.contains(anchor)) anchorsAfter.insert(add(anchor, offset));  // moves with its voxel
        else if (!moved.contains(anchor)) anchorsAfter.insert(anchor);       // untouched; overwritten ones go
    }
    return std::make_unique<ApplyVoxelBooleanCommand>(object.id, std::move(changes), anchorsBefore,
                                                      std::vector<Int3>(anchorsAfter.begin(), anchorsAfter.end()),
                                                      "Move voxels");
}

VoxelClipboard copy_voxels(const EditorObject& object, const std::set<Int3>& cells) {
    VoxelClipboard clipboard;
    clipboard.name = object.name;
    clipboard.voxelSizeMeters = object.voxelSizeMeters;
    clipboard.voxelScale = object.voxelScale;
    clipboard.transform = object.transform;
    clipboard.parent = object.parent;
    clipboard.flags = object.flags;
    clipboard.flags.locked = false;
    if (!object.voxels) return clipboard;
    for (const Int3 cell : cells) {
        const MaterialId material = object.voxels->material_at(cell);
        if (material == kAirMaterial) continue;
        clipboard.cells.push_back({cell, material});
        if (object.anchors.contains(cell)) clipboard.anchors.push_back(cell);
    }
    return clipboard;
}

EditorObject make_object_from_voxels(EditorObjectId id, const VoxelClipboard& clipboard, std::string name) {
    EditorObject object(id, std::move(name));
    object.voxelSizeMeters = clipboard.voxelSizeMeters;
    object.voxelScale = clipboard.voxelScale;
    object.transform = clipboard.transform;
    object.parent = clipboard.parent;
    object.flags = clipboard.flags;
    for (const SparseVoxelStateEntry& cell : clipboard.cells) (void)object.voxels->set_voxel(cell.voxel, cell.material);
    object.anchors.insert(clipboard.anchors.begin(), clipboard.anchors.end());
    return object;
}

ExtractVoxelsBuild build_extract_voxels_command(EditorDocument& document, EditorObjectId id, const std::set<Int3>& cells) {
    ExtractVoxelsBuild build;
    const EditorObject* object = document.find_object(id);
    if (!object) { build.error = "The object no longer exists"; return build; }
    if (std::string problem = object_problem(*object); !problem.empty()) { build.error = problem; return build; }
    // Same rules as Separate Islands and Slice's Separate (#97, #100): a prefab instance's part
    // would silently diverge from the prefab, and parts of an attached object would all hang off
    // the same socket.
    if (object->prefabLink) { build.error = object->name + " is a prefab instance; unpack it first"; return build; }
    if (object->attachment) { build.error = object->name + " is attached to a socket; detach it first"; return build; }
    const VoxelClipboard clipboard = copy_voxels(*object, cells);
    if (clipboard.cells.empty()) { build.error = "No voxels selected"; return build; }
    if (clipboard.cells.size() == object->voxels->occupied_voxel_count()) {
        build.error = "Every voxel is selected; that is the whole object (use Duplicate or Separate Islands)";
        return build;
    }
    std::string error;
    auto remove = build_delete_voxels_command(*object, cells, &error);
    if (!remove) { build.error = error; return build; }
    build.newObject = document.allocate_object_id();
    EditorObject part = make_object_from_voxels(build.newObject, clipboard, object->name + " (extract)");
    // Like Separate Islands' pieces: keep flags, tags, groups, layer and membership; geometry no
    // longer matches an import source or prefab, so those (and other components) stay behind.
    part.flags = object->flags;
    part.tags = object->tags;
    part.groups = object->groups;
    part.layer = object->layer;
    for (const Component& component : object->components)
        if (component.type == "dve.membership" || component.type == "dve.tags") part.components.push_back(component);
    build.command = std::make_unique<CompoundCommand>("Extract voxels");
    build.command->add(std::move(remove));
    build.command->add(std::make_unique<AddObjectCommand>(std::move(part), "Extracted voxels"));
    return build;
}

} // namespace dve::editor
