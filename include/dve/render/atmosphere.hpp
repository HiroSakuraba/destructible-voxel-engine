#pragma once

// A physical sky: single and multiple scattering in an Earth-like atmosphere, after Hillaire,
// "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (EGSR 2020), with
// Bruneton's Earth parameters and transmittance parameterisation.
//
// The air is a sphere shell (planet radius 6360 km, top 6460 km) holding three media:
//   Rayleigh  - air molecules: scattering ~ 1/wavelength^4 (blue sky, red sunsets), scale height
//               8 km, phase 3/(16 pi) (1 + cos^2);
//   Mie       - aerosols: grey scattering and absorption (haze, the halo round the sun), scale
//               height 1.2 km, Cornette-Shanks phase with anisotropy g;
//   ozone     - absorption only, a tent profile peaking at 25 km (the blue of twilight).
//
// Everything here is per unit of sunlight arriving at the top of the atmosphere (irradiance 1 on
// a surface facing the sun); multiply by the sun's irradiance.
//
// Lookup tables (atmosphere_model caches them):
//   transmittance (256 x 64): light kept along a ray from altitude r at zenith cosine mu to the
//     top of the atmosphere (zero when the ray hits the ground);
//   multiple scattering (32 x 32): Hillaire's psi_ms, all orders beyond the first, as an isotropic
//     source per unit scattering coefficient;
//   sky view (192 x 128) at the observer's altitude: radiance of the sky (or of the ground below
//     the horizon) by azimuth from the sun and zenith angle, spending resolution near the horizon;
//   sky irradiance (32 x 16): light on a surface by its normal's elevation and azimuth from the
//     sun, the sky-view radiance integrated over the normal's hemisphere.
//
// The direct functions (integrate_*) march the same integrals with as many steps as asked: they
// are what the tables are checked against.

#include <cstdint>
#include <memory>
#include <vector>

#include "dve/render_environment.hpp"
#include "dve/types.hpp"

namespace dve::render {

struct AtmosphereParameters {
    float bottomRadiusMeters{6360.0e3F};
    float topRadiusMeters{6460.0e3F};
    Float3 rayleighScattering{5.802e-6F, 13.558e-6F, 33.1e-6F};   // per metre at sea level
    float rayleighScaleHeightMeters{8000.0F};
    float mieScattering{3.996e-6F};
    float mieAbsorption{4.40e-6F};
    float mieScaleHeightMeters{1200.0F};
    float mieAnisotropy{0.8F};
    Float3 ozoneAbsorption{0.650e-6F, 1.881e-6F, 0.085e-6F};      // per metre at the peak
    float ozoneCenterAltitudeMeters{25000.0F};
    float ozoneHalfWidthMeters{15000.0F};
    float groundAlbedo{0.3F};

    [[nodiscard]] bool operator==(const AtmosphereParameters& o) const noexcept {
        const auto same3 = [](Float3 a, Float3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
        return bottomRadiusMeters == o.bottomRadiusMeters && topRadiusMeters == o.topRadiusMeters &&
               same3(rayleighScattering, o.rayleighScattering) && rayleighScaleHeightMeters == o.rayleighScaleHeightMeters &&
               mieScattering == o.mieScattering && mieAbsorption == o.mieAbsorption && mieScaleHeightMeters == o.mieScaleHeightMeters &&
               mieAnisotropy == o.mieAnisotropy && same3(ozoneAbsorption, o.ozoneAbsorption) &&
               ozoneCenterAltitudeMeters == o.ozoneCenterAltitudeMeters && ozoneHalfWidthMeters == o.ozoneHalfWidthMeters &&
               groundAlbedo == o.groundAlbedo;
    }
};

// The parameters RenderEnvironment's atmosphere settings select.
[[nodiscard]] AtmosphereParameters atmosphere_parameters(const RenderEnvironment& environment) noexcept;

// Medium at an altitude (metres above the ground).
struct AtmosphereMedium {
    Float3 scatteringRayleigh{};
    float scatteringMie{};
    Float3 extinction{};
};
[[nodiscard]] AtmosphereMedium atmosphere_medium(const AtmosphereParameters& p, float altitudeMeters) noexcept;
[[nodiscard]] float rayleigh_phase(float cosTheta) noexcept;
[[nodiscard]] float cornette_shanks_phase(float g, float cosTheta) noexcept;

// Distance from radius r along zenith cosine mu to the top of the atmosphere / to the ground (-1
// when the ray misses the ground).
[[nodiscard]] float distance_to_top(const AtmosphereParameters& p, float r, float mu) noexcept;
[[nodiscard]] float distance_to_ground(const AtmosphereParameters& p, float r, float mu) noexcept;

// Tables that depend only on the parameters.
class AtmosphereTables {
public:
    explicit AtmosphereTables(const AtmosphereParameters& parameters);
    [[nodiscard]] const AtmosphereParameters& parameters() const noexcept { return parameters_; }
    [[nodiscard]] Float3 transmittance_to_top(float r, float mu) const noexcept;
    [[nodiscard]] Float3 multiple_scattering(float r, float muSun) const noexcept;
    // Sunlight reaching radius r from a sun at zenith cosine muSun (zero once the planet hides it).
    [[nodiscard]] Float3 sun_transmittance(float r, float muSun) const noexcept;

    static constexpr std::uint32_t kTransmittanceWidth = 256U, kTransmittanceHeight = 64U;
    static constexpr std::uint32_t kMultipleScatteringSize = 32U;
    [[nodiscard]] const std::vector<Float3>& transmittance_table() const noexcept { return transmittance_; }
    [[nodiscard]] const std::vector<Float3>& multiple_scattering_table() const noexcept { return multipleScattering_; }

private:
    AtmosphereParameters parameters_;
    std::vector<Float3> transmittance_;
    std::vector<Float3> multipleScattering_;
};

// The sky seen from one altitude with the sun at one elevation (the sun's azimuth is free: the
// tables are relative to it).
class AtmosphereModel {
public:
    AtmosphereModel(std::shared_ptr<const AtmosphereTables> tables, float observerAltitudeMeters, Float3 sunDirection);

    [[nodiscard]] const AtmosphereTables& tables() const noexcept { return *tables_; }
    [[nodiscard]] const AtmosphereParameters& parameters() const noexcept { return tables_->parameters(); }
    [[nodiscard]] float observer_radius() const noexcept { return observerRadius_; }
    [[nodiscard]] Float3 sun_direction() const noexcept { return sun_; }

    // Radiance seen from the observer along `direction` (world, +y up), per unit top-of-atmosphere
    // sun irradiance (sky-view table; no sun disc). Below the horizon: the lit ground and the air
    // in front of it.
    [[nodiscard]] Float3 sky_radiance(Float3 direction) const noexcept;
    // Irradiance from the sky (not the sun disc) on a surface at the observer facing `normal`.
    [[nodiscard]] Float3 sky_irradiance(Float3 normal) const noexcept;
    // Sunlight at the observer.
    [[nodiscard]] Float3 sun_transmittance() const noexcept;
    // Light scattered toward the camera along a straight segment of `lengthMeters` starting
    // `altitudeMeters` above the ground in `direction`, and the transmittance across it.
    struct Segment {
        Float3 inscattered{};
        Float3 transmittance{1.0F, 1.0F, 1.0F};
    };
    [[nodiscard]] Segment segment(float altitudeMeters, Float3 direction, float lengthMeters, std::uint32_t steps = 16U) const noexcept;

    static constexpr std::uint32_t kSkyViewWidth = 192U, kSkyViewHeight = 128U;
    static constexpr std::uint32_t kIrradianceWidth = 32U, kIrradianceHeight = 16U;
    [[nodiscard]] const std::vector<Float3>& sky_view_table() const noexcept { return skyView_; }

private:
    std::shared_ptr<const AtmosphereTables> tables_;
    float observerRadius_{};
    Float3 sun_{};
    std::vector<Float3> skyView_;
    std::vector<Float3> irradiance_;
};

struct AtmosphereUv {
    float u{};
    float v{};
};
// Parameterisations shared with the GPU tables, uv in [0,1]^2. The transmittance and irradiance
// tables are node-centred (texel i holds uv = i / (n - 1)); the sky-view and multiple-scattering
// tables are cell-centred (texel i holds (i + 0.5) / n).
[[nodiscard]] AtmosphereUv transmittance_uv(const AtmosphereParameters& p, float r, float mu) noexcept;
void transmittance_r_mu(const AtmosphereParameters& p, AtmosphereUv uv, float& r, float& mu) noexcept;
[[nodiscard]] AtmosphereUv sky_view_uv(const AtmosphereParameters& p, float r, float zenithCos, float azimuthFromSun) noexcept;
void sky_view_angles(const AtmosphereParameters& p, float r, AtmosphereUv uv, float& zenithCos, float& azimuthFromSun) noexcept;

// Brute-force references: march the integrals directly (no tables except where named).
[[nodiscard]] Float3 integrate_transmittance(const AtmosphereParameters& p, float r, float mu, float lengthMeters,
                                             std::uint32_t steps) noexcept;
// Sky radiance along a ray from radius r, single scattering with the multiple-scattering term
// taken from `model` (pass nullptr for single scattering only), plus the lit ground it may hit.
// view and sun are unit vectors in the observer's frame (+y up at radius r); `tables` supplies the
// sun transmittance and the multiple-scattering source (nullptr: single scattering with the sun
// transmittance integrated directly).
[[nodiscard]] Float3 integrate_sky_radiance(const AtmosphereParameters& p, const AtmosphereTables* tables, float r, Float3 view,
                                            Float3 sun, std::uint32_t steps) noexcept;

// The cached model for an environment's atmosphere: tables per parameter set, sky per observer
// altitude and sun direction (thread-safe; a few recent ones are kept). Null in Colors mode.
[[nodiscard]] std::shared_ptr<const AtmosphereModel> atmosphere_model(const RenderEnvironment& environment);

// The engine-facing sky, in the CPU renderers' convention (what a white diffuse surface would
// draw: pi x radiance for directions, irradiance for normals). Colors mode: the skyColor /
// groundColor blend and sunColor x sunIntensity, exactly as before.
[[nodiscard]] Float3 sky_radiance(const RenderEnvironment& environment, Float3 direction) noexcept;
[[nodiscard]] Float3 sky_irradiance(const RenderEnvironment& environment, Float3 normal) noexcept;
// Sunlight on a surface facing the sun at the observer (sun colour x intensity x transmittance).
[[nodiscard]] Float3 sun_irradiance(const RenderEnvironment& environment) noexcept;
// The background: sky radiance plus the sun disc (angular radius kSunAngularRadius).
inline constexpr float kSunAngularRadius = 0.004675F;
[[nodiscard]] Float3 background_radiance(const RenderEnvironment& environment, Float3 direction) noexcept;

} // namespace dve::render
