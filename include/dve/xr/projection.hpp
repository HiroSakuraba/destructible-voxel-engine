#pragma once
#include <openxr/openxr.h>
#include <array>
#include <string>
namespace dve::xr {
// Right-handed OpenXR eye space (-Z forward), Vulkan clip depth [0,1],
// Vulkan framebuffer Y down with a positive-height viewport. Column-major,
// multiplied by a column vector. near/far are positive finite distances in metres.
// Does not convert DVE world-space coordinates or construct a view matrix.
bool vulkan_projection(const XrFovf& fov, float nearPlane, float farPlane,
                       std::array<float,16>& matrix, std::string& error);
}
