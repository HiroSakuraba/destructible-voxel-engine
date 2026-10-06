#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "dve/types.hpp"

namespace dve {

struct Bitset512 {
    std::array<std::uint64_t, 8> words{};

    [[nodiscard]] static constexpr Bitset512 full() {
        Bitset512 value;
        value.words.fill(~std::uint64_t{0});
        return value;
    }

    [[nodiscard]] constexpr bool test(std::uint16_t index) const {
        return (words[index >> 6U] & (std::uint64_t{1} << (index & 63U))) != 0;
    }

    constexpr void set(std::uint16_t index) {
        words[index >> 6U] |= std::uint64_t{1} << (index & 63U);
    }

    constexpr void reset(std::uint16_t index) {
        words[index >> 6U] &= ~(std::uint64_t{1} << (index & 63U));
    }

    constexpr void assign(std::uint16_t index, bool value) {
        value ? set(index) : reset(index);
    }

    [[nodiscard]] constexpr bool any() const {
        for (std::uint64_t word : words) {
            if (word != 0) return true;
        }
        return false;
    }

    [[nodiscard]] constexpr bool none() const { return !any(); }

    [[nodiscard]] constexpr std::uint16_t count() const {
        std::uint32_t total = 0;
        for (std::uint64_t word : words) total += std::popcount(word);
        return static_cast<std::uint16_t>(total);
    }

    // Each word is one z plane, with eight contiguous x bits per y row.
    [[nodiscard]] constexpr bool bounds(Int3& minimum, Int3& maximum) const noexcept {
        minimum = kInt3Max;
        maximum = kInt3Min;
        for (std::int32_t z = 0; z < kBrickDim; ++z) {
            const std::uint64_t plane = words[static_cast<std::size_t>(z)];
            if (plane == 0) continue;
            std::uint64_t columns = plane | (plane >> 32U);
            columns |= columns >> 16U;
            columns |= columns >> 8U;
            columns &= 0xffU;
            minimum = min_components(minimum, {static_cast<std::int32_t>(std::countr_zero(columns)),
                static_cast<std::int32_t>(std::countr_zero(plane) / 8), z});
            maximum = max_components(maximum, {static_cast<std::int32_t>(std::bit_width(columns) - 1),
                static_cast<std::int32_t>((63 - std::countl_zero(plane)) / 8), z});
        }
        return minimum.x != kInt3Max.x;
    }

    template <class Fn>
    constexpr void for_each_set(Fn&& fn) const {
        for (std::uint16_t wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
            std::uint64_t word = words[wordIndex];
            while (word != 0) {
                const unsigned bit = std::countr_zero(word);
                fn(static_cast<std::uint16_t>(wordIndex * 64U + bit));
                word &= word - 1;
            }
        }
    }

    friend constexpr Bitset512 operator~(Bitset512 a) {
        for (auto& word : a.words) word = ~word;
        return a;
    }

    friend constexpr Bitset512 operator&(Bitset512 a, const Bitset512& b) {
        for (std::size_t i = 0; i < a.words.size(); ++i) a.words[i] &= b.words[i];
        return a;
    }

    friend constexpr Bitset512 operator|(Bitset512 a, const Bitset512& b) {
        for (std::size_t i = 0; i < a.words.size(); ++i) a.words[i] |= b.words[i];
        return a;
    }

    friend constexpr Bitset512 operator^(Bitset512 a, const Bitset512& b) {
        for (std::size_t i = 0; i < a.words.size(); ++i) a.words[i] ^= b.words[i];
        return a;
    }

    constexpr Bitset512& operator&=(const Bitset512& b) {
        for (std::size_t i = 0; i < words.size(); ++i) words[i] &= b.words[i];
        return *this;
    }

    constexpr Bitset512& operator|=(const Bitset512& b) {
        for (std::size_t i = 0; i < words.size(); ++i) words[i] |= b.words[i];
        return *this;
    }

    [[nodiscard]] bool operator==(const Bitset512&) const = default;
};

} // namespace dve
