#include "dve/packed_brickmap.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace dve {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t bytes) noexcept {
    const auto* input = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        hash ^= input[i];
        hash *= kFnvPrime;
    }
}

template <class T>
void hash_value(std::uint64_t& hash, const T& value) noexcept {
    hash_bytes(hash, &value, sizeof(T));
}


[[nodiscard]] float next_voxel_boundary_time(
    float origin,
    float direction,
    std::int32_t voxel) noexcept {
    if (direction > 0.0F) {
        return (static_cast<float>(voxel + 1) - origin) / direction;
    }
    if (direction < 0.0F) {
        return (static_cast<float>(voxel) - origin) / direction;
    }
    return std::numeric_limits<float>::infinity();
}



[[nodiscard]] std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    if (alignment <= 1U) return value;
    const std::size_t remainder = value % alignment;
    return remainder == 0U ? value : value + (alignment - remainder);
}

[[nodiscard]] std::uint32_t checked_u32(std::size_t value) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("GPU brickmap offset exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace

std::size_t BrickmapBounds::cell_count() const noexcept {
    if (!valid || extent.x <= 0 || extent.y <= 0 || extent.z <= 0) return 0;
    return static_cast<std::size_t>(extent.x) * static_cast<std::size_t>(extent.y) *
           static_cast<std::size_t>(extent.z);
}

bool BrickmapBounds::contains(BrickKey key) const noexcept {
    return valid && key.x >= minKey.x && key.x <= maxKey.x && key.y >= minKey.y &&
           key.y <= maxKey.y && key.z >= minKey.z && key.z <= maxKey.z;
}

std::optional<std::size_t> BrickmapBounds::linear_index(BrickKey key) const noexcept {
    if (!contains(key)) return std::nullopt;
    const std::size_t x = static_cast<std::size_t>(key.x - minKey.x);
    const std::size_t y = static_cast<std::size_t>(key.y - minKey.y);
    const std::size_t z = static_cast<std::size_t>(key.z - minKey.z);
    return x + static_cast<std::size_t>(extent.x) *
        (y + static_cast<std::size_t>(extent.y) * z);
}

void PackedBrickmapScene::reserve(std::size_t brickSlots, std::size_t materialBytes) {
    records_.reserve(brickSlots);
    materialArena_.reserve(materialBytes);
    materialFree_.reserve(brickSlots / 4U + 1U);
}

std::uint32_t PackedBrickmapScene::allocate_material(std::size_t bytes) {
    if (bytes == 0U) return 0U;
    const std::uint32_t wanted = checked_u32(bytes);
    auto best = materialFree_.end();
    for (auto it = materialFree_.begin(); it != materialFree_.end(); ++it) {
        if (it->bytes < wanted) continue;
        if (best == materialFree_.end() || it->bytes < best->bytes) best = it;
    }
    if (best != materialFree_.end()) {
        const std::uint32_t offset = best->offset;
        best->offset += wanted;
        best->bytes -= wanted;
        if (best->bytes == 0U) materialFree_.erase(best);
        materialLiveBytes_ += bytes;
        return offset;
    }
    const std::uint32_t offset = checked_u32(materialArena_.size());
    materialArena_.resize(materialArena_.size() + bytes);
    materialLiveBytes_ += bytes;
    return offset;
}

void PackedBrickmapScene::release_material(std::uint32_t offset, std::uint32_t bytes) {
    if (bytes == 0U) return;
    assert(materialLiveBytes_ >= bytes);
    materialLiveBytes_ -= bytes;
    materialFree_.push_back({offset, bytes});
    coalesce_free_spans();
}

void PackedBrickmapScene::coalesce_free_spans() {
    if (materialFree_.size() < 2U) return;
    std::sort(materialFree_.begin(), materialFree_.end());
    std::size_t output = 0;
    for (std::size_t i = 1; i < materialFree_.size(); ++i) {
        FreeSpan& current = materialFree_[output];
        const FreeSpan& next = materialFree_[i];
        if (current.offset + current.bytes == next.offset) {
            current.bytes += next.bytes;
        } else {
            ++output;
            materialFree_[output] = next;
        }
    }
    materialFree_.resize(output + 1U);
}

void PackedBrickmapScene::rebuild(const VoxelObject& object) {
    bounds_ = {};
    indexGrid_.clear();
    records_.clear();
    materialArena_.clear();
    materialFree_.clear();
    materialLiveBytes_ = 0U;

    const std::vector<BrickKey> keys = object.bricks().sorted_keys();
    if (keys.empty()) return;
    bounds_.valid = true;
    bounds_.minKey = keys.front();
    bounds_.maxKey = keys.front();
    for (const BrickKey key : keys) {
        bounds_.minKey = {std::min(bounds_.minKey.x, key.x), std::min(bounds_.minKey.y, key.y),
                          std::min(bounds_.minKey.z, key.z)};
        bounds_.maxKey = {std::max(bounds_.maxKey.x, key.x), std::max(bounds_.maxKey.y, key.y),
                          std::max(bounds_.maxKey.z, key.z)};
    }
    bounds_.extent = {
        bounds_.maxKey.x - bounds_.minKey.x + 1,
        bounds_.maxKey.y - bounds_.minKey.y + 1,
        bounds_.maxKey.z - bounds_.minKey.z + 1,
    };
    constexpr std::size_t kMaximumDenseIndexCells = 64U * 1024U * 1024U;
    if (bounds_.cell_count() > kMaximumDenseIndexCells) {
        throw std::length_error("dense GPU brick index grid exceeds prototype safety limit");
    }

    records_.reserve(keys.size());
    PackedBrickmapUpdateStats ignored;
    for (const BrickKey key : keys) {
        records_.push_back({});
        write_upload(static_cast<std::uint32_t>(records_.size() - 1U),
                     build_gpu_brick_upload(object, key), ignored);
    }
    rebuild_index_grid();
}

std::uint32_t PackedBrickmapScene::find_slot_linear(BrickKey key) const noexcept {
    for (std::uint32_t i = 0; i < records_.size(); ++i) {
        if (records_[i].key == key) return i;
    }
    return kInvalidGpuBrickSlot;
}

void PackedBrickmapScene::rebuild_index_grid() {
    if (!bounds_.valid) {
        indexGrid_.clear();
        return;
    }
    indexGrid_.assign(bounds_.cell_count(), kInvalidGpuBrickSlot);
    for (std::uint32_t slot = 0; slot < records_.size(); ++slot) {
        const auto index = bounds_.linear_index(records_[slot].key);
        if (index) indexGrid_[*index] = slot;
    }
}

void PackedBrickmapScene::write_upload(
    std::uint32_t slot,
    const GpuBrickUpload& upload,
    PackedBrickmapUpdateStats& stats) {
    assert(slot < records_.size());
    PackedGpuBrickRecord& record = records_[slot];
    if (record.materialBytes != 0U) {
        release_material(record.materialOffset, record.materialBytes);
    }
    record = {};
    record.key = upload.key;
    record.generation = upload.generation;
    record.encoding = static_cast<std::uint32_t>(upload.encoding);
    record.uniformMaterial = upload.uniformMaterial;
    record.occupancy32 = upload.occupancy32;
    record.halo32 = upload.halo32;
    if (!upload.materialPayload.empty()) {
        record.materialOffset = allocate_material(upload.materialPayload.size());
        record.materialBytes = checked_u32(upload.materialPayload.size());
        std::memcpy(materialArena_.data() + record.materialOffset,
                    upload.materialPayload.data(), upload.materialPayload.size());
    }
    stats.recordBytes += sizeof(PackedGpuBrickRecord);
    stats.occupancyBytes += sizeof(record.occupancy32);
    stats.haloBytes += sizeof(record.halo32);
    stats.materialBytes += upload.materialPayload.size();
}

PackedBrickmapUpdateStats PackedBrickmapScene::publish_uploads(
    std::span<const GpuBrickUpload> uploads) {
    PackedBrickmapUpdateStats stats;
    stats.candidateUploads = uploads.size();
    bool boundsChanged = false;

    for (const GpuBrickUpload& upload : uploads) {
        std::uint32_t slot = find_slot_linear(upload.key);
        if (slot == kInvalidGpuBrickSlot) {
            slot = static_cast<std::uint32_t>(records_.size());
            records_.push_back({});
            ++stats.newSlots;
            if (!bounds_.valid) {
                bounds_.valid = true;
                bounds_.minKey = upload.key;
                bounds_.maxKey = upload.key;
                boundsChanged = true;
            } else if (!bounds_.contains(upload.key)) {
                bounds_.minKey = {std::min(bounds_.minKey.x, upload.key.x),
                                  std::min(bounds_.minKey.y, upload.key.y),
                                  std::min(bounds_.minKey.z, upload.key.z)};
                bounds_.maxKey = {std::max(bounds_.maxKey.x, upload.key.x),
                                  std::max(bounds_.maxKey.y, upload.key.y),
                                  std::max(bounds_.maxKey.z, upload.key.z)};
                boundsChanged = true;
            }
        } else if (upload.generation < records_[slot].generation) {
            ++stats.staleDropped;
            continue;
        }
        write_upload(slot, upload, stats);
        ++stats.published;
    }

    if (bounds_.valid) {
        bounds_.extent = {
            bounds_.maxKey.x - bounds_.minKey.x + 1,
            bounds_.maxKey.y - bounds_.minKey.y + 1,
            bounds_.maxKey.z - bounds_.minKey.z + 1,
        };
    }
    if (boundsChanged || indexGrid_.empty()) {
        constexpr std::size_t kMaximumDenseIndexCells = 64U * 1024U * 1024U;
        if (bounds_.cell_count() > kMaximumDenseIndexCells) {
            throw std::length_error("dense GPU brick index grid exceeds prototype safety limit");
        }
        rebuild_index_grid();
        stats.indexGridRebuilt = true;
        stats.indexGridBytes = indexGrid_.size() * sizeof(std::uint32_t);
    } else {
        for (const GpuBrickUpload& upload : uploads) {
            const auto index = bounds_.linear_index(upload.key);
            if (!index) continue;
            indexGrid_[*index] = find_slot_linear(upload.key);
        }
    }
    return stats;
}

PackedBrickmapUpdateStats PackedBrickmapScene::update(
    const VoxelObject& object,
    std::span<const AppliedBrickEdit> edits) {
    PackedBrickmapUpdateStats stats;
    stats.submittedEdits = edits.size();
    std::vector<BrickKey> keys;
    keys.reserve(edits.size() * 7U);
    constexpr std::array<BrickKey, 7> deltas{{
        {0, 0, 0}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0},
        {0, 1, 0}, {0, 0, -1}, {0, 0, 1},
    }};
    for (const AppliedBrickEdit& edit : edits) {
        const Brick* source = object.find_brick(edit.key);
        if (source == nullptr || source->generation() != edit.generation) {
            ++stats.staleDropped;
            continue;
        }
        for (const BrickKey delta : deltas) {
            const BrickKey key{edit.key.x + delta.x, edit.key.y + delta.y, edit.key.z + delta.z};
            if (object.find_brick(key) != nullptr) keys.push_back(key);
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::vector<GpuBrickUpload> uploads;
    uploads.reserve(keys.size());
    for (const BrickKey key : keys) uploads.push_back(build_gpu_brick_upload(object, key));
    PackedBrickmapUpdateStats publish = publish_uploads(uploads);
    publish.submittedEdits = stats.submittedEdits;
    publish.staleDropped += stats.staleDropped;
    return publish;
}

std::uint32_t PackedBrickmapScene::slot_at(BrickKey key) const noexcept {
    const auto index = bounds_.linear_index(key);
    if (!index || *index >= indexGrid_.size()) return kInvalidGpuBrickSlot;
    return indexGrid_[*index];
}

const PackedGpuBrickRecord* PackedBrickmapScene::find_record(BrickKey key) const noexcept {
    const std::uint32_t slot = slot_at(key);
    return record(slot);
}

const PackedGpuBrickRecord* PackedBrickmapScene::record(std::uint32_t slot) const noexcept {
    return slot == kInvalidGpuBrickSlot || slot >= records_.size() ? nullptr : &records_[slot];
}

MaterialId PackedBrickmapScene::material_at(Int3 globalVoxel) const noexcept {
    const PackedGpuBrickRecord* packed = find_record(brick_key_from_voxel(globalVoxel));
    if (packed == nullptr) return kAirMaterial;
    const std::uint16_t index = voxel_index_unchecked(local_voxel_from_global(globalVoxel));
    const std::uint32_t word = packed->occupancy32[index >> 5U];
    if (((word >> (index & 31U)) & 1U) == 0U) return kAirMaterial;
    const BrickEncoding encoding = static_cast<BrickEncoding>(packed->encoding);
    if (encoding == BrickEncoding::UniformSolid || encoding == BrickEncoding::MaskUniform) {
        return static_cast<MaterialId>(packed->uniformMaterial);
    }
    if (packed->materialOffset + packed->materialBytes > materialArena_.size()) return kAirMaterial;
    const std::uint8_t* payload = materialArena_.data() + packed->materialOffset;
    if (encoding == BrickEncoding::LocalPalette4 && packed->materialBytes >= 272U) {
        const std::uint8_t nibbleByte = payload[index >> 1U];
        const std::uint8_t paletteIndex = (index & 1U) == 0U ? nibbleByte & 0x0FU : nibbleByte >> 4U;
        return payload[256U + paletteIndex];
    }
    if (encoding == BrickEncoding::Palette8 && packed->materialBytes >= kBrickVoxelCount) {
        return payload[index];
    }
    return kAirMaterial;
}

PackedBrickmapStorageStats PackedBrickmapScene::storage_stats() const noexcept {
    const std::size_t freeBytes = std::accumulate(
        materialFree_.begin(), materialFree_.end(), std::size_t{0},
        [](std::size_t sum, const FreeSpan& span) { return sum + span.bytes; });
    return {
        records_.size(), indexGrid_.size(), records_.size() * sizeof(PackedGpuBrickRecord),
        materialLiveBytes_, materialArena_.size(), freeBytes, materialFree_.size(),
    };
}

std::uint64_t PackedBrickmapScene::readback_hash() const noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_value(hash, bounds_.minKey);
    hash_value(hash, bounds_.maxKey);
    hash_value(hash, bounds_.extent);
    for (const std::uint32_t slot : indexGrid_) hash_value(hash, slot);
    for (const PackedGpuBrickRecord& recordValue : records_) {
        hash_value(hash, recordValue.key);
        hash_value(hash, recordValue.generation);
        hash_value(hash, recordValue.encoding);
        hash_value(hash, recordValue.uniformMaterial);
        hash_value(hash, recordValue.materialBytes);
        hash_bytes(hash, recordValue.occupancy32.data(), sizeof(recordValue.occupancy32));
        hash_bytes(hash, recordValue.halo32.data(), sizeof(recordValue.halo32));
        if (recordValue.materialBytes != 0U &&
            recordValue.materialOffset + recordValue.materialBytes <= materialArena_.size()) {
            hash_bytes(hash, materialArena_.data() + recordValue.materialOffset, recordValue.materialBytes);
        }
    }
    return hash;
}

bool PackedBrickmapScene::validate_against(const VoxelObject& object) const {
    if (records_.size() != object.brick_count()) return false;
    for (const BrickKey key : object.bricks().sorted_keys()) {
        const Brick* brick = object.find_brick(key);
        const PackedGpuBrickRecord* packed = find_record(key);
        if (brick == nullptr || packed == nullptr || brick->generation() != packed->generation) return false;
        for (std::uint16_t i = 0; i < kBrickVoxelCount; ++i) {
            if (material_at(global_from_local(key, local_from_index_unchecked(i))) != brick->material(i)) return false;
        }
    }
    return true;
}

std::optional<RayHit> raycast_packed_brickmap_hdda(
    const PackedBrickmapScene& scene,
    Float3 origin,
    Float3 direction,
    float maxDistance,
    BrickmapRayTraceStats* stats) {
    BrickmapRayTraceStats localStats;
    if (stats == nullptr) stats = &localStats;
    *stats = {};
    const float magnitude = length(direction);
    if (!(magnitude > 0.0F) || !(maxDistance >= 0.0F)) return std::nullopt;
    const float maximumParameter = maxDistance / magnitude;

    Int3 voxel{
        static_cast<std::int32_t>(std::floor(origin.x)),
        static_cast<std::int32_t>(std::floor(origin.y)),
        static_cast<std::int32_t>(std::floor(origin.z)),
    };
    const Int3 step{
        direction.x > 0.0F ? 1 : (direction.x < 0.0F ? -1 : 0),
        direction.y > 0.0F ? 1 : (direction.y < 0.0F ? -1 : 0),
        direction.z > 0.0F ? 1 : (direction.z < 0.0F ? -1 : 0),
    };
    Float3 tMax{
        next_voxel_boundary_time(origin.x, direction.x, voxel.x),
        next_voxel_boundary_time(origin.y, direction.y, voxel.y),
        next_voxel_boundary_time(origin.z, direction.z, voxel.z),
    };
    const Float3 tDelta{
        direction.x == 0.0F ? std::numeric_limits<float>::infinity() : std::abs(1.0F / direction.x),
        direction.y == 0.0F ? std::numeric_limits<float>::infinity() : std::abs(1.0F / direction.y),
        direction.z == 0.0F ? std::numeric_limits<float>::infinity() : std::abs(1.0F / direction.z),
    };

    float parameter = 0.0F;
    Int3 normal{};
    while (parameter <= maximumParameter) {
        const BrickKey key = brick_key_from_voxel(voxel);
        ++stats->brickLookups;
        const PackedGpuBrickRecord* packed = scene.find_record(key);
        bool occupiedBrick = false;
        if (packed != nullptr) {
            for (const std::uint32_t word : packed->occupancy32) occupiedBrick = occupiedBrick || word != 0U;
        }
        if (!occupiedBrick) {
            ++stats->emptyBrickSkips;
            const BrickKey emptyBrick = key;
            // Exact contract path: advance the voxel DDA without material reads
            // until the ray leaves this empty brick. This preserves corner/edge
            // tie ordering bit-for-bit with the authoritative oracle. A future
            // arithmetic coarse-step optimization must prove equality to this.
            while (parameter <= maximumParameter && brick_key_from_voxel(voxel) == emptyBrick) {
                ++stats->voxelSteps;
                if (tMax.x <= tMax.y && tMax.x <= tMax.z) {
                    voxel.x += step.x; parameter = tMax.x; tMax.x += tDelta.x; normal = {-step.x, 0, 0};
                } else if (tMax.y <= tMax.z) {
                    voxel.y += step.y; parameter = tMax.y; tMax.y += tDelta.y; normal = {0, -step.y, 0};
                } else {
                    voxel.z += step.z; parameter = tMax.z; tMax.z += tDelta.z; normal = {0, 0, -step.z};
                }
            }
            continue;
        }

        ++stats->occupiedBrickVisits;
        const BrickKey activeBrick = key;
        while (parameter <= maximumParameter && brick_key_from_voxel(voxel) == activeBrick) {
            ++stats->materialDecodes;
            const MaterialId material = scene.material_at(voxel);
            if (material != kAirMaterial) return RayHit{voxel, normal, parameter * magnitude, material};
            ++stats->voxelSteps;
            if (tMax.x <= tMax.y && tMax.x <= tMax.z) {
                voxel.x += step.x;
                parameter = tMax.x;
                tMax.x += tDelta.x;
                normal = {-step.x, 0, 0};
            } else if (tMax.y <= tMax.z) {
                voxel.y += step.y;
                parameter = tMax.y;
                tMax.y += tDelta.y;
                normal = {0, -step.y, 0};
            } else {
                voxel.z += step.z;
                parameter = tMax.z;
                tMax.z += tDelta.z;
                normal = {0, 0, -step.z};
            }
        }
    }
    return std::nullopt;
}

FenceUploadRing::FenceUploadRing(std::size_t capacityBytes) { reset(capacityBytes); }

void FenceUploadRing::reset(std::size_t capacityBytes) {
    storage_.assign(capacityBytes, std::byte{});
    inFlight_.clear();
    head_ = 0U;
}

void FenceUploadRing::retire(std::uint64_t completedFenceValue) noexcept {
    while (!inFlight_.empty() && inFlight_.front().fenceValue <= completedFenceValue) {
        inFlight_.pop_front();
    }
    if (inFlight_.empty() && head_ == storage_.size()) head_ = 0U;
}

std::optional<UploadRingAllocation> FenceUploadRing::allocate(
    std::size_t bytes,
    std::size_t alignment,
    std::uint64_t fenceValue,
    std::uint64_t completedFenceValue) {
    retire(completedFenceValue);
    if (bytes == 0U || bytes > storage_.size()) return std::nullopt;
    const auto overlaps = [&](std::size_t begin, std::size_t end) {
        for (const UploadRingAllocation& allocation : inFlight_) {
            const std::size_t otherEnd = allocation.offset + allocation.bytes;
            if (begin < otherEnd && allocation.offset < end) return true;
        }
        return false;
    };

    std::size_t candidate = align_up(head_, alignment);
    if (candidate + bytes <= storage_.size() && !overlaps(candidate, candidate + bytes)) {
        UploadRingAllocation allocation{candidate, bytes, fenceValue};
        inFlight_.push_back(allocation);
        head_ = candidate + bytes;
        return allocation;
    }
    candidate = 0U;
    candidate = align_up(candidate, alignment);
    if (candidate + bytes <= storage_.size() && !overlaps(candidate, candidate + bytes)) {
        UploadRingAllocation allocation{candidate, bytes, fenceValue};
        inFlight_.push_back(allocation);
        head_ = candidate + bytes;
        return allocation;
    }
    return std::nullopt;
}

std::span<std::byte> FenceUploadRing::mapped_span(const UploadRingAllocation& allocation) noexcept {
    if (allocation.offset + allocation.bytes > storage_.size()) return {};
    return {storage_.data() + static_cast<std::ptrdiff_t>(allocation.offset), allocation.bytes};
}

std::size_t FenceUploadRing::in_flight_bytes() const noexcept {
    std::size_t total = 0U;
    for (const UploadRingAllocation& allocation : inFlight_) total += allocation.bytes;
    return total;
}

} // namespace dve
