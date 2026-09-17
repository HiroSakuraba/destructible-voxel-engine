#pragma once

#include <cstdint>
#include <span>

#include "dve/asset_cooker.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/transform.hpp"
#include "dve/voxel_object.hpp"

namespace dve::render {

// CPU reference path for validating the shared voxel/polygon camera-depth contract. It traces
// authoritative VoxelObject occupancy directly and writes the same HDR/depth/ID target used by
// ReferencePolygonRenderer. This is validation and editor-preview code, not the production GPU
// brickmap tracer.
struct VoxelReferenceInstance {
    std::uint64_t objectId{};
    const VoxelObject* object{};
    RigidTransform transform{};
    // Indexed by MaterialId. Missing entries use a visible magenta diagnostic material.
    std::span<const VoxelMaterialDefinition> materials{};
    bool visible{true};
};

struct VoxelReferenceRenderStats {
    std::uint64_t submittedInstances{};
    std::uint64_t tracedRays{};
    std::uint64_t hitRays{};
    std::uint64_t materialFallbacks{};
    std::uint64_t shadowRays{};
    std::uint64_t shadowBlockedRays{};
    std::uint64_t globalIlluminationRays{};
    std::uint64_t globalIlluminationHits{};
};

class ReferenceVoxelRenderer {
public:
    [[nodiscard]] VoxelReferenceRenderStats render(
        std::span<const VoxelReferenceInstance> instances,
        const PolygonCamera& camera,
        const RenderEnvironment& environment,
        PolygonRenderTarget& target,
        bool preserveExistingDepth = false) const;
};

// Direct hybrid reference composition. Voxel occupancy is traced into the shared target first;
// polygons then depth-test and alpha-blend against that target. This validates polygon-in-front,
// voxel-in-front, and translucent-over-voxel behavior without a synthetic voxel layer.
struct HybridReferenceRenderStats {
    VoxelReferenceRenderStats voxels{};
    PolygonRenderStats polygons{};
};

[[nodiscard]] HybridReferenceRenderStats render_hybrid_reference(
    std::span<const VoxelReferenceInstance> voxels,
    std::span<const PolygonRenderInstance> polygons,
    const PolygonCamera& camera,
    const RenderEnvironment& environment,
    PolygonRenderTarget& target,
    const PolygonRenderOptions& polygonOptions = {});

} // namespace dve::render
