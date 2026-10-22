#include "dve/render/ambient_occlusion.hpp"

#include <algorithm>
#include <cmath>

#include "dve/render/ray_lighting_reference.hpp"
#include "dve/transform.hpp"

namespace dve::render {
Float3 ambient_occlusion_ray_direction(Float3 normal, std::uint32_t sampleIndex, std::uint32_t sampleCount,
                                       std::uint32_t pixelIndex, std::uint32_t frameSeed) noexcept {
    const Float2 random = rotated_hammersley_2d(sampleIndex, sampleCount, hash_2d(pcg_hash_32(pixelIndex ^ frameSeed)));
    return cosine_weighted_hemisphere_sample(normal, random);
}

float ambient_occlusion_origin_bias(const RenderEnvironment& environment, float metersPerVoxel) noexcept {
    return std::max(1.0e-3F, meters_to_voxel_units(environment.shadowBiasMeters, metersPerVoxel));
}

float reference_ambient_occlusion(Float3 point, Float3 normal, std::uint32_t pixelIndex, std::uint32_t frameSeed,
                                  const RenderEnvironment& environment, float metersPerVoxel,
                                  const AmbientOcclusionRay& nearest) {
    const std::uint32_t samples = std::min(environment.ambientOcclusionSamples, kMaximumAmbientOcclusionSamples);
    const float maximum = meters_to_voxel_units(environment.ambientOcclusionMaxDistanceMeters, metersPerVoxel);
    if (samples == 0U || !(maximum > 0.0F)) return 1.0F;
    const Float3 n = normalize(normal);
    const Float3 origin = add(point, multiply(n, ambient_occlusion_origin_bias(environment, metersPerVoxel)));
    float occlusion = 0.0F;
    for (std::uint32_t i = 0; i < samples; ++i) {
        const auto hit = nearest(origin, ambient_occlusion_ray_direction(n, i, samples, pixelIndex, frameSeed), maximum);
        if (!hit) continue;
        const float proximity = 1.0F - std::clamp(*hit / maximum, 0.0F, 1.0F);
        occlusion += proximity * proximity;
    }
    return std::clamp(1.0F - occlusion / static_cast<float>(samples), 0.0F, 1.0F);
}

} // namespace dve::render
