#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "dve/job_system.hpp"
#include "dve/surface.hpp"

namespace dve {

constexpr std::size_t kMinimumBricksPerDerivedWorker = 2048;

[[nodiscard]] constexpr bool should_parallelize_derived_bricks(
    std::size_t brickCount, std::size_t workerCount) noexcept {
    return workerCount > 0 && brickCount >= (workerCount + 1) * kMinimumBricksPerDerivedWorker;
}

struct BrickFaceMaskResult {
    BrickKey key{};
    std::uint32_t generation{};
    BrickFaceMasks masks{};
};

struct BrickSurfaceResult {
    BrickKey key{};
    std::uint32_t generation{};
    std::vector<SurfaceQuad> quads{};
};

[[nodiscard]] std::vector<BrickFaceMaskResult> extract_face_masks_parallel(
    const VoxelObject& object,
    std::span<const BrickKey> keys,
    JobSystem& jobs);

// Sorts keys before dispatch. Workers write disjoint output slots; publication may therefore
// consume results in key order, independent of completion order.
[[nodiscard]] std::vector<BrickSurfaceResult> extract_surfaces_parallel(
    const VoxelObject& object,
    std::span<const BrickKey> keys,
    JobSystem& jobs);

} // namespace dve
