#pragma once
#include <cstdint>
#include <compare>
namespace dve {
struct Int3 {std::int32_t x{}, y{}, z{}; auto operator<=>(const Int3&) const = default;};
struct BrickKey {std::int32_t x{}, y{}, z{}; auto operator<=>(const BrickKey&) const = default;};
constexpr std::int32_t kBrickDim = 8;
constexpr std::int32_t floor_div(std::int32_t a, std::int32_t d) {
 const auto q=a/d, r=a%d; return r<0 ? q-1 : q;
}
}
