#include "dve/voxel_object.hpp"

#include <atomic>
#include <cstdint>
#include <utility>

namespace dve {

namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hash_byte(std::uint64_t& hash, std::uint8_t value) {
    hash ^= value;
    hash *= kFnvPrime;
}

template <class T>
void hash_scalar(std::uint64_t& hash, T value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    for (std::size_t i = 0; i < sizeof(T); ++i) hash_byte(hash, bytes[i]);
}

std::atomic<std::uint64_t> gNextRevision{1};

std::uint64_t next_revision() noexcept {
    return gNextRevision.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

void VoxelObject::touch() noexcept { revision_ = next_revision(); }

VoxelObject::VoxelObject(std::uint64_t id)
    : id_(id), revision_(next_revision()), pools_(std::make_unique<BrickPayloadPools>()) {}
VoxelObject::~VoxelObject() = default;
VoxelObject::VoxelObject(VoxelObject&& other) noexcept
    : id_(other.id_), revision_(other.revision_), pools_(std::move(other.pools_)), bricks_(std::move(other.bricks_)) {
    // The source no longer holds this content; it must not keep the revision that names it.
    other.touch();
}

VoxelObject& VoxelObject::operator=(VoxelObject&& other) noexcept {
    if (this == &other) return *this;
    VoxelObject temporary(std::move(other));
    std::swap(id_, temporary.id_);
    std::swap(revision_, temporary.revision_);
    std::swap(pools_, temporary.pools_);
    std::swap(bricks_, temporary.bricks_);
    std::swap(derivedRevision_, temporary.derivedRevision_);
    std::swap(brickExtentCache_, temporary.brickExtentCache_);
    std::swap(occupiedCache_, temporary.occupiedCache_);
    std::swap(occupiedCountCache_, temporary.occupiedCountCache_);
    return *this;
}

void VoxelObject::refresh_derived() const {
    Int3 extentMinimum = kInt3Max;
    Int3 extentMaximum = kInt3Min;
    Int3 occupiedMinimum = kInt3Max;
    Int3 occupiedMaximum = kInt3Min;
    std::uint64_t occupiedCount = 0;
    bool anyOccupied = false;
    for (const auto& entry : bricks_) {
        const Int3 origin = brick_origin(entry.first);
        extentMinimum = min_components(extentMinimum, origin);
        extentMaximum = max_components(extentMaximum,
            {origin.x + kBrickDim, origin.y + kBrickDim, origin.z + kBrickDim});
        occupiedCount += entry.second.occupied_count();
        Int3 brickMinimum, brickMaximum;
        if (entry.second.occupancy().bounds(brickMinimum, brickMaximum)) {
            occupiedMinimum = min_components(occupiedMinimum, global_from_local(entry.first, brickMinimum));
            occupiedMaximum = max_components(occupiedMaximum, global_from_local(entry.first, brickMaximum));
            anyOccupied = true;
        }
    }
    brickExtentCache_ = {extentMinimum, extentMaximum, !bricks_.empty()};
    occupiedCache_ = {occupiedMinimum, occupiedMaximum, anyOccupied};
    occupiedCountCache_ = occupiedCount;
    derivedRevision_ = revision_;
}

VoxelObject::DerivedBounds VoxelObject::brick_extent_bounds() const {
    if (derivedRevision_ != revision_) refresh_derived();
    return brickExtentCache_;
}

VoxelObject::DerivedBounds VoxelObject::occupied_bounds() const {
    if (derivedRevision_ != revision_) refresh_derived();
    return occupiedCache_;
}

MaterialId VoxelObject::material_at(Int3 globalVoxel) const {
    const BrickKey key = brick_key_from_voxel(globalVoxel);
    const Brick* brick = find_brick(key);
    if (brick == nullptr) return kAirMaterial;
    return brick->material(voxel_index_unchecked(local_voxel_from_global(globalVoxel)));
}

BrickApplyResult VoxelObject::set_voxel(Int3 globalVoxel, MaterialId material) {
    const BrickKey key = brick_key_from_voxel(globalVoxel);
    auto [it, inserted] = bricks_.try_emplace(key, *pools_);
    const BrickApplyResult result =
        it->second.set_voxel(voxel_index_unchecked(local_voxel_from_global(globalVoxel)), material);
    if (inserted || result.changed) touch();
    return result;
}

void VoxelObject::fill_brick(BrickKey key, MaterialId material) {
    touch();
    Brick replacement = Brick::uniform_solid(*pools_, material);
    auto it = bricks_.find(key);
    if (it == bricks_.end()) bricks_.emplace(key, std::move(replacement));
    else it->second = std::move(replacement);
}

AppliedBrickEdit VoxelObject::apply(BrickKey key, const BrickMutation& mutation) {
    auto [it, inserted] = bricks_.try_emplace(key, *pools_);
    const BrickApplyResult result = it->second.apply(mutation);
    if (inserted || result.changed) touch();
    return {key, result.changedMask, result.generation};
}

void VoxelObject::reserve_payloads(
    std::size_t maskUniformPayloads,
    std::size_t localPalette4Payloads,
    std::size_t palette8Payloads) {
    pools_->reserve(maskUniformPayloads, localPalette4Payloads, palette8Payloads);
}

const Brick* VoxelObject::find_brick(BrickKey key) const {
    const auto it = bricks_.find(key);
    return it == bricks_.end() ? nullptr : &it->second;
}

Brick* VoxelObject::find_brick(BrickKey key) {
    const auto it = bricks_.find(key);
    if (it == bricks_.end()) return nullptr;
    // A mutable pointer may be used to edit the brick; assume it will be.
    touch();
    return &it->second;
}

std::uint64_t VoxelObject::brick_content_hash(
    const std::array<MaterialId, kBrickVoxelCount>& materials) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (MaterialId material : materials) {
        hash ^= static_cast<std::uint8_t>(material & 0xFFU);
        hash *= 1099511628211ULL;
        hash ^= static_cast<std::uint8_t>((static_cast<std::uint32_t>(material) >> 8U) & 0xFFU);
        hash *= 1099511628211ULL;
    }
    return hash;
}

VoxelBrickSnapshot VoxelObject::snapshot_brick(BrickKey key) const {
    VoxelBrickSnapshot snapshot;
    snapshot.key = key;
    if (const Brick* brick = find_brick(key)) {
        snapshot.generation = brick->generation();
        snapshot.materials = brick->materials();
    }
    snapshot.contentHash = brick_content_hash(snapshot.materials);
    return snapshot;
}

bool VoxelObject::replace_brick(const VoxelBrickSnapshot& snapshot, std::string* error) {
    if (brick_content_hash(snapshot.materials) != snapshot.contentHash) {
        if (error != nullptr) *error = "voxel brick snapshot content hash does not match its materials";
        return false;
    }
    touch();
    auto [it, inserted] = bricks_.try_emplace(snapshot.key, *pools_);
    (void)inserted;
    it->second.replace_materials(snapshot.materials, snapshot.generation);
    return it->second.validate();
}

std::uint64_t VoxelObject::occupied_voxel_count() const {
    if (derivedRevision_ != revision_) refresh_derived();
    return occupiedCountCache_;
}

std::size_t VoxelObject::payload_bytes() const { return payload_pool_stats().livePayloadBytes; }

std::size_t VoxelObject::logical_storage_bytes() const {
    return bricks_.size() * sizeof(Brick) + payload_pool_stats().livePayloadBytes;
}

BrickPoolStats VoxelObject::payload_pool_stats() const noexcept {
    return pools_ == nullptr ? BrickPoolStats{} : pools_->stats();
}

std::uint64_t VoxelObject::state_hash() const {
    std::uint64_t hash = kFnvOffset;
    hash_scalar(hash, id_);
    for (const BrickKey key : bricks_.sorted_keys()) {
        const Brick* brick = find_brick(key);
        if (brick == nullptr) continue;
        hash_scalar(hash, key.x);
        hash_scalar(hash, key.y);
        hash_scalar(hash, key.z);
        hash_scalar(hash, brick->generation());
        for (std::uint16_t i = 0; i < kBrickVoxelCount; ++i) hash_byte(hash, brick->material(i));
    }
    return hash;
}

bool VoxelObject::validate() const {
    if (pools_ == nullptr) return bricks_.empty();
    if (!bricks_.validate()) return false;
    for (const auto& [key, brick] : bricks_) {
        (void)key;
        if (!brick.validate()) return false;
    }
    return true;
}

} // namespace dve
