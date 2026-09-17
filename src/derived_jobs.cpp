#include "dve/derived_jobs.hpp"

#include <algorithm>

namespace dve {


std::vector<BrickFaceMaskResult> extract_face_masks_parallel(
    const VoxelObject& object,
    std::span<const BrickKey> keys,
    JobSystem& jobs) {
    std::vector<BrickKey> orderedKeys(keys.begin(), keys.end());
    std::sort(orderedKeys.begin(), orderedKeys.end());
    orderedKeys.erase(std::unique(orderedKeys.begin(), orderedKeys.end()), orderedKeys.end());
    std::vector<BrickFaceMaskResult> results(orderedKeys.size());
    if (orderedKeys.empty()) return results;

    const std::size_t participantCount = should_parallelize_derived_bricks(orderedKeys.size(), jobs.worker_count())
        ? jobs.worker_count() + 1
        : 1;
    const std::size_t chunkCount = std::min(participantCount, orderedKeys.size());
    const std::size_t chunkSize = (orderedKeys.size() + chunkCount - 1) / chunkCount;
    jobs.parallel_for(chunkCount, [&](std::size_t chunk) {
        const std::size_t begin = chunk * chunkSize;
        const std::size_t end = std::min(begin + chunkSize, orderedKeys.size());
        for (std::size_t index = begin; index < end; ++index) {
            const BrickKey key = orderedKeys[index];
            const Brick* brick = object.find_brick(key);
            results[index] = {key, brick == nullptr ? 0U : brick->generation(), extract_brick_face_masks(object, key)};
        }
    });
    return results;
}

std::vector<BrickSurfaceResult> extract_surfaces_parallel(
    const VoxelObject& object,
    std::span<const BrickKey> keys,
    JobSystem& jobs) {
    std::vector<BrickKey> orderedKeys(keys.begin(), keys.end());
    std::sort(orderedKeys.begin(), orderedKeys.end());
    orderedKeys.erase(std::unique(orderedKeys.begin(), orderedKeys.end()), orderedKeys.end());

    std::vector<BrickSurfaceResult> results(orderedKeys.size());
    if (orderedKeys.empty()) return results;

    // Brick extraction is intentionally tiny. Dispatch coarse contiguous ranges rather than one
    // scheduler item per brick; this avoids atomics and condition-variable overhead dominating.
    const std::size_t participantCount = should_parallelize_derived_bricks(orderedKeys.size(), jobs.worker_count())
        ? jobs.worker_count() + 1
        : 1;
    const std::size_t chunkCount = std::min(participantCount, orderedKeys.size());
    const std::size_t chunkSize = (orderedKeys.size() + chunkCount - 1) / chunkCount;
    jobs.parallel_for(chunkCount, [&](std::size_t chunk) {
        const std::size_t begin = chunk * chunkSize;
        const std::size_t end = std::min(begin + chunkSize, orderedKeys.size());
        for (std::size_t index = begin; index < end; ++index) {
            const BrickKey key = orderedKeys[index];
            const Brick* brick = object.find_brick(key);
            const std::uint32_t generation = brick == nullptr ? 0U : brick->generation();
            results[index] = {key, generation, extract_brick_surface_bitwise(object, key)};
        }
    });
    return results;
}

} // namespace dve
