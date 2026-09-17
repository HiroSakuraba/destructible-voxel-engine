#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "dve/gpu_brick_mirror.hpp"

namespace dve {

constexpr std::uint32_t kInvalidGpuBrickSlot = 0xFFFFFFFFU;

// Fixed-layout record designed to map directly to a StructuredBuffer in HLSL.
// Material payloads live in a compact byte arena and are addressed by offset.
struct PackedGpuBrickRecord {
    BrickKey key{};
    std::uint32_t generation{};
    std::uint32_t encoding{};
    std::uint32_t uniformMaterial{};
    std::uint32_t materialOffset{};
    std::uint32_t materialBytes{};
    std::array<std::uint32_t, 16> occupancy32{};
    std::array<std::uint32_t, 12> halo32{};
};

static_assert(sizeof(PackedGpuBrickRecord) == 144U);
static_assert(offsetof(PackedGpuBrickRecord, generation) == 12U);
static_assert(offsetof(PackedGpuBrickRecord, occupancy32) == 32U);
static_assert(offsetof(PackedGpuBrickRecord, halo32) == 96U);

// Trace origins and maximum distances are in voxel-index units. Directions are dimensionless;
// GpuTraceResult::distance is returned in the same voxel units. Authored metre values must cross
// the explicit metres-per-voxel conversion boundary before populating this record.
struct GpuTraceRay {
    Float3 origin{};
    float maxDistance{};
    Float3 direction{};
    std::uint32_t rayId{};
};

struct GpuTraceResult {
    Int3 voxel{};
    std::uint32_t hit{};
    Int3 normal{};
    std::uint32_t material{};
    float distance{};
    std::uint32_t brickLookups{};
    std::uint32_t emptyBrickSkips{};
    std::uint32_t voxelSteps{};
};

static_assert(sizeof(GpuTraceRay) == 32U);
static_assert(sizeof(GpuTraceResult) == 48U);
static_assert(offsetof(GpuTraceResult, distance) == 32U);

struct BrickmapBounds {
    BrickKey minKey{};
    BrickKey maxKey{}; // inclusive
    Int3 extent{};
    bool valid{};

    [[nodiscard]] std::size_t cell_count() const noexcept;
    [[nodiscard]] bool contains(BrickKey key) const noexcept;
    [[nodiscard]] std::optional<std::size_t> linear_index(BrickKey key) const noexcept;
};

struct PackedBrickmapUpdateStats {
    std::size_t submittedEdits{};
    std::size_t candidateUploads{};
    std::size_t published{};
    std::size_t staleDropped{};
    std::size_t newSlots{};
    std::size_t recordBytes{};
    std::size_t occupancyBytes{};
    std::size_t haloBytes{};
    std::size_t materialBytes{};
    std::size_t indexGridBytes{};
    bool indexGridRebuilt{};
};

struct PackedBrickmapStorageStats {
    std::size_t slots{};
    std::size_t indexCells{};
    std::size_t recordBytes{};
    std::size_t materialLiveBytes{};
    std::size_t materialArenaBytes{};
    std::size_t materialFreeBytes{};
    std::size_t materialFreeSpans{};
};

// CPU reference for the actual GPU buffers. It models stable slot indices,
// generation-validated publication, a dense object-local brick index grid,
// and compact material-payload suballocation.
class PackedBrickmapScene {
public:
    void reserve(std::size_t brickSlots, std::size_t materialBytes);
    void rebuild(const VoxelObject& object);
    [[nodiscard]] PackedBrickmapUpdateStats update(
        const VoxelObject& object,
        std::span<const AppliedBrickEdit> edits);
    [[nodiscard]] PackedBrickmapUpdateStats publish_uploads(
        std::span<const GpuBrickUpload> uploads);

    [[nodiscard]] const BrickmapBounds& bounds() const noexcept { return bounds_; }
    [[nodiscard]] std::uint32_t slot_at(BrickKey key) const noexcept;
    [[nodiscard]] const PackedGpuBrickRecord* find_record(BrickKey key) const noexcept;
    [[nodiscard]] const PackedGpuBrickRecord* record(std::uint32_t slot) const noexcept;
    [[nodiscard]] MaterialId material_at(Int3 globalVoxel) const noexcept;
    [[nodiscard]] bool occupied_at(Int3 globalVoxel) const noexcept {
        return material_at(globalVoxel) != kAirMaterial;
    }

    [[nodiscard]] std::span<const std::uint32_t> index_grid() const noexcept { return indexGrid_; }
    [[nodiscard]] std::span<const PackedGpuBrickRecord> records() const noexcept { return records_; }
    [[nodiscard]] std::span<const std::uint8_t> material_arena() const noexcept { return materialArena_; }
    [[nodiscard]] PackedBrickmapStorageStats storage_stats() const noexcept;
    [[nodiscard]] std::uint64_t readback_hash() const noexcept;
    [[nodiscard]] bool validate_against(const VoxelObject& object) const;

private:
    struct FreeSpan {
        std::uint32_t offset{};
        std::uint32_t bytes{};
        auto operator<=>(const FreeSpan&) const = default;
    };

    BrickmapBounds bounds_{};
    std::vector<std::uint32_t> indexGrid_{};
    std::vector<PackedGpuBrickRecord> records_{};
    std::vector<std::uint8_t> materialArena_{};
    std::vector<FreeSpan> materialFree_{};
    std::size_t materialLiveBytes_{};

    [[nodiscard]] std::uint32_t allocate_material(std::size_t bytes);
    void release_material(std::uint32_t offset, std::uint32_t bytes);
    void coalesce_free_spans();
    void rebuild_index_grid();
    [[nodiscard]] std::uint32_t find_slot_linear(BrickKey key) const noexcept;
    void write_upload(std::uint32_t slot, const GpuBrickUpload& upload, PackedBrickmapUpdateStats& stats);
};

struct BrickmapRayTraceStats {
    std::uint32_t brickLookups{};
    std::uint32_t emptyBrickSkips{};
    std::uint32_t occupiedBrickVisits{};
    std::uint32_t voxelSteps{};
    std::uint32_t materialDecodes{};
};

// Hierarchical digital differential analyzer (HDDA) reference. Empty 8^3
// bricks are skipped in one event while occupied bricks use exact voxel DDA.
// Tie ordering is intentionally identical to raycast_voxels().
[[nodiscard]] std::optional<RayHit> raycast_packed_brickmap_hdda(
    const PackedBrickmapScene& scene,
    Float3 origin,
    Float3 direction,
    float maxDistance,
    BrickmapRayTraceStats* stats = nullptr);

struct UploadRingAllocation {
    std::size_t offset{};
    std::size_t bytes{};
    std::uint64_t fenceValue{};
};

// Platform-independent model of a persistently mapped, fence-owned upload ring.
// The D3D12 implementation uses the same allocation policy over an UPLOAD heap.
class FenceUploadRing {
public:
    explicit FenceUploadRing(std::size_t capacityBytes = 0);
    void reset(std::size_t capacityBytes);
    void retire(std::uint64_t completedFenceValue) noexcept;
    [[nodiscard]] std::optional<UploadRingAllocation> allocate(
        std::size_t bytes,
        std::size_t alignment,
        std::uint64_t fenceValue,
        std::uint64_t completedFenceValue);
    [[nodiscard]] std::span<std::byte> mapped_span(const UploadRingAllocation& allocation) noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }
    [[nodiscard]] std::size_t in_flight_bytes() const noexcept;
    [[nodiscard]] std::size_t allocation_count() const noexcept { return inFlight_.size(); }

private:
    std::vector<std::byte> storage_{};
    std::deque<UploadRingAllocation> inFlight_{};
    std::size_t head_{};
};

} // namespace dve
