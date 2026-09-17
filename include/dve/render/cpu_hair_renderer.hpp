#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dve/cpu_hair.hpp"
#include "dve/cpu_hair_runtime.hpp"
#include "dve/render/polygon_renderer.hpp"

namespace dve::render {

// Visible-hair generation is intentionally separate from guide simulation. The solver publishes
// a compact CpuHairView; this bridge expands it into many inexpensive camera-facing ribbon
// strands without adding simulated particles.
struct CpuHairRibbonBuildSettings {
    std::uint32_t visibleStrandsPerGuide{4U}; // includes the guide centerline when requested
    std::uint32_t maximumVisibleGuides{};     // zero means every guide
    std::uint32_t maximumVisibleStrands{};    // zero means the guide-derived total
    std::uint32_t maximumPointsPerStrand{};   // zero means every guide point; otherwise >= 2
    float rootSpreadMeters{0.0004F};
    float tipSpreadMeters{0.0040F};
    float rootWidthMeters{0.0012F};
    float tipWidthMeters{0.00015F};
    float spreadExponent{1.15F};
    float longitudinalJitterMeters{0.00025F};
    Float4 rootColor{0.06F, 0.035F, 0.02F, 0.92F};
    Float4 tipColor{0.16F, 0.09F, 0.04F, 0.72F};
    std::uint64_t seed{0x48524942554c4c55ULL};
    bool includeGuideCenterline{true};
};

struct CpuHairRibbonVertex {
    Float3 position{};
    Float3 tangent{0.0F, -1.0F, 0.0F};
    Float2 texcoord{};
    Float4 color{1.0F, 1.0F, 1.0F, 1.0F};
};

struct CpuHairRibbonRange {
    std::uint32_t firstVertex{};
    std::uint32_t vertexCount{};
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
    std::uint32_t sourceGuide{};
    std::uint32_t childIndex{};
    std::uint32_t layerId{};
};

struct CpuHairRibbonBuildTelemetry {
    std::uint64_t sourceGuides{};
    std::uint64_t selectedGuides{};
    std::uint64_t visibleStrands{};
    std::uint64_t sampledCenterlinePoints{};
    std::uint64_t ribbonVertices{};
    std::uint64_t ribbonTriangles{};
    std::uint64_t degenerateTangents{};
    std::uint64_t cameraParallelFallbacks{};
    std::uint64_t guideBudgetDrops{};
    std::uint64_t strandBudgetDrops{};
    std::uint64_t workerBatches{};
};

struct CpuHairRibbonPacketView {
    std::span<const CpuHairRibbonVertex> vertices{};
    std::span<const std::uint32_t> indices{};
    std::span<const std::uint32_t> triangleLayerIds{};
    std::span<const CpuHairRibbonRange> strands{};
    Float3 boundsMinimum{};
    Float3 boundsMaximum{};
    std::uint64_t sourceSimulationFrame{};
    std::uint64_t topologyHash{};
    bool visible{};
};

[[nodiscard]] std::size_t default_cpu_hair_ribbon_worker_count() noexcept;

class CpuHairRibbonExpander {
public:
    explicit CpuHairRibbonExpander(
        std::size_t workerCount = default_cpu_hair_ribbon_worker_count());

    [[nodiscard]] bool build(const CpuHairView& view,
                             const HairAsset& asset,
                             const PolygonCamera& camera,
                             const CpuHairRibbonBuildSettings& settings = {},
                             std::string* error = nullptr);
    [[nodiscard]] bool build(const CpuHairRuntime& runtime,
                             CpuHairOwnerId owner,
                             const PolygonCamera& camera,
                             const CpuHairRibbonBuildSettings& settings = {},
                             std::string* error = nullptr);

    [[nodiscard]] CpuHairRibbonPacketView view() const noexcept;
    [[nodiscard]] const CpuHairRibbonBuildTelemetry& telemetry() const noexcept {
        return telemetry_;
    }

    void clear() noexcept;
    [[nodiscard]] std::size_t worker_count() const noexcept { return jobs_.worker_count(); }

private:
    struct BuildWorkItem {
        std::uint32_t guideIndex{};
        std::uint32_t childIndex{};
        std::uint32_t sampleCount{};
        std::uint32_t firstVertex{};
        std::uint32_t firstIndex{};
        std::uint32_t firstTriangle{};
        std::uint32_t layerId{};
        std::uint64_t degenerateTangents{};
        std::uint64_t cameraParallelFallbacks{};
        Float3 boundsMinimum{};
        Float3 boundsMaximum{};
        bool boundsInitialized{};
    };

    JobSystem jobs_;
    std::vector<BuildWorkItem> work_;
    std::vector<CpuHairRibbonVertex> vertices_;
    std::vector<std::uint32_t> indices_;
    std::vector<std::uint32_t> triangleLayerIds_;
    std::vector<CpuHairRibbonRange> strands_;
    CpuHairRibbonBuildTelemetry telemetry_{};
    Float3 boundsMinimum_{};
    Float3 boundsMaximum_{};
    std::uint64_t sourceSimulationFrame_{};
    std::uint64_t topologyHash_{};
    bool visible_{};
};


enum class CpuHairRibbonLodTier : std::uint8_t {
    Culled,
    Distant,
    Far,
    Medium,
    Near,
};

struct CpuHairRibbonLodPolicy {
    float cullBelowProjectedPixels{12.0F};
    float farProjectedPixels{50.0F};
    float mediumProjectedPixels{140.0F};
    float nearProjectedPixels{300.0F};
};

struct CpuHairRibbonLodSelection {
    CpuHairRibbonBuildSettings settings{};
    CpuHairRibbonLodTier tier{CpuHairRibbonLodTier::Culled};
    bool visible{};
};

[[nodiscard]] CpuHairRibbonLodSelection select_cpu_hair_ribbon_lod(
    float projectedDiameterPixels,
    const CpuHairRibbonBuildSettings& nearSettings = {},
    const CpuHairRibbonLodPolicy& policy = {}) noexcept;

[[nodiscard]] float estimate_cpu_hair_projected_diameter_pixels(
    const CpuHairView& view,
    const PolygonCamera& camera,
    std::uint32_t viewportHeightPixels) noexcept;

struct CpuHairReferenceRenderOptions {
    float alphaCutoff{0.015F};
    float specularStrength{0.35F};
    float specularExponent{36.0F};
    bool enableLighting{true};
    bool preserveExistingDepth{true};
    bool writeDepth{false};
    bool writeObjectIds{true};
};

struct CpuHairReferenceRenderStats {
    std::uint64_t submittedStrands{};
    std::uint64_t submittedTriangles{};
    std::uint64_t rejectedTriangles{};
    std::uint64_t rasterizedTriangles{};
    std::uint64_t shadedFragments{};
    std::uint64_t depthRejectedFragments{};
    std::uint64_t alphaRejectedFragments{};
};

// A deterministic CPU reference path for tests, editor thumbnails, and backend bring-up. Production
// GPU backends can upload the same packet layout and perform ribbon shading in their normal passes.
class ReferenceCpuHairRenderer {
public:
    [[nodiscard]] CpuHairReferenceRenderStats render(
        const CpuHairRibbonPacketView& packet,
        std::uint64_t objectId,
        const PolygonCamera& camera,
        const RenderEnvironment& environment,
        PolygonRenderTarget& target,
        const CpuHairReferenceRenderOptions& options = {});

private:
    struct ScreenVertex {
        float x{};
        float y{};
        float depth{};
        float cameraZ{};
        float invZ{};
        Float3 worldPosition{};
        Float3 tangent{};
        Float4 color{};
    };
    struct TriangleOrder {
        std::uint32_t triangle{};
        float cameraZ{};
    };
    std::vector<ScreenVertex> screenVertices_;
    std::vector<std::uint8_t> projected_;
    std::vector<TriangleOrder> triangleOrder_;
    std::vector<float> nearestHairDepth_;
};

[[nodiscard]] bool validate_cpu_hair_ribbon_settings(
    const CpuHairRibbonBuildSettings& settings,
    std::string* error = nullptr) noexcept;

} // namespace dve::render
