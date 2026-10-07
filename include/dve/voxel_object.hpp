#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <array>
#include <string>

#include "dve/flat_brick_map.hpp"

namespace dve {

struct VoxelBrickSnapshot {
    BrickKey key{};
    std::uint32_t generation{};
    std::array<MaterialId, kBrickVoxelCount> materials{};
    std::uint64_t contentHash{};
};

struct AppliedBrickEdit {
    BrickKey key{};
    Bitset512 changedMask{};
    std::uint32_t generation{};
};

class VoxelObject {
public:
    explicit VoxelObject(std::uint64_t id = 1);
    ~VoxelObject();

    VoxelObject(const VoxelObject&) = delete;
    VoxelObject& operator=(const VoxelObject&) = delete;
    VoxelObject(VoxelObject&& other) noexcept;
    VoxelObject& operator=(VoxelObject&& other) noexcept;

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    // Content revision for derived-data caches. It changes on every mutating call
    // (set_voxel, fill_brick, apply, replace_brick, the mutable find_brick, and move
    // assignment) and is drawn from a process-wide counter, so two different
    // objects or states never share a value, even at a reused address. Edits made
    // later through a held Brick* are not seen; mutate through apply() instead.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] MaterialId material_at(Int3 globalVoxel) const;
    [[nodiscard]] bool occupied_at(Int3 globalVoxel) const { return material_at(globalVoxel) != kAirMaterial; }

    // Bounds and totals derived from the brick set. They are computed in one
    // walk and cached against revision(), so repeated queries between edits
    // cost O(1) instead of a walk over every brick; hover picking, selection
    // boxes and inspector counts all ask for these per frame or per mouse move.
    // brick_extent_bounds covers whole bricks (maximum is the far brick
    // corner); occupied_bounds is the tight box around occupied voxels
    // (maximum inclusive). valid is false when no brick / no voxel qualifies.
    struct DerivedBounds {
        Int3 minimum{};
        Int3 maximum{};
        bool valid{};
    };
    [[nodiscard]] DerivedBounds brick_extent_bounds() const;
    [[nodiscard]] DerivedBounds occupied_bounds() const;

    BrickApplyResult set_voxel(Int3 globalVoxel, MaterialId material);
    void fill_brick(BrickKey key, MaterialId material);
    AppliedBrickEdit apply(BrickKey key, const BrickMutation& mutation);

    [[nodiscard]] const Brick* find_brick(BrickKey key) const;
    [[nodiscard]] Brick* find_brick(BrickKey key);
    [[nodiscard]] VoxelBrickSnapshot snapshot_brick(BrickKey key) const;
    bool replace_brick(const VoxelBrickSnapshot& snapshot, std::string* error = nullptr);
    [[nodiscard]] static std::uint64_t brick_content_hash(
        const std::array<MaterialId, kBrickVoxelCount>& materials) noexcept;
    [[nodiscard]] const FlatBrickMap& bricks() const noexcept { return bricks_; }
    void reserve_bricks(std::size_t count) { bricks_.reserve(count); }
    void reserve_payloads(
        std::size_t maskUniformPayloads,
        std::size_t localPalette4Payloads = 0,
        std::size_t palette8Payloads = 0);

    [[nodiscard]] std::size_t brick_count() const noexcept { return bricks_.size(); }
    [[nodiscard]] std::uint64_t occupied_voxel_count() const;
    [[nodiscard]] std::size_t payload_bytes() const;
    [[nodiscard]] std::size_t logical_storage_bytes() const;
    [[nodiscard]] BrickPoolStats payload_pool_stats() const noexcept;
    [[nodiscard]] std::uint64_t state_hash() const;
    [[nodiscard]] bool validate() const;

private:
    void touch() noexcept;
    void refresh_derived() const;

    std::uint64_t id_{};
    std::uint64_t revision_{};
    // Declared before bricks_ so brick destructors return payloads before the pools are destroyed.
    std::unique_ptr<BrickPayloadPools> pools_{};
    FlatBrickMap bricks_{};
    // Derived-data cache, keyed by revision_ (never 0, so 0 means "stale"). Const queries may
    // run on several threads at once (an object shared read-only between workers), so the
    // first query after an edit refreshes the cache under derivedMutex_ and publishes it by
    // storing derivedRevision_ with release ordering; later queries only load it (acquire).
    void ensure_derived() const;
    mutable std::mutex derivedMutex_;
    mutable std::atomic<std::uint64_t> derivedRevision_{};
    mutable DerivedBounds brickExtentCache_{};
    mutable DerivedBounds occupiedCache_{};
    mutable std::uint64_t occupiedCountCache_{};
};

} // namespace dve
