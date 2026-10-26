#include "dve/region_streaming.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace dve {
namespace {

std::int32_t clamp_coordinate(std::int64_t value) noexcept {
    return static_cast<std::int32_t>(std::clamp(value,
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
}

BrickKey corner(RegionKey camera, std::int32_t radius, bool high) noexcept {
    const auto delta = high ? static_cast<std::int64_t>(radius) : -static_cast<std::int64_t>(radius);
    return {clamp_coordinate(static_cast<std::int64_t>(camera.x) + delta),
            clamp_coordinate(static_cast<std::int64_t>(camera.y) + delta),
            clamp_coordinate(static_cast<std::int64_t>(camera.z) + delta)};
}

std::optional<std::unordered_set<std::uint64_t>> members_near(
    const RegionCatalog& catalog, RegionKey camera, std::int32_t radius, std::size_t maximumExamined) {
    const auto found = catalog.query(camera.worldId, camera.lod,
                                      corner(camera, radius, false), corner(camera, radius, true), maximumExamined);
    if (!found) return std::nullopt;
    std::unordered_set<std::uint64_t> result;
    for (const auto& region : *found)
        for (const auto id : region.objectIds) result.insert(id);
    return result;
}

std::uint64_t distance(RegionKey from, RegionKey to) noexcept {
    const auto d = [](std::int32_t x, std::int32_t y) {
        const auto a = static_cast<std::int64_t>(x) - static_cast<std::int64_t>(y);
        return static_cast<std::uint64_t>(a < 0 ? -a : a);
    };
    return std::max({d(from.x,to.x), d(from.y,to.y), d(from.z,to.z)});
}

} // namespace

bool RegionStreamingPlanner::valid() const noexcept {
    return config_.loadRadius >= 0 && config_.retainRadius >= config_.loadRadius &&
           config_.maximumExamined > 0;
}

std::optional<RegionKey> RegionStreamingPlanner::camera_region(
    const RegionCatalog& catalog, std::uint32_t worldId, std::uint8_t lod,
    std::array<double, 3> worldMeters, double metresPerVoxel) noexcept {
    if (!std::isfinite(metresPerVoxel) || metresPerVoxel <= 0.0) return std::nullopt;
    std::array<std::int32_t, 3> coordinates{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(worldMeters[i])) return std::nullopt;
        const double brick = std::floor(worldMeters[i] / metresPerVoxel / static_cast<double>(kBrickDim));
        if (!std::isfinite(brick) || brick < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
            brick > static_cast<double>(std::numeric_limits<std::int32_t>::max())) return std::nullopt;
        coordinates[i] = static_cast<std::int32_t>(brick);
    }
    return catalog.region_of_brick(worldId, lod, {coordinates[0], coordinates[1], coordinates[2]});
}

std::optional<RegionStreamingPlan> RegionStreamingPlanner::plan(
    const RegionCatalog& catalog, RegionKey camera, std::span<const RegionStreamingObject> objects,
    std::uint64_t tick) const {
    if (!valid()) return std::nullopt;
    auto near = members_near(catalog, camera, config_.loadRadius, config_.maximumExamined);
    auto retained = members_near(catalog, camera, config_.retainRadius, config_.maximumExamined);
    if (!near || !retained) return std::nullopt;
    RegionStreamingPlan result;
    result.nearIds.assign(near->begin(), near->end());
    result.retainIds.assign(retained->begin(), retained->end());
    std::sort(result.nearIds.begin(), result.nearIds.end());
    std::sort(result.retainIds.begin(), result.retainIds.end());

    // The catalog may describe other scenes. Only the passed runtime reservations
    // are actionable, and objects without catalog membership must be left alone.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> candidates;
    for (const auto& object : objects) {
        if (object.objectId == 0) continue;
        const auto memberships = catalog.object_regions(object.objectId);
        const bool inWorld = std::any_of(memberships.begin(), memberships.end(), [&](RegionKey key) {
            return key.worldId == camera.worldId && key.lod == camera.lod;
        });
        if (!inWorld) continue; // never evict an unrelated world/LOD's object
        if (!object.loaded && !object.inFlight && !object.retryBackoff && near->contains(object.objectId)) {
            std::uint64_t nearest = std::numeric_limits<std::uint64_t>::max();
            for (const auto key : memberships) {
                if (key.worldId == camera.worldId && key.lod == camera.lod)
                    nearest = std::min(nearest, distance(camera, key));
            }
            candidates.emplace_back(nearest, object.objectId);
        } else if (object.loaded && !object.pinned && object.mayUnload &&
                   !retained->contains(object.objectId) && tick >= object.loadedSinceTick &&
                   tick - object.loadedSinceTick >= config_.minimumResidentTicks) {
            result.unloadIds.push_back(object.objectId);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    std::sort(result.unloadIds.begin(), result.unloadIds.end());
    for (std::size_t i = 0; i < std::min(config_.loadsPerTick, candidates.size()); ++i)
        result.loadIds.push_back(candidates[i].second);
    if (result.unloadIds.size() > config_.unloadsPerTick) result.unloadIds.resize(config_.unloadsPerTick);
    return result;
}
} // namespace dve

