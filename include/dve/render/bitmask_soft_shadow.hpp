#pragma once

// Bitmask soft shadows for a directional sun (Schwarz & Stamminger, "Bitmask Soft Shadows", 2007),
// the CPU mirror of SampleCascadedShadowBitmask in shaders/common/cascaded_shadow_map.hlsli.
//
// The sun is a disc of angular radius theta. A shadow-map texel in front of a receiver hides the
// part of that disc it covers when seen from the receiver: its square, projected back from its
// depth onto the disc. 64 disc samples are kept as a bitmask; each blocking texel ORs in the
// samples it covers, over all texels and both depth layers (static and dynamic). Visibility is
// the uncovered fraction of the disc. OR-ing is the union of what each occluder hides, so two
// penumbrae that meet darken the gap between them more than either alone (the shadow blister),
// and penumbra width follows the blocker distance and the sun's size.
//
// The samples are a rook pattern on [-1,1]^2 (sample i: x slot i, y slot (19 i + 9) mod 64; slot
// centres (slot + 0.5) / 32 - 1); the 52 inside the unit disc count. Bits are in x-slot order, so a
// texel's square costs a run of bits AND a difference of two prefix masks.

#include <array>
#include <cstdint>
#include <span>

namespace dve::render {

inline constexpr std::uint32_t kSunDiscSampleCount = 64U;
[[nodiscard]] constexpr std::uint32_t sun_disc_y_slot(std::uint32_t sample) noexcept { return (19U * sample + 9U) % 64U; }
[[nodiscard]] constexpr float sun_disc_slot_centre(std::uint32_t slot) noexcept {
    return (static_cast<float>(slot) + 0.5F) / 32.0F - 1.0F;
}
// Samples inside the unit disc (52 of 64).
[[nodiscard]] std::uint64_t sun_disc_valid_mask() noexcept;
// [k]: samples whose y slot is below k (k = 0..64); kSunDiscYBelow in the HLSL.
[[nodiscard]] const std::array<std::uint64_t, 65>& sun_disc_y_below() noexcept;
// Samples inside the box [x0, x1] x [y0, y1] (disc units; the sun disc is the unit disc).
[[nodiscard]] std::uint64_t sun_disc_box_mask(float x0, float x1, float y0, float y1) noexcept;

// One depth layer of one cascade: normalized light depth (0 nearest the sun), row-major, texel
// (x, y) covering [x, x+1) x [y, y+1) in texel coordinates.
struct SoftShadowDepthLayer {
    std::uint32_t width{};
    std::uint32_t height{};
    std::span<const float> depth;
};

struct BitmaskSoftShadowParameters {
    float texelMeters{};          // texel size in the light plane
    float depthRangeMeters{};     // metres per unit of normalized depth
    float tanSunAngularRadius{};
    std::uint32_t searchTexels{6};   // 1..16
};

// Visibility (0..1) at a receiver at continuous texel coordinates (texelX, texelY) with normalized
// depth receiverDepth (receiver bias already applied), over one or two layers.
[[nodiscard]] float bitmask_soft_shadow_visibility(std::span<const SoftShadowDepthLayer> layers, float texelX,
                                                   float texelY, float receiverDepth,
                                                   const BitmaskSoftShadowParameters& parameters) noexcept;

// For comparison: the existing filter, a (2r+1)^2 point-comparison kernel per layer, darker layer
// wins (SampleCascadedShadowPcf without the hardware bilinear comparison).
[[nodiscard]] float pcf_shadow_visibility(std::span<const SoftShadowDepthLayer> layers, float texelX, float texelY,
                                          float receiverDepth, std::uint32_t radius) noexcept;

} // namespace dve::render
