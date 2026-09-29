#include "dve/damage.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace dve {

namespace {

[[nodiscard]] constexpr std::int64_t floor_div64(std::int64_t value, std::int64_t divisor) noexcept {
    const std::int64_t q = value / divisor;
    const std::int64_t r = value % divisor;
    return (r != 0 && ((r < 0) != (divisor < 0))) ? q - 1 : q;
}

[[nodiscard]] constexpr std::int64_t ceil_div64(std::int64_t value, std::int64_t divisor) noexcept {
    return -floor_div64(-value, divisor);
}

[[nodiscard]] std::uint64_t integer_sqrt(std::uint64_t value) noexcept {
    // Bitwise integer square root; deterministic across compilers and instruction sets.
    std::uint64_t result = 0;
    std::uint64_t bit = std::uint64_t{1} << 62U;
    while (bit > value) bit >>= 2U;
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1U) + bit;
        } else {
            result >>= 1U;
        }
        bit >>= 2U;
    }
    return result;
}

void set_local_row_span(Bitset512& mask, std::int32_t localY, std::int32_t localZ,
                        std::int32_t startX, std::int32_t endX) noexcept {
    if (startX > endX) return;
    const std::uint32_t width = static_cast<std::uint32_t>(endX - startX + 1);
    const std::uint64_t bits = width == 64U ? ~std::uint64_t{0} : ((std::uint64_t{1} << width) - 1U);
    const std::uint32_t shift = static_cast<std::uint32_t>(localY * kBrickDim + startX);
    mask.words[static_cast<std::size_t>(localZ)] |= bits << shift;
}

} // namespace

QuantizedSphereDamageCommand quantize_damage_command(SphereDamageCommand command) noexcept {
    auto quantize = [](float value) -> std::int32_t {
        const double scaled = static_cast<double>(value) * static_cast<double>(kDamageSubvoxelScale);
        const double clamped = std::clamp(
            scaled,
            static_cast<double>(std::numeric_limits<std::int32_t>::min()),
            static_cast<double>(std::numeric_limits<std::int32_t>::max()));
        return static_cast<std::int32_t>(std::llround(clamped));
    };
    return {
        quantize(command.center.x),
        quantize(command.center.y),
        quantize(command.center.z),
        std::clamp<std::int32_t>(quantize(command.radius), 0, kMaxDamageRadiusVoxels * kDamageSubvoxelScale),
        command.sequence,
    };
}


void DamageBatchWorkspace::begin_growth_probe() noexcept {
    if (growthProbeDepth_++ != 0) return;
    startCommandCapacity_ = commandScratch_.capacity();
    startBatchCapacity_ = batches_.capacity();
    startEditCapacity_ = edits_.capacity();
    lastGrowthEvents_ = 0;
}

void DamageBatchWorkspace::end_growth_probe() noexcept {
    if (growthProbeDepth_ == 0 || --growthProbeDepth_ != 0) return;
    lastGrowthEvents_ += commandScratch_.capacity() > startCommandCapacity_ ? 1U : 0U;
    lastGrowthEvents_ += batches_.capacity() > startBatchCapacity_ ? 1U : 0U;
    lastGrowthEvents_ += edits_.capacity() > startEditCapacity_ ? 1U : 0U;
    totalGrowthEvents_ += lastGrowthEvents_;
}

void DamageBatchWorkspace::reserve(
    std::size_t commandCapacity,
    std::size_t brickBatchCapacity,
    std::size_t editCapacity) {
    commandScratch_.reserve(commandCapacity);
    batches_.reserve(brickBatchCapacity);
    edits_.reserve(editCapacity);
}

DamageClipBounds damage_clip_bounds(const VoxelObject& object) noexcept {
    DamageClipBounds bounds{
        {std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max()},
        {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::min()}};
    for (const auto& entry : object.bricks()) {
        const BrickKey key = entry.first;
        bounds.minVoxel.x = std::min(bounds.minVoxel.x, key.x * kBrickDim);
        bounds.minVoxel.y = std::min(bounds.minVoxel.y, key.y * kBrickDim);
        bounds.minVoxel.z = std::min(bounds.minVoxel.z, key.z * kBrickDim);
        bounds.maxVoxel.x = std::max(bounds.maxVoxel.x, key.x * kBrickDim + kBrickDim - 1);
        bounds.maxVoxel.y = std::max(bounds.maxVoxel.y, key.y * kBrickDim + kBrickDim - 1);
        bounds.maxVoxel.z = std::max(bounds.maxVoxel.z, key.z * kBrickDim + kBrickDim - 1);
    }
    return bounds;
}

std::span<const DamageBrickBatch> build_damage_batches(
    std::span<const SphereDamageCommand> commands,
    DamageBatchWorkspace& workspace,
    const DamageClipBounds* clip) {
    workspace.begin_growth_probe();
    workspace.commandScratch_.clear();
    for (const SphereDamageCommand command : commands) workspace.commandScratch_.push_back(quantize_damage_command(command));
    std::stable_sort(workspace.commandScratch_.begin(), workspace.commandScratch_.end(), [](const auto& a, const auto& b) {
        return a.sequence < b.sequence;
    });
    workspace.batches_.clear();
    if (clip != nullptr && clip->empty()) {
        workspace.end_growth_probe();
        return workspace.batches_;
    }

    auto batch_for = [&](BrickKey key) -> DamageBrickBatch& {
        const auto it = std::lower_bound(
            workspace.batches_.begin(), workspace.batches_.end(), key,
            [](const DamageBrickBatch& batch, BrickKey candidate) { return batch.key < candidate; });
        if (it != workspace.batches_.end() && it->key == key) return *it;
        return *workspace.batches_.insert(it, DamageBrickBatch{key, {}});
    };

    constexpr std::int64_t scale = kDamageSubvoxelScale;
    constexpr std::int64_t half = kDamageSubvoxelScale / 2;

    for (const QuantizedSphereDamageCommand& command : workspace.commandScratch_) {
        if (command.radius <= 0) continue;
        const std::int64_t radius = command.radius;
        const std::int64_t radiusSquared = radius * radius;

        // Conservative center-coordinate bounds. Exact row spans below reject the surplus.
        std::int64_t minZ = floor_div64(command.centerZ - radius - half, scale);
        std::int64_t maxZ = floor_div64(command.centerZ + radius - half, scale);
        std::int64_t minY = floor_div64(command.centerY - radius - half, scale);
        std::int64_t maxY = floor_div64(command.centerY + radius - half, scale);
        std::int64_t clipMinX = std::numeric_limits<std::int32_t>::min();
        std::int64_t clipMaxX = std::numeric_limits<std::int32_t>::max();
        if (clip != nullptr) {
            // Clip the sphere's row range to the object's voxel box: rows outside it
            // can never touch an existing voxel, so their cost was pure waste
            // (a 30 m sphere on a 0.1 m grid walked ~283k rows / 200k bricks).
            minZ = std::max<std::int64_t>(minZ, clip->minVoxel.z);
            maxZ = std::min<std::int64_t>(maxZ, clip->maxVoxel.z);
            minY = std::max<std::int64_t>(minY, clip->minVoxel.y);
            maxY = std::min<std::int64_t>(maxY, clip->maxVoxel.y);
            clipMinX = clip->minVoxel.x;
            clipMaxX = clip->maxVoxel.x;
        }
        minZ = std::max<std::int64_t>(minZ, std::numeric_limits<std::int32_t>::min());
        maxZ = std::min<std::int64_t>(maxZ, std::numeric_limits<std::int32_t>::max());
        minY = std::max<std::int64_t>(minY, std::numeric_limits<std::int32_t>::min());
        maxY = std::min<std::int64_t>(maxY, std::numeric_limits<std::int32_t>::max());

        for (std::int64_t z64 = minZ; z64 <= maxZ; ++z64) {
            const auto z = static_cast<std::int32_t>(z64);
            const std::int64_t dz = static_cast<std::int64_t>(z) * scale + half - command.centerZ;
            const std::int64_t dzSquared = dz * dz;
            if (dzSquared > radiusSquared) continue;

            for (std::int64_t y64 = minY; y64 <= maxY; ++y64) {
                const auto y = static_cast<std::int32_t>(y64);
                const std::int64_t dy = static_cast<std::int64_t>(y) * scale + half - command.centerY;
                const std::int64_t planeSquared = dzSquared + dy * dy;
                if (planeSquared > radiusSquared) continue;

                const std::int64_t xExtent = static_cast<std::int64_t>(
                    integer_sqrt(static_cast<std::uint64_t>(radiusSquared - planeSquared)));
                const std::int64_t minX64 = std::max(clipMinX,
                    ceil_div64(static_cast<std::int64_t>(command.centerX) - xExtent - half, scale));
                const std::int64_t maxX64 = std::min(clipMaxX,
                    floor_div64(static_cast<std::int64_t>(command.centerX) + xExtent - half, scale));
                if (minX64 > maxX64) continue;
                const auto minX = static_cast<std::int32_t>(minX64);
                const auto maxX = static_cast<std::int32_t>(maxX64);

                const std::int32_t bz = floor_div(z, kBrickDim);
                const std::int32_t by = floor_div(y, kBrickDim);
                const std::int32_t localZ = floor_mod(z, kBrickDim);
                const std::int32_t localY = floor_mod(y, kBrickDim);
                const std::int32_t firstBx = floor_div(minX, kBrickDim);
                const std::int32_t lastBx = floor_div(maxX, kBrickDim);

                for (std::int32_t bx = firstBx; bx <= lastBx; ++bx) {
                    const std::int32_t brickMinX = bx * kBrickDim;
                    const std::int32_t startX = std::max(minX, brickMinX) - brickMinX;
                    const std::int32_t endX = std::min(maxX, brickMinX + kBrickDim - 1) - brickMinX;
                    DamageBrickBatch& batch = batch_for({bx, by, bz});
                    set_local_row_span(batch.mutation.removeMask, localY, localZ, startX, endX);
                }
            }
        }
    }

    workspace.end_growth_probe();
    return workspace.batches_;
}

DamageApplyReportView apply_damage_commands(
    VoxelObject& object,
    std::span<const SphereDamageCommand> commands,
    DamageBatchWorkspace& workspace) {
    workspace.begin_growth_probe();
    const DamageClipBounds clip = damage_clip_bounds(object);
    const std::span<const DamageBrickBatch> batches = build_damage_batches(commands, workspace, &clip);
    workspace.edits_.clear();
    std::uint64_t removedVoxelCount = 0;

    for (const DamageBrickBatch& batch : batches) {
        const Brick* before = object.find_brick(batch.key);
        // Removal-only commands that touch absent space must not materialize empty
        // authority brick headers. Besides wasting memory, that previously made
        // renderer mirrors appear stale even though no voxel changed.
        if (before == nullptr && batch.mutation.writes.empty()) continue;
        const std::uint16_t oldCount = before == nullptr ? 0 : before->occupied_count();
        AppliedBrickEdit edit = object.apply(batch.key, batch.mutation);
        const Brick* after = object.find_brick(batch.key);
        const std::uint16_t newCount = after == nullptr ? 0 : after->occupied_count();
        if (oldCount > newCount) removedVoxelCount += oldCount - newCount;
        if (edit.changedMask.any()) workspace.edits_.push_back(edit);
    }
    workspace.end_growth_probe();
    return {workspace.edits_, removedVoxelCount};
}

std::map<BrickKey, BrickMutation> build_damage_batches(std::vector<SphereDamageCommand> commands) {
    DamageBatchWorkspace workspace;
    const auto batches = build_damage_batches(commands, workspace);
    std::map<BrickKey, BrickMutation> result;
    for (const DamageBrickBatch& batch : batches) {
        BrickMutation mutation;
        mutation.removeMask = batch.mutation.removeMask;
        for (const MaterialWrite& write : batch.mutation.writes) mutation.writes.push_back(write);
        result.emplace(batch.key, std::move(mutation));
    }
    return result;
}

DamageApplyReport apply_damage_commands(VoxelObject& object, std::vector<SphereDamageCommand> commands) {
    DamageBatchWorkspace workspace;
    const DamageApplyReportView view = apply_damage_commands(object, commands, workspace);
    DamageApplyReport report;
    report.removedVoxelCount = view.removedVoxelCount;
    report.edits.assign(view.edits.begin(), view.edits.end());
    return report;
}

} // namespace dve
