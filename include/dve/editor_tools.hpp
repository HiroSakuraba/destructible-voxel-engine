#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "dve/editor_command.hpp"

namespace dve::editor {

enum class VoxelToolOperation : std::uint8_t { Add, Remove, Paint };
enum class BrushShape : std::uint8_t { Cube, Sphere };

struct BrushSettings {
    BrushShape shape{BrushShape::Sphere};
    VoxelToolOperation operation{VoxelToolOperation::Add};
    std::int32_t radius{1};
    MaterialId material{1};
    std::uint64_t strokeId{};
};

[[nodiscard]] std::vector<Int3> cube_brush_voxels(Int3 center, std::int32_t radius);
[[nodiscard]] std::vector<Int3> sphere_brush_voxels(Int3 center, std::int32_t radius);
[[nodiscard]] std::vector<Int3> box_voxels(Int3 minimum, Int3 maximum, bool hollow = false);
[[nodiscard]] std::vector<Int3> line_voxels(Int3 start, Int3 end, std::int32_t thickness = 1);

[[nodiscard]] std::unique_ptr<IEditorCommand> make_brush_command(
    const EditorDocument& document, EditorObjectId objectId, Int3 center,
    const BrushSettings& settings);
[[nodiscard]] std::unique_ptr<IEditorCommand> make_box_command(
    const EditorDocument& document, EditorObjectId objectId, Int3 minimum, Int3 maximum,
    VoxelToolOperation operation, MaterialId material, bool hollow = false);
[[nodiscard]] std::unique_ptr<IEditorCommand> make_line_command(
    const EditorDocument& document, EditorObjectId objectId, Int3 start, Int3 end,
    VoxelToolOperation operation, MaterialId material, std::int32_t thickness = 1);
[[nodiscard]] std::unique_ptr<IEditorCommand> make_anchor_brush_command(
    const EditorDocument& document, EditorObjectId objectId, Int3 center,
    std::int32_t radius, bool add);
[[nodiscard]] std::unique_ptr<IEditorCommand> make_rescale_voxel_object_command(
    const EditorDocument& document, EditorObjectId objectId, Float3 scale);

} // namespace dve::editor
