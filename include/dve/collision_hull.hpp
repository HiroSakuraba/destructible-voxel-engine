#pragma once
#include "dve/component.hpp"
namespace dve {
inline constexpr std::string_view kCollisionHullComponent="dve.collision_hull";
[[nodiscard]] bool validate_collision_hull_points(std::span<const Float3>) noexcept;
[[nodiscard]] std::string encode_collision_hull_points(std::span<const Float3>);
[[nodiscard]] std::optional<std::vector<Float3>> decode_collision_hull_points(std::string_view);
[[nodiscard]] std::optional<std::vector<Float3>> collision_hull_component_points(const Component&);
[[nodiscard]] std::vector<Float3> active_collision_hull_points(std::span<const Component>);
[[nodiscard]] Component bake_collision_hull_component(Component,Float3 metric);
} // namespace dve
