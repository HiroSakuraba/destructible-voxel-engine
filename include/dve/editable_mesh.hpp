#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "dve/polygon_asset.hpp"

namespace dve {

template <typename Tag>
struct MeshHandle {
    std::uint32_t index{0xFFFFFFFFU};
    std::uint32_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept { return index != 0xFFFFFFFFU; }
    auto operator<=>(const MeshHandle&) const = default;
};
struct VertexTag {};
struct EdgeTag {};
struct HalfEdgeTag {};
struct FaceTag {};
struct CornerTag {};
using VertexHandle = MeshHandle<VertexTag>;
using EdgeHandle = MeshHandle<EdgeTag>;
using HalfEdgeHandle = MeshHandle<HalfEdgeTag>;
using FaceHandle = MeshHandle<FaceTag>;
using CornerHandle = MeshHandle<CornerTag>;

enum class MeshAttributeDomain : std::uint8_t { Point, Edge, Face, Corner, Detail };
using MeshAttributeValue = std::variant<bool, std::int64_t, double, Float2, Float3, Float4, std::string>;

class EditableMesh {
public:
    struct Vertex { Float3 position{}; HalfEdgeHandle outgoing{}; std::uint32_t generation{1}; bool alive{true}; };
    struct Edge { HalfEdgeHandle halfEdge{}; std::uint32_t generation{1}; bool alive{true}; };
    struct HalfEdge {
        VertexHandle origin{};
        HalfEdgeHandle twin{};
        HalfEdgeHandle next{};
        FaceHandle face{};
        EdgeHandle edge{};
        CornerHandle corner{};
        std::uint32_t generation{1};
        bool alive{true};
    };
    struct Face { HalfEdgeHandle halfEdge{}; std::uint32_t material{}; std::uint32_t generation{1}; bool alive{true}; };
    struct Corner { HalfEdgeHandle halfEdge{}; std::uint32_t generation{1}; bool alive{true}; };

    [[nodiscard]] VertexHandle add_vertex(Float3 position);
    [[nodiscard]] std::optional<FaceHandle> add_face(const std::vector<VertexHandle>& vertices, std::uint32_t material = 0U);
    [[nodiscard]] bool remove_face(FaceHandle face);
    [[nodiscard]] bool valid(VertexHandle handle) const noexcept;
    [[nodiscard]] bool valid(EdgeHandle handle) const noexcept;
    [[nodiscard]] bool valid(HalfEdgeHandle handle) const noexcept;
    [[nodiscard]] bool valid(FaceHandle handle) const noexcept;
    [[nodiscard]] bool valid(CornerHandle handle) const noexcept;
    [[nodiscard]] const Vertex* vertex(VertexHandle handle) const noexcept;
    [[nodiscard]] Vertex* vertex(VertexHandle handle) noexcept;
    [[nodiscard]] const Face* face(FaceHandle handle) const noexcept;
    [[nodiscard]] Face* face(FaceHandle handle) noexcept;
    [[nodiscard]] std::vector<VertexHandle> face_vertices(FaceHandle face) const;
    [[nodiscard]] std::vector<FaceHandle> faces() const;
    [[nodiscard]] std::vector<VertexHandle> vertices() const;
    [[nodiscard]] std::vector<HalfEdgeHandle> vertex_halfedges(VertexHandle vertex) const;
    [[nodiscard]] std::size_t vertex_count() const noexcept;
    [[nodiscard]] std::size_t edge_count() const noexcept;
    [[nodiscard]] std::size_t face_count() const noexcept;
    [[nodiscard]] std::size_t corner_count() const noexcept;
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] std::uint64_t topology_revision() const noexcept { return topologyRevision_; }
    [[nodiscard]] std::uint64_t attribute_revision() const noexcept { return attributeRevision_; }
    [[nodiscard]] PolygonBounds bounds() const noexcept;

    bool set_attribute(MeshAttributeDomain domain, std::string name, std::uint32_t index, MeshAttributeValue value);
    [[nodiscard]] const MeshAttributeValue* attribute(MeshAttributeDomain domain, std::string_view name, std::uint32_t index) const;
    bool set_detail_attribute(std::string name, MeshAttributeValue value);
    [[nodiscard]] const MeshAttributeValue* detail_attribute(std::string_view name) const;

private:
    struct AttributeKey {
        MeshAttributeDomain domain{};
        std::string name;
        bool operator==(const AttributeKey&) const = default;
    };
    struct AttributeKeyHash {
        std::size_t operator()(const AttributeKey& key) const noexcept;
    };
    struct DirectedEdgeKey {
        std::uint32_t from{};
        std::uint32_t to{};
        bool operator==(const DirectedEdgeKey&) const = default;
    };
    struct DirectedEdgeHash {
        std::size_t operator()(const DirectedEdgeKey& key) const noexcept;
    };

    [[nodiscard]] std::size_t domain_size(MeshAttributeDomain domain) const noexcept;
    std::vector<Vertex> vertices_;
    std::vector<Edge> edges_;
    std::vector<HalfEdge> halfEdges_;
    std::vector<Face> faces_;
    std::vector<Corner> corners_;
    std::unordered_map<DirectedEdgeKey, HalfEdgeHandle, DirectedEdgeHash> openDirectedEdges_;
    std::unordered_map<AttributeKey, std::vector<std::optional<MeshAttributeValue>>, AttributeKeyHash> attributes_;
    std::unordered_map<std::string, MeshAttributeValue> detailAttributes_;
    std::uint64_t topologyRevision_{1};
    std::uint64_t attributeRevision_{1};
};

class MeshBuilder {
public:
    [[nodiscard]] VertexHandle add_vertex(Float3 position);
    [[nodiscard]] std::optional<FaceHandle> add_triangle(VertexHandle a, VertexHandle b, VertexHandle c, std::uint32_t material = 0U);
    [[nodiscard]] std::optional<FaceHandle> add_quad(VertexHandle a, VertexHandle b, VertexHandle c, VertexHandle d, std::uint32_t material = 0U);
    [[nodiscard]] std::optional<FaceHandle> add_polygon(const std::vector<VertexHandle>& vertices, std::uint32_t material = 0U);
    [[nodiscard]] EditableMesh&& finish() && noexcept { return std::move(mesh_); }
    [[nodiscard]] EditableMesh& mesh() noexcept { return mesh_; }
private:
    EditableMesh mesh_;
};

[[nodiscard]] EditableMesh editable_mesh_from_cooked(const CookedPolygonAsset& asset, std::string* error = nullptr);
[[nodiscard]] CookedPolygonAsset cooked_polygon_from_editable(const EditableMesh& mesh, std::uint64_t objectId = 1U, std::string* error = nullptr);
[[nodiscard]] std::uint64_t editable_mesh_content_hash(const EditableMesh& mesh) noexcept;

} // namespace dve
