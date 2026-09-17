#include "dve/collision_proxy.hpp"

#include <algorithm>
#include <cstdint>
#include <map>

namespace dve {

std::vector<VoxelBox> build_brick_box_proxy(const VoxelObject& object, BrickKey key) {
    const Brick* brick = object.find_brick(key);
    if (brick == nullptr || brick->empty()) return {};

    Bitset512 remaining = brick->occupancy();
    std::vector<VoxelBox> boxes;
    while (remaining.any()) {
        std::uint16_t seed = 0;
        bool found = false;
        remaining.for_each_set([&](std::uint16_t index) {
            if (!found) {
                seed = index;
                found = true;
            }
        });
        const Int3 start = local_from_index_unchecked(seed);

        std::int32_t endX = start.x + 1;
        while (endX < kBrickDim && remaining.test(voxel_index_unchecked({endX, start.y, start.z}))) ++endX;

        std::int32_t endY = start.y + 1;
        while (endY < kBrickDim) {
            bool fullRow = true;
            for (std::int32_t x = start.x; x < endX; ++x) {
                if (!remaining.test(voxel_index_unchecked({x, endY, start.z}))) {
                    fullRow = false;
                    break;
                }
            }
            if (!fullRow) break;
            ++endY;
        }

        std::int32_t endZ = start.z + 1;
        while (endZ < kBrickDim) {
            bool fullPlane = true;
            for (std::int32_t y = start.y; y < endY && fullPlane; ++y) {
                for (std::int32_t x = start.x; x < endX; ++x) {
                    if (!remaining.test(voxel_index_unchecked({x, y, endZ}))) {
                        fullPlane = false;
                        break;
                    }
                }
            }
            if (!fullPlane) break;
            ++endZ;
        }

        for (std::int32_t z = start.z; z < endZ; ++z) {
            for (std::int32_t y = start.y; y < endY; ++y) {
                for (std::int32_t x = start.x; x < endX; ++x) {
                    remaining.reset(voxel_index_unchecked({x, y, z}));
                }
            }
        }

        const Int3 origin = brick_origin(key);
        boxes.push_back({
            {origin.x + start.x, origin.y + start.y, origin.z + start.z},
            {origin.x + endX, origin.y + endY, origin.z + endZ},
        });
    }
    return boxes;
}

std::vector<VoxelBox> build_object_box_proxy(const VoxelObject& object) {
    std::vector<VoxelBox> result;
    for (const auto& [key, brick] : object.bricks()) {
        if (brick.empty()) continue;
        auto boxes = build_brick_box_proxy(object, key);
        result.insert(result.end(), boxes.begin(), boxes.end());
    }
    return result;
}

bool validate_box_proxy(const VoxelObject& object, const std::vector<VoxelBox>& boxes) {
    std::map<Int3, std::uint16_t> coverage;
    for (const VoxelBox& box : boxes) {
        if (box.min.x >= box.maxExclusive.x || box.min.y >= box.maxExclusive.y || box.min.z >= box.maxExclusive.z) {
            return false;
        }
        for (std::int32_t z = box.min.z; z < box.maxExclusive.z; ++z) {
            for (std::int32_t y = box.min.y; y < box.maxExclusive.y; ++y) {
                for (std::int32_t x = box.min.x; x < box.maxExclusive.x; ++x) {
                    Int3 voxel{x, y, z};
                    if (!object.occupied_at(voxel)) return false;
                    if (++coverage[voxel] != 1) return false;
                }
            }
        }
    }
    for (const auto& [key, brick] : object.bricks()) {
        const Bitset512 occupied = brick.occupancy();
        bool valid = true;
        occupied.for_each_set([&](std::uint16_t index) {
            if (coverage[global_from_local(key, local_from_index_unchecked(index))] != 1) valid = false;
        });
        if (!valid) return false;
    }
    return true;
}

} // namespace dve
