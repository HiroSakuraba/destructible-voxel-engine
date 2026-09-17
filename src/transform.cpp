#include "dve/transform.hpp"

#include <algorithm>
#include <cmath>

namespace dve {

Float3 add(Float3 a, Float3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Float3 subtract(Float3 a, Float3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Float3 multiply(Float3 value, float scalar) noexcept {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}
float dot(Float3 a, Float3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length_squared(Float3 value) noexcept { return dot(value, value); }
float length(Float3 value) noexcept { return std::sqrt(length_squared(value)); }
Float3 normalize(Float3 value) noexcept {
    const float magnitude = length(value);
    return magnitude > 0.0F ? multiply(value, 1.0F / magnitude) : Float3{};
}

Quaternion normalize(Quaternion value) noexcept {
    const float norm = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w);
    if (!(norm > 0.0F)) return {};
    const float inverse = 1.0F / norm;
    return {value.x * inverse, value.y * inverse, value.z * inverse, value.w * inverse};
}

Quaternion conjugate(Quaternion value) noexcept { return {-value.x, -value.y, -value.z, value.w}; }

Quaternion multiply(Quaternion a, Quaternion b) noexcept {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quaternion quaternion_from_axis_angle(Float3 axis, float radians) noexcept {
    axis = normalize(axis);
    if (!(length_squared(axis) > 0.0F) || !std::isfinite(radians)) return {};
    const float half = radians * 0.5F;
    const float sine = std::sin(half);
    return normalize({axis.x * sine, axis.y * sine, axis.z * sine, std::cos(half)});
}

Quaternion quaternion_from_euler_xyz(Float3 radians) noexcept {
    const Quaternion qx = quaternion_from_axis_angle({1.0F, 0.0F, 0.0F}, radians.x);
    const Quaternion qy = quaternion_from_axis_angle({0.0F, 1.0F, 0.0F}, radians.y);
    const Quaternion qz = quaternion_from_axis_angle({0.0F, 0.0F, 1.0F}, radians.z);
    return normalize(multiply(multiply(qz, qy), qx));
}

Float3 quaternion_to_euler_xyz(Quaternion rotation) noexcept {
    rotation = normalize(rotation);
    const float sinrCosp = 2.0F * (rotation.w * rotation.x + rotation.y * rotation.z);
    const float cosrCosp = 1.0F - 2.0F * (rotation.x * rotation.x + rotation.y * rotation.y);
    const float x = std::atan2(sinrCosp, cosrCosp);

    const float sinp = 2.0F * (rotation.w * rotation.y - rotation.z * rotation.x);
    const float y = std::abs(sinp) >= 1.0F ? std::copysign(1.5707963267948966F, sinp) : std::asin(sinp);

    const float sinyCosp = 2.0F * (rotation.w * rotation.z + rotation.x * rotation.y);
    const float cosyCosp = 1.0F - 2.0F * (rotation.y * rotation.y + rotation.z * rotation.z);
    const float z = std::atan2(sinyCosp, cosyCosp);
    return {x, y, z};
}

Float3 rotate(Quaternion rotation, Float3 value) noexcept {
    rotation = normalize(rotation);
    const Quaternion pure{value.x, value.y, value.z, 0.0F};
    const Quaternion result = multiply(multiply(rotation, pure), conjugate(rotation));
    return {result.x, result.y, result.z};
}

RigidTransform make_rigid_transform(Float3 position, Quaternion rotation) noexcept {
    return {position, normalize(rotation)};
}

RigidTransform compose_rigid_transforms(
    const RigidTransform& parent, const RigidTransform& local) noexcept {
    const Quaternion parentRotation = normalize(parent.rotation);
    return make_rigid_transform(
        add(parent.position, rotate(parentRotation, local.position)),
        multiply(parentRotation, normalize(local.rotation)));
}

RigidTransform relative_rigid_transform(
    const RigidTransform& parent, const RigidTransform& world) noexcept {
    const Quaternion inverseParent = conjugate(normalize(parent.rotation));
    return make_rigid_transform(
        rotate(inverseParent, subtract(world.position, parent.position)),
        multiply(inverseParent, normalize(world.rotation)));
}

Float3 transform_point(const RigidTransform& transform, Float3 point) noexcept {
    return add(transform.position, rotate(transform.rotation, point));
}

Float3 transform_vector(const RigidTransform& transform, Float3 vector) noexcept {
    return rotate(transform.rotation, vector);
}

Float3 inverse_transform_point(const RigidTransform& transform, Float3 point) noexcept {
    return rotate(conjugate(normalize(transform.rotation)), subtract(point, transform.position));
}

Float3 inverse_transform_vector(const RigidTransform& transform, Float3 vector) noexcept {
    return rotate(conjugate(normalize(transform.rotation)), vector);
}

RigidTransform interpolate_rigid_transform(
    const RigidTransform& previous,
    const RigidTransform& current,
    float alpha) noexcept {
    alpha = std::clamp(alpha, 0.0F, 1.0F);
    const Float3 position = add(previous.position, multiply(subtract(current.position, previous.position), alpha));
    Quaternion a = normalize(previous.rotation);
    Quaternion b = normalize(current.rotation);
    float cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (cosine < 0.0F) {
        b = {-b.x, -b.y, -b.z, -b.w};
        cosine = -cosine;
    }
    Quaternion rotation;
    if (cosine > 0.9995F) {
        rotation = normalize({
            a.x + alpha * (b.x - a.x),
            a.y + alpha * (b.y - a.y),
            a.z + alpha * (b.z - a.z),
            a.w + alpha * (b.w - a.w),
        });
    } else {
        const float angle = std::acos(std::clamp(cosine, -1.0F, 1.0F));
        const float sine = std::sin(angle);
        const float weightA = std::sin((1.0F - alpha) * angle) / sine;
        const float weightB = std::sin(alpha * angle) / sine;
        rotation = {
            weightA * a.x + weightB * b.x,
            weightA * a.y + weightB * b.y,
            weightA * a.z + weightB * b.z,
            weightA * a.w + weightB * b.w,
        };
    }
    return {position, normalize(rotation)};
}

} // namespace dve
