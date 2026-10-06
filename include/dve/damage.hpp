#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "dve/voxel_object.hpp"

namespace dve {

constexpr std::int32_t kDamageSubvoxelScale = 256;
// Global radius cap in voxel units, applied at quantization. Keeps radius^2 in
// subvoxel units far inside int64 and bounds worst-case work for callers that
// do not supply clip bounds (65536 voxels = 6.5 km at 0.1 m voxels).
constexpr std::int32_t kMaxDamageRadiusVoxels = 65536;

// Inclusive voxel-coordinate box that damage rasterization is clipped to.
// apply_damage_commands derives it from the object's occupied bricks, so the
// cost of a command is bounded by the object's size, not by the sphere's.
struct DamageClipBounds {
    Int3 minVoxel{};
    Int3 maxVoxel{};
    [[nodiscard]] bool empty() const noexcept {
        return minVoxel.x > maxVoxel.x || minVoxel.y > maxVoxel.y || minVoxel.z > maxVoxel.z;
    }
};
// Voxel bounds covering every brick of the object (empty() when it has none).
[[nodiscard]] DamageClipBounds damage_clip_bounds(const VoxelObject& object) noexcept;

struct SphereDamageCommand {
    Float3 center{};      // object-local voxel units; quantized at command ingestion
    float radius{};
    std::uint64_t sequence{};
};

struct QuantizedSphereDamageCommand {
    std::int32_t centerX{};
    std::int32_t centerY{};
    std::int32_t centerZ{};
    std::int32_t radius{};
    std::uint64_t sequence{};
    auto operator<=>(const QuantizedSphereDamageCommand&) const = default;
};

[[nodiscard]] QuantizedSphereDamageCommand quantize_damage_command(SphereDamageCommand command) noexcept;

struct DamageBrickBatch {
    BrickKey key{};
    BrickMutation mutation{};
};

struct DamageApplyReport {
    std::vector<AppliedBrickEdit> edits{};
    std::uint64_t removedVoxelCount{};
};

struct DamageApplyReportView {
    std::span<const AppliedBrickEdit> edits{};
    std::uint64_t removedVoxelCount{};
};

// Caller-owned scratch storage for the frame loop. Capacity is retained between calls.
class DamageBatchWorkspace {
public:
    void reserve(std::size_t commandCapacity, std::size_t brickBatchCapacity, std::size_t editCapacity);
    [[nodiscard]] std::size_t command_capacity() const noexcept { return commandScratch_.capacity(); }
    [[nodiscard]] std::size_t batch_capacity() const noexcept { return batches_.capacity(); }
    [[nodiscard]] std::size_t edit_capacity() const noexcept { return edits_.capacity(); }
    [[nodiscard]] std::size_t last_growth_events() const noexcept { return lastGrowthEvents_; }
    [[nodiscard]] std::size_t total_growth_events() const noexcept { return totalGrowthEvents_; }

private:
    std::vector<QuantizedSphereDamageCommand> commandScratch_{};
    std::vector<DamageBrickBatch> batches_{};
    std::vector<AppliedBrickEdit> edits_{};
    std::size_t growthProbeDepth_{};
    std::size_t startCommandCapacity_{};
    std::size_t startBatchCapacity_{};
    std::size_t startEditCapacity_{};
    std::size_t lastGrowthEvents_{};
    std::size_t totalGrowthEvents_{};

    void begin_growth_probe() noexcept;
    void end_growth_probe() noexcept;

    friend std::span<const DamageBrickBatch> build_damage_batches(
        std::span<const SphereDamageCommand>, DamageBatchWorkspace&, const DamageClipBounds*);
    friend DamageApplyReportView apply_damage_commands(
        VoxelObject&, std::span<const SphereDamageCommand>, DamageBatchWorkspace&);
};

// clip (optional) restricts rasterization to the given voxel box.
[[nodiscard]] std::span<const DamageBrickBatch> build_damage_batches(
    std::span<const SphereDamageCommand> commands,
    DamageBatchWorkspace& workspace,
    const DamageClipBounds* clip = nullptr);

[[nodiscard]] DamageApplyReportView apply_damage_commands(
    VoxelObject& object,
    std::span<const SphereDamageCommand> commands,
    DamageBatchWorkspace& workspace);

[[nodiscard]] std::map<BrickKey, BrickMutation> build_damage_batches(
    std::vector<SphereDamageCommand> commands);
DamageApplyReport apply_damage_commands(VoxelObject& object, std::vector<SphereDamageCommand> commands);

} // namespace dve
