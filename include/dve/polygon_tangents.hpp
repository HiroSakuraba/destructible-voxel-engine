#pragma once

#include "dve/polygon_asset.hpp"

namespace dve {

// Fills CookedPolygonAsset vertex tangents with MikkTSpace — the tangent
// space convention Blender, Unreal, and the glTF ecosystem bake normal maps
// against, so imported normal maps shade as authored instead of through the
// cooker's accumulated UV-derivative frame. Tangents are written per vertex;
// MikkTSpace assigns identical tangents to corners sharing position, normal,
// and texcoord, which is exactly DVE's bundled-vertex model. Returns false,
// leaving tangents untouched, when the asset has no indexed triangles or a
// vertex's corners disagree (a mesh that shares one vertex record across a
// UV seam) — callers then fall back to the accumulated method.
[[nodiscard]] bool compute_polygon_tangents_mikktspace(CookedPolygonAsset& asset);

} // namespace dve
