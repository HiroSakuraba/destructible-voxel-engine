#include "dve/editor_mesh_authoring.hpp"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace dve::editor {
namespace {

[[nodiscard]] PolygonEditResult failure(std::string error) {
    PolygonEditResult result;
    result.error = std::move(error);
    return result;
}

} // namespace

MeshDisplacementBakeResult bake_editor_mesh_displacement(
    const CookedPolygonAsset& source, const MeshDisplacementBakeSettings& settings) {
    MeshDisplacementBakeResult out;
    const auto fail = [&out](std::string error) {
        out.polygon = failure(std::move(error));
        return out;
    };
    if (!std::isfinite(settings.strengthMeters) || settings.strengthMeters <= 0.0F)
        return fail("Displacement strength must be a positive number of meters");
    if (!std::isfinite(settings.midpoint) || settings.midpoint < 0.0F || settings.midpoint > 1.0F)
        return fail("Displacement midpoint must be within 0..1");
    if (settings.subdivisionLevels > 4U)
        return fail("Displacement subdivision levels must be within 0..4");
    if (source.materials.empty() || source.vertices.empty() || source.indices.empty())
        return fail("Displacement needs a polygon mesh with at least one material");
    // The runtime path counts a vertex as displaced even when its material
    // has no height binding (it tracks those in missingHeightBindings), so
    // the no-height-source refusal must be decided here, from the bindings.
    bool anyHeightBound = false;
    for (std::size_t i = 0; i < source.materials.size() && i < source.materialBindings.size(); ++i)
        anyHeightBound = anyHeightBound || source.materialBindings[i].height.texture.has_value();
    if (!anyHeightBound)
        return fail("No vertices were displaced: bind a height texture to a material first");

    // One VisualOnly entry per material: the visual asset is the bake target,
    // sampled at camera distance 0 so the LOD fade is exactly 1.
    std::vector<MaterialVertexDisplacementSettings> materialSettings(source.materials.size());
    for (auto& entry : materialSettings) {
        entry.policy = MaterialDisplacementPolicy::VisualOnly;
        entry.scaleMeters = settings.strengthMeters;
        entry.referencePlane = settings.midpoint;
        entry.offlineSubdivisionLevels = settings.subdivisionLevels;
    }
    std::string error;
    auto built = build_displaced_polygon_asset(
        source, materialSettings, 0.0F, settings.maximumVertices, &error);
    if (!built) return fail(error.empty() ? "Displacement build failed" : error);
    out.stats = built->stats;
    if (built->stats.displacedVisualVertices == 0)
        return fail("No vertices were displaced");

    out.polygon.mesh = std::move(built->visualAsset);
    // The runtime path displaces along the authored normals and keeps them;
    // a bake must stand on its own geometry, so rebuild smooth normals,
    // tangents, bounds and the content hash from the displaced surface.
    refresh_editor_polygon(out.polygon.mesh, true);
    out.polygon.selection.object = source.objectId;
    return out;
}

} // namespace dve::editor
