#pragma once
#include "dve/transform.hpp"
#include <cmath>

namespace dve {
// Positive object-local cell dimensions. Scale belongs to geometry, not rigid poses.
inline Float3 multiply_components(Float3 a, Float3 b) noexcept { return {a.x*b.x,a.y*b.y,a.z*b.z}; }
inline Float3 divide_components(Float3 a, Float3 b) noexcept { return {a.x/b.x,a.y/b.y,a.z/b.z}; }
inline bool valid_voxel_metric(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
        value.x>0 && value.y>0 && value.z>0;
}
inline bool unit_voxel_scale(Float3 value) noexcept { return value.x==1 && value.y==1 && value.z==1; }
inline Float3 voxel_to_world(const RigidTransform& pose, Float3 spacing, Float3 point) noexcept {
    return transform_point(pose,multiply_components(point,spacing));
}
inline Float3 world_to_voxel(const RigidTransform& pose, Float3 spacing, Float3 point) noexcept {
    return divide_components(inverse_transform_point(pose,point),spacing);
}
inline Float3 voxel_normal_to_world(const RigidTransform& pose, Float3 spacing, Float3 normal) noexcept {
    return normalize(transform_vector(pose,divide_components(normal,spacing)));
}
} // namespace dve
