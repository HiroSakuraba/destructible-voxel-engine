#include "dve/render_environment.hpp"

#include <cmath>

namespace dve {
namespace {
bool finite3(Float3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
}

bool RenderEnvironment::validate(std::string* error) const noexcept {
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    switch (globalIlluminationMode) {
        case GlobalIlluminationMode::Off:
        case GlobalIlluminationMode::AmbientHemisphere:
        case GlobalIlluminationMode::VoxelOneBounce: break;
        default: return fail("globalIlluminationMode is invalid");
    }
    switch (shadowMode) {
        case ShadowMode::Off:
        case ShadowMode::Hard:
        case ShadowMode::Soft:
        case ShadowMode::Contact:
        case ShadowMode::Hybrid: break;
        default: return fail("shadowMode is invalid");
    }
    if (!finite3(sunDirection)) return fail("sunDirection must be finite");
    if (sunDirection.x == 0.0F && sunDirection.y == 0.0F && sunDirection.z == 0.0F)
        return fail("sunDirection must not be the zero vector");
    if (!std::isfinite(sunIntensity) || sunIntensity < 0.0F) return fail("sunIntensity must be finite and non-negative");
    if (!finite3(sunColor) || sunColor.x < 0.0F || sunColor.y < 0.0F || sunColor.z < 0.0F)
        return fail("sunColor must be finite and non-negative");
    if (!std::isfinite(subsurfaceMaxDistanceMeters) || subsurfaceMaxDistanceMeters < 0.0F)
        return fail("subsurfaceMaxDistanceMeters must be finite and non-negative");
    if (!finite3(skyColor) || skyColor.x < 0.0F || skyColor.y < 0.0F || skyColor.z < 0.0F)
        return fail("skyColor must be finite and non-negative");
    if (!std::isfinite(exposure) || exposure <= 0.0F) return fail("exposure must be finite and positive");
    if (!finite3(groundColor) || groundColor.x < 0.0F || groundColor.y < 0.0F || groundColor.z < 0.0F)
        return fail("groundColor must be finite and non-negative");
    if (!finite3(globalTint) || globalTint.x < 0.0F || globalTint.y < 0.0F || globalTint.z < 0.0F)
        return fail("globalTint must be finite and non-negative");
    if (!std::isfinite(bloomThreshold) || bloomThreshold < 0.0F) return fail("bloomThreshold must be finite and non-negative");
    if (!std::isfinite(bloomIntensity) || bloomIntensity < 0.0F) return fail("bloomIntensity must be finite and non-negative");
    if (!std::isfinite(bloomRadius) || bloomRadius < 0.0F) return fail("bloomRadius must be finite and non-negative");
    if (!std::isfinite(globalIlluminationIntensity) || globalIlluminationIntensity < 0.0F || globalIlluminationIntensity > 4.0F)
        return fail("globalIlluminationIntensity must be finite and in [0,4]");
    if (!std::isfinite(globalIlluminationMaxDistanceMeters) || globalIlluminationMaxDistanceMeters <= 0.0F)
        return fail("globalIlluminationMaxDistanceMeters must be finite and positive");
    if (globalIlluminationSamples == 0U || globalIlluminationSamples > 16U)
        return fail("globalIlluminationSamples must be in [1,16]");
    if (!std::isfinite(shadowStrength) || shadowStrength < 0.0F || shadowStrength > 1.0F)
        return fail("shadowStrength must be finite and in [0,1]");
    if (!std::isfinite(shadowSoftnessRadians) || shadowSoftnessRadians < 0.0F || shadowSoftnessRadians > 0.25F)
        return fail("shadowSoftnessRadians must be finite and in [0,0.25]");
    if (shadowSamples == 0U || shadowSamples > 16U) return fail("shadowSamples must be in [1,16]");
    if (!std::isfinite(shadowMaxDistanceMeters) || shadowMaxDistanceMeters <= 0.0F)
        return fail("shadowMaxDistanceMeters must be finite and positive");
    if (!std::isfinite(contactShadowDistanceMeters) || contactShadowDistanceMeters <= 0.0F)
        return fail("contactShadowDistanceMeters must be finite and positive");
    if (!std::isfinite(shadowBiasMeters) || shadowBiasMeters < 0.0F || shadowBiasMeters > 1.0F)
        return fail("shadowBiasMeters must be finite and in [0,1]");
    return true;
}

} // namespace dve
