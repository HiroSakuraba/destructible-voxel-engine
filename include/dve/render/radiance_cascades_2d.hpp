#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "dve/types.hpp"

namespace dve {
class VoxelObject;
class PackedBrickmapScene;
}

namespace dve::render::rc2d {

// Deterministic CPU reference for 2D ("flatland") Radiance Cascades over a voxel slice.
//
// Phase 1 of the RC plan (docs/RADIANCE_CASCADES.md): a byte-stable reference that the
// Phase 2 CPU SPWI prototype and any later GPU pass can be certified against. It is not a
// production renderer and makes no GPU or timing claim. Distances are in grid-cell (voxel)
// units; one cell of a slice is one voxel of the source scene.
//
// Radiance model (shared by RC and the brute-force reference): opaque cells are emitters
// (emission may be zero, i.e. a black occluder); a ray that leaves the grid sees `sky`.
// Albedo is carried for Phase 2 bounce work but is not used by the Phase 1 transport.
// Output is fluence: the mean incoming radiance over all 2D directions at each cell centre.

struct Rgb {
    float r{};
    float g{};
    float b{};
};

[[nodiscard]] float luminance(Rgb value) noexcept;

struct Grid2D {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> opaque;
    std::vector<Rgb> emission;
    std::vector<Rgb> albedo;

    [[nodiscard]] static Grid2D empty(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t y) const noexcept {
        return static_cast<std::size_t>(y) * width + x;
    }
    [[nodiscard]] bool is_opaque(std::uint32_t x, std::uint32_t y) const noexcept {
        return opaque[index(x, y)] != 0U;
    }
    // Inclusive-exclusive rectangle [x0,x1) x [y0,y1), clipped to the grid.
    void fill_rect(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                   bool isOpaque, Rgb emissionValue = {}, Rgb albedoValue = {0.5F, 0.5F, 0.5F});
    // Cells whose centre lies within `radius` of (cx, cy).
    void fill_disc(float cx, float cy, float radius, bool isOpaque, Rgb emissionValue = {},
                   Rgb albedoValue = {0.5F, 0.5F, 0.5F});
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

// ---- Voxel slicing -------------------------------------------------------------------------

// Axis normal to the slicing plane. The slice's (u, v) grid maps to world voxel axes as:
// Z-normal -> (x, y), Y-normal -> (x, z), X-normal -> (z, y).
enum class SliceNormal : std::uint8_t { X, Y, Z };

struct SliceMaterial {
    Rgb albedo{0.5F, 0.5F, 0.5F};
    Rgb emission{};
};

struct SliceRequest {
    SliceNormal normal{SliceNormal::Z};
    std::int32_t plane{};      // voxel coordinate along the normal axis
    std::int32_t originU{};    // voxel coordinate of slice cell (0, 0)
    std::int32_t originV{};
    std::uint32_t width{};
    std::uint32_t height{};
};

[[nodiscard]] Int3 slice_cell_to_voxel(const SliceRequest& request, std::uint32_t u,
                                       std::uint32_t v) noexcept;

// Materials are indexed by MaterialId. Air (id 0) is always empty; an occupied voxel whose id
// has no entry becomes a grey non-emissive occluder so missing tables never create light.
[[nodiscard]] Grid2D slice_voxels(const std::function<MaterialId(Int3)>& materialAt,
                                  const SliceRequest& request,
                                  std::span<const SliceMaterial> materials);
[[nodiscard]] Grid2D slice_voxel_object(const VoxelObject& object, const SliceRequest& request,
                                        std::span<const SliceMaterial> materials);
[[nodiscard]] Grid2D slice_packed_brickmap(const PackedBrickmapScene& scene,
                                           const SliceRequest& request,
                                           std::span<const SliceMaterial> materials);

// ---- Synthetic scenes ----------------------------------------------------------------------

enum class SyntheticScene : std::uint8_t {
    Empty,           // no cells, sky only
    SingleLight,     // one emissive disc at the centre
    OccluderShadow,  // light + black bar casting a shadow with penumbra
    ThinWall,        // light left of a full-height 1-cell black wall
    Room,            // closed room, several coloured lights, pillars, a doorway
};

struct SyntheticSceneOptions {
    std::uint32_t width{256};
    std::uint32_t height{256};
    float lightRadius{4.0F};
    Rgb lightColor{1.0F, 1.0F, 1.0F};
};

[[nodiscard]] Grid2D make_synthetic_scene(SyntheticScene scene,
                                          const SyntheticSceneOptions& options = {});

// ---- Cascades ------------------------------------------------------------------------------

enum class MergeMode : std::uint8_t {
    // One ray per interval; the upper cascade is bilinearly interpolated (pre-averaged).
    Vanilla,
    // Osborne & Sannikov 2024 sec. 2.5 / MytinoDev: each interval is traced to the start of the
    // child interval of each of the 4 bilinear upper probes and merged per probe (4x rays).
    BilinearFix,
    // Direction reprojection (radiance.wiki "parallax fix", after mxacop): one ray per interval,
    // but each upper probe is sampled in the direction from that probe towards the point the
    // lower ray reaches at the middle of the upper interval, with a cone-width angular box
    // filter over the un-averaged upper directions. No extra rays.
    ParallaxFix,
};

[[nodiscard]] const char* merge_mode_name(MergeMode mode) noexcept;

struct CascadeConfig {
    std::uint32_t baseProbeSpacing{1};  // cells between cascade-0 probes (power of two)
    std::uint32_t baseRayCount{4};      // directions per cascade-0 probe
    std::uint32_t rayBranching{4};      // direction multiplier per cascade (probe spacing x2)
    float baseIntervalLength{1.0F};     // cascade-0 interval length in cells
    float intervalGrowth{4.0F};         // interval length multiplier per cascade
    // Extends every interval end (except the last) by this multiple of the next cascade's probe
    // diagonal (sqrt(2) * spacing_{i+1}); GM Shaders part 2 uses 1.0 to close the gap between
    // cascades. 0 disables the extension.
    float intervalOverlap{0.0F};
    std::uint32_t cascadeCount{0};      // 0 = enough cascades for the interval to cover the diagonal
    MergeMode mergeMode{MergeMode::Vanilla};
    Rgb sky{};
    std::uint32_t threads{0};           // 0 = hardware concurrency, capped at 16

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CascadeLevel {
    std::uint32_t index{};
    std::uint32_t probeSpacing{};
    std::uint32_t probesX{};
    std::uint32_t probesY{};
    std::uint32_t rayCount{};
    float intervalStart{};
    float intervalEnd{};  // including overlap extension
};

[[nodiscard]] std::uint32_t auto_cascade_count(const CascadeConfig& config, std::uint32_t width,
                                               std::uint32_t height) noexcept;
[[nodiscard]] std::vector<CascadeLevel> build_cascade_levels(const CascadeConfig& config,
                                                             std::uint32_t width,
                                                             std::uint32_t height);

struct FluenceImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<Rgb> pixels;
};

struct CascadeLevelStats {
    std::uint64_t rays{};
    std::uint64_t ddaSteps{};
};

struct RadianceCascadesResult {
    FluenceImage fluence;
    std::vector<CascadeLevel> levels;
    std::vector<CascadeLevelStats> levelStats;
    std::uint64_t totalRays{};
    std::uint64_t totalDdaSteps{};
    // Peak bytes held by cascade storage at once (current level + the level above it), using
    // RGB + transmittance as 4 floats per stored direction.
    std::uint64_t peakCascadeBytes{};
};

[[nodiscard]] RadianceCascadesResult solve_radiance_cascades(const Grid2D& grid,
                                                             const CascadeConfig& config);

// ---- Brute-force reference -----------------------------------------------------------------

struct BruteForceConfig {
    std::uint32_t raysPerPixel{2048};
    Rgb sky{};
    std::uint32_t threads{0};
    // Per-pixel deterministic rotation of the stratified directions (hash of the pixel index).
    bool rotatePerPixel{true};
};

struct BruteForceResult {
    FluenceImage fluence;
    std::uint64_t totalRays{};
    std::uint64_t totalDdaSteps{};
};

[[nodiscard]] BruteForceResult solve_brute_force(const Grid2D& grid, const BruteForceConfig& config);

// ---- Shared primitives (exposed for tests) -------------------------------------------------

struct SegmentResult {
    Rgb radiance{};
    float transmittance{1.0F};  // 1 = nothing hit inside the segment, 0 = hit or escaped
    std::uint32_t steps{};
};

// Amanatides-Woo DDA from `origin` along unit `direction` for `length` cells. Returns the
// emission of the first opaque cell entered at t < length, `sky` if the ray leaves the grid,
// otherwise transmittance 1.
[[nodiscard]] SegmentResult trace_segment(const Grid2D& grid, float originX, float originY,
                                          float directionX, float directionY, float length,
                                          Rgb sky) noexcept;

// ---- Metrics -------------------------------------------------------------------------------

struct ImageError {
    std::uint64_t comparedPixels{};
    double rmse{};           // luminance RMSE over non-opaque cells
    double maxAbs{};         // luminance max |a - b|
    double meanReference{};  // mean reference luminance over the same cells
    double relativeRmse{};   // rmse / meanReference
    double relativeMax{};    // maxAbs / meanReference
};

[[nodiscard]] ImageError compare_fluence(const Grid2D& grid, const FluenceImage& candidate,
                                         const FluenceImage& reference);

// ---- Penumbra-condition calculator (port of kornel.ski/radiance, 2024.08) ------------------

// Port of the calculator's default mode (no mips, no diagonal fudge, interval power 1). It
// derives intervals from the penumbra condition for probe spacing initialSpacing * 2^i, and
// the ray counts needed to resolve a light of width minLightWidth at the interval end.
struct PenumbraCalculatorInput {
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    std::uint32_t initialSpacing{1};
    float minLightWidth{1.0F};
    float penumbraAngleDegrees{90.0F};
    bool diagonals{false};
};

struct PenumbraCascade {
    std::uint32_t probeSpacing{};
    float intervalStart{};
    float intervalStartNominal{};
    float intervalEnd{};
    std::uint32_t minRays{};  // rays for the probe exactly at the interval end
    std::uint32_t maxRays{};  // worst case: probe one (diagonal) spacing further away
    float penumbraAngleDegrees{};
    std::uint64_t probes{};
    std::uint32_t intervalSteps{};
    bool outsideBounds{};
};

[[nodiscard]] PenumbraCascade penumbra_cascade(const PenumbraCalculatorInput& input,
                                               std::uint32_t cascade) noexcept;

// For each configured level: does its ray count meet the calculator's minRays for a light of
// `minLightWidth` at the level's interval end, and is its probe spacing no larger than the
// penumbra width at its interval start? Informational; RC still works when it fails, it just
// blurs lights smaller than the condition assumes.
struct PenumbraLevelCheck {
    std::uint32_t index{};
    std::uint32_t rayCount{};
    std::uint32_t requiredRays{};
    bool angularOk{};
    float penumbraWidthAtStart{};
    bool spatialOk{};
};

[[nodiscard]] std::vector<PenumbraLevelCheck> check_penumbra_condition(
    std::span<const CascadeLevel> levels, float minLightWidth, float penumbraAngleDegrees);

} // namespace dve::render::rc2d
