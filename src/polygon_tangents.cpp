#include "dve/polygon_tangents.hpp"

#include <cmath>
#include <vector>

#include "../third_party/mikktspace/mikktspace.h"

namespace dve {
namespace {

struct MikkUserData {
    const CookedPolygonAsset* asset = nullptr;
    std::vector<Float4>* cornerTangents = nullptr; // one per index

    [[nodiscard]] const PolygonVertex& vertex(int face, int vert) const {
        return asset->vertices[asset->indices[static_cast<std::size_t>(face) * 3U +
                                              static_cast<std::size_t>(vert)]];
    }
};

int get_num_faces(const SMikkTSpaceContext* context) {
    const auto* data = static_cast<const MikkUserData*>(context->m_pUserData);
    return static_cast<int>(data->asset->indices.size() / 3U);
}
int get_num_vertices_of_face(const SMikkTSpaceContext*, int) { return 3; }
void get_position(const SMikkTSpaceContext* context, float out[], int face, int vert) {
    const auto* data = static_cast<const MikkUserData*>(context->m_pUserData);
    const Float3 p = data->vertex(face, vert).position;
    out[0] = p.x; out[1] = p.y; out[2] = p.z;
}
void get_normal(const SMikkTSpaceContext* context, float out[], int face, int vert) {
    const auto* data = static_cast<const MikkUserData*>(context->m_pUserData);
    const Float3 n = data->vertex(face, vert).normal;
    out[0] = n.x; out[1] = n.y; out[2] = n.z;
}
void get_texcoord(const SMikkTSpaceContext* context, float out[], int face, int vert) {
    const auto* data = static_cast<const MikkUserData*>(context->m_pUserData);
    const Float2 uv = data->vertex(face, vert).texcoord;
    out[0] = uv.x; out[1] = uv.y;
}
void set_tspace_basic(const SMikkTSpaceContext* context, const float tangent[], float sign,
                      int face, int vert) {
    auto* data = static_cast<MikkUserData*>(context->m_pUserData);
    (*data->cornerTangents)[static_cast<std::size_t>(face) * 3U + static_cast<std::size_t>(vert)] =
        {tangent[0], tangent[1], tangent[2], sign};
}

} // namespace

bool compute_polygon_tangents_mikktspace(CookedPolygonAsset& asset) {
    if (asset.vertices.empty() || asset.indices.size() < 3U || asset.indices.size() % 3U != 0U)
        return false;
    for (const std::uint32_t index : asset.indices)
        if (index >= asset.vertices.size()) return false;

    std::vector<Float4> corners(asset.indices.size(), {0.0F, 0.0F, 0.0F, 0.0F});
    MikkUserData data{&asset, &corners};
    SMikkTSpaceInterface interface{};
    interface.m_getNumFaces = &get_num_faces;
    interface.m_getNumVerticesOfFace = &get_num_vertices_of_face;
    interface.m_getPosition = &get_position;
    interface.m_getNormal = &get_normal;
    interface.m_getTexCoord = &get_texcoord;
    interface.m_setTSpaceBasic = &set_tspace_basic;
    SMikkTSpaceContext context{&interface, &data};
    if (!genTangSpaceDefault(&context)) return false;

    // Scatter corners to vertices, refusing (as documented in the header) if
    // one vertex record is shared by corners whose tangents disagree — that
    // mesh needs split vertices, and the accumulated fallback serves it
    // better than a last-write-wins tangent.
    std::vector<Float4> tangents(asset.vertices.size(), {1.0F, 0.0F, 0.0F, 1.0F});
    std::vector<bool> written(asset.vertices.size(), false);
    for (std::size_t corner = 0; corner < corners.size(); ++corner) {
        const std::uint32_t vertex = asset.indices[corner];
        const Float4 t = corners[corner];
        if (!written[vertex]) {
            tangents[vertex] = t;
            written[vertex] = true;
            continue;
        }
        const Float4 existing = tangents[vertex];
        if (std::abs(existing.x - t.x) > 1.0e-6F || std::abs(existing.y - t.y) > 1.0e-6F ||
            std::abs(existing.z - t.z) > 1.0e-6F || std::abs(existing.w - t.w) > 1.0e-6F)
            return false;
    }
    for (std::size_t i = 0; i < asset.vertices.size(); ++i)
        if (written[i]) asset.vertices[i].tangent = tangents[i];
    return true;
}

} // namespace dve
