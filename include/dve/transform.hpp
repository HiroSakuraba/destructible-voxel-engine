#pragma once

#include "dve/types.hpp"

namespace dve {

struct Quaternion {
    float x{};
    float y{};
    float z{};
    float w{1.0F};
};

struct RigidTransform {
    Float3 position{};
    Quaternion rotation{};
};

struct MovingRigidTransform {
    RigidTransform previous{};
    RigidTransform current{};
};

[[nodiscard]] Float3 add(Float3 a, Float3 b) noexcept;
[[nodiscard]] Float3 subtract(Float3 a, Float3 b) noexcept;
[[nodiscard]] Float3 multiply(Float3 value, float scalar) noexcept;
[[nodiscard]] float dot(Float3 a, Float3 b) noexcept;
[[nodiscard]] float length_squared(Float3 value) noexcept;
[[nodiscard]] float length(Float3 value) noexcept;
[[nodiscard]] Float3 normalize(Float3 value) noexcept;

[[nodiscard]] Quaternion normalize(Quaternion value) noexcept;
[[nodiscard]] Quaternion conjugate(Quaternion value) noexcept;
[[nodiscard]] Quaternion multiply(Quaternion a, Quaternion b) noexcept;
[[nodiscard]] Quaternion quaternion_from_axis_angle(Float3 axis, float radians) noexcept;
[[nodiscard]] Quaternion quaternion_from_euler_xyz(Float3 radians) noexcept;
[[nodiscard]] Float3 quaternion_to_euler_xyz(Quaternion rotation) noexcept;
[[nodiscard]] Float3 rotate(Quaternion rotation, Float3 value) noexcept;

[[nodiscard]] RigidTransform make_rigid_transform(Float3 position, Quaternion rotation) noexcept;
[[nodiscard]] RigidTransform compose_rigid_transforms(
    const RigidTransform& parent, const RigidTransform& local) noexcept;
[[nodiscard]] RigidTransform relative_rigid_transform(
    const RigidTransform& parent, const RigidTransform& world) noexcept;
[[nodiscard]] Float3 transform_point(const RigidTransform& transform, Float3 point) noexcept;
[[nodiscard]] Float3 transform_vector(const RigidTransform& transform, Float3 vector) noexcept;
[[nodiscard]] Float3 inverse_transform_point(const RigidTransform& transform, Float3 point) noexcept;
[[nodiscard]] Float3 inverse_transform_vector(const RigidTransform& transform, Float3 vector) noexcept;
[[nodiscard]] RigidTransform interpolate_rigid_transform(
    const RigidTransform& previous,
    const RigidTransform& current,
    float alpha) noexcept;

} // namespace dve
