#pragma once

// Experimental spatial discovery index. Not a voxel store or a streaming scheduler.
// Mutation is authority-thread-only; callers may hand immutable copies of entries to workers.
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "dve/types.hpp"

namespace dve {

struct RegionKey {
    std::uint32_t worldId{};
    std::uint8_t lod{};
    std::int32_t x{}, y{}, z{};
    auto operator<=>(const RegionKey&) const = default;
};

struct RegionBrickBounds {
    BrickKey minimum{};       // inclusive
    BrickKey maximum{};       // inclusive
};

struct RegionWorldAabb {
    std::array<double, 3> minimum{};
    std::array<double, 3> maximum{}; // exclusive
};

// Row-major local-to-world affine 3x4 matrix, including translation and scale.
struct RegionAffine3x4 {
    std::array<double, 12> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
};

struct RegionEntry {
    RegionKey key{};
    std::vector<std::uint64_t> objectIds; // ascending, unique, stable IDs (never pointers)
    bool operator==(const RegionEntry&) const = default;
};

enum class RegionIndexBackend : std::uint8_t { HashSorted, OrderedMap };

struct RegionCatalogConfig {
    std::int32_t bricksPerRegionAxis{16};
    RegionIndexBackend backend{RegionIndexBackend::HashSorted};
    std::size_t maximumRegionsPerObject{65536};
};

class RegionCatalog {
public:
    explicit RegionCatalog(RegionCatalogConfig config = {});
    [[nodiscard]] const RegionCatalogConfig& config() const noexcept { return config_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] std::size_t region_count() const noexcept;
    [[nodiscard]] std::size_t object_count() const noexcept { return objectRegions_.size(); }

    // Signed floor division is essential: brick -1 with region width 16 is region -1.
    [[nodiscard]] RegionKey region_of_brick(std::uint32_t world, std::uint8_t lod, BrickKey key) const noexcept;
    // Conservative transformed world AABB converted into inclusive brick coordinates.
    // Reflected axes (all non-zero coefficients negative) keep their included upper endpoint,
    // so a boundary exactly on a brick edge belongs to the brick that starts there.
    // Reject NaN, infinity, zero-volume or unrepresentable coordinates.
    [[nodiscard]] static std::optional<RegionBrickBounds> transformed_brick_bounds(
        RegionWorldAabb local, RegionAffine3x4 transform, double metresPerVoxel) noexcept;
    // Reject oversized boxes before enumeration, including int32 extreme endpoints.
    [[nodiscard]] std::optional<std::vector<RegionKey>> keys_for_brick_bounds(
        std::uint32_t world, std::uint8_t lod, RegionBrickBounds bounds) const;

    // Replace an object's membership atomically at the logical API level. The caller
    // serializes writers; allocations that fail during additions are rolled back.
    // Returns false for bad ID, limits or invalid input; no-op writes keep revision.
    bool set_object_regions(std::uint64_t objectId, std::span<const RegionKey> keys);
    bool set_object_bounds(std::uint64_t objectId, std::uint32_t world, std::uint8_t lod,
                           RegionWorldAabb local, RegionAffine3x4 transform, double metresPerVoxel);
    bool erase_object(std::uint64_t objectId);
    [[nodiscard]] std::vector<RegionKey> object_regions(std::uint64_t objectId) const;
    [[nodiscard]] std::vector<std::uint64_t> region_objects(RegionKey key) const;
    [[nodiscard]] std::vector<RegionEntry> entries() const;

    // Inclusive region-coordinate query. World/LOD are matched exactly; an optional
    // result denotes budget violation or invalid bounds, never a partial result.
    // This is not an arbitrary-AABB spatial index; it filters y/z after x-range seek.
    [[nodiscard]] std::optional<std::vector<RegionEntry>> query(
        std::uint32_t world, std::uint8_t lod,
        BrickKey minimumRegion, BrickKey maximumRegion,
        std::size_t maximumExamined = 100000) const;

private:
    struct RegionKeyHash {
        std::size_t operator()(RegionKey key) const noexcept;
    };
    using HashIndex = std::unordered_map<RegionKey, std::vector<std::uint64_t>, RegionKeyHash>;
    using TreeIndex = std::map<RegionKey, std::vector<std::uint64_t>>;

    [[nodiscard]] std::vector<std::uint64_t>* mutable_members(RegionKey key);
    void add_member(RegionKey key, std::uint64_t objectId);
    void remove_member(RegionKey key, std::uint64_t objectId);
    void refresh_cache() const;

    RegionCatalogConfig config_{};
    HashIndex hashIndex_{};
    TreeIndex treeIndex_{};
    std::unordered_map<std::uint64_t, std::vector<RegionKey>> objectRegions_{};
    std::uint64_t revision_{};
    mutable std::uint64_t cachedRevision_{static_cast<std::uint64_t>(-1)};
    mutable std::vector<RegionEntry> sortedCache_{};
};

} // namespace dve