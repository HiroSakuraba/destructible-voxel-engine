#pragma once

// View > Lighting Only in the software viewport: what the scene's light does to a white face
// with this normal, without materials. The sun term is N.L with the environment's sun
// direction, colour and intensity; the ambient term blends the sky and ground colours by how
// much the face points up. Exposure scales the sum, which is clamped to [0, 1]. No shadows or
// bounce light: those are the GPU renderer's.

#include <algorithm>
#include <cmath>

#include "dve/render_environment.hpp"

namespace dve::editor {

[[nodiscard]] inline Float3 lighting_only_color(Float3 normal, const RenderEnvironment& environment) noexcept {
    const auto unit = [](Float3 v, Float3 fallback) {
        const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (!(length > 1.0e-6F) || !std::isfinite(length)) return fallback;
        return Float3{v.x / length, v.y / length, v.z / length};
    };
    const Float3 n = unit(normal, {0.0F, 1.0F, 0.0F});
    const Float3 sun = unit(environment.sunDirection, {0.0F, 1.0F, 0.0F});
    const float nDotL = std::max(0.0F, n.x * sun.x + n.y * sun.y + n.z * sun.z);
    const float up = 0.5F + 0.5F * n.y;
    constexpr float kAmbientGain = 1.6F;   // sky/ground colours are dim radiances (about 0.2)
    constexpr float kSunGain = 0.32F;      // default sun intensity 2.4 -> about 0.77 at N.L = 1
    const auto channel = [&](float sky, float ground, float sunColor) {
        const float ambient = (ground + (sky - ground) * up) * kAmbientGain;
        const float direct = sunColor * environment.sunIntensity * kSunGain * nDotL;
        return std::clamp((ambient + direct) * environment.exposure, 0.0F, 1.0F);
    };
    return {channel(environment.skyColor.x, environment.groundColor.x, environment.sunColor.x),
            channel(environment.skyColor.y, environment.groundColor.y, environment.sunColor.y),
            channel(environment.skyColor.z, environment.groundColor.z, environment.sunColor.z)};
}

} // namespace dve::editor
