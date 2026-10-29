#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "dve/polygon_asset.hpp"
#include "dve/types.hpp"
#include "dve/voxel_object.hpp"

namespace dve::render {

// Turns a VoxelObject's exposed faces into a CookedPolygonAsset so GPU rasterizers can draw
// voxels through the same material, shadow and IBL pipelines as authored polygon meshes.
//
// Geometry is in object space metres: voxel v occupies [v, v + 1) * cellSizeMeters per axis,
// the same convention the CPU reference tracer uses (VoxelReferenceInstance::spacing), so the
// object's rigid transform places both renderers' output identically.
//
// A face is emitted where a solid voxel meets air (kAirMaterial). Faces are merged greedily
// into rectangles inside each 8^3 brick, per material and per slice, so flat walls cost a few
// quads per brick instead of one per voxel. There is one submesh per material actually used;
// `materials` is indexed by MaterialId (ids past its end use default_voxel_material).
struct VoxelSurfaceMeshOptions {
    Float3 cellSizeMeters{0.1F, 0.1F, 0.1F};
    std::uint64_t objectId{1};
    // Merge coplanar same-material faces within a brick (off = one quad per voxel face; tests).
    bool greedy{true};
};

struct VoxelSurfaceMeshStats {
    std::uint64_t bricksVisited{};
    std::uint64_t exposedVoxelFaces{};
    std::uint64_t quads{};
    std::uint32_t materials{};
};

// Returns false only on invalid input (non-positive cell size). An object with no exposed
// faces yields an asset with no vertices (asset.vertices.empty()).
[[nodiscard]] bool build_voxel_surface_mesh(const VoxelObject& object,
                                            std::span<const VoxelMaterialDefinition> materials,
                                            const VoxelSurfaceMeshOptions& options,
                                            CookedPolygonAsset& asset,
                                            VoxelSurfaceMeshStats* stats = nullptr,
                                            std::string* error = nullptr);

} // namespace dve::render
