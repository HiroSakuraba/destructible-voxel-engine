#include "dve/region_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <climits>
#include <iterator>
#include <stdexcept>

namespace dve {
namespace {

[[nodiscard]] constexpr std::int32_t floor_region(std::int32_t value, std::int32_t width) noexcept {
    const std::int32_t q = value / width;
    return q - (value % width < 0 ? 1 : 0); // width > 0, including INT32_MIN
}

[[nodiscard]] bool valid_bounds(BrickKey low, BrickKey high) noexcept {
    return low.x <= high.x && low.y <= high.y && low.z <= high.z;
}

[[nodiscard]] std::uint64_t mix(std::uint64_t x) noexcept {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

} // namespace

std::size_t RegionCatalog::RegionKeyHash::operator()(RegionKey key) const noexcept {
    std::uint64_t h = mix((static_cast<std::uint64_t>(key.worldId) << 8) | key.lod);
    for (std::int32_t c : {key.x, key.y, key.z}) {
        h = mix(h ^ static_cast<std::uint32_t>(c));
    }
    return static_cast<std::size_t>(h);
}

RegionCatalog::RegionCatalog(RegionCatalogConfig config) : config_(config) {
    if (config.bricksPerRegionAxis <= 0 || config.maximumRegionsPerObject == 0)
        throw std::invalid_argument("invalid region index configuration");
}

std::size_t RegionCatalog::region_count() const noexcept {
    return config_.backend == RegionIndexBackend::OrderedMap ? treeIndex_.size() : hashIndex_.size();
}

RegionKey RegionCatalog::region_of_brick(std::uint32_t world, std::uint8_t lod, BrickKey key) const noexcept {
    const auto n = config_.bricksPerRegionAxis;
    return {world, lod, floor_region(key.x, n), floor_region(key.y, n), floor_region(key.z, n)};
}

std::optional<RegionBrickBounds> RegionCatalog::transformed_brick_bounds(
    RegionWorldAabb local, RegionAffine3x4 transform, double metresPerVoxel) noexcept {
    if (!std::isfinite(metresPerVoxel) || metresPerVoxel <= 0) return std::nullopt;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(local.minimum[axis]) || !std::isfinite(local.maximum[axis]) ||
            local.minimum[axis] >= local.maximum[axis]) return std::nullopt;
    }
    for (double v : transform.m) if (!std::isfinite(v)) return std::nullopt;
    std::array<double, 3> low{INFINITY, INFINITY, INFINITY};
    std::array<double, 3> high{-INFINITY, -INFINITY, -INFINITY};
    // The local box is half-open: [minimum, maximum). A transformed world axis is the
    // affine function sum(c_j * local_j) + t. Its supremum over the half-open box is
    // *attained* (so the world upper endpoint is inclusive and may lie in the next
    // brick) only when every non-zero coefficient is negative, because then it is
    // reached at local minimums. Any positive coefficient needs a local maximum, which
    // is excluded, so the upper endpoint stays exclusive.
    std::array<bool, 3> highInclusive{false, false, false};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto row = axis * 4;
        bool anyNegative = false, anyPositive = false;
        for (std::size_t j = 0; j < 3; ++j) {
            const double c = transform.m[row + j];
            if (c < 0) anyNegative = true;
            else if (c > 0) anyPositive = true;
        }
        highInclusive[axis] = anyNegative && !anyPositive;
    }
    for (int corner = 0; corner != 8; ++corner) {
        std::array<double, 3> p{};
        for (int axis = 0; axis != 3; ++axis)
            p[static_cast<std::size_t>(axis)] = (corner & (1 << axis)) ?
                local.maximum[static_cast<std::size_t>(axis)] : local.minimum[static_cast<std::size_t>(axis)];
        for (int axis = 0; axis != 3; ++axis) {
            const auto row = static_cast<std::size_t>(axis) * 4;
            const double v = transform.m[row] * p[0] + transform.m[row + 1] * p[1] +
                             transform.m[row + 2] * p[2] + transform.m[row + 3];
            if (!std::isfinite(v)) return std::nullopt;
            low[static_cast<std::size_t>(axis)] = std::min(low[static_cast<std::size_t>(axis)], v);
            high[static_cast<std::size_t>(axis)] = std::max(high[static_cast<std::size_t>(axis)], v);
        }
    }
    const double brickMetres = metresPerVoxel * static_cast<double>(kBrickDim);
    if (!std::isfinite(brickMetres) || brickMetres <= 0) return std::nullopt;
    std::array<std::int32_t, 3> from{}, to{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (low[axis] >= high[axis]) return std::nullopt;
        const double a = std::floor(low[axis] / brickMetres);
        // Exclusive endpoints stop in the brick before the boundary; inclusive ones
        // (reflected axes) belong to the brick that starts at the boundary.
        const double b = highInclusive[axis] ? std::floor(high[axis] / brickMetres)
                                             : std::ceil(high[axis] / brickMetres) - 1.0;
        if (!std::isfinite(a) || !std::isfinite(b) ||
            a < static_cast<double>(INT32_MIN) || b > static_cast<double>(INT32_MAX) || a > b)
            return std::nullopt;
        from[axis] = static_cast<std::int32_t>(a);
        to[axis] = static_cast<std::int32_t>(b);
    }
    return RegionBrickBounds{{from[0], from[1], from[2]}, {to[0], to[1], to[2]}};
}

std::optional<std::vector<RegionKey>> RegionCatalog::keys_for_brick_bounds(
    std::uint32_t world, std::uint8_t lod, RegionBrickBounds bounds) const {
    if (!valid_bounds(bounds.minimum, bounds.maximum)) return std::nullopt;
    const auto lo = region_of_brick(world, lod, bounds.minimum);
    const auto hi = region_of_brick(world, lod, bounds.maximum);
    const std::array<std::uint64_t, 3> extent{
        static_cast<std::uint64_t>(static_cast<std::int64_t>(hi.x) - lo.x + 1),
        static_cast<std::uint64_t>(static_cast<std::int64_t>(hi.y) - lo.y + 1),
        static_cast<std::uint64_t>(static_cast<std::int64_t>(hi.z) - lo.z + 1)};
    std::uint64_t count = 1;
    for (auto n : extent) {
        if (n > config_.maximumRegionsPerObject / count) return std::nullopt;
        count *= n;
    }
    std::vector<RegionKey> keys;
    keys.reserve(static_cast<std::size_t>(count));
    for (std::int64_t x = lo.x; x <= hi.x; ++x)
        for (std::int64_t y = lo.y; y <= hi.y; ++y)
            for (std::int64_t z = lo.z; z <= hi.z; ++z)
                keys.push_back({world, lod, static_cast<std::int32_t>(x),
                                static_cast<std::int32_t>(y), static_cast<std::int32_t>(z)});
    return keys;
}

std::vector<std::uint64_t>* RegionCatalog::mutable_members(RegionKey key) {
    if (config_.backend == RegionIndexBackend::OrderedMap) return &treeIndex_[key];
    return &hashIndex_[key];
}

void RegionCatalog::add_member(RegionKey key, std::uint64_t objectId) {
    auto& members = *mutable_members(key);
    const auto pos = std::lower_bound(members.begin(), members.end(), objectId);
    if (pos == members.end() || *pos != objectId) members.insert(pos, objectId);
}

void RegionCatalog::remove_member(RegionKey key, std::uint64_t objectId) {
    auto remove = [objectId](auto& index, RegionKey k) {
        const auto it = index.find(k);
        if (it == index.end()) return;
        auto& members = it->second;
        const auto m = std::lower_bound(members.begin(), members.end(), objectId);
        if (m != members.end() && *m == objectId) members.erase(m);
        if (members.empty()) index.erase(it);
    };
    if (config_.backend == RegionIndexBackend::OrderedMap) remove(treeIndex_, key);
    else remove(hashIndex_, key);
}

bool RegionCatalog::set_object_regions(std::uint64_t objectId, std::span<const RegionKey> keys) {
    if (objectId == 0 || keys.size() > config_.maximumRegionsPerObject) return false;
    std::vector<RegionKey> next(keys.begin(), keys.end());
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    const auto previous = objectRegions_.find(objectId);
    if (previous == objectRegions_.end() && next.empty()) return true;
    const auto old = previous == objectRegions_.end() ? std::vector<RegionKey>{} : previous->second;
    if (next == old) return true;
    std::vector<RegionKey> added, removed;
    std::set_difference(next.begin(), next.end(), old.begin(), old.end(), std::back_inserter(added));
    std::set_difference(old.begin(), old.end(), next.begin(), next.end(), std::back_inserter(removed));
    // Reserve reverse-map ownership before changing the forward index. This
    // makes all remaining reverse updates noexcept (vector swap).
    const bool firstMembership = previous == objectRegions_.end();
    std::vector<RegionKey> applied;
    applied.reserve(added.size());
    if (firstMembership) objectRegions_.try_emplace(objectId);
    try {
        for (RegionKey k : added) {
            try {
                add_member(k, objectId);
            } catch (...) {
                // A failed allocation could have inserted an empty bucket.
                remove_member(k, objectId);
                throw;
            }
            applied.push_back(k);
        }
    } catch (...) {
        for (RegionKey k : applied) remove_member(k, objectId);
        if (firstMembership) objectRegions_.erase(objectId);
        throw;
    }
    for (RegionKey k : removed) remove_member(k, objectId);
    if (next.empty()) objectRegions_.erase(objectId);
    else objectRegions_.find(objectId)->second.swap(next);
    ++revision_;
    return true;
}

bool RegionCatalog::set_object_bounds(std::uint64_t objectId, std::uint32_t world, std::uint8_t lod,
                                      RegionWorldAabb local, RegionAffine3x4 transform, double metresPerVoxel) {
    const auto bounds = transformed_brick_bounds(local, transform, metresPerVoxel);
    if (!bounds) return false;
    const auto keys = keys_for_brick_bounds(world, lod, *bounds);
    return keys && set_object_regions(objectId, *keys);
}

bool RegionCatalog::erase_object(std::uint64_t objectId) {
    if (!objectRegions_.contains(objectId)) return false;
    return set_object_regions(objectId, {});
}

std::vector<RegionKey> RegionCatalog::object_regions(std::uint64_t objectId) const {
    const auto it = objectRegions_.find(objectId);
    return it == objectRegions_.end() ? std::vector<RegionKey>{} : it->second;
}

std::vector<std::uint64_t> RegionCatalog::region_objects(RegionKey key) const {
    if (config_.backend == RegionIndexBackend::OrderedMap) {
        const auto it = treeIndex_.find(key);
        return it == treeIndex_.end() ? std::vector<std::uint64_t>{} : it->second;
    }
    const auto it = hashIndex_.find(key);
    return it == hashIndex_.end() ? std::vector<std::uint64_t>{} : it->second;
}

void RegionCatalog::refresh_cache() const {
    if (cachedRevision_ == revision_) return;
    sortedCache_.clear();
    sortedCache_.reserve(hashIndex_.size());
    for (const auto& [key, ids] : hashIndex_) sortedCache_.push_back({key, ids});
    std::sort(sortedCache_.begin(), sortedCache_.end(), [](const RegionEntry& a, const RegionEntry& b) {
        return a.key < b.key;
    });
    cachedRevision_ = revision_;
}

std::vector<RegionEntry> RegionCatalog::entries() const {
    if (config_.backend == RegionIndexBackend::OrderedMap) {
        std::vector<RegionEntry> out;
        out.reserve(treeIndex_.size());
        for (const auto& [key, ids] : treeIndex_) out.push_back({key, ids});
        return out;
    }
    refresh_cache();
    return sortedCache_;
}

std::optional<std::vector<RegionEntry>> RegionCatalog::query(
    std::uint32_t world, std::uint8_t lod, BrickKey low, BrickKey high, std::size_t maximumExamined) const {
    if (!valid_bounds(low, high)) return std::nullopt;
    const RegionKey begin{world, lod, low.x, INT32_MIN, INT32_MIN};
    const RegionKey end{world, lod, high.x, INT32_MAX, INT32_MAX};
    std::vector<RegionEntry> out;
    std::size_t examined = 0;
    auto visit = [&](RegionKey key, const auto& ids) {
        if (++examined > maximumExamined) return false;
        if (key.y >= low.y && key.y <= high.y && key.z >= low.z && key.z <= high.z)
            out.push_back({key, ids});
        return true;
    };
    if (config_.backend == RegionIndexBackend::OrderedMap) {
        for (auto it = treeIndex_.lower_bound(begin); it != treeIndex_.end() && it->first <= end; ++it)
            if (!visit(it->first, it->second)) return std::nullopt;
    } else {
        refresh_cache();
        auto it = std::lower_bound(sortedCache_.begin(), sortedCache_.end(), begin,
                                   [](const RegionEntry& entry, RegionKey key) { return entry.key < key; });
        for (; it != sortedCache_.end() && it->key <= end; ++it)
            if (!visit(it->key, it->objectIds)) return std::nullopt;
    }
    return out;
}

} // namespace dve