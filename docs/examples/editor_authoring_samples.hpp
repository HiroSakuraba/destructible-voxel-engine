#pragma once
// Compiled reference code for Muse. No scene/editor integration is implied.
#include "dve/editable_mesh.hpp"
#include "dve/transform.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <vector>
namespace dve::editor::samples {
inline Float3 cross(Float3 a, Float3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline bool finite(Float3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
// Store a full affine basis so rotated children under nonuniform parent scale
// retain shear. Converting every intermediate transform to rotation+scale loses
// it.
struct Affine {
    Float3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1}, translation{};
    Float3 vector(Float3 p) const { return add(add(multiply(x, p.x), multiply(y, p.y)), multiply(z, p.z)); }
    Float3 point(Float3 p) const { return add(translation, vector(p)); }
};
inline Affine from_rigid(const RigidTransform& rigid, Float3 scale = {1, 1, 1}) {
    return {rotate(rigid.rotation, {scale.x, 0, 0}), rotate(rigid.rotation, {0, scale.y, 0}),
            rotate(rigid.rotation, {0, 0, scale.z}), rigid.position};
}
inline Affine compose(const Affine& parent, const Affine& child) {
    return {parent.vector(child.x), parent.vector(child.y), parent.vector(child.z),
            parent.point(child.translation)};
}
inline std::optional<Affine> inverse(const Affine& a) {
    if (!finite(a.x) || !finite(a.y) || !finite(a.z) || !finite(a.translation))
        return std::nullopt;
    const auto r0 = cross(a.y, a.z), r1 = cross(a.z, a.x), r2 = cross(a.x, a.y);
    const float det = dot(a.x, r0);
    const float magnitude = length(a.x) * length(a.y) * length(a.z);
    if (!std::isfinite(det) || !std::isfinite(magnitude) || magnitude == 0 ||
        std::abs(det) <= 1e-6F * magnitude)
        return std::nullopt;
    Affine result{{r0.x / det, r1.x / det, r2.x / det},
                  {r0.y / det, r1.y / det, r2.y / det},
                  {r0.z / det, r1.z / det, r2.z / det},
                  {}};
    result.translation = multiply(result.vector(a.translation), -1);
    if (!finite(result.x) || !finite(result.y) || !finite(result.z) || !finite(result.translation))
        return std::nullopt;
    return result;
}
inline std::optional<Float3> normal_to_world(const Affine& a, Float3 normal) {
    const auto inv = inverse(a);
    if (!inv)
        return std::nullopt;
    const Float3 n = {dot(inv->x, normal), dot(inv->y, normal), dot(inv->z, normal)};
    if (!finite(n) || length(n) < 1e-8F)
        return std::nullopt;
    return normalize(n);
}
inline std::optional<Affine> scale_world_about(const Affine& before, Float3 pivot, Float3 factors) {
    if (!finite(pivot) || !finite(factors) || factors.x <= 0 || factors.y <= 0 || factors.z <= 0)
        return std::nullopt;
    const Affine scale{{factors.x, 0, 0},
                       {0, factors.y, 0},
                       {0, 0, factors.z},
                       {pivot.x * (1 - factors.x), pivot.y * (1 - factors.y), pivot.z * (1 - factors.z)}};
    const auto after = compose(scale, before);
    if (!inverse(after))
        return std::nullopt;
    return after;
}
struct Ray {
    Float3 origin{}, direction{};
};
inline std::optional<Ray> ray_to_local(const Affine& a, Ray world) {
    const auto inv = inverse(a);
    if (!inv)
        return std::nullopt;
    // Do not normalise direction: world and local rays then use the same t.
    return Ray{inv->point(world.origin), inv->vector(world.direction)};
}
struct MeshSelection {
    std::set<VertexHandle> vertices;
    // Until EditableMesh exposes edge()/edges(), use endpoint handles and guard
    // topology changes. A production edge picker should return EdgeHandle.
    std::vector<std::pair<VertexHandle, VertexHandle>> edges;
    std::set<FaceHandle> faces;
};
inline std::optional<std::set<VertexHandle>> resolve_vertices(const EditableMesh& mesh,
                                                              const MeshSelection& selection) {
    std::set<VertexHandle> result = selection.vertices;
    for (const auto& [a, b] : selection.edges) {
        bool adjacent = false;
        for (const auto face : mesh.faces()) {
            const auto loop = mesh.face_vertices(face);
            for (std::size_t i = 0; i < loop.size(); ++i)
                if ((loop[i] == a && loop[(i + 1) % loop.size()] == b) ||
                    (loop[i] == b && loop[(i + 1) % loop.size()] == a))
                    adjacent = true;
        }
        if (!adjacent)
            return std::nullopt;
        result.insert(a);
        result.insert(b);
    }
    for (auto f : selection.faces) {
        if (!mesh.valid(f))
            return std::nullopt;
        for (auto v : mesh.face_vertices(f))
            result.insert(v);
    }
    for (auto v : result)
        if (!mesh.valid(v))
            return std::nullopt;
    return result;
}
// Own preview copies; never write through EditorObject during the gesture.
// Commit returns before/after snapshots for the eventual scene command.
class MeshGesture {
  public:
    bool begin(const EditableMesh& source, const MeshSelection& selection) {
        cancel();
        const auto selected = resolve_vertices(source, selection);
        if (!selected || selected->empty())
            return false;
        before_ = source;
        preview_ = source;
        vertices_ = *selected;
        sourceHash_ = editable_mesh_content_hash(source);
        active_ = true;
        return true;
    }
    bool transform(const Affine& delta) {
        if (!active_ || !inverse(delta))
            return false;
        EditableMesh candidate = before_;
        for (auto handle : vertices_) {
            const auto p = delta.point(before_.vertex(handle)->position);
            if (!finite(p))
                return false;
            candidate.vertex(handle)->position = p;
        }
        // Connectivity validation alone does not detect flipped/degenerate faces.
        // Reject collapsed faces before replacing the preview.
        for (auto face : candidate.faces()) {
            const auto loop = candidate.face_vertices(face);
            Float3 area{};
            const auto origin = candidate.vertex(loop[0])->position;
            for (std::size_t i = 1; i + 1 < loop.size(); ++i)
                area = add(area, cross(subtract(candidate.vertex(loop[i])->position, origin),
                                       subtract(candidate.vertex(loop[i + 1])->position, origin)));
            if (!finite(area) || length_squared(area) < 1e-16F)
                return false;
        }
        if (!candidate.validate())
            return false;
        preview_ = std::move(candidate);
        return true;
    }
    bool commit(EditableMesh& current, std::string* error = nullptr) {
        if (!active_ || editable_mesh_content_hash(current) != sourceHash_) {
            if (error)
                *error = "Mesh changed during gesture; cancel and retry.";
            return false;
        }
        current = preview_;
        active_ = false;
        return true;
    }
    const EditableMesh& before() const { return before_; }
    const EditableMesh& preview() const { return preview_; }
    void cancel() {
        active_ = false;
        vertices_.clear();
    }

  private:
    bool active_{};
    std::uint64_t sourceHash_{};
    EditableMesh before_, preview_;
    std::set<VertexHandle> vertices_;
};
} // namespace dve::editor::samples
