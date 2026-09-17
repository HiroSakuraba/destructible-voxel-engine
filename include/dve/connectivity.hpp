#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "dve/voxel_object.hpp"

namespace dve {

constexpr std::uint16_t kNoComponent = 0xFFFFU;
constexpr std::size_t kBrickFaceCount = 6;

// Boundary order: -X,+X,-Y,+Y,-Z,+Z. Each 64-bit mask addresses the 8x8
// cells on that face. Opposing faces use the same (u,v) bit convention, so
// cross-brick connectivity reduces to one bitwise AND per component pair.
struct LocalComponent {
    std::uint16_t voxelCount{};
    std::uint8_t faceMask{};
    bool anchored{};
    Int3 minLocal{kInt3Max};
    Int3 maxLocal{kInt3Min};
    std::array<std::uint64_t, kBrickFaceCount> boundaryPorts{};

    auto operator<=>(const LocalComponent&) const = default;
};

// Compact steady-state connectivity. Interior voxel labels are intentionally
// absent; they are reconstructed only when a split plan needs voxel membership.
struct BrickConnectivity {
    BrickKey key{};
    std::uint32_t generation{};
    std::vector<LocalComponent> components{};
};

struct ComponentNodeRef {
    BrickKey brick{};
    std::uint16_t localComponent{};
    auto operator<=>(const ComponentNodeRef&) const = default;
};

struct ObjectComponent {
    std::vector<ComponentNodeRef> nodes{};
    std::uint64_t voxelCount{};
    Int3 minVoxel{kInt3Max};
    Int3 maxVoxel{kInt3Min};
    bool anchored{};
};

struct ConnectivitySnapshot {
    std::map<BrickKey, BrickConnectivity> brickData{};
    std::vector<ObjectComponent> components{};
};

using AnchorPredicate = std::function<bool(Int3)>;
using AnchorMaskProvider = std::function<Bitset512(BrickKey)>;

[[nodiscard]] Bitset512 build_anchor_mask(
    const VoxelObject& object,
    BrickKey key,
    const AnchorPredicate& anchorPredicate);

[[nodiscard]] BrickConnectivity build_brick_connectivity_from_mask(
    const VoxelObject& object,
    BrickKey key,
    const Bitset512& anchorMask = {});

[[nodiscard]] BrickConnectivity build_brick_connectivity(
    const VoxelObject& object,
    BrickKey key,
    const AnchorPredicate& anchorPredicate = {});

// Reference implementation. It intentionally rebuilds every occupied brick and
// is retained as a correctness oracle for the incremental cache.
[[nodiscard]] ConnectivitySnapshot build_connectivity_snapshot_with_anchor_masks(
    const VoxelObject& object,
    const AnchorMaskProvider& anchorMaskProvider);

[[nodiscard]] ConnectivitySnapshot build_connectivity_snapshot(
    const VoxelObject& object,
    const AnchorPredicate& anchorPredicate = {});

struct ConnectivityUpdateStats {
    bool fullRebuild{};
    std::size_t dirtyBricks{};
    std::size_t bricksRecomputed{};
    std::size_t topologyStableBricks{};
    std::size_t boundaryPairsRebuilt{};
    std::size_t graphNodesVisited{};
    std::size_t metadataNodesVisited{};
    std::size_t componentsRebuilt{};
    std::size_t componentsReaggregated{};
    std::size_t graphCapacityGrowthEvents{};
    std::size_t scratchCapacityGrowthEvents{};
};

struct ConnectivityStorageStats {
    std::size_t brickCount{};
    std::size_t localComponentCount{};
    std::size_t graphNodeCount{};
    std::size_t graphEdgeCount{};
    std::size_t brickHeaderBytes{};
    std::size_t localComponentLiveBytes{};
    std::size_t localComponentCapacityBytes{};
    std::size_t graphLiveBytes{};
    std::size_t graphCapacityBytes{};
    std::size_t objectComponentLiveBytes{};
    std::size_t objectComponentCapacityBytes{};
    std::size_t totalLiveBytes{};
    std::size_t totalCapacityBytes{};
    std::size_t cumulativeCapacityGrowthEvents{};
};

[[nodiscard]] constexpr std::size_t ordinary_single_component_connectivity_bytes() noexcept {
    return sizeof(BrickConnectivity) + sizeof(LocalComponent);
}

// Dynamic deletion recomputes only dirty 8^3 bricks in the common case. If a
// dirty brick preserves one local component and the same boundary neighbor set,
// the global graph is untouched. Topology-changing edits rebuild a compact CSR
// graph from retained buffers; this replaces node-based map<node,set<node>>
// storage and removes per-edge heap objects.
class IncrementalConnectivityCache {
public:
    void reserve(std::size_t brickCapacity, std::size_t graphNodeCapacity, std::size_t graphEdgeCapacity);

    void initialize_with_anchor_masks(
        const VoxelObject& object,
        const AnchorMaskProvider& anchorMaskProvider);
    void initialize(const VoxelObject& object, const AnchorPredicate& anchorPredicate = {});

    [[nodiscard]] ConnectivityUpdateStats update_with_anchor_masks(
        const VoxelObject& object,
        std::span<const AppliedBrickEdit> edits,
        const AnchorMaskProvider& anchorMaskProvider);

    [[nodiscard]] ConnectivityUpdateStats update(
        const VoxelObject& object,
        std::span<const AppliedBrickEdit> edits,
        const AnchorPredicate& anchorPredicate = {});

    [[nodiscard]] ConnectivitySnapshot snapshot() const;
    [[nodiscard]] std::vector<ObjectComponent> components() const;
    [[nodiscard]] ConnectivityStorageStats storage_stats() const noexcept;
    [[nodiscard]] std::size_t cached_brick_count() const noexcept { return brickData_.size(); }
    [[nodiscard]] std::size_t graph_node_count() const noexcept;
    [[nodiscard]] std::size_t graph_edge_count() const noexcept;
    [[nodiscard]] std::size_t object_component_count() const noexcept { return componentsById_.size(); }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] bool validate(const VoxelObject& object) const;

private:
    using ComponentId = std::uint64_t;

    struct ComponentEdge {
        ComponentNodeRef a{};
        ComponentNodeRef b{};
        auto operator<=>(const ComponentEdge&) const = default;
    };

    class CompactGraph {
    public:
        void reserve(std::size_t nodeCapacity, std::size_t edgeCapacity);
        void clear() noexcept;
        void rebuild(const std::map<BrickKey, BrickConnectivity>& brickData);

        [[nodiscard]] std::span<const ComponentNodeRef> nodes() const noexcept { return nodes_; }
        [[nodiscard]] std::span<const std::uint32_t> neighbor_indices(std::size_t nodeIndex) const noexcept;
        [[nodiscard]] std::optional<std::size_t> find_node(ComponentNodeRef node) const noexcept;
        [[nodiscard]] bool contains(ComponentNodeRef node) const noexcept { return find_node(node).has_value(); }
        [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
        [[nodiscard]] std::size_t edge_count() const noexcept { return edges_.size(); }
        [[nodiscard]] std::size_t live_bytes() const noexcept;
        [[nodiscard]] std::size_t capacity_bytes() const noexcept;
        [[nodiscard]] std::size_t growth_events() const noexcept { return growthEvents_; }
        [[nodiscard]] bool validate() const noexcept;

    private:
        std::vector<ComponentNodeRef> nodes_{};
        std::vector<ComponentEdge> edges_{};
        std::vector<std::uint32_t> offsets_{};
        std::vector<std::uint32_t> neighbors_{};
        std::vector<std::uint32_t> cursorScratch_{};
        std::size_t growthEvents_{};

        void note_capacity_growth(
            std::size_t nodeCapacity,
            std::size_t edgeCapacity,
            std::size_t offsetCapacity,
            std::size_t neighborCapacity,
            std::size_t cursorCapacity) noexcept;
    };

    struct NodeAssignment {
        ComponentNodeRef node{};
        ComponentId componentId{};
        auto operator<=>(const NodeAssignment&) const = default;
    };

    struct StoredComponent {
        ComponentId id{};
        ObjectComponent component{};
    };

    bool initialized_{};
    ComponentId nextComponentId_{1};
    std::map<BrickKey, BrickConnectivity> brickData_{};
    CompactGraph graph_{};
    std::vector<NodeAssignment> nodeAssignments_{}; // sorted by node
    std::vector<StoredComponent> componentsById_{}; // sorted by id

    // Retained update scratch. Capacity growth is surfaced in telemetry.
    std::vector<BrickKey> dirtyKeysScratch_{};
    std::vector<BrickKey> stableKeysScratch_{};
    std::vector<BrickKey> unstableKeysScratch_{};
    std::vector<BrickConnectivity> newDataScratch_{};
    std::vector<ComponentNodeRef> oldNeighborScratch_{};
    std::vector<ComponentNodeRef> newNeighborScratch_{};
    std::vector<std::uint32_t> queueScratch_{};
    std::vector<std::uint8_t> visitedScratch_{};
    std::vector<ComponentId> componentIdsScratch_{};
    std::size_t scratchGrowthEvents_{};

    void rebuild_all_components(ConnectivityUpdateStats* stats = nullptr);
    [[nodiscard]] ObjectComponent aggregate_component(std::span<const ComponentNodeRef> nodes) const;
    [[nodiscard]] std::optional<ComponentId> component_id_for(ComponentNodeRef node) const noexcept;
    [[nodiscard]] StoredComponent* stored_component(ComponentId id) noexcept;
    [[nodiscard]] const BrickConnectivity* data_after(BrickKey key) const noexcept;
    [[nodiscard]] BrickConnectivity* new_data_for(BrickKey key) noexcept;
    [[nodiscard]] const BrickConnectivity* new_data_for(BrickKey key) const noexcept;
    void collect_boundary_neighbors(
        BrickKey key,
        const BrickConnectivity& data,
        bool after,
        std::vector<ComponentNodeRef>& output) const;
    void reserve_scratch_for(std::size_t brickCount, std::size_t graphNodeCount);
};

struct SplitVoxel {
    std::uint16_t index{};
    MaterialId material{};
};

struct SplitBrickPlan {
    BrickKey key{};
    std::uint32_t sourceGeneration{};
    Bitset512 removeMask{};
    std::vector<SplitVoxel> voxels{};
};

struct SplitPlan {
    std::uint64_t sourceObjectId{};
    std::vector<SplitBrickPlan> bricks{};
    std::uint64_t voxelCount{};
};

[[nodiscard]] std::optional<SplitPlan> build_split_plan(
    const VoxelObject& object,
    const ConnectivitySnapshot& snapshot,
    std::size_t componentIndex);

[[nodiscard]] std::optional<VoxelObject> commit_split_plan(
    VoxelObject& source,
    const SplitPlan& plan,
    std::uint64_t newObjectId);

} // namespace dve
