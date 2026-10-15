#include "dve/editor_mirror.hpp"

#include <algorithm>
#include <array>
#include <cmath>

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

}  // namespace dve::editor
