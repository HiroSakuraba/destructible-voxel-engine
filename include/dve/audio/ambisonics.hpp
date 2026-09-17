#pragma once

#include <cstddef>
#include <span>

#include "dve/audio/spatializer.hpp"

namespace dve::audio {

// Compact ACN-like/SN3D real spherical-harmonic field used for diffuse ambience and reflections.
// Orders 0-2 are supported (1, 4, or 9 channels). Buffers are frame-major interleaved fields.
[[nodiscard]] constexpr std::size_t ambisonic_channel_count(std::size_t order) noexcept {
    return order <= 2U ? (order + 1U) * (order + 1U) : 0U;
}

class AmbisonicFieldProcessor {
public:
    explicit AmbisonicFieldProcessor(std::size_t order = 1U) noexcept;
    [[nodiscard]] std::size_t order() const noexcept { return order_; }
    [[nodiscard]] std::size_t channels() const noexcept { return ambisonic_channel_count(order_); }

    // Adds a mono source into an existing field. No allocation is performed.
    bool encode_add(std::span<const float> monoInput, std::span<float> interleavedField,
                    AudioVec3 direction, float gain = 1.0F) const noexcept;

    // Decodes the field to stereo virtual speakers. The output is overwritten.
    bool decode_stereo(std::span<const float> interleavedField,
                       std::span<float> interleavedStereo) const noexcept;

private:
    std::size_t order_{1U};
};

} // namespace dve::audio
