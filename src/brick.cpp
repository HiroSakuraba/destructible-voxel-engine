#include "dve/brick.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace dve {

namespace {

template <class T, std::size_t BlockSlots>
class SlabPool {
    struct Slot {
        alignas(T) std::byte storage[sizeof(T)]{};
        Slot* next{};
        bool live{};
    };

    static_assert(std::is_standard_layout_v<Slot>);
    static_assert(offsetof(Slot, storage) == 0);

public:
    SlabPool() = default;
    ~SlabPool() {
        for (const auto& block : blocks_) {
            for (std::size_t i = 0; i < BlockSlots; ++i) {
                if (block[i].live) std::destroy_at(std::launder(reinterpret_cast<T*>(block[i].storage)));
            }
        }
    }

    SlabPool(const SlabPool&) = delete;
    SlabPool& operator=(const SlabPool&) = delete;

    T* create(const T& value) {
        if (free_ == nullptr) allocate_block();
        Slot* slot = free_;
        free_ = slot->next;
        T* object = std::construct_at(reinterpret_cast<T*>(slot->storage), value);
        slot->live = true;
        slot->next = nullptr;
        ++liveCount_;
        return object;
    }

    void destroy(T* object) noexcept {
        if (object == nullptr) return;
        auto* slot = reinterpret_cast<Slot*>(object);
        assert(slot->live);
        std::destroy_at(object);
        slot->live = false;
        slot->next = free_;
        free_ = slot;
        --liveCount_;
    }

    void reserve_slots(std::size_t count) {
        while (blocks_.size() * BlockSlots < count) allocate_block();
    }

    [[nodiscard]] std::size_t live_count() const noexcept { return liveCount_; }
    [[nodiscard]] std::size_t reserved_bytes() const noexcept { return blocks_.size() * BlockSlots * sizeof(Slot); }

private:
    void allocate_block() {
        auto block = std::make_unique<Slot[]>(BlockSlots);
        for (std::size_t i = 0; i < BlockSlots; ++i) {
            block[i].next = free_;
            free_ = &block[i];
        }
        blocks_.push_back(std::move(block));
    }

    std::vector<std::unique_ptr<Slot[]>> blocks_{};
    Slot* free_{};
    std::size_t liveCount_{};
};

[[nodiscard]] Bitset512 occupancy_from_dense(const std::array<MaterialId, kBrickVoxelCount>& dense) {
    Bitset512 occupancy;
    for (std::uint16_t i = 0; i < kBrickVoxelCount; ++i) {
        if (dense[i] != kAirMaterial) occupancy.set(i);
    }
    return occupancy;
}

} // namespace

struct BrickPayloadPools::Impl {
    SlabPool<MaskUniformPayload, 256> maskUniform{};
    SlabPool<LocalPalette4Payload, 64> localPalette4{};
    SlabPool<Palette8Payload, 64> palette8{};
};

BrickPayloadPools::BrickPayloadPools() : impl_(std::make_unique<Impl>()) {}
BrickPayloadPools::~BrickPayloadPools() = default;

void BrickPayloadPools::reserve(
    std::size_t maskUniformPayloads,
    std::size_t localPalette4Payloads,
    std::size_t palette8Payloads) {
    impl_->maskUniform.reserve_slots(maskUniformPayloads);
    impl_->localPalette4.reserve_slots(localPalette4Payloads);
    impl_->palette8.reserve_slots(palette8Payloads);
}

MaskUniformPayload* BrickPayloadPools::create(const MaskUniformPayload& value) {
    return impl_->maskUniform.create(value);
}
LocalPalette4Payload* BrickPayloadPools::create(const LocalPalette4Payload& value) {
    return impl_->localPalette4.create(value);
}
Palette8Payload* BrickPayloadPools::create(const Palette8Payload& value) {
    return impl_->palette8.create(value);
}
void BrickPayloadPools::destroy(MaskUniformPayload* value) noexcept { impl_->maskUniform.destroy(value); }
void BrickPayloadPools::destroy(LocalPalette4Payload* value) noexcept { impl_->localPalette4.destroy(value); }
void BrickPayloadPools::destroy(Palette8Payload* value) noexcept { impl_->palette8.destroy(value); }

BrickPoolStats BrickPayloadPools::stats() const noexcept {
    BrickPoolStats result;
    result.liveMaskUniform = impl_->maskUniform.live_count();
    result.liveLocalPalette4 = impl_->localPalette4.live_count();
    result.livePalette8 = impl_->palette8.live_count();
    result.livePayloadBytes = result.liveMaskUniform * sizeof(MaskUniformPayload) +
                              result.liveLocalPalette4 * sizeof(LocalPalette4Payload) +
                              result.livePalette8 * sizeof(Palette8Payload);
    result.reservedBytes = impl_->maskUniform.reserved_bytes() + impl_->localPalette4.reserved_bytes() +
                           impl_->palette8.reserved_bytes();
    return result;
}

Brick::~Brick() { release_payload(); }

Brick::Brick(Brick&& other) noexcept
    : pools_(other.pools_),
      payload_(other.payload_),
      encoding_(other.encoding_),
      inlineMaterial_(other.inlineMaterial_),
      occupiedCount_(other.occupiedCount_),
      generation_(other.generation_) {
    other.payload_ = nullptr;
    other.encoding_ = BrickEncoding::Empty;
    other.inlineMaterial_ = kAirMaterial;
    other.occupiedCount_ = 0;
    other.generation_ = 0;
}

Brick& Brick::operator=(Brick&& other) noexcept {
    if (this == &other) return *this;
    release_payload();
    pools_ = other.pools_;
    payload_ = other.payload_;
    encoding_ = other.encoding_;
    inlineMaterial_ = other.inlineMaterial_;
    occupiedCount_ = other.occupiedCount_;
    generation_ = other.generation_;
    other.payload_ = nullptr;
    other.encoding_ = BrickEncoding::Empty;
    other.inlineMaterial_ = kAirMaterial;
    other.occupiedCount_ = 0;
    other.generation_ = 0;
    return *this;
}

Brick Brick::uniform_solid(BrickPayloadPools& pools, MaterialId material) {
    Brick brick(pools);
    if (material != kAirMaterial) brick.become_uniform(material);
    return brick;
}

const MaskUniformPayload& Brick::mask_uniform() const {
    assert(encoding_ == BrickEncoding::MaskUniform && payload_ != nullptr);
    return *static_cast<const MaskUniformPayload*>(payload_);
}
MaskUniformPayload& Brick::mask_uniform() {
    assert(encoding_ == BrickEncoding::MaskUniform && payload_ != nullptr);
    return *static_cast<MaskUniformPayload*>(payload_);
}
const LocalPalette4Payload& Brick::local_palette4() const {
    assert(encoding_ == BrickEncoding::LocalPalette4 && payload_ != nullptr);
    return *static_cast<const LocalPalette4Payload*>(payload_);
}
LocalPalette4Payload& Brick::local_palette4() {
    assert(encoding_ == BrickEncoding::LocalPalette4 && payload_ != nullptr);
    return *static_cast<LocalPalette4Payload*>(payload_);
}
const Palette8Payload& Brick::palette8() const {
    assert(encoding_ == BrickEncoding::Palette8 && payload_ != nullptr);
    return *static_cast<const Palette8Payload*>(payload_);
}
Palette8Payload& Brick::palette8() {
    assert(encoding_ == BrickEncoding::Palette8 && payload_ != nullptr);
    return *static_cast<Palette8Payload*>(payload_);
}

void Brick::release_payload() noexcept {
    if (payload_ == nullptr || pools_ == nullptr) return;
    switch (encoding_) {
        case BrickEncoding::MaskUniform:
            pools_->destroy(static_cast<MaskUniformPayload*>(payload_));
            break;
        case BrickEncoding::LocalPalette4:
            pools_->destroy(static_cast<LocalPalette4Payload*>(payload_));
            break;
        case BrickEncoding::Palette8:
            pools_->destroy(static_cast<Palette8Payload*>(payload_));
            break;
        case BrickEncoding::Empty:
        case BrickEncoding::UniformSolid:
            break;
    }
    payload_ = nullptr;
}

void Brick::become_empty() noexcept {
    release_payload();
    encoding_ = BrickEncoding::Empty;
    inlineMaterial_ = kAirMaterial;
    occupiedCount_ = 0;
}

void Brick::become_uniform(MaterialId material) noexcept {
    release_payload();
    if (material == kAirMaterial) {
        encoding_ = BrickEncoding::Empty;
        inlineMaterial_ = kAirMaterial;
        occupiedCount_ = 0;
        return;
    }
    encoding_ = BrickEncoding::UniformSolid;
    inlineMaterial_ = material;
    occupiedCount_ = kBrickVoxelCount;
}

void Brick::become_mask_uniform(const Bitset512& occupancyValue, MaterialId material) {
    const std::uint16_t count = occupancyValue.count();
    if (count == 0 || material == kAirMaterial) {
        become_empty();
        return;
    }
    if (count == kBrickVoxelCount) {
        become_uniform(material);
        return;
    }
    MaskUniformPayload* replacement = pools_->create(MaskUniformPayload{occupancyValue, material});
    release_payload();
    payload_ = replacement;
    encoding_ = BrickEncoding::MaskUniform;
    inlineMaterial_ = kAirMaterial;
    occupiedCount_ = count;
}

std::uint8_t Brick::read_nibble(const LocalPalette4Payload& payload, std::uint16_t index) {
    const std::uint8_t byte = payload.packedIndices[index >> 1U];
    return (index & 1U) == 0 ? static_cast<std::uint8_t>(byte & 0x0FU)
                            : static_cast<std::uint8_t>((byte >> 4U) & 0x0FU);
}

void Brick::write_nibble(LocalPalette4Payload& payload, std::uint16_t index, std::uint8_t value) {
    std::uint8_t& byte = payload.packedIndices[index >> 1U];
    if ((index & 1U) == 0) {
        byte = static_cast<std::uint8_t>((byte & 0xF0U) | (value & 0x0FU));
    } else {
        byte = static_cast<std::uint8_t>((byte & 0x0FU) | ((value & 0x0FU) << 4U));
    }
}

Bitset512 Brick::occupancy() const {
    switch (encoding_) {
        case BrickEncoding::Empty: return {};
        case BrickEncoding::UniformSolid: return Bitset512::full();
        case BrickEncoding::MaskUniform: return mask_uniform().occupancy;
        case BrickEncoding::LocalPalette4: return local_palette4().occupancy;
        case BrickEncoding::Palette8: return palette8().occupancy;
    }
    return {};
}

MaterialId Brick::material(std::uint16_t index) const {
    if (index >= kBrickVoxelCount) return kAirMaterial;
    switch (encoding_) {
        case BrickEncoding::Empty:
            return kAirMaterial;
        case BrickEncoding::UniformSolid:
            return inlineMaterial_;
        case BrickEncoding::MaskUniform: {
            const auto& value = mask_uniform();
            return value.occupancy.test(index) ? value.material : kAirMaterial;
        }
        case BrickEncoding::LocalPalette4: {
            const auto& value = local_palette4();
            if (!value.occupancy.test(index)) return kAirMaterial;
            const std::uint8_t paletteIndex = read_nibble(value, index);
            return paletteIndex > 0 && paletteIndex <= value.paletteSize ? value.palette[paletteIndex]
                                                                         : kAirMaterial;
        }
        case BrickEncoding::Palette8: {
            const auto& value = palette8();
            return value.occupancy.test(index) ? value.materials[index] : kAirMaterial;
        }
    }
    return kAirMaterial;
}

std::array<MaterialId, kBrickVoxelCount> Brick::materials() const {
    return dense_materials();
}

void Brick::replace_materials(
    const std::array<MaterialId, kBrickVoxelCount>& dense,
    std::uint32_t authoritativeGeneration) {
    canonicalize(dense);
    generation_ = authoritativeGeneration;
}

std::array<MaterialId, kBrickVoxelCount> Brick::dense_materials() const {
    std::array<MaterialId, kBrickVoxelCount> dense{};
    switch (encoding_) {
        case BrickEncoding::Empty:
            break;
        case BrickEncoding::UniformSolid:
            dense.fill(inlineMaterial_);
            break;
        case BrickEncoding::MaskUniform: {
            const auto& value = mask_uniform();
            value.occupancy.for_each_set([&](std::uint16_t index) { dense[index] = value.material; });
            break;
        }
        case BrickEncoding::LocalPalette4: {
            const auto& value = local_palette4();
            value.occupancy.for_each_set([&](std::uint16_t index) {
                const std::uint8_t paletteIndex = read_nibble(value, index);
                dense[index] = value.palette[paletteIndex];
            });
            break;
        }
        case BrickEncoding::Palette8: {
            const auto& value = palette8();
            dense = value.materials;
            for (std::uint16_t i = 0; i < kBrickVoxelCount; ++i) {
                if (!value.occupancy.test(i)) dense[i] = kAirMaterial;
            }
            break;
        }
    }
    return dense;
}

void Brick::canonicalize(const std::array<MaterialId, kBrickVoxelCount>& dense) {
    const Bitset512 occupied = occupancy_from_dense(dense);
    const std::uint16_t occupiedCount = occupied.count();
    if (occupiedCount == 0) {
        become_empty();
        return;
    }

    std::array<bool, 256> seen{};
    std::array<MaterialId, 255> unique{};
    std::uint16_t uniqueCount = 0;
    occupied.for_each_set([&](std::uint16_t index) {
        const MaterialId materialId = dense[index];
        if (!seen[materialId]) {
            seen[materialId] = true;
            unique[uniqueCount++] = materialId;
        }
    });

    if (uniqueCount == 1) {
        if (occupiedCount == kBrickVoxelCount) become_uniform(unique[0]);
        else become_mask_uniform(occupied, unique[0]);
        return;
    }

    if (uniqueCount <= 15) {
        LocalPalette4Payload value;
        value.occupancy = occupied;
        value.paletteSize = static_cast<std::uint8_t>(uniqueCount);
        for (std::uint16_t i = 0; i < uniqueCount; ++i) value.palette[i + 1] = unique[i];

        std::array<std::uint8_t, 256> reverse{};
        for (std::uint16_t i = 0; i < uniqueCount; ++i) reverse[unique[i]] = static_cast<std::uint8_t>(i + 1);
        occupied.for_each_set([&](std::uint16_t index) { write_nibble(value, index, reverse[dense[index]]); });

        LocalPalette4Payload* replacement = pools_->create(value);
        release_payload();
        payload_ = replacement;
        encoding_ = BrickEncoding::LocalPalette4;
        inlineMaterial_ = kAirMaterial;
        occupiedCount_ = occupiedCount;
        return;
    }

    Palette8Payload value;
    value.occupancy = occupied;
    value.materials = dense;
    Palette8Payload* replacement = pools_->create(value);
    release_payload();
    payload_ = replacement;
    encoding_ = BrickEncoding::Palette8;
    inlineMaterial_ = kAirMaterial;
    occupiedCount_ = occupiedCount;
}

BrickApplyResult Brick::apply(const BrickMutation& mutation) {
    std::array<MaterialId, kBrickVoxelCount> desired{};
    Bitset512 touched = mutation.removeMask;
    for (const MaterialWrite& write : mutation.writes) {
        if (write.index >= kBrickVoxelCount) continue;
        touched.set(write.index);
        desired[write.index] = write.material;
    }

    Bitset512 changed;
    touched.for_each_set([&](std::uint16_t index) {
        if (material(index) != desired[index]) changed.set(index);
    });
    if (changed.none()) return {false, {}, generation_};

    // Common destruction fast paths never materialize a dense 512-byte array.
    if (encoding_ == BrickEncoding::UniformSolid) {
        bool compatible = true;
        changed.for_each_set([&](std::uint16_t index) {
            const MaterialId target = desired[index];
            if (target != kAirMaterial && target != inlineMaterial_) compatible = false;
        });
        if (compatible) {
            Bitset512 next = Bitset512::full();
            changed.for_each_set([&](std::uint16_t index) { next.assign(index, desired[index] != kAirMaterial); });
            become_mask_uniform(next, inlineMaterial_);
            ++generation_;
            return {true, changed, generation_};
        }
    } else if (encoding_ == BrickEncoding::MaskUniform) {
        const MaterialId baseMaterial = mask_uniform().material;
        bool compatible = true;
        changed.for_each_set([&](std::uint16_t index) {
            const MaterialId target = desired[index];
            if (target != kAirMaterial && target != baseMaterial) compatible = false;
        });
        if (compatible) {
            Bitset512 next = mask_uniform().occupancy;
            changed.for_each_set([&](std::uint16_t index) { next.assign(index, desired[index] != kAirMaterial); });
            if (next.none()) become_empty();
            else if (next.count() == kBrickVoxelCount) become_uniform(baseMaterial);
            else {
                mask_uniform().occupancy = next;
                occupiedCount_ = next.count();
            }
            ++generation_;
            return {true, changed, generation_};
        }
    } else if (encoding_ == BrickEncoding::LocalPalette4) {
        auto& value = local_palette4();
        std::array<std::uint8_t, 256> reverse{};
        for (std::uint8_t i = 1; i <= value.paletteSize; ++i) reverse[value.palette[i]] = i;
        std::uint8_t needed = value.paletteSize;
        bool fits = true;
        changed.for_each_set([&](std::uint16_t index) {
            const MaterialId target = desired[index];
            if (target == kAirMaterial || reverse[target] != 0) return;
            if (needed == 15) {
                fits = false;
                return;
            }
            ++needed;
            reverse[target] = needed;
        });
        if (fits) {
            for (std::uint16_t materialValue = 1; materialValue < reverse.size(); ++materialValue) {
                const std::uint8_t paletteIndex = reverse[materialValue];
                if (paletteIndex > value.paletteSize) value.palette[paletteIndex] = static_cast<MaterialId>(materialValue);
            }
            value.paletteSize = needed;
            changed.for_each_set([&](std::uint16_t index) {
                const MaterialId target = desired[index];
                if (target == kAirMaterial) {
                    value.occupancy.reset(index);
                    write_nibble(value, index, 0);
                } else {
                    value.occupancy.set(index);
                    write_nibble(value, index, reverse[target]);
                }
            });
            occupiedCount_ = value.occupancy.count();
            if (occupiedCount_ == 0) become_empty();
            // Material writes are less common than removals. Repack after writes so long-lived
            // bricks do not retain dead palette entries; pure removals remain allocation-free.
            else if (!mutation.writes.empty()) canonicalize(dense_materials());
            ++generation_;
            return {true, changed, generation_};
        }
    } else if (encoding_ == BrickEncoding::Palette8) {
        auto& value = palette8();
        changed.for_each_set([&](std::uint16_t index) {
            const MaterialId target = desired[index];
            value.materials[index] = target;
            value.occupancy.assign(index, target != kAirMaterial);
        });
        occupiedCount_ = value.occupancy.count();
        if (occupiedCount_ == 0) become_empty();
        else if (!mutation.writes.empty()) canonicalize(dense_materials());
        ++generation_;
        return {true, changed, generation_};
    }

    // Rare representation-changing path: one stack-resident dense scratch image, followed by
    // canonical encoding selection. There are no per-mutation heap allocations beyond the slab.
    auto dense = dense_materials();
    changed.for_each_set([&](std::uint16_t index) { dense[index] = desired[index]; });
    canonicalize(dense);
    ++generation_;
    return {true, changed, generation_};
}

BrickApplyResult Brick::set_voxel(std::uint16_t index, MaterialId materialId) {
    BrickMutation mutation;
    mutation.writes.push_back({index, materialId});
    return apply(mutation);
}

void Brick::compact_encoding() { canonicalize(dense_materials()); }

std::size_t Brick::payload_bytes() const noexcept {
    switch (encoding_) {
        case BrickEncoding::Empty:
        case BrickEncoding::UniformSolid:
            return 0;
        case BrickEncoding::MaskUniform:
            return sizeof(MaskUniformPayload);
        case BrickEncoding::LocalPalette4:
            return sizeof(LocalPalette4Payload);
        case BrickEncoding::Palette8:
            return sizeof(Palette8Payload);
    }
    return 0;
}

bool Brick::validate() const {
    if (pools_ == nullptr) return false;
    const bool needsPayload = encoding_ == BrickEncoding::MaskUniform || encoding_ == BrickEncoding::LocalPalette4 ||
                              encoding_ == BrickEncoding::Palette8;
    if (needsPayload != (payload_ != nullptr)) return false;
    if (occupied_count() != occupancy().count()) return false;
    if (encoding_ == BrickEncoding::UniformSolid && inlineMaterial_ == kAirMaterial) return false;
    if (encoding_ == BrickEncoding::MaskUniform && mask_uniform().material == kAirMaterial) return false;
    if (encoding_ == BrickEncoding::LocalPalette4) {
        const auto& value = local_palette4();
        if (value.paletteSize < 2 || value.paletteSize > 15) return false;
        for (std::uint8_t i = 1; i <= value.paletteSize; ++i) {
            if (value.palette[i] == kAirMaterial) return false;
        }
        bool valid = true;
        value.occupancy.for_each_set([&](std::uint16_t index) {
            const std::uint8_t paletteIndex = read_nibble(value, index);
            if (paletteIndex == 0 || paletteIndex > value.paletteSize) valid = false;
        });
        if (!valid) return false;
    }
    if (encoding_ == BrickEncoding::Palette8) {
        const auto& value = palette8();
        for (std::uint16_t i = 0; i < kBrickVoxelCount; ++i) {
            if ((value.materials[i] != kAirMaterial) != value.occupancy.test(i)) return false;
        }
    }
    return true;
}

const char* to_string(BrickEncoding encoding) noexcept {
    switch (encoding) {
        case BrickEncoding::Empty: return "Empty";
        case BrickEncoding::UniformSolid: return "UniformSolid";
        case BrickEncoding::MaskUniform: return "MaskUniform";
        case BrickEncoding::LocalPalette4: return "LocalPalette4";
        case BrickEncoding::Palette8: return "Palette8";
    }
    return "Unknown";
}

} // namespace dve
