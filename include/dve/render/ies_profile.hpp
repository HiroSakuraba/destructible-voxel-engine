#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace dve::render {

// Azimuthally averaged candela distribution parsed from an IESNA LM-63 (.ies)
// photometric file, in the shape real-time engines (and EXT_lights_ies) use:
// samples[i] is the relative intensity at i degrees of vertical angle off the
// light's axis (its direction), peak normalized to 1. The light's own
// intensity still carries the absolute scale; the profile only reshapes it
// angularly. Azimuthal variation is averaged away, as most fixtures are
// axially symmetric; TILT data is consumed but not applied.
//
// Parser structure follows NVIDIA's vk_gltf_renderer ies_profile.cpp
// (Apache-2.0, Copyright (c) 2026 NVIDIA CORPORATION), adapted to DVE types.
inline constexpr std::size_t kIesProfileSampleCount = 181U;

struct IesProfile {
    std::array<float, kIesProfileSampleCount> samples{};
    // Relative intensity at a vertical angle off the light axis, linearly
    // interpolated between the 1-degree samples; angles clamp to [0, 180].
    [[nodiscard]] float sample(float angleRadians) const noexcept;
    [[nodiscard]] bool empty() const noexcept {
        for (const float s : samples)
            if (s > 0.0F) return false;
        return true;
    }
};

// Parses the file text. Returns false with an error message on malformed
// input, leaving `out` untouched.
[[nodiscard]] bool parse_ies_profile(std::string_view text, IesProfile& out,
                                     std::string* error = nullptr);

} // namespace dve::render
