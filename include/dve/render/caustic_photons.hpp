#pragma once

// Ground truth for caustics: sunlight shot forward as photons through glass and water
// (OpticalScene), stored where it lands, and turned back into irradiance by counting photons
// around a point. Slow and noisy by design, but exact in the limit: what the real-time caustic
// map is measured against.
//
// Units: irradiance relative to the sun's irradiance on a surface facing it (1 = full sun, head
// on). Only light that went through or off a refractive object is stored; light that reached a
// surface directly is the direct-sun term, which the shadows already account for.

#include <cstdint>
#include <vector>

#include "dve/render/optical_scene.hpp"

namespace dve::render {

struct SunLight {
    Float3 towardSun{0.0F, 1.0F, 0.0F};
    float angularRadius{0.0F};   // radians; 0 = a point sun (parallel rays)
};

// A rectangle in a plane facing the sun, above the scene: photons and caustic-map rays start on
// it and travel away from the sun. corner + a*edgeU + b*edgeV for a, b in [0, 1].
struct CausticLightRegion {
    Float3 corner{};
    Float3 edgeU{};
    Float3 edgeV{};
    Float3 towardSun{0.0F, 1.0F, 0.0F};
    [[nodiscard]] float area() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
};
// The rectangle covering the refractive objects as seen from the sun (plus `margin` world units
// on each side), placed above everything in the scene. Invalid when there is nothing refractive.
[[nodiscard]] CausticLightRegion caustic_light_region(const OpticalScene& scene, Float3 towardSun, float margin = 0.0F);

struct CausticPhoton {
    Float3 position{};
    Float3 normal{};
    Float3 power{};   // irradiance-relative flux: sum over a patch / patch area = irradiance
    std::uint64_t receiver{};
};

struct CausticPhotonSettings {
    std::uint64_t photonCount{200000};
    std::uint64_t seed{1};
    // false: only the transmitted branch at each surface (Fresnel-weighted), the light the caustic
    // map models; true: reflections too, chosen by Fresnel probability (full ground truth).
    bool includeReflections{true};
    LightPathSettings path{};
};

struct CausticPhotonResult {
    std::vector<CausticPhoton> photons;
    std::uint64_t emitted{};
    std::uint64_t direct{};     // landed without touching a refractive object (not stored)
    std::uint64_t lost{};       // left the scene or ran out of interactions
    // The region does not face sun.towardSun (built for another direction): nothing traced.
    bool regionMismatch{};
};

[[nodiscard]] CausticPhotonResult trace_caustic_photons(const OpticalScene& scene, const SunLight& sun,
                                                        const CausticLightRegion& region,
                                                        const CausticPhotonSettings& settings = {});

// Irradiance estimate: photons within `radius` of a point on a surface with normal `normal`
// (same-facing photons, close to the surface's plane), divided by the disc area.
class CausticPhotonMap {
public:
    explicit CausticPhotonMap(std::vector<CausticPhoton> photons, float cellSize);
    [[nodiscard]] Float3 irradiance(Float3 point, Float3 normal, float radius) const;
    [[nodiscard]] Float3 total_power() const noexcept;
    [[nodiscard]] const std::vector<CausticPhoton>& photons() const noexcept { return photons_; }

private:
    std::vector<CausticPhoton> photons_;
    float cell_{1.0F};
    std::vector<std::uint64_t> keys_;      // sorted cell keys
    std::vector<std::uint32_t> order_;     // photon indices sorted by cell key
};

} // namespace dve::render
