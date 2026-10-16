#include "dve/editor_mirror.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

#include "dve/editor_polygon.hpp"

namespace dve::editor {
namespace {
using Matrix3 = std::array<std::array<float, 3>, 3>;  // [row][column]

Matrix3 matrix_from(Quaternion q) {
    Matrix3 m{};
    const std::array<Float3, 3> basis{rotate(q, {1, 0, 0}), rotate(q, {0, 1, 0}), rotate(q, {0, 0, 1})};
    for (int c = 0; c < 3; ++c) {
        m[0][c] = basis[static_cast<std::size_t>(c)].x;
        m[1][c] = basis[static_cast<std::size_t>(c)].y;
        m[2][c] = basis[static_cast<std::size_t>(c)].z;
    }
    return m;
}
Matrix3 multiply3(const Matrix3& a, const Matrix3& b) {
    Matrix3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[i][j] += a[i][k] * b[k][j];
    return r;
}
Quaternion quaternion_from(const Matrix3& m) {
    const float trace = m[0][0] + m[1][1] + m[2][2];
    Quaternion q{};
    if (trace > 0.0F) {
        const float s = std::sqrt(trace + 1.0F) * 2.0F;
        q.w = 0.25F * s;
        q.x = (m[2][1] - m[1][2]) / s;
        q.y = (m[0][2] - m[2][0]) / s;
        q.z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = std::sqrt(1.0F + m[0][0] - m[1][1] - m[2][2]) * 2.0F;
        q.w = (m[2][1] - m[1][2]) / s;
        q.x = 0.25F * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const float s = std::sqrt(1.0F + m[1][1] - m[0][0] - m[2][2]) * 2.0F;
        q.w = (m[0][2] - m[2][0]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25F * s;
        q.z = (m[1][2] + m[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0F + m[2][2] - m[0][0] - m[1][1]) * 2.0F;
        q.w = (m[1][0] - m[0][1]) / s;
        q.x = (m[0][2] + m[2][0]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25F * s;
    }
    return normalize(q);
}
float& component(Float3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }
int& component(Int3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }
}  // namespace

MirroredPose mirror_pose(const RigidTransform& transform, int axis, Float3 point) noexcept {
    MirroredPose result;
    Float3 n{};
    component(n, axis) = 1.0F;
    // Translation: reflect the origin across the plane.
    const Float3 t = transform.position;
    const float distance = (axis == 0 ? t.x - point.x : axis == 1 ? t.y - point.y : t.z - point.z);
    result.transform.position = t;
    component(result.transform.position, axis) -= 2.0F * distance;
    // Rotation: R' = R F_m F_e with m = R^T n (the plane normal in the object's frame) and e the
    // local axis closest to m (exact and identity-preserving for axis-aligned objects).
    const Quaternion r = normalize(transform.rotation);
    const Float3 m = rotate(conjugate(r), n);
    const std::array<float, 3> magnitude{std::abs(m.x), std::abs(m.y), std::abs(m.z)};
    const int e = static_cast<int>(std::max_element(magnitude.begin(), magnitude.end()) - magnitude.begin());
    result.localAxis = e;
    Matrix3 fm{};
    const std::array<float, 3> mv{m.x, m.y, m.z};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) fm[i][j] = (i == j ? 1.0F : 0.0F) - 2.0F * mv[static_cast<std::size_t>(i)] * mv[static_cast<std::size_t>(j)];
    Matrix3 fe{};
    for (int i = 0; i < 3; ++i) fe[i][i] = i == e ? -1.0F : 1.0F;
    result.transform.rotation = quaternion_from(multiply3(matrix_from(r), multiply3(fm, fe)));
    return result;
}

VoxelObjectState mirror_voxel_state(const VoxelObjectState& state, int localAxis) {
    VoxelObjectState result;
    result.voxels.reserve(state.voxels.size());
    for (SparseVoxelStateEntry entry : state.voxels) {
        int& value = component(entry.voxel, localAxis);
        value = -value - 1;
        result.voxels.push_back(entry);
    }
    result.anchors.reserve(state.anchors.size());
    for (Int3 anchor : state.anchors) {
        int& value = component(anchor, localAxis);
        value = -value - 1;
        result.anchors.push_back(anchor);
    }
    std::sort(result.voxels.begin(), result.voxels.end(),
              [](const SparseVoxelStateEntry& a, const SparseVoxelStateEntry& b) { return a.voxel < b.voxel; });
    std::sort(result.anchors.begin(), result.anchors.end());
    return result;
}

bool mirror_polygon_mesh(CookedPolygonAsset& mesh, int localAxis, SubdivisionSettings* subdivision, ModelingTopology* topology,
                         std::string* error) {
    if (localAxis < 0 || localAxis > 2) { if (error) *error = "Mirror axis must be X, Y or Z"; return false; }
    if (mesh.indices.size() % 3 != 0) { if (error) *error = "The mesh is not made of triangles"; return false; }
    if (topology && topology->corners.size() != mesh.indices.size()) { if (error) *error = "Modeling identities do not match the mesh"; return false; }
    for (PolygonVertex& vertex : mesh.vertices) {
        component(vertex.position, localAxis) = -component(vertex.position, localAxis);
        component(vertex.normal, localAxis) = -component(vertex.normal, localAxis);
    }
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
        if (topology) std::swap(topology->corners[i + 1], topology->corners[i + 2]);
    }
    if (subdivision)
        for (std::vector<std::uint32_t>& face : subdivision->controlFaces)
            for (std::size_t i = 0; i + 2 < face.size(); i += 3) std::swap(face[i + 1], face[i + 2]);
    refresh_editor_polygon(mesh, false);
    return true;
}

PolygonTopologyLineage mirror_polygon_lineage(const CookedPolygonAsset& before, const SubdivisionSettings* subdivision) {
    PolygonTopologyLineage lineage = identity_polygon_lineage(before, subdivision);
    for (std::uint32_t i = 0; i + 2 < before.indices.size(); i += 3) {
        lineage.cornerDescendants[i + 1] = {i + 2};
        lineage.cornerDescendants[i + 2] = {i + 1};
    }
    return lineage;
}


namespace {
std::vector<std::uint32_t> canonical_vertices(const CookedPolygonAsset& mesh) {
    std::map<std::tuple<float, float, float>, std::uint32_t> first;
    std::vector<std::uint32_t> result;
    result.reserve(mesh.vertices.size());
    for (std::uint32_t i = 0; i < mesh.vertices.size(); ++i) {
        const Float3 p = mesh.vertices[i].position;
        result.push_back(first.emplace(std::tuple{p.x, p.y, p.z}, i).first->second);
    }
    return result;
}
std::uint64_t edge_key(std::uint32_t a, std::uint32_t b) {
    return (static_cast<std::uint64_t>(std::min(a, b)) << 32U) | std::max(a, b);
}
}  // namespace

MeshMirrorResult mirror_polygon_half(const CookedPolygonAsset& source, const SubdivisionSettings* subdivision, int axis,
                                     const std::set<std::uint32_t>* faces, float tolerance) {
    MeshMirrorResult result;
    const auto fail = [&](std::string message) { result.error = std::move(message); return result; };
    if (axis < 0 || axis > 2) return fail("Mirror axis must be X, Y or Z");
    if (source.indices.empty() || source.indices.size() % 3 != 0) return fail("The mesh has no triangles");
    const std::uint32_t triangleCount = static_cast<std::uint32_t>(source.indices.size() / 3);
    std::set<std::uint32_t> chosen;
    if (faces) {
        for (const std::uint32_t f : *faces) if (f < triangleCount) chosen.insert(f);
        if (chosen.empty()) return fail("Select faces to mirror");
    } else {
        for (std::uint32_t f = 0; f < triangleCount; ++f) chosen.insert(f);
    }
    const auto coordinate = [&](std::uint32_t v) {
        const Float3 p = source.vertices[v].position;
        return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
    };
    // One side only: mirroring geometry that already crosses the plane would overlap itself.
    float lowest = 0.0F, highest = 0.0F;
    for (const std::uint32_t f : chosen)
        for (int j = 0; j < 3; ++j) {
            const float c = coordinate(source.indices[std::size_t(f) * 3 + std::size_t(j)]);
            lowest = std::min(lowest, c);
            highest = std::max(highest, c);
        }
    const char* axisName = axis == 0 ? "X" : axis == 1 ? "Y" : "Z";
    if (lowest < -tolerance && highest > tolerance)
        return fail(std::string("The ") + (faces ? "selected faces cross" : "mesh crosses") + " the mirror plane (local " + axisName +
                    " = 0); keep one half on one side (Slice can cut it)");
    if (lowest >= -tolerance && highest <= tolerance)
        return fail(std::string("Everything lies on the mirror plane (local ") + axisName + " = 0); nothing to mirror");
    const auto onPlane = [&](std::uint32_t v) { return std::abs(coordinate(v)) <= tolerance; };

    // Triangles on the plane: removed for a whole mesh (they would be inside), skipped for faces.
    std::set<std::uint32_t> planar;
    for (const std::uint32_t f : chosen) {
        bool flat = true;
        for (int j = 0; j < 3; ++j) flat = flat && onPlane(source.indices[std::size_t(f) * 3 + std::size_t(j)]);
        if (flat) planar.insert(f);
    }
    std::vector<std::uint32_t> materials(triangleCount);
    std::vector<std::string> names(triangleCount, "Surface");
    for (const PolygonSubmesh& sub : source.submeshes)
        for (std::uint32_t t = sub.firstIndex / 3; t < (sub.firstIndex + sub.indexCount) / 3 && t < triangleCount; ++t) {
            materials[t] = sub.materialIndex;
            names[t] = sub.name;
        }

    CookedPolygonAsset& mesh = result.mesh;
    mesh = source;
    mesh.indices.clear();
    std::vector<std::uint32_t> newMaterials;
    std::vector<std::string> newNames;
    // Seam vertices sit exactly on the plane and their normals lie in it, so both halves meet smoothly.
    std::map<std::uint32_t, std::uint32_t> mirrorOf;  // source vertex -> its mirror image (itself on the seam)
    for (const std::uint32_t f : chosen) {
        if (planar.contains(f)) continue;
        for (int j = 0; j < 3; ++j) {
            const std::uint32_t v = source.indices[std::size_t(f) * 3 + std::size_t(j)];
            if (mirrorOf.contains(v)) continue;
            if (onPlane(v)) {
                mirrorOf[v] = v;
                PolygonVertex& seam = mesh.vertices[v];
                (axis == 0 ? seam.position.x : axis == 1 ? seam.position.y : seam.position.z) = 0.0F;
                float& n = axis == 0 ? seam.normal.x : axis == 1 ? seam.normal.y : seam.normal.z;
                n = 0.0F;
                if (length_squared(seam.normal) > 1e-12F) seam.normal = normalize(seam.normal);
                ++result.welded;
            } else {
                PolygonVertex copy = source.vertices[v];
                (axis == 0 ? copy.position.x : axis == 1 ? copy.position.y : copy.position.z) *= -1.0F;
                (axis == 0 ? copy.normal.x : axis == 1 ? copy.normal.y : copy.normal.z) *= -1.0F;
                mirrorOf[v] = static_cast<std::uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back(copy);
            }
        }
    }
    PolygonTopologyLineage& lineage = result.lineage;
    for (std::uint32_t v = 0; v < source.vertices.size(); ++v) lineage.vertexSurvivors[v] = v;
    for (const auto& [v, image] : mirrorOf) if (image != v) lineage.geometricVertexCopies[v].insert(image);
    std::map<std::uint32_t, std::uint32_t> triangleMap;  // source triangle -> output triangle
    for (std::uint32_t f = 0; f < triangleCount; ++f) {
        if (!faces && planar.contains(f)) { ++result.removed; continue; }
        const std::uint32_t out = static_cast<std::uint32_t>(mesh.indices.size() / 3);
        triangleMap[f] = out;
        lineage.triangleDescendants[f] = {out};
        for (std::uint32_t j = 0; j < 3; ++j) {
            mesh.indices.push_back(source.indices[std::size_t(f) * 3 + j]);
            lineage.cornerDescendants[f * 3 + j] = {out * 3 + j};
        }
        newMaterials.push_back(materials[f]);
        newNames.push_back(names[f]);
    }
    std::map<std::uint32_t, std::uint32_t> mirroredTriangle;  // source triangle -> its mirror copy
    for (const std::uint32_t f : chosen) {
        if (planar.contains(f)) continue;
        const std::uint32_t a = source.indices[std::size_t(f) * 3], b = source.indices[std::size_t(f) * 3 + 1],
                            c = source.indices[std::size_t(f) * 3 + 2];
        const std::uint32_t out = static_cast<std::uint32_t>(mesh.indices.size() / 3);
        mesh.indices.insert(mesh.indices.end(), {mirrorOf.at(a), mirrorOf.at(c), mirrorOf.at(b)});  // reversed winding
        mirroredTriangle[f] = out;
        result.newFaces.insert(out);
        newMaterials.push_back(materials[f]);
        newNames.push_back(names[f]);
    }
    mesh.submeshes.clear();
    for (std::size_t t = 0; t < newMaterials.size(); ++t) {
        if (mesh.submeshes.empty() || mesh.submeshes.back().materialIndex != newMaterials[t] || mesh.submeshes.back().name != newNames[t])
            mesh.submeshes.push_back({newNames[t], static_cast<std::uint32_t>(t * 3), 3, newMaterials[t]});
        else mesh.submeshes.back().indexCount += 3;
    }
    refresh_editor_polygon(mesh, false);

    // Geometric partition of the output (exact positions, like the identity lineage).
    lineage.afterVertexGroups = canonical_vertices(mesh);
    const std::vector<std::uint32_t> before = canonical_vertices(source);
    const std::vector<std::uint32_t> after = lineage.afterVertexGroups;
    std::set<std::uint64_t> outputEdges;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        for (int j = 0; j < 3; ++j) {
            const std::uint32_t p = after[mesh.indices[i + std::size_t(j)]], q = after[mesh.indices[i + std::size_t((j + 1) % 3)]];
            if (p != q) outputEdges.insert(edge_key(p, q));
        }
    for (std::size_t i = 0; i + 2 < source.indices.size(); i += 3)
        for (int j = 0; j < 3; ++j) {
            const std::uint32_t p = before[source.indices[i + std::size_t(j)]], q = before[source.indices[i + std::size_t((j + 1) % 3)]];
            if (p == q) continue;
            // Same vertex indices survive, so an edge keeps its key when it still exists.
            const std::uint64_t key = edge_key(p, q);
            const std::uint64_t now = edge_key(after[p], after[q]);
            lineage.edgeDescendants[key] = outputEdges.contains(now) ? std::set<std::uint64_t>{now} : std::set<std::uint64_t>{};
        }

    if (subdivision) {
        SubdivisionSettings settings = *subdivision;
        const auto beforeFaces = subdivision_control_face_triangles(source, subdivision);
        // Control-face records: kept faces are re-pointed at their surviving triangles, and each
        // mirrored face gets a reversed record so the cage pairs the copy the same way.
        settings.controlFaces.clear();
        for (const auto& face : beforeFaces) {
            bool kept = true;
            for (const std::uint32_t t : face) kept = kept && triangleMap.contains(t);
            if (kept) {
                std::vector<std::uint32_t> record;
                for (const std::uint32_t t : face)
                    for (int j = 0; j < 3; ++j) record.push_back(mesh.indices[std::size_t(triangleMap.at(t)) * 3 + std::size_t(j)]);
                settings.controlFaces.push_back(std::move(record));
            }
            bool mirrored = true;
            for (const std::uint32_t t : face) mirrored = mirrored && mirroredTriangle.contains(t);
            if (mirrored) {
                std::vector<std::uint32_t> record;
                for (const std::uint32_t t : face)
                    for (int j = 0; j < 3; ++j) record.push_back(mesh.indices[std::size_t(mirroredTriangle.at(t)) * 3 + std::size_t(j)]);
                settings.controlFaces.push_back(std::move(record));
            } else {
                for (const std::uint32_t t : face)
                    if (mirroredTriangle.contains(t)) {
                        std::vector<std::uint32_t> record;
                        for (int j = 0; j < 3; ++j) record.push_back(mesh.indices[std::size_t(mirroredTriangle.at(t)) * 3 + std::size_t(j)]);
                        settings.controlFaces.push_back(std::move(record));
                    }
            }
        }
        // Creases: kept as they are, and copied onto the mirrored edges.
        std::map<std::uint64_t, float> creases;
        for (const auto& [key, strength] : subdivision->edgeCreases) {
            const auto at = lineage.edgeDescendants.find(key);
            if (at == lineage.edgeDescendants.end() || at->second.empty()) continue;
            creases[*at->second.begin()] = strength;
            const std::uint32_t p = static_cast<std::uint32_t>(key >> 32U), q = static_cast<std::uint32_t>(key);
            if (mirrorOf.contains(p) && mirrorOf.contains(q)) {
                const std::uint64_t image = edge_key(after[mirrorOf.at(p)], after[mirrorOf.at(q)]);
                if (outputEdges.contains(image)) creases[image] = strength;
            }
        }
        settings.edgeCreases = std::move(creases);
        // Map old control faces to the output's faces through their first triangle.
        const auto afterFaces = subdivision_control_face_triangles(mesh, &settings);
        std::map<std::uint32_t, std::uint32_t> owner;
        for (std::uint32_t g = 0; g < afterFaces.size(); ++g)
            for (const std::uint32_t t : afterFaces[g]) owner[t] = g;
        for (std::uint32_t g = 0; g < beforeFaces.size(); ++g) {
            std::set<std::uint32_t> next;
            for (const std::uint32_t t : beforeFaces[g])
                if (triangleMap.contains(t) && owner.contains(triangleMap.at(t))) next.insert(owner.at(triangleMap.at(t)));
            lineage.controlFaceDescendants[g] = std::vector<std::uint32_t>(next.begin(), next.end());
        }
        result.subdivision = std::move(settings);
    } else {
        for (std::uint32_t t = 0; t < triangleCount; ++t)
            lineage.controlFaceDescendants[t] = triangleMap.contains(t) ? std::vector<std::uint32_t>{triangleMap.at(t)} : std::vector<std::uint32_t>{};
    }
    return result;
}

}  // namespace dve::editor
