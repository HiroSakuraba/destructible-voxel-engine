#pragma once
// T2: opt-in bridge to Binomial Basis Universal's internal bc7f block encoder.
// Only available when DVE_ENABLE_BC7F_ENCODER=ON, not part of default DVE.
#include "dve/render/texture_residency.hpp"

namespace dve::render {

enum class Bc7fQuality : std::uint8_t { Analytical, PartiallyAnalytical };

// Thread-safe callback factory. The returned encoder works on semantic RGBA8
// mip chains, copies/clamps edge texels, and emits native BC7 (16 bytes/4x4).
// Color-space selection is handled by TextureResidencyManager on the RHI side.
[[nodiscard]] TextureBc7Encoder make_basis_bc7f_encoder(
    Bc7fQuality quality = Bc7fQuality::Analytical);

} // namespace dve::render
