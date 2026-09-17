#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "dve/editable_mesh.hpp"

namespace dve {

struct GeometrySet;
struct GeometryInstance {
    std::shared_ptr<const GeometrySet> geometry;
    Matrix4 transform{Matrix4::identity()};
    std::uint64_t stableId{};
};
struct GeometrySet {
    std::shared_ptr<EditableMesh> mesh;
    std::vector<GeometryInstance> instances;
    [[nodiscard]] bool empty() const noexcept { return !mesh && instances.empty(); }
};

[[nodiscard]] GeometrySet realize_geometry(const GeometrySet& geometry);
[[nodiscard]] std::uint64_t geometry_set_content_hash(const GeometrySet& geometry) noexcept;

enum class GeometrySocketType : std::uint8_t { Geometry, Float, Float3, UInt, Bool, String };
using GeometryValue = std::variant<std::monostate, GeometrySet, double, Float3, std::uint32_t, bool, std::string>;
using GeometryParameters = std::unordered_map<std::string, GeometryValue>;

struct GeometrySocketDefinition { std::string name; GeometrySocketType type{}; bool required{true}; };
struct GeometryNodeEvaluationContext {
    const std::vector<GeometryValue>& inputs;
    const GeometryParameters& parameters;
    const std::atomic_bool* cancelled{};
};
using GeometryNodeEvaluator = std::function<GeometryValue(const GeometryNodeEvaluationContext&, std::string*)>;
using GeometryNodeMigrator = std::function<bool(std::uint32_t, GeometryParameters&, std::string*)>;
struct GeometryNodeDefinition {
    std::string type;
    std::uint32_t schemaVersion{1};
    std::vector<GeometrySocketDefinition> inputs;
    std::vector<GeometrySocketDefinition> outputs;
    GeometryNodeEvaluator evaluate;
    GeometryNodeMigrator migrate;
    bool pure{true};
};

class GeometryNodeRegistry {
public:
    bool register_node(GeometryNodeDefinition definition, std::string* error = nullptr);
    [[nodiscard]] const GeometryNodeDefinition* find(std::string_view type) const noexcept;
private:
    std::unordered_map<std::string, GeometryNodeDefinition> definitions_;
};

using GeometryNodeId = std::uint64_t;
struct GeometryNodeInstance {
    GeometryNodeId id{};
    std::string type;
    std::uint32_t schemaVersion{1};
    GeometryParameters parameters;
};
struct GeometryLink {
    GeometryNodeId source{};
    std::uint32_t sourceSocket{};
    GeometryNodeId destination{};
    std::uint32_t destinationSocket{};
};
class GeometryGraph {
public:
    bool add_node(GeometryNodeInstance node, std::string* error = nullptr);
    bool remove_node(GeometryNodeId id);
    bool connect(GeometryLink link, const GeometryNodeRegistry& registry, std::string* error = nullptr);
    bool set_parameter(GeometryNodeId id, std::string name, GeometryValue value);
    [[nodiscard]] const GeometryNodeInstance* node(GeometryNodeId id) const noexcept;
    [[nodiscard]] const std::vector<GeometryLink>& links() const noexcept { return links_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] bool validate(const GeometryNodeRegistry& registry, std::string* error = nullptr) const;
private:
    std::unordered_map<GeometryNodeId, GeometryNodeInstance> nodes_;
    std::vector<GeometryLink> links_;
    std::uint64_t revision_{1};
};

struct GeometryEvaluationRequest {
    GeometryNodeId node{};
    std::uint32_t outputSocket{};
    std::uint64_t expectedGraphRevision{};
    const std::atomic_bool* cancelled{};
};
enum class GeometryEvaluationStatus : std::uint8_t { Success, InvalidGraph, Cancelled, Stale, Failed };
struct GeometryEvaluationResult {
    GeometryEvaluationStatus status{GeometryEvaluationStatus::Failed};
    GeometryValue value;
    std::string error;
    std::uint64_t contentHash{};
    std::size_t evaluatedNodes{};
    std::size_t cacheHits{};
};

class GeometryGraphEvaluator {
public:
    explicit GeometryGraphEvaluator(const GeometryNodeRegistry& registry) : registry_(registry) {}
    [[nodiscard]] GeometryEvaluationResult evaluate(const GeometryGraph& graph, const GeometryEvaluationRequest& request);
    void clear_cache() { cache_.clear(); }
    [[nodiscard]] std::size_t cache_size() const noexcept { return cache_.size(); }
private:
    struct CachedValue { GeometryValue value; std::uint64_t hash{}; };
    const GeometryNodeRegistry& registry_;
    std::unordered_map<std::uint64_t, CachedValue> cache_;
};

void register_builtin_geometry_nodes(GeometryNodeRegistry& registry);

} // namespace dve
