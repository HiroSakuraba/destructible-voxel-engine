#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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
    [[nodiscard]] MaterialId material_at(Int3 globalVoxel) const;
    [[nodiscard]] bool occupied_at(Int3 globalVoxel) const { return material_at(globalVoxel) != kAirMaterial; }

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
    std::uint64_t id_{};
    // Declared before bricks_ so brick destructors return payloads before the pools are destroyed.
    std::unique_ptr<BrickPayloadPools> pools_{};
    FlatBrickMap bricks_{};
};

} // namespace dve
