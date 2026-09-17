#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <vector>

#include "dve/bitset512.hpp"

namespace dve {

enum class BrickEncoding : std::uint8_t {
    Empty,
    UniformSolid,
    MaskUniform,
    LocalPalette4,
    Palette8,
};

struct MaterialWrite {
    std::uint16_t index{};
    MaterialId material{}; // zero removes the voxel
};

// Most gameplay mutations contain zero or one material write. Keep those writes inline so
// single-voxel edits and removal-only destruction do not touch the general heap.
class MaterialWriteList {
public:
    static constexpr std::size_t kInlineCapacity = 8;

    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = MaterialWrite;
        using difference_type = std::ptrdiff_t;
        using pointer = const MaterialWrite*;
        using reference = const MaterialWrite&;

        reference operator*() const { return (*owner_)[index_]; }
        pointer operator->() const { return &(*owner_)[index_]; }
        const_iterator& operator++() { ++index_; return *this; }
        const_iterator operator++(int) { const_iterator copy = *this; ++*this; return copy; }
        friend bool operator==(const const_iterator&, const const_iterator&) = default;

    private:
        friend class MaterialWriteList;
        const_iterator(const MaterialWriteList* owner, std::size_t index) : owner_(owner), index_(index) {}
        const MaterialWriteList* owner_{};
        std::size_t index_{};
    };

    void push_back(MaterialWrite value) {
        if (size_ < kInlineCapacity) inline_[size_] = value;
        else overflow_.push_back(value);
        ++size_;
    }
    void reserve(std::size_t count) {
        if (count > kInlineCapacity) overflow_.reserve(count - kInlineCapacity);
    }
    void clear() noexcept { size_ = 0; overflow_.clear(); }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const MaterialWrite& operator[](std::size_t index) const {
        return index < kInlineCapacity ? inline_[index] : overflow_[index - kInlineCapacity];
    }
    [[nodiscard]] const_iterator begin() const { return {this, 0}; }
    [[nodiscard]] const_iterator end() const { return {this, size_}; }

private:
    std::array<MaterialWrite, kInlineCapacity> inline_{};
    std::vector<MaterialWrite> overflow_{};
    std::size_t size_{};
};

struct BrickMutation {
    Bitset512 removeMask{};
    MaterialWriteList writes{};
};

struct BrickApplyResult {
    bool changed{};
    Bitset512 changedMask{};
    std::uint32_t generation{};
};

struct MaskUniformPayload {
    Bitset512 occupancy{};
    MaterialId material{};
};

struct LocalPalette4Payload {
    Bitset512 occupancy{};
    std::array<std::uint8_t, 256> packedIndices{};
    std::array<MaterialId, 16> palette{}; // palette[0] is air; occupied values use 1..15
    std::uint8_t paletteSize{};
};

struct Palette8Payload {
    Bitset512 occupancy{};
    std::array<MaterialId, kBrickVoxelCount> materials{};
};

struct BrickPoolStats {
    std::size_t liveMaskUniform{};
    std::size_t liveLocalPalette4{};
    std::size_t livePalette8{};
    std::size_t livePayloadBytes{};
    std::size_t reservedBytes{};
};

// Encoding-specific payloads are allocated from stable slab pools owned by a VoxelObject.
// Mutation is owner-thread only; read-only snapshots may be consumed by worker jobs.
class BrickPayloadPools {
public:
    BrickPayloadPools();
    ~BrickPayloadPools();

    BrickPayloadPools(const BrickPayloadPools&) = delete;
    BrickPayloadPools& operator=(const BrickPayloadPools&) = delete;
    BrickPayloadPools(BrickPayloadPools&&) = delete;
    BrickPayloadPools& operator=(BrickPayloadPools&&) = delete;

    [[nodiscard]] BrickPoolStats stats() const noexcept;
    void reserve(
        std::size_t maskUniformPayloads,
        std::size_t localPalette4Payloads,
        std::size_t palette8Payloads);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    MaskUniformPayload* create(const MaskUniformPayload& value);
    LocalPalette4Payload* create(const LocalPalette4Payload& value);
    Palette8Payload* create(const Palette8Payload& value);
    void destroy(MaskUniformPayload* value) noexcept;
    void destroy(LocalPalette4Payload* value) noexcept;
    void destroy(Palette8Payload* value) noexcept;

    friend class Brick;
};

class Brick {
public:
    explicit Brick(BrickPayloadPools& pools) noexcept : pools_(&pools) {}
    ~Brick();

    Brick(const Brick&) = delete;
    Brick& operator=(const Brick&) = delete;
    Brick(Brick&& other) noexcept;
    Brick& operator=(Brick&& other) noexcept;

    [[nodiscard]] static Brick uniform_solid(BrickPayloadPools& pools, MaterialId material);

    [[nodiscard]] BrickEncoding encoding() const noexcept { return encoding_; }
    [[nodiscard]] std::uint32_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint16_t occupied_count() const noexcept { return occupiedCount_; }
    [[nodiscard]] bool empty() const noexcept { return occupiedCount_ == 0; }

    [[nodiscard]] Bitset512 occupancy() const;
    [[nodiscard]] MaterialId material(std::uint16_t index) const;
    [[nodiscard]] std::array<MaterialId, kBrickVoxelCount> materials() const;
    // Authoritative repair path: replace the complete dense brick and preserve the
    // authority-issued generation exactly. Normal gameplay mutation should use apply().
    void replace_materials(
        const std::array<MaterialId, kBrickVoxelCount>& dense,
        std::uint32_t authoritativeGeneration);
    [[nodiscard]] bool occupied(std::uint16_t index) const { return material(index) != kAirMaterial; }

    BrickApplyResult apply(const BrickMutation& mutation);
    BrickApplyResult set_voxel(std::uint16_t index, MaterialId material);

    // Explicitly repacks a brick to the smallest supported representation. This is useful
    // for background maintenance; removal-only hot paths intentionally avoid unnecessary scans.
    void compact_encoding();

    [[nodiscard]] std::size_t payload_bytes() const noexcept;
    [[nodiscard]] bool validate() const;

private:
    BrickPayloadPools* pools_{};
    void* payload_{};
    BrickEncoding encoding_{BrickEncoding::Empty};
    MaterialId inlineMaterial_{kAirMaterial};
    std::uint16_t occupiedCount_{};
    std::uint32_t generation_{};

    [[nodiscard]] const MaskUniformPayload& mask_uniform() const;
    [[nodiscard]] MaskUniformPayload& mask_uniform();
    [[nodiscard]] const LocalPalette4Payload& local_palette4() const;
    [[nodiscard]] LocalPalette4Payload& local_palette4();
    [[nodiscard]] const Palette8Payload& palette8() const;
    [[nodiscard]] Palette8Payload& palette8();

    [[nodiscard]] std::array<MaterialId, kBrickVoxelCount> dense_materials() const;
    void canonicalize(const std::array<MaterialId, kBrickVoxelCount>& dense);
    void release_payload() noexcept;
    void become_empty() noexcept;
    void become_uniform(MaterialId material) noexcept;
    void become_mask_uniform(const Bitset512& occupancy, MaterialId material);

    static std::uint8_t read_nibble(const LocalPalette4Payload& payload, std::uint16_t index);
    static void write_nibble(LocalPalette4Payload& payload, std::uint16_t index, std::uint8_t value);
};

[[nodiscard]] const char* to_string(BrickEncoding encoding) noexcept;

} // namespace dve
