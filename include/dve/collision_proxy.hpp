#pragma once

#include <vector>

#include "dve/voxel_object.hpp"

namespace dve {

struct VoxelBox {
    Int3 min{};          // inclusive voxel coordinate
    Int3 maxExclusive{}; // exclusive voxel coordinate
    auto operator<=>(const VoxelBox&) const = default;
};

// Deterministic bounded baseline for fragment collision. Each 8^3 brick is decomposed
// independently, so work is capped and no box crosses a brick boundary in v1.0.
[[nodiscard]] std::vector<VoxelBox> build_brick_box_proxy(const VoxelObject& object, BrickKey key);
[[nodiscard]] std::vector<VoxelBox> build_object_box_proxy(const VoxelObject& object);
[[nodiscard]] bool validate_box_proxy(const VoxelObject& object, const std::vector<VoxelBox>& boxes);

} // namespace dve
