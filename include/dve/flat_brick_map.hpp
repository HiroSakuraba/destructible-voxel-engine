#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "dve/brick.hpp"

namespace dve {

// Open-addressed key-to-index table with Brick objects stored contiguously.
// The engine never erases brick headers on the authority hot path; empty bricks remain reusable.
// That restriction removes tombstones and keeps lookup/rehash behavior simple and deterministic.
class FlatBrickMap {
public:
    struct Entry {
        BrickKey first{};
        Brick second;

        Entry(BrickKey key, BrickPayloadPools& pools) : first(key), second(pools) {}
        Entry(BrickKey key, Brick&& brick) : first(key), second(std::move(brick)) {}

        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
        Entry(Entry&&) noexcept = default;
        Entry& operator=(Entry&&) noexcept = default;
    };

    using iterator = std::vector<Entry>::iterator;
    using const_iterator = std::vector<Entry>::const_iterator;

    FlatBrickMap() { rehash(16); }

    FlatBrickMap(const FlatBrickMap&) = delete;
    FlatBrickMap& operator=(const FlatBrickMap&) = delete;
    FlatBrickMap(FlatBrickMap&&) noexcept = default;
    FlatBrickMap& operator=(FlatBrickMap&&) noexcept = default;

    [[nodiscard]] iterator begin() noexcept { return entries_.begin(); }
    [[nodiscard]] iterator end() noexcept { return entries_.end(); }
    [[nodiscard]] const_iterator begin() const noexcept { return entries_.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return entries_.end(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return entries_.cbegin(); }
    [[nodiscard]] const_iterator cend() const noexcept { return entries_.cend(); }

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return entries_.capacity(); }
    [[nodiscard]] std::size_t bucket_count() const noexcept { return buckets_.size(); }

    void reserve(std::size_t entryCount) {
        entries_.reserve(entryCount);
        const std::size_t requiredBuckets = next_power_of_two(std::max<std::size_t>(16, (entryCount * 10 + 6) / 7));
        if (requiredBuckets > buckets_.size()) rehash(requiredBuckets);
    }

    [[nodiscard]] iterator find(BrickKey key) noexcept {
        const std::uint32_t index = find_index(key);
        return index == kEmpty ? entries_.end() : entries_.begin() + static_cast<std::ptrdiff_t>(index);
    }

    [[nodiscard]] const_iterator find(BrickKey key) const noexcept {
        const std::uint32_t index = find_index(key);
        return index == kEmpty ? entries_.end() : entries_.begin() + static_cast<std::ptrdiff_t>(index);
    }

    std::pair<iterator, bool> try_emplace(BrickKey key, BrickPayloadPools& pools) {
        if (const std::uint32_t existing = find_index(key); existing != kEmpty) {
            return {entries_.begin() + static_cast<std::ptrdiff_t>(existing), false};
        }
        ensure_insert_capacity();
        const std::uint32_t index = checked_index(entries_.size());
        entries_.emplace_back(key, pools);
        insert_bucket(key, index);
        return {entries_.begin() + static_cast<std::ptrdiff_t>(index), true};
    }

    std::pair<iterator, bool> emplace(BrickKey key, Brick&& brick) {
        if (const std::uint32_t existing = find_index(key); existing != kEmpty) {
            return {entries_.begin() + static_cast<std::ptrdiff_t>(existing), false};
        }
        ensure_insert_capacity();
        const std::uint32_t index = checked_index(entries_.size());
        entries_.emplace_back(key, std::move(brick));
        insert_bucket(key, index);
        return {entries_.begin() + static_cast<std::ptrdiff_t>(index), true};
    }

    [[nodiscard]] std::vector<BrickKey> sorted_keys() const {
        std::vector<BrickKey> keys;
        keys.reserve(entries_.size());
        for (const Entry& entry : entries_) keys.push_back(entry.first);
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    [[nodiscard]] bool validate() const noexcept {
        if (buckets_.empty() || (buckets_.size() & (buckets_.size() - 1)) != 0) return false;
        std::size_t seen = 0;
        for (std::uint32_t bucket : buckets_) {
            if (bucket == kEmpty) continue;
            if (bucket >= entries_.size()) return false;
            if (find_index(entries_[bucket].first) != bucket) return false;
            ++seen;
        }
        return seen == entries_.size();
    }

private:
    static constexpr std::uint32_t kEmpty = std::numeric_limits<std::uint32_t>::max();
    std::vector<Entry> entries_{};
    std::vector<std::uint32_t> buckets_{};

    [[nodiscard]] static constexpr std::uint32_t zigzag(std::int32_t value) noexcept {
        return (static_cast<std::uint32_t>(value) << 1U) ^ static_cast<std::uint32_t>(value >> 31);
    }

    [[nodiscard]] static constexpr std::uint64_t spread21(std::uint32_t value) noexcept {
        std::uint64_t x = value & 0x1FFFFFU;
        x = (x | (x << 32U)) & 0x1F00000000FFFFULL;
        x = (x | (x << 16U)) & 0x1F0000FF0000FFULL;
        x = (x | (x << 8U)) & 0x100F00F00F00F00FULL;
        x = (x | (x << 4U)) & 0x10C30C30C30C30C3ULL;
        x = (x | (x << 2U)) & 0x1249249249249249ULL;
        return x;
    }

    [[nodiscard]] static constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
        x ^= x >> 30U;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27U;
        x *= 0x94D049BB133111EBULL;
        return x ^ (x >> 31U);
    }

    [[nodiscard]] static constexpr std::uint64_t hash_key(BrickKey key) noexcept {
        const std::uint32_t x = zigzag(key.x);
        const std::uint32_t y = zigzag(key.y);
        const std::uint32_t z = zigzag(key.z);
        const std::uint64_t morton = spread21(x) | (spread21(y) << 1U) | (spread21(z) << 2U);
        const std::uint64_t high = (static_cast<std::uint64_t>(x >> 21U) << 42U) ^
                                   (static_cast<std::uint64_t>(y >> 21U) << 21U) ^
                                   static_cast<std::uint64_t>(z >> 21U);
        return mix64(morton ^ mix64(high));
    }

    [[nodiscard]] static std::size_t next_power_of_two(std::size_t value) noexcept {
        std::size_t result = 1;
        while (result < value) result <<= 1U;
        return result;
    }

    [[nodiscard]] static std::uint32_t checked_index(std::size_t index) {
        assert(index < static_cast<std::size_t>(kEmpty));
        return static_cast<std::uint32_t>(index);
    }

    [[nodiscard]] std::uint32_t find_index(BrickKey key) const noexcept {
        if (buckets_.empty()) return kEmpty;
        const std::size_t mask = buckets_.size() - 1;
        std::size_t bucketIndex = static_cast<std::size_t>(hash_key(key)) & mask;
        for (;;) {
            const std::uint32_t entryIndex = buckets_[bucketIndex];
            if (entryIndex == kEmpty) return kEmpty;
            if (entries_[entryIndex].first == key) return entryIndex;
            bucketIndex = (bucketIndex + 1) & mask;
        }
    }

    void ensure_insert_capacity() {
        if ((entries_.size() + 1) * 10 >= buckets_.size() * 7) rehash(buckets_.size() * 2);
    }

    void insert_bucket(BrickKey key, std::uint32_t entryIndex) noexcept {
        const std::size_t mask = buckets_.size() - 1;
        std::size_t bucketIndex = static_cast<std::size_t>(hash_key(key)) & mask;
        while (buckets_[bucketIndex] != kEmpty) bucketIndex = (bucketIndex + 1) & mask;
        buckets_[bucketIndex] = entryIndex;
    }

    void rehash(std::size_t requestedBuckets) {
        const std::size_t bucketCount = next_power_of_two(std::max<std::size_t>(16, requestedBuckets));
        buckets_.assign(bucketCount, kEmpty);
        for (std::uint32_t i = 0; i < entries_.size(); ++i) insert_bucket(entries_[i].first, i);
    }
};

} // namespace dve
