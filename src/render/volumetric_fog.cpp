#include "dve/render/volumetric_fog.hpp"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

#include "dve/render/atmosphere.hpp"
#include "dve/transform.hpp"

namespace dve::render {
namespace {
constexpr double kPi = 3.14159265358979323846;

std::uint32_t hash32(std::uint32_t v) noexcept {
    v = v * 747796405U + 2891336453U;
    const std::uint32_t word = ((v >> ((v >> 28U) + 4U)) ^ v) * 277803737U;
    return (word >> 22U) ^ word;
}

// Integral of the exponential part from t = a to t = c (above the base), density
// rho0 e^{-k (h0 + s t - base)}.
double above(double rho0, double k, double h0, double s, double base, double a, double c) noexcept {
    if (!(c > a)) return 0.0;
    const double ks = k * s;
    const double start = rho0 * std::exp(-k * (h0 + s * a - base));
    if (std::abs(ks * (c - a)) < 1.0e-9) return start * (c - a);
    return start * (1.0 - std::exp(-ks * (c - a))) / ks;
}
} // namespace

float volumetric_fog_density(const RenderEnvironment& e, float h) noexcept {
    const float rho0 = e.volumetricFogDensityPerMeter;
    if (h <= e.volumetricFogBaseHeightMeters) return rho0;
    return rho0 * std::exp(-e.volumetricFogHeightFalloffPerMeter * (h - e.volumetricFogBaseHeightMeters));
}

double volumetric_fog_optical_depth(const RenderEnvironment& e, float startHeight, float dirY, float length) noexcept {
    const double rho0 = e.volumetricFogDensityPerMeter, k = e.volumetricFogHeightFalloffPerMeter, base = e.volumetricFogBaseHeightMeters;
    const double h0 = startHeight, s = dirY, L = length;
    if (!(L > 0.0) || !(rho0 > 0.0)) return 0.0;
    // Split at the base height: constant below it, exponential above.
    double cross = s != 0.0 ? (base - h0) / s : (h0 <= base ? 2.0 * L : -1.0);
    if (s == 0.0) return h0 <= base ? rho0 * L : above(rho0, k, h0, s, base, 0.0, L);
    cross = std::clamp(cross, 0.0, L);
    const bool startsBelow = h0 <= base;
    if (startsBelow) {
        // Below until the crossing (if climbing), then above.
        if (s > 0.0) return rho0 * cross + above(rho0, k, h0, s, base, cross, L);
        return rho0 * L;   // descending from below the base: stays below
    }
    // Above until the crossing (if descending), then below.
    if (s < 0.0) return above(rho0, k, h0, s, base, 0.0, cross) + rho0 * (L - cross);
    return above(rho0, k, h0, s, base, 0.0, L);
}

float henyey_greenstein_phase(float g, float c) noexcept {
    const double g2 = static_cast<double>(g) * g;
    return static_cast<float>((1.0 - g2) / (4.0 * kPi * std::pow(std::max(1.0e-12, 1.0 + g2 - 2.0 * g * c), 1.5)));
}

VolumetricFogSegment integrate_volumetric_fog(const RenderEnvironment& e, Float3 origin, Float3 direction, float length, float metersPerUnit,
                                              const AmbientOcclusionRay& occluder, std::uint32_t steps, float jitter) {
    VolumetricFogSegment out;
    if (!(e.volumetricFogDensityPerMeter > 0.0F) || !(length > 0.0F)) return out;
    metersPerUnit = metersPerUnit > 0.0F && std::isfinite(metersPerUnit) ? metersPerUnit : kDefaultMetersPerVoxel;
    steps = std::max(1U, steps);
    direction = normalize(direction);
    const Float3 sun = normalize(e.sunDirection);
    const Float3 sunLight = sun_irradiance(e);
    const float phase = static_cast<float>(kPi) * henyey_greenstein_phase(e.volumetricFogAnisotropy, dot(direction, sun));
    Float3 ambient{};
    if (e.globalIlluminationMode != GlobalIlluminationMode::Off) {
        ambient = multiply(add(sky_irradiance(e, {0, 1, 0}), sky_irradiance(e, {0, -1, 0})), 0.5F * e.globalIlluminationIntensity);
    }
    const bool shadows = e.shadowMode != ShadowMode::Off && static_cast<bool>(occluder);
    const bool hasAmbient = ambient.x > 0.0F || ambient.y > 0.0F || ambient.z > 0.0F;
    const float strength = std::clamp(e.shadowStrength, 0.0F, 1.0F);
    const float shadowReach = e.shadowMaxDistanceMeters / metersPerUnit;
    const float lengthMeters = length * metersPerUnit;
    const float stepMeters = lengthMeters / static_cast<float>(steps);
    double transmittance = 1.0;
    Float3 sum{};
    for (std::uint32_t i = 0; i < steps; ++i) {
        const float t0 = stepMeters * static_cast<float>(i);
        const float h0 = origin.y * metersPerUnit + direction.y * t0;
        const double tau = volumetric_fog_optical_depth(e, h0, direction.y, stepMeters);
        const double scattered = transmittance * (1.0 - std::exp(-tau));
        if (scattered > 0.0) {
            float visibility = 1.0F, skyVisibility = 1.0F;
            const float tSample = (t0 + stepMeters * std::clamp(jitter, 0.0F, 0.999999F)) / metersPerUnit;
            const Float3 point = add(origin, multiply(direction, tSample));
            if (shadows && (sunLight.x > 0.0F || sunLight.y > 0.0F || sunLight.z > 0.0F)) {
                ++out.shadowRays;
                if (occluder(point, sun, shadowReach)) visibility = 1.0F - strength;
            }
            // Sky light only where the sky is open overhead (one ray straight up): fog in a room
            // is lit by what comes through its openings, not by the whole sky.
            if (hasAmbient && occluder) {
                ++out.shadowRays;
                if (occluder(point, {0.0F, 1.0F, 0.0F}, shadowReach)) skyVisibility = 0.0F;
            }
            const Float3 light = add(multiply(sunLight, phase * visibility), multiply(ambient, skyVisibility));
            sum = add(sum, multiply(Float3{e.volumetricFogAlbedo.x * light.x, e.volumetricFogAlbedo.y * light.y,
                                           e.volumetricFogAlbedo.z * light.z},
                                    static_cast<float>(scattered)));
        }
        transmittance *= std::exp(-tau);
    }
    out.inscattered = sum;
    out.transmittance = static_cast<float>(transmittance);
    return out;
}

VolumetricFogStats apply_volumetric_fog(PolygonRenderTarget& target, const PolygonCamera& camera, const RenderEnvironment& e,
                                        float metersPerUnit, const AmbientOcclusionRay& occluder, std::uint32_t frameSeed,
                                        std::uint32_t threads) {
    VolumetricFogStats total;
    if (!(e.volumetricFogDensityPerMeter > 0.0F) || !target.valid() || !(camera.farPlane > camera.nearPlane)) return total;
    metersPerUnit = metersPerUnit > 0.0F && std::isfinite(metersPerUnit) ? metersPerUnit : kDefaultMetersPerVoxel;
    const Float3 forward = normalize(subtract(camera.target, camera.position));
    Float3 right = normalize(Float3{forward.y * camera.up.z - forward.z * camera.up.y, forward.z * camera.up.x - forward.x * camera.up.z,
                                    forward.x * camera.up.y - forward.y * camera.up.x});
    if (!(length(right) > 0.5F)) right = {1.0F, 0.0F, 0.0F};
    const Float3 up{right.y * forward.z - right.z * forward.y, right.z * forward.x - right.x * forward.z,
                    right.x * forward.y - right.y * forward.x};
    const float tanHalf = std::tan(camera.verticalFieldOfViewRadians * 0.5F);
    const float aspect = static_cast<float>(target.width) / static_cast<float>(target.height);
    const float maxUnits = e.volumetricFogMaxDistanceMeters / metersPerUnit;
    threads = threads == 0U ? std::max(1U, std::thread::hardware_concurrency()) : threads;
    threads = std::clamp(threads, 1U, std::min(16U, target.height));
    std::vector<VolumetricFogStats> partial(threads);
    const auto rows = [&](std::uint32_t begin, std::uint32_t end, VolumetricFogStats& stats) {
        RenderPassBuffers& p = target.passes;
        for (std::uint32_t y = begin; y < end; ++y)
            for (std::uint32_t x = 0; x < target.width; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * target.width + x;
                const float nx = (2.0F * ((static_cast<float>(x) + 0.5F) / static_cast<float>(target.width)) - 1.0F) * tanHalf * aspect;
                const float ny = (1.0F - 2.0F * ((static_cast<float>(y) + 0.5F) / static_cast<float>(target.height))) * tanHalf;
                const Float3 direction = normalize(add(forward, add(multiply(right, nx), multiply(up, ny))));
                float distance = maxUnits;
                if (target.depth[i] < 1.0F) {
                    const float cameraZ = camera.nearPlane + target.depth[i] * (camera.farPlane - camera.nearPlane);
                    distance = std::min(distance, cameraZ * std::sqrt(1.0F + nx * nx + ny * ny));
                }
                const float jitter = static_cast<float>(hash32(static_cast<std::uint32_t>(i) ^ (frameSeed * 0x9E3779B9U)) >> 8U) / 16777216.0F;
                const VolumetricFogSegment fog =
                    integrate_volumetric_fog(e, camera.position, direction, distance, metersPerUnit, occluder, e.volumetricFogSamples, jitter);
                const float t = fog.transmittance;
                Float4& c = target.hdrColor[i];
                c = {c.x * t + fog.inscattered.x, c.y * t + fog.inscattered.y, c.z * t + fog.inscattered.z, c.w};
                if (p.enabled) {
                    for (std::vector<Float3>* pass : {&p.directDiffuse, &p.directSpecular, &p.indirectDiffuse, &p.indirectSpecular,
                                                      &p.emission, &p.transmission, &p.background, &p.fog})
                        (*pass)[i] = multiply((*pass)[i], t);
                    p.fog[i] = add(p.fog[i], fog.inscattered);
                }
                ++stats.pixels;
                stats.samples += e.volumetricFogSamples;
                stats.shadowRays += fog.shadowRays;
            }
    };
    if (threads == 1U) {
        rows(0U, target.height, partial[0]);
    } else {
        std::vector<std::thread> workers;
        for (std::uint32_t k = 0; k < threads; ++k)
            workers.emplace_back(rows, target.height * k / threads, target.height * (k + 1U) / threads, std::ref(partial[k]));
        for (std::thread& worker : workers) worker.join();
    }
    for (const VolumetricFogStats& s : partial) {
        total.pixels += s.pixels;
        total.samples += s.samples;
        total.shadowRays += s.shadowRays;
    }
    return total;
}

} // namespace dve::render
