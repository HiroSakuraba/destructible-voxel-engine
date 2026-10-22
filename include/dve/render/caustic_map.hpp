#pragma once

// The caustic map: real-time-style caustics from glass and water, without noise.
//
// Sunlight is treated as a mesh: a grid of rays over the refractive objects as the sun sees them
// (caustic_light_region). Each grid vertex is followed through the media
// (follow_light_transmitted) to where it lands. Each grid triangle carries a fixed share of
// sunlight; where it lands, that light is spread over the landed triangle, so
// irradiance = flux / landed area: a triangle that shrinks has focused its light (Evan Wallace's
// WebGL Water; caustics mapping, Shah et al. 2007). The result is then filtered: irradiance at a
// point is the average over a small disc around it (a regular 16-gon, clipped exactly against each
// landed triangle), whose radius is tan(sun radius) x the distance from the last surface the light
// left (a finite sun blurs caustics), and at least a quarter of a grid cell. The filter keeps
// energy exact and keeps folds, where a triangle collapses to nothing, finite.
//
// This CPU version builds the landed triangles once per scene and answers "how much caustic light
// reaches this point"; the reference renderer adds it to the sun term. The GPU version will
// rasterize the same triangles. Irradiance is relative to the sun's irradiance on a surface
// facing it (as CausticPhotonMap), so 1 = full sun head on.

#include <cstdint>
#include <vector>

#include "dve/render/caustic_photons.hpp"

namespace dve::render {

struct CausticMapSettings {
    std::uint32_t resolution{256};       // grid cells per side over the light region
    float margin{0.5F};                  // world units around the refractive objects
    float minimumFilterCells{0.25F};     // smallest filter radius, in grid cells (point sun)
    // Landed triangles must stay on one surface: receivers' normals within this cosine, and no
    // vertex farther than this many grid cells (in the receiver's plane) from the triangle's plane.
    float sameSurfaceCosine{0.9F};
    float planeToleranceCells{2.0F};
    LightPathSettings path{};
};

struct CausticTriangle {
    Float3 points[3]{};
    Float3 normal{};           // receiver plane normal, facing the light
    Float3 flux{};             // sunlight share x Fresnel/absorption throughput (x full-sun area)
    float area{};              // landed area (flux / area = irradiance before filtering)
    float filterRadius{};      // world units: finite-sun blur, at least minimumFilterCells
    std::uint64_t receiver{};
};

struct CausticMapStats {
    std::uint64_t rays{};
    std::uint64_t causticRays{};   // landed after at least one interface
    std::uint64_t triangles{};
    std::uint64_t splitTriangles{};   // dropped: vertices on different surfaces or branches
};

class CausticMap {
public:
    CausticMap() = default;
    // Caustic irradiance at a point on a surface with the given normal (RGB, x full sun). With a
    // receiver object id, light that landed on another object counts only where the two surfaces
    // are in the same plane (a seam, across which the sun-size blur spreads it): a surface just
    // behind or below the one that caught the light gets none of it.
    static constexpr std::uint64_t kAnyReceiver = ~std::uint64_t{0};
    [[nodiscard]] Float3 irradiance(Float3 point, Float3 normal, std::uint64_t receiver = kAnyReceiver) const;
    [[nodiscard]] const std::vector<CausticTriangle>& triangles() const noexcept { return triangles_; }
    [[nodiscard]] const CausticMapStats& stats() const noexcept { return stats_; }
    [[nodiscard]] bool empty() const noexcept { return triangles_.empty(); }
    // Total caustic power (flux summed): what the grid sent through, after Fresnel and absorption.
    [[nodiscard]] Float3 total_power() const noexcept;

private:
    friend CausticMap build_caustic_map(const OpticalScene&, const SunLight&, const CausticMapSettings&);
    void index();
    std::vector<CausticTriangle> triangles_;
    CausticMapStats stats_;
    float cell_{1.0F};          // finest index level; level L cells are cell_ x 4^L
    std::vector<std::uint64_t> keys_;
    std::vector<std::uint32_t> entries_;
};

[[nodiscard]] CausticMap build_caustic_map(const OpticalScene& scene, const SunLight& sun,
                                           const CausticMapSettings& settings = {});

} // namespace dve::render
