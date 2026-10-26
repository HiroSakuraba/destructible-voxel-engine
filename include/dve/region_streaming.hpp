#pragma once
// R2: deterministic region residency planning. This is metadata only: it does
// not read voxels, start jobs or modify loaded scene objects.
#include "dve/region_catalog.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dve {

struct RegionStreamingConfig {
    std::int32_t loadRadius{1};          // Chebyshev distance in region cells
    std::int32_t retainRadius{3};        // must be >= loadRadius
    std::size_t maximumExamined{100000};
    std::size_t loadsPerTick{2};
    std::size_t unloadsPerTick{2};
    std::uint64_t minimumResidentTicks{30};
};

struct RegionStreamingObject {
    std::uint64_t objectId{};
    bool loaded{};
    bool pinned{};
    bool mayUnload{};
    std::uint64_t loadedSinceTick{};
    bool inFlight{};
};

struct RegionStreamingPlan {
    std::vector<std::uint64_t> nearIds;   // inside load radius, sorted unique
    std::vector<std::uint64_t> retainIds; // inside outer hysteresis radius
    std::vector<std::uint64_t> loadIds;   // priority then object ID
    std::vector<std::uint64_t> unloadIds; // ascending object ID
};

class RegionStreamingPlanner {
public:
    explicit RegionStreamingPlanner(RegionStreamingConfig config = {}) : config_(config) {}
    [[nodiscard]] const RegionStreamingConfig& config() const noexcept { return config_; }
    [[nodiscard]] bool valid() const noexcept;
    // Failure (nullopt) is not an empty world: do not unload on query failure.
    [[nodiscard]] std::optional<RegionStreamingPlan> plan(
        const RegionCatalog& catalog, RegionKey camera, std::span<const RegionStreamingObject> objects,
        std::uint64_t tick) const;
    [[nodiscard]] static std::optional<RegionKey> camera_region(
        const RegionCatalog& catalog, std::uint32_t worldId, std::uint8_t lod,
        std::array<double, 3> worldMeters, double metresPerVoxel) noexcept;
private:
    RegionStreamingConfig config_{};
};

} // namespace dve

