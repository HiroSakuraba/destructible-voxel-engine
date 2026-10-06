#include "dve/collision_proxy.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace dve {
namespace {

void append_brick_box_proxy(const Brick& brick, BrickKey key, std::vector<VoxelBox>& boxes) {
    Bitset512 remaining = brick.occupancy();
    while (remaining.any()) {
        std::uint16_t seed = 0;
        for (std::size_t word = 0; word < remaining.words.size(); ++word) {
            if (remaining.words[word] == 0) continue;
            seed = static_cast<std::uint16_t>(word * 64U + std::countr_zero(remaining.words[word]));
            break;
        }
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
}

} // namespace

std::vector<VoxelBox> build_brick_box_proxy(const VoxelObject& object, BrickKey key) {
    std::vector<VoxelBox> boxes;
    if (const Brick* brick = object.find_brick(key); brick != nullptr && !brick->empty()) {
        append_brick_box_proxy(*brick, key, boxes);
    }
    return boxes;
}

std::vector<VoxelBox> build_object_box_proxy(const VoxelObject& object) {
    std::vector<VoxelBox> result;
    for (const auto& [key, brick] : object.bricks()) {
        if (brick.empty()) continue;
        append_brick_box_proxy(brick, key, result);
    }
    return result;
}

bool validate_box_proxy(const VoxelObject& object, const std::vector<VoxelBox>& boxes) {
    const auto& bricks = object.bricks();
    std::vector<Bitset512> coverage(bricks.size());
    for (const VoxelBox& box : boxes) {
        if (box.min.x >= box.maxExclusive.x || box.min.y >= box.maxExclusive.y || box.min.z >= box.maxExclusive.z) {
            return false;
        }
        const BrickKey first = brick_key_from_voxel(box.min);
        const BrickKey last = brick_key_from_voxel({box.maxExclusive.x - 1,
            box.maxExclusive.y - 1, box.maxExclusive.z - 1});
        for (std::int32_t z = first.z; z <= last.z; ++z) {
            for (std::int32_t y = first.y; y <= last.y; ++y) {
                for (std::int32_t x = first.x; x <= last.x; ++x) {
                    const auto brick = bricks.find({x, y, z});
                    if (brick == bricks.end()) return false;
                    auto& covered = coverage[static_cast<std::size_t>(brick - bricks.begin())];
                    const auto occupied = brick->second.occupancy();
                    // Wide origins keep clipping valid at the ends of the voxel coordinate range.
                    const std::int64_t ox = static_cast<std::int64_t>(x) * kBrickDim;
                    const std::int64_t oy = static_cast<std::int64_t>(y) * kBrickDim;
                    const std::int64_t oz = static_cast<std::int64_t>(z) * kBrickDim;
                    const auto low = [](std::int32_t value, std::int64_t origin) {
                        return static_cast<unsigned>(std::max<std::int64_t>(0, value - origin));
                    };
                    const auto high = [](std::int32_t value, std::int64_t origin) {
                        return static_cast<unsigned>(std::min<std::int64_t>(kBrickDim, value - origin));
                    };
                    const unsigned minX = low(box.min.x, ox), maxX = high(box.maxExclusive.x, ox);
                    const std::uint64_t row = ((std::uint64_t{1} << (maxX - minX)) - 1U) << minX;
                    for (unsigned localZ = low(box.min.z, oz); localZ < high(box.maxExclusive.z, oz); ++localZ) {
                        for (unsigned localY = low(box.min.y, oy); localY < high(box.maxExclusive.y, oy); ++localY) {
                            const std::uint64_t mask = row << (localY * kBrickDim);
                            if ((occupied.words[localZ] & mask) != mask || (covered.words[localZ] & mask) != 0) return false;
                            covered.words[localZ] |= mask;
                        }
                    }
                }
            }
        }
    }
    for (std::size_t i = 0; i < bricks.size(); ++i) {
        if (coverage[i] != bricks.begin()[i].second.occupancy()) return false;
    }
    return true;
}

} // namespace dve
