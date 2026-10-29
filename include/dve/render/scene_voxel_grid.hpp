#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <unordered_map>
#include <vector>

#include "dve/game_world.hpp"
#include "dve/transform.hpp"
#include "dve/types.hpp"
#include "dve/voxel_object.hpp"

namespace dve::render {

// One world-space block of voxels holding every voxel object of a scene, so a GPU pass can trace
// rays through the whole scene with a single DDA (traced ambient occlusion, bounce lighting,
// radiance cascades). Objects keep their rigid transforms: each grid cell takes the material of
// the object voxel under the cell's centre, so a rotated object is resampled at the grid's
// resolution (exact for objects aligned with the grid, a one-cell staircase otherwise).
//
// Cells are bytes: 0 is air, 1..255 index `palette()`. Layout is x fastest, then y, then z.
// update() only redoes what changed: an object's cells are cached against its voxel revision,
// transform and the grid placement, so moving one object does not re-rasterize the others.
struct SceneGridObject {
    std::uint64_t id{};
    const VoxelObject* voxels{};
    RigidTransform transform{};
    Float3 cellMeters{0.1F, 0.1F, 0.1F};            // object voxel size per axis, metres
    std::span<const VoxelMaterialDefinition> materials; // empty: default_voxel_material
};

struct SceneGridMaterial {
    Float3 albedo{};       // base colour, linear
    float metallic{};
    Float3 emissive{};
    float reserved{};
};
static_assert(sizeof(SceneGridMaterial) == 32U);

struct SceneVoxelGridSettings {
    float cellMeters{0.1F};                // 0: half the smallest object voxel, at least minimumCellMeters
    float minimumCellMeters{0.02F};
    std::uint32_t maximumDimension{256U};  // per axis; larger scenes keep a window around `focus`
    std::uint32_t alignmentCells{16U};     // placement snaps to this many cells so moving
                                           // objects do not shift the grid
};

class SceneVoxelGrid {
public:
    // Returns true when the cells, palette or placement changed (the caller re-uploads).
    bool update(std::span<const SceneGridObject> objects, const SceneVoxelGridSettings& settings,
                Float3 focus);

    [[nodiscard]] bool empty() const noexcept { return cells_.empty(); }
    [[nodiscard]] std::array<std::uint32_t, 3> dimensions() const noexcept { return dimensions_; }
    // World position of the grid's minimum corner, metres.
    [[nodiscard]] Float3 origin() const noexcept { return origin_; }
    [[nodiscard]] float cell_meters() const noexcept { return cellMeters_; }
    [[nodiscard]] std::span<const std::uint8_t> cells() const noexcept { return cells_; }
    [[nodiscard]] std::span<const SceneGridMaterial> palette() const noexcept { return palette_; }
    // Material at a world position (0 = air or outside the grid); the CPU twin of the GPU lookup.
    [[nodiscard]] std::uint8_t cell_at_world(Float3 position) const noexcept;
    // Number of objects rasterized by the last update (the rest came from cache).
    [[nodiscard]] std::size_t last_rasterized_objects() const noexcept { return lastRasterized_; }

private:
    struct CachedObject {
        std::uint64_t key{};   // hash of revision, transform, placement, materials
        std::vector<std::pair<std::uint32_t, std::uint8_t>> cells;
        std::uint64_t seenUpdate{};
    };
    std::uint8_t palette_slot(const VoxelMaterialDefinition& material, const void* table, std::uint32_t id);

    std::array<std::uint32_t, 3> dimensions_{};
    Float3 origin_{};
    float cellMeters_{};
    std::vector<std::uint8_t> cells_;
    std::vector<SceneGridMaterial> palette_;
    std::map<std::pair<const void*, std::uint32_t>, std::uint8_t> paletteIndex_;
    std::unordered_map<std::uint64_t, CachedObject> cache_;
    std::uint64_t placementKey_{};
    std::uint64_t contentKey_{};
    std::uint64_t updateCounter_{};
    std::size_t lastRasterized_{};
};

} // namespace dve::render
