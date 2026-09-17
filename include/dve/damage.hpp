#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "dve/voxel_object.hpp"

namespace dve {

constexpr std::int32_t kDamageSubvoxelScale = 256;

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
        std::span<const SphereDamageCommand>, DamageBatchWorkspace&);
    friend DamageApplyReportView apply_damage_commands(
        VoxelObject&, std::span<const SphereDamageCommand>, DamageBatchWorkspace&);
};

[[nodiscard]] std::span<const DamageBrickBatch> build_damage_batches(
    std::span<const SphereDamageCommand> commands,
    DamageBatchWorkspace& workspace);

[[nodiscard]] DamageApplyReportView apply_damage_commands(
    VoxelObject& object,
    std::span<const SphereDamageCommand> commands,
    DamageBatchWorkspace& workspace);

[[nodiscard]] std::map<BrickKey, BrickMutation> build_damage_batches(
    std::vector<SphereDamageCommand> commands);
DamageApplyReport apply_damage_commands(VoxelObject& object, std::vector<SphereDamageCommand> commands);

} // namespace dve
