#include "dve/render/subsurface_transmission.hpp"

#include <algorithm>
#include <cmath>

namespace dve::render {
namespace {
constexpr float kTable[kSubsurfaceTransmissionCosines * kSubsurfaceTransmissionAlbedos * kSubsurfaceTransmissionThicknesses] =
#include "subsurface_transmission_table.inc"
    ;

// Bilinear in (cosine, albedo) at one thickness sample.
float at_thickness(float fm, float fa, std::size_t k) noexcept {
    const std::size_t m0 = std::min<std::size_t>(static_cast<std::size_t>(fm), kSubsurfaceTransmissionCosines - 2U);
    const std::size_t a0 = std::min<std::size_t>(static_cast<std::size_t>(fa), kSubsurfaceTransmissionAlbedos - 2U);
    const float tm = fm - static_cast<float>(m0), ta = fa - static_cast<float>(a0);
    const auto v = [&](std::size_t m, std::size_t a) { return subsurface_transmission_entry(m, a, k); };
    const float low = v(m0, a0) + (v(m0, a0 + 1U) - v(m0, a0)) * ta;
    const float high = v(m0 + 1U, a0) + (v(m0 + 1U, a0 + 1U) - v(m0 + 1U, a0)) * ta;
    return low + (high - low) * tm;
}
}  // namespace

float subsurface_transmission_entry(std::size_t cosine, std::size_t albedo, std::size_t thickness) noexcept {
    return kTable[(cosine * kSubsurfaceTransmissionAlbedos + albedo) * kSubsurfaceTransmissionThicknesses + thickness];
}

float subsurface_slab_transmission(float albedo, float x, float mu) noexcept {
    if (!(x > 0.0F) || !(albedo > 0.0F)) return 0.0F;
    const float fm = (std::clamp(mu, 0.2F, 1.0F) - 0.2F) * 10.0F;
    const float fa = std::clamp(albedo, 0.0F, 1.0F) * 20.0F;
    constexpr std::size_t last = kSubsurfaceTransmissionThicknesses - 1U;
    const float maximum = subsurface_transmission_thickness(last);
    if (x <= maximum) {
        const float fu = std::sqrt(x / maximum) * static_cast<float>(last);
        const std::size_t k0 = std::min<std::size_t>(static_cast<std::size_t>(fu), last - 1U);
        const float tk = fu - static_cast<float>(k0);
        const float a = at_thickness(fm, fa, k0), b = at_thickness(fm, fa, k0 + 1U);
        return std::max(0.0F, a + (b - a) * tk);
    }
    const float v16 = at_thickness(fm, fa, last), v15 = at_thickness(fm, fa, last - 1U);
    if (!(v16 > 0.0F)) return 0.0F;
    const float span = maximum - subsurface_transmission_thickness(last - 1U);
    const float decay = v15 > v16 ? std::log(v15 / v16) / span : 0.25F;
    return v16 * std::exp(-decay * (x - maximum));
}

} // namespace dve::render
