#pragma once

#include "dve/types.hpp"

#include <cmath>

namespace dve::render {

// Ray-origin offset in the style of PBRT's OffsetRayOrigin (PBRT 6.8.6), as
// implemented by NVIDIA's vk_gltf_renderer `safeOffsetRay`
// (shaders/pathtrace_functions.h.slang, Apache-2.0): step from the hit point
// along the offset, then nudge each component one float ULP further in the
// offset's direction. The nudge is the point of the construction: when the
// position is large and the offset is below one ULP of it, the plain addition
// rounds back to the surface point and the next ray self-intersects; the ULP
// step guarantees the origin actually leaves the surface.
[[nodiscard]] inline Float3 offset_ray_origin(Float3 point, Float3 offset) noexcept {
    Float3 origin{point.x + offset.x, point.y + offset.y, point.z + offset.z};
    if (offset.x > 0.0F) origin.x = std::nextafter(origin.x, HUGE_VALF);
    else if (offset.x < 0.0F) origin.x = std::nextafter(origin.x, -HUGE_VALF);
    if (offset.y > 0.0F) origin.y = std::nextafter(origin.y, HUGE_VALF);
    else if (offset.y < 0.0F) origin.y = std::nextafter(origin.y, -HUGE_VALF);
    if (offset.z > 0.0F) origin.z = std::nextafter(origin.z, HUGE_VALF);
    else if (offset.z < 0.0F) origin.z = std::nextafter(origin.z, -HUGE_VALF);
    return origin;
}

[[nodiscard]] inline Float3 offset_ray_origin(Float3 point, Float3 normal, float distance) noexcept {
    return offset_ray_origin(point, Float3{normal.x * distance, normal.y * distance,
                                           normal.z * distance});
}

} // namespace dve::render
