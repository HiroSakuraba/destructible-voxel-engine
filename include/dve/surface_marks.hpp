#pragma once

// Damage marks: the visible aftermath of destruction on voxel surfaces. Each mark is a sphere,
// such as a crater left by GameWorld damage, in its object's own space, so it moves with the
// object. It shades three things around the crater:
//
//   exposure  the freshly broken surface inside the crater (d <= r): lighter, duller, the
//             material's untouched interior.
//   scorch    soot over the crater and spreading past the rim to reachScale x r: darker and
//             rougher, with a ragged edge.
//   cracks    thin dark fracture lines radiating through the ring r .. crackReach x r (3D
//             Voronoi edges, about 0.35 r apart), fading outward.
//
// Strengths are 0..1. The marks only change shading (colour, roughness, metallic): the crater's
// shape is the voxels'. SurfaceMarkList keeps the most recent marks up to a limit and turns
// GameDamageEvents into marks (dve/game_world.hpp). The CPU player feeds it from its world.

#include <cstdint>
#include <span>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/types.hpp"

namespace dve {

struct SurfaceMark {
    std::uint64_t objectId{};
    Float3 centerMeters{};   // object-local, metres
    float radiusMeters{};
    float exposure{0.6F};
    float scorch{0.85F};
    float cracks{0.8F};
    float reachScale{1.8F};    // scorch reaches this x radius
    float crackReach{2.2F};    // cracks reach this x radius
    std::uint32_t seed{1};
};
[[nodiscard]] bool surface_mark_valid(const SurfaceMark&) noexcept;

struct SurfaceMarkSample {
    float exposure{};
    float scorch{};
    float cracks{};
    [[nodiscard]] bool any() const noexcept { return exposure > 0.0F || scorch > 0.0F || cracks > 0.0F; }
};

// The marks of `objectId` at an object-space point (metres). Overlapping marks take the strongest
// of each effect. footprintMeters (the pixel's width on the surface) fades the cracks once they
// are thinner than a pixel.
[[nodiscard]] SurfaceMarkSample sample_surface_marks(std::span<const SurfaceMark> marks, std::uint64_t objectId, Float3 positionMeters,
                                                     float footprintMeters = 0.0F) noexcept;

// Applies a sample to a shading copy of the material.
void apply_surface_marks(VoxelMaterialDefinition& material, const SurfaceMarkSample& sample) noexcept;

struct SurfaceMarkSettings {
    bool enabled{true};
    float exposure{0.6F};
    float scorch{0.85F};
    float cracks{0.8F};
    float reachScale{1.8F};
    float crackReach{2.2F};
    std::size_t maximumMarks{256};
};

class SurfaceMarkList {
public:
    explicit SurfaceMarkList(SurfaceMarkSettings settings = {}) : settings_(settings) {}
    // Adds a mark (dropping the oldest past maximumMarks); invalid marks are ignored.
    void add(const SurfaceMark& mark);
    // A crater at object-local centre (metres) with the settings' strengths. Seeds follow the
    // order of the marks, so a replay gives the same cracks.
    void add_crater(std::uint64_t objectId, Float3 centerMeters, float radiusMeters);
    void clear() noexcept { marks_.clear(); }
    [[nodiscard]] std::span<const SurfaceMark> marks() const noexcept { return marks_; }
    [[nodiscard]] const SurfaceMarkSettings& settings() const noexcept { return settings_; }
    void set_settings(const SurfaceMarkSettings& settings);

private:
    SurfaceMarkSettings settings_;
    std::vector<SurfaceMark> marks_;
    std::uint32_t next_{1};
};

} // namespace dve
