#include "dve/connectivity.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <tuple>
#include <utility>
#include <vector>

namespace dve {

namespace {

constexpr std::array<Int3, 6> kNeighborDelta{{
    {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1},
}};

constexpr std::array<BrickKey, 3> kPositiveBrickDelta{{
    {1, 0, 0}, {0, 1, 0}, {0, 0, 1},
}};

constexpr std::array<std::uint8_t, 6> kOppositeFace{{1, 0, 3, 2, 5, 4}};

class DisjointSet {
public:
    explicit DisjointSet(std::size_t count) : parent_(count), rank_(count, 0) {
        std::iota(parent_.begin(), parent_.end(), 0U);
    }

    std::size_t find(std::size_t value) {
        if (parent_[value] != value) parent_[value] = find(parent_[value]);
        return parent_[value];
    }

    void unite(std::size_t a, std::size_t b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (rank_[a] < rank_[b]) std::swap(a, b);
        parent_[b] = a;
        if (rank_[a] == rank_[b]) ++rank_[a];
    }

private:
    std::vector<std::size_t> parent_;
    std::vector<std::uint8_t> rank_;
};

struct LocalLabelBuild {
    std::array<std::uint16_t, kBrickVoxelCount> labels{};
    std::vector<LocalComponent> components{};
};

[[nodiscard]] BrickKey add(BrickKey a, BrickKey b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] bool component_less(const ObjectComponent& a, const ObjectComponent& b) {
    return std::tie(a.minVoxel.x, a.minVoxel.y, a.minVoxel.z, a.voxelCount, a.nodes) <
           std::tie(b.minVoxel.x, b.minVoxel.y, b.minVoxel.z, b.voxelCount, b.nodes);
}

[[nodiscard]] std::uint8_t face_bits(Int3 local) noexcept {
    std::uint8_t bits = 0;
    if (local.x == 0) bits |= 1U << 0U;
    if (local.x == kBrickDim - 1) bits |= 1U << 1U;
    if (local.y == 0) bits |= 1U << 2U;
    if (local.y == kBrickDim - 1) bits |= 1U << 3U;
    if (local.z == 0) bits |= 1U << 4U;
    if (local.z == kBrickDim - 1) bits |= 1U << 5U;
    return bits;
}

[[nodiscard]] std::uint8_t face_from_delta(BrickKey delta) noexcept {
    if (delta == BrickKey{-1, 0, 0}) return 0;
    if (delta == BrickKey{1, 0, 0}) return 1;
    if (delta == BrickKey{0, -1, 0}) return 2;
    if (delta == BrickKey{0, 1, 0}) return 3;
    if (delta == BrickKey{0, 0, -1}) return 4;
    return 5;
}

[[nodiscard]] std::uint8_t face_port_bit(Int3 local, std::uint8_t face) noexcept {
    switch (face) {
        case 0:
        case 1:
            return static_cast<std::uint8_t>(local.y + kBrickDim * local.z);
        case 2:
        case 3:
            return static_cast<std::uint8_t>(local.x + kBrickDim * local.z);
        default:
            return static_cast<std::uint8_t>(local.x + kBrickDim * local.y);
    }
}

void record_boundary_ports(LocalComponent& component, Int3 local) noexcept {
    const std::uint8_t bits = face_bits(local);
    component.faceMask |= bits;
    for (std::uint8_t face = 0; face < kBrickFaceCount; ++face) {
        if ((bits & (1U << face)) == 0) continue;
        component.boundaryPorts[face] |= std::uint64_t{1} << face_port_bit(local, face);
    }
}

[[nodiscard]] LocalLabelBuild label_brick(
    const VoxelObject& object,
    BrickKey key,
    const Bitset512& anchorMask) {
    LocalLabelBuild result;
    result.labels.fill(kNoComponent);

    const Brick* brick = object.find_brick(key);
    if (brick == nullptr || brick->empty()) return result;

    const Bitset512 occupancy = brick->occupancy();
    std::array<std::uint16_t, kBrickVoxelCount> queue{};

    for (std::uint16_t seed = 0; seed < kBrickVoxelCount; ++seed) {
        if (!occupancy.test(seed) || result.labels[seed] != kNoComponent) continue;

        const std::uint16_t componentId = static_cast<std::uint16_t>(result.components.size());
        LocalComponent component;
        std::size_t head = 0;
        std::size_t tail = 0;
        queue[tail++] = seed;
        result.labels[seed] = componentId;

        while (head < tail) {
            const std::uint16_t index = queue[head++];
            const Int3 local = local_from_index_unchecked(index);
            ++component.voxelCount;
            component.minLocal = min_components(component.minLocal, local);
            component.maxLocal = max_components(component.maxLocal, local);
            component.anchored = component.anchored || anchorMask.test(index);
            record_boundary_ports(component, local);

            for (const Int3 delta : kNeighborDelta) {
                const Int3 adjacent{local.x + delta.x, local.y + delta.y, local.z + delta.z};
                if (adjacent.x < 0 || adjacent.x >= kBrickDim || adjacent.y < 0 || adjacent.y >= kBrickDim ||
                    adjacent.z < 0 || adjacent.z >= kBrickDim) {
                    continue;
                }
                const std::uint16_t next = voxel_index_unchecked(adjacent);
                if (occupancy.test(next) && result.labels[next] == kNoComponent) {
                    result.labels[next] = componentId;
                    queue[tail++] = next;
                }
            }
        }
        result.components.push_back(component);
    }
    return result;
}

[[nodiscard]] bool components_touch_across_face(
    const LocalComponent& a,
    std::uint8_t faceA,
    const LocalComponent& b) noexcept {
    const std::uint8_t faceB = kOppositeFace[faceA];
    return (a.boundaryPorts[faceA] & b.boundaryPorts[faceB]) != 0;
}

[[nodiscard]] std::size_t brick_graph_node_count(const std::map<BrickKey, BrickConnectivity>& brickData) noexcept {
    std::size_t count = 0;
    for (const auto& [key, data] : brickData) {
        (void)key;
        count += data.components.size();
    }
    return count;
}

[[nodiscard]] const LocalComponent* local_component(
    const std::map<BrickKey, BrickConnectivity>& brickData,
    ComponentNodeRef node) noexcept {
    const auto brickIt = brickData.find(node.brick);
    if (brickIt == brickData.end() || node.localComponent >= brickIt->second.components.size()) return nullptr;
    return &brickIt->second.components[node.localComponent];
}

[[nodiscard]] std::size_t capacity_bytes(const std::vector<ComponentNodeRef>& value) noexcept {
    return value.capacity() * sizeof(ComponentNodeRef);
}

[[nodiscard]] std::size_t live_bytes(const std::vector<ComponentNodeRef>& value) noexcept {
    return value.size() * sizeof(ComponentNodeRef);
}

} // namespace

Bitset512 build_anchor_mask(
    const VoxelObject& object,
    BrickKey key,
    const AnchorPredicate& anchorPredicate) {
    Bitset512 mask;
    if (!anchorPredicate) return mask;
    const Brick* brick = object.find_brick(key);
    if (brick == nullptr || brick->empty()) return mask;
    const Bitset512 occupancy = brick->occupancy();
    occupancy.for_each_set([&](std::uint16_t index) {
        if (anchorPredicate(global_from_local(key, local_from_index_unchecked(index)))) mask.set(index);
    });
    return mask;
}

BrickConnectivity build_brick_connectivity_from_mask(
    const VoxelObject& object,
    BrickKey key,
    const Bitset512& anchorMask) {
    BrickConnectivity result;
    result.key = key;
    const Brick* brick = object.find_brick(key);
    if (brick == nullptr || brick->empty()) return result;
    result.generation = brick->generation();
    LocalLabelBuild labeled = label_brick(object, key, anchorMask);
    result.components = std::move(labeled.components);
    return result;
}

BrickConnectivity build_brick_connectivity(
    const VoxelObject& object,
    BrickKey key,
    const AnchorPredicate& anchorPredicate) {
    return build_brick_connectivity_from_mask(object, key, build_anchor_mask(object, key, anchorPredicate));
}

ConnectivitySnapshot build_connectivity_snapshot_with_anchor_masks(
    const VoxelObject& object,
    const AnchorMaskProvider& anchorMaskProvider) {
    ConnectivitySnapshot snapshot;
    std::map<BrickKey, std::size_t> baseNode;
    std::size_t totalNodes = 0;

    for (const auto& [key, brick] : object.bricks()) {
        if (brick.empty()) continue;
        const Bitset512 anchorMask = anchorMaskProvider ? anchorMaskProvider(key) : Bitset512{};
        BrickConnectivity data = build_brick_connectivity_from_mask(object, key, anchorMask);
        baseNode[key] = totalNodes;
        totalNodes += data.components.size();
        snapshot.brickData.emplace(key, std::move(data));
    }

    DisjointSet dsu(totalNodes);
    auto node_id = [&](BrickKey key, std::uint16_t localComponentId) {
        return baseNode.at(key) + localComponentId;
    };

    for (const auto& [key, data] : snapshot.brickData) {
        for (const BrickKey delta : kPositiveBrickDelta) {
            const BrickKey neighborKey = add(key, delta);
            const auto neighborIt = snapshot.brickData.find(neighborKey);
            if (neighborIt == snapshot.brickData.end()) continue;
            const std::uint8_t faceA = face_from_delta(delta);
            const BrickConnectivity& other = neighborIt->second;
            for (std::uint16_t a = 0; a < data.components.size(); ++a) {
                for (std::uint16_t b = 0; b < other.components.size(); ++b) {
                    if (components_touch_across_face(data.components[a], faceA, other.components[b])) {
                        dsu.unite(node_id(key, a), node_id(neighborKey, b));
                    }
                }
            }
        }
    }

    std::map<std::size_t, ObjectComponent> grouped;
    for (const auto& [key, data] : snapshot.brickData) {
        for (std::uint16_t localId = 0; localId < data.components.size(); ++localId) {
            const std::size_t root = dsu.find(node_id(key, localId));
            ObjectComponent& objectComponent = grouped[root];
            objectComponent.nodes.push_back({key, localId});
            const LocalComponent& local = data.components[localId];
            objectComponent.voxelCount += local.voxelCount;
            objectComponent.anchored = objectComponent.anchored || local.anchored;
            objectComponent.minVoxel = min_components(objectComponent.minVoxel, global_from_local(key, local.minLocal));
            objectComponent.maxVoxel = max_components(objectComponent.maxVoxel, global_from_local(key, local.maxLocal));
        }
    }

    for (auto& [root, component] : grouped) {
        (void)root;
        std::sort(component.nodes.begin(), component.nodes.end());
        snapshot.components.push_back(std::move(component));
    }
    std::sort(snapshot.components.begin(), snapshot.components.end(), component_less);
    return snapshot;
}

ConnectivitySnapshot build_connectivity_snapshot(
    const VoxelObject& object,
    const AnchorPredicate& anchorPredicate) {
    AnchorMaskProvider provider;
    if (anchorPredicate) provider = [&](BrickKey key) { return build_anchor_mask(object, key, anchorPredicate); };
    return build_connectivity_snapshot_with_anchor_masks(object, provider);
}

void IncrementalConnectivityCache::CompactGraph::reserve(
    std::size_t nodeCapacity,
    std::size_t edgeCapacity) {
    const auto oldNode = nodes_.capacity();
    const auto oldEdge = edges_.capacity();
    const auto oldOffset = offsets_.capacity();
    const auto oldNeighbor = neighbors_.capacity();
    const auto oldCursor = cursorScratch_.capacity();
    nodes_.reserve(nodeCapacity);
    edges_.reserve(edgeCapacity);
    offsets_.reserve(nodeCapacity + 1);
    neighbors_.reserve(edgeCapacity * 2);
    cursorScratch_.reserve(nodeCapacity + 1);
    note_capacity_growth(oldNode, oldEdge, oldOffset, oldNeighbor, oldCursor);
}

void IncrementalConnectivityCache::CompactGraph::clear() noexcept {
    nodes_.clear();
    edges_.clear();
    offsets_.clear();
    neighbors_.clear();
    cursorScratch_.clear();
}

void IncrementalConnectivityCache::CompactGraph::note_capacity_growth(
    std::size_t nodeCapacity,
    std::size_t edgeCapacity,
    std::size_t offsetCapacity,
    std::size_t neighborCapacity,
    std::size_t cursorCapacity) noexcept {
    growthEvents_ += nodes_.capacity() > nodeCapacity ? 1U : 0U;
    growthEvents_ += edges_.capacity() > edgeCapacity ? 1U : 0U;
    growthEvents_ += offsets_.capacity() > offsetCapacity ? 1U : 0U;
    growthEvents_ += neighbors_.capacity() > neighborCapacity ? 1U : 0U;
    growthEvents_ += cursorScratch_.capacity() > cursorCapacity ? 1U : 0U;
}

void IncrementalConnectivityCache::CompactGraph::rebuild(
    const std::map<BrickKey, BrickConnectivity>& brickData) {
    const auto oldNode = nodes_.capacity();
    const auto oldEdge = edges_.capacity();
    const auto oldOffset = offsets_.capacity();
    const auto oldNeighbor = neighbors_.capacity();
    const auto oldCursor = cursorScratch_.capacity();

    clear();
    const std::size_t expectedNodes = brick_graph_node_count(brickData);
    nodes_.reserve(expectedNodes);

    for (const auto& [key, data] : brickData) {
        for (std::uint16_t localId = 0; localId < data.components.size(); ++localId) {
            nodes_.push_back({key, localId});
        }
    }

    // Each neighboring brick pair can contribute at most componentA*componentB edges.
    // The vector retains its prior high-water mark across rebuilds.
    for (const auto& [key, data] : brickData) {
        for (const BrickKey delta : kPositiveBrickDelta) {
            const BrickKey neighborKey = add(key, delta);
            const auto neighborIt = brickData.find(neighborKey);
            if (neighborIt == brickData.end()) continue;
            const std::uint8_t faceA = face_from_delta(delta);
            const BrickConnectivity& other = neighborIt->second;
            for (std::uint16_t a = 0; a < data.components.size(); ++a) {
                for (std::uint16_t b = 0; b < other.components.size(); ++b) {
                    if (!components_touch_across_face(data.components[a], faceA, other.components[b])) continue;
                    ComponentEdge edge{{key, a}, {neighborKey, b}};
                    if (edge.b < edge.a) std::swap(edge.a, edge.b);
                    edges_.push_back(edge);
                }
            }
        }
    }
    std::sort(edges_.begin(), edges_.end());
    edges_.erase(std::unique(edges_.begin(), edges_.end()), edges_.end());

    offsets_.assign(nodes_.size() + 1, 0U);
    auto index_of = [&](ComponentNodeRef node) -> std::uint32_t {
        const auto it = std::lower_bound(nodes_.begin(), nodes_.end(), node);
        assert(it != nodes_.end() && *it == node);
        return static_cast<std::uint32_t>(std::distance(nodes_.begin(), it));
    };
    for (const ComponentEdge& edge : edges_) {
        const std::uint32_t a = index_of(edge.a);
        const std::uint32_t b = index_of(edge.b);
        ++offsets_[static_cast<std::size_t>(a) + 1];
        ++offsets_[static_cast<std::size_t>(b) + 1];
    }
    for (std::size_t i = 1; i < offsets_.size(); ++i) offsets_[i] += offsets_[i - 1];

    neighbors_.assign(edges_.size() * 2, 0U);
    cursorScratch_.assign(offsets_.begin(), offsets_.end());
    for (const ComponentEdge& edge : edges_) {
        const std::uint32_t a = index_of(edge.a);
        const std::uint32_t b = index_of(edge.b);
        neighbors_[cursorScratch_[a]++] = b;
        neighbors_[cursorScratch_[b]++] = a;
    }
    for (std::size_t node = 0; node < nodes_.size(); ++node) {
        auto begin = neighbors_.begin() + static_cast<std::ptrdiff_t>(offsets_[node]);
        auto end = neighbors_.begin() + static_cast<std::ptrdiff_t>(offsets_[node + 1]);
        std::sort(begin, end);
    }
    note_capacity_growth(oldNode, oldEdge, oldOffset, oldNeighbor, oldCursor);
}

std::span<const std::uint32_t> IncrementalConnectivityCache::CompactGraph::neighbor_indices(
    std::size_t nodeIndex) const noexcept {
    if (nodeIndex >= nodes_.size() || offsets_.size() != nodes_.size() + 1) return {};
    const std::size_t begin = offsets_[nodeIndex];
    const std::size_t end = offsets_[nodeIndex + 1];
    return {neighbors_.data() + static_cast<std::ptrdiff_t>(begin), end - begin};
}

std::optional<std::size_t> IncrementalConnectivityCache::CompactGraph::find_node(
    ComponentNodeRef node) const noexcept {
    const auto it = std::lower_bound(nodes_.begin(), nodes_.end(), node);
    if (it == nodes_.end() || *it != node) return std::nullopt;
    return static_cast<std::size_t>(std::distance(nodes_.begin(), it));
}

std::size_t IncrementalConnectivityCache::CompactGraph::live_bytes() const noexcept {
    return nodes_.size() * sizeof(ComponentNodeRef) + edges_.size() * sizeof(ComponentEdge) +
           offsets_.size() * sizeof(std::uint32_t) + neighbors_.size() * sizeof(std::uint32_t) +
           cursorScratch_.size() * sizeof(std::uint32_t);
}

std::size_t IncrementalConnectivityCache::CompactGraph::capacity_bytes() const noexcept {
    return nodes_.capacity() * sizeof(ComponentNodeRef) + edges_.capacity() * sizeof(ComponentEdge) +
           offsets_.capacity() * sizeof(std::uint32_t) + neighbors_.capacity() * sizeof(std::uint32_t) +
           cursorScratch_.capacity() * sizeof(std::uint32_t);
}

bool IncrementalConnectivityCache::CompactGraph::validate() const noexcept {
    if (!std::is_sorted(nodes_.begin(), nodes_.end())) return false;
    if (!std::is_sorted(edges_.begin(), edges_.end())) return false;
    if (std::adjacent_find(edges_.begin(), edges_.end()) != edges_.end()) return false;
    if (offsets_.size() != nodes_.size() + 1) return false;
    if (!offsets_.empty() && offsets_.back() != neighbors_.size()) return false;
    for (std::size_t node = 0; node < nodes_.size(); ++node) {
        const auto neighbors = neighbor_indices(node);
        if (!std::is_sorted(neighbors.begin(), neighbors.end())) return false;
        for (const std::uint32_t neighbor : neighbors) {
            if (neighbor >= nodes_.size()) return false;
            const auto reverse = neighbor_indices(neighbor);
            if (!std::binary_search(reverse.begin(), reverse.end(), static_cast<std::uint32_t>(node))) return false;
        }
    }
    return true;
}

void IncrementalConnectivityCache::reserve(
    std::size_t brickCapacity,
    std::size_t graphNodeCapacity,
    std::size_t graphEdgeCapacity) {
    graph_.reserve(graphNodeCapacity, graphEdgeCapacity);
    reserve_scratch_for(brickCapacity, graphNodeCapacity);
    nodeAssignments_.reserve(graphNodeCapacity);
    componentsById_.reserve(graphNodeCapacity);
}

void IncrementalConnectivityCache::reserve_scratch_for(
    std::size_t brickCount,
    std::size_t graphNodeCount) {
    auto reserve_counted = [&](auto& vector, std::size_t capacity) {
        const std::size_t before = vector.capacity();
        vector.reserve(capacity);
        if (vector.capacity() > before) ++scratchGrowthEvents_;
    };
    reserve_counted(dirtyKeysScratch_, brickCount);
    reserve_counted(stableKeysScratch_, brickCount);
    reserve_counted(unstableKeysScratch_, brickCount);
    reserve_counted(newDataScratch_, brickCount);
    reserve_counted(oldNeighborScratch_, 32);
    reserve_counted(newNeighborScratch_, 32);
    reserve_counted(queueScratch_, graphNodeCount);
    reserve_counted(visitedScratch_, graphNodeCount);
    reserve_counted(componentIdsScratch_, graphNodeCount);
}

ObjectComponent IncrementalConnectivityCache::aggregate_component(
    std::span<const ComponentNodeRef> nodes) const {
    ObjectComponent component;
    component.nodes.assign(nodes.begin(), nodes.end());
    for (const ComponentNodeRef node : nodes) {
        const LocalComponent* local = local_component(brickData_, node);
        if (local == nullptr) continue;
        component.voxelCount += local->voxelCount;
        component.anchored = component.anchored || local->anchored;
        component.minVoxel = min_components(component.minVoxel, global_from_local(node.brick, local->minLocal));
        component.maxVoxel = max_components(component.maxVoxel, global_from_local(node.brick, local->maxLocal));
    }
    std::sort(component.nodes.begin(), component.nodes.end());
    return component;
}

std::optional<IncrementalConnectivityCache::ComponentId>
IncrementalConnectivityCache::component_id_for(ComponentNodeRef node) const noexcept {
    const auto it = std::lower_bound(
        nodeAssignments_.begin(), nodeAssignments_.end(), node,
        [](const NodeAssignment& assignment, ComponentNodeRef candidate) { return assignment.node < candidate; });
    if (it == nodeAssignments_.end() || it->node != node) return std::nullopt;
    return it->componentId;
}

IncrementalConnectivityCache::StoredComponent*
IncrementalConnectivityCache::stored_component(ComponentId id) noexcept {
    const auto it = std::lower_bound(
        componentsById_.begin(), componentsById_.end(), id,
        [](const StoredComponent& component, ComponentId candidate) { return component.id < candidate; });
    return it == componentsById_.end() || it->id != id ? nullptr : &*it;
}

BrickConnectivity* IncrementalConnectivityCache::new_data_for(BrickKey key) noexcept {
    const auto it = std::lower_bound(
        newDataScratch_.begin(), newDataScratch_.end(), key,
        [](const BrickConnectivity& data, BrickKey candidate) { return data.key < candidate; });
    return it == newDataScratch_.end() || it->key != key ? nullptr : &*it;
}

const BrickConnectivity* IncrementalConnectivityCache::new_data_for(BrickKey key) const noexcept {
    const auto it = std::lower_bound(
        newDataScratch_.begin(), newDataScratch_.end(), key,
        [](const BrickConnectivity& data, BrickKey candidate) { return data.key < candidate; });
    return it == newDataScratch_.end() || it->key != key ? nullptr : &*it;
}

const BrickConnectivity* IncrementalConnectivityCache::data_after(BrickKey key) const noexcept {
    if (const BrickConnectivity* pending = new_data_for(key); pending != nullptr) {
        return pending->components.empty() ? nullptr : pending;
    }
    const auto it = brickData_.find(key);
    return it == brickData_.end() ? nullptr : &it->second;
}

void IncrementalConnectivityCache::collect_boundary_neighbors(
    BrickKey key,
    const BrickConnectivity& data,
    bool after,
    std::vector<ComponentNodeRef>& output) const {
    output.clear();
    if (data.components.size() != 1) return;
    for (std::uint8_t face = 0; face < kBrickFaceCount; ++face) {
        const Int3 delta = kNeighborDelta[face];
        const BrickKey neighborKey{key.x + delta.x, key.y + delta.y, key.z + delta.z};
        const BrickConnectivity* neighbor = nullptr;
        if (after) {
            neighbor = data_after(neighborKey);
        } else {
            const auto it = brickData_.find(neighborKey);
            if (it != brickData_.end()) neighbor = &it->second;
        }
        if (neighbor == nullptr) continue;
        for (std::uint16_t localId = 0; localId < neighbor->components.size(); ++localId) {
            if (components_touch_across_face(data.components[0], face, neighbor->components[localId])) {
                output.push_back({neighborKey, localId});
            }
        }
    }
    std::sort(output.begin(), output.end());
    output.erase(std::unique(output.begin(), output.end()), output.end());
}

void IncrementalConnectivityCache::rebuild_all_components(ConnectivityUpdateStats* stats) {
    const std::size_t oldAssignmentCapacity = nodeAssignments_.capacity();
    const std::size_t oldComponentCapacity = componentsById_.capacity();
    const std::size_t oldQueueCapacity = queueScratch_.capacity();
    const std::size_t oldVisitedCapacity = visitedScratch_.capacity();

    nodeAssignments_.clear();
    componentsById_.clear();
    nodeAssignments_.reserve(graph_.node_count());
    componentsById_.reserve(graph_.node_count());
    visitedScratch_.assign(graph_.node_count(), 0U);
    queueScratch_.clear();
    queueScratch_.reserve(graph_.node_count());

    const auto nodes = graph_.nodes();
    for (std::size_t seed = 0; seed < nodes.size(); ++seed) {
        if (visitedScratch_[seed] != 0) continue;
        const ComponentId id = nextComponentId_++;
        ObjectComponent objectComponent;
        queueScratch_.clear();
        queueScratch_.push_back(static_cast<std::uint32_t>(seed));
        visitedScratch_[seed] = 1U;
        std::size_t head = 0;
        while (head < queueScratch_.size()) {
            const std::uint32_t index = queueScratch_[head++];
            const ComponentNodeRef node = nodes[index];
            objectComponent.nodes.push_back(node);
            nodeAssignments_.push_back({node, id});
            if (stats != nullptr) ++stats->graphNodesVisited;

            const LocalComponent* local = local_component(brickData_, node);
            if (local != nullptr) {
                objectComponent.voxelCount += local->voxelCount;
                objectComponent.anchored = objectComponent.anchored || local->anchored;
                objectComponent.minVoxel = min_components(
                    objectComponent.minVoxel, global_from_local(node.brick, local->minLocal));
                objectComponent.maxVoxel = max_components(
                    objectComponent.maxVoxel, global_from_local(node.brick, local->maxLocal));
            }

            for (const std::uint32_t neighbor : graph_.neighbor_indices(index)) {
                if (visitedScratch_[neighbor] == 0U) {
                    visitedScratch_[neighbor] = 1U;
                    queueScratch_.push_back(neighbor);
                }
            }
        }
        std::sort(objectComponent.nodes.begin(), objectComponent.nodes.end());
        componentsById_.push_back({id, std::move(objectComponent)});
        if (stats != nullptr) ++stats->componentsRebuilt;
    }
    std::sort(nodeAssignments_.begin(), nodeAssignments_.end(), [](const NodeAssignment& a, const NodeAssignment& b) {
        return a.node < b.node;
    });

    scratchGrowthEvents_ += nodeAssignments_.capacity() > oldAssignmentCapacity ? 1U : 0U;
    scratchGrowthEvents_ += componentsById_.capacity() > oldComponentCapacity ? 1U : 0U;
    scratchGrowthEvents_ += queueScratch_.capacity() > oldQueueCapacity ? 1U : 0U;
    scratchGrowthEvents_ += visitedScratch_.capacity() > oldVisitedCapacity ? 1U : 0U;
}

void IncrementalConnectivityCache::initialize_with_anchor_masks(
    const VoxelObject& object,
    const AnchorMaskProvider& anchorMaskProvider) {
    initialized_ = true;
    nextComponentId_ = 1;
    brickData_.clear();
    graph_.clear();
    nodeAssignments_.clear();
    componentsById_.clear();

    reserve_scratch_for(object.brick_count(), object.brick_count());
    for (const auto& [key, brick] : object.bricks()) {
        if (brick.empty()) continue;
        const Bitset512 anchorMask = anchorMaskProvider ? anchorMaskProvider(key) : Bitset512{};
        brickData_.emplace(key, build_brick_connectivity_from_mask(object, key, anchorMask));
    }
    graph_.reserve(brick_graph_node_count(brickData_), brick_graph_node_count(brickData_) * 3);
    graph_.rebuild(brickData_);
    reserve_scratch_for(object.brick_count(), graph_.node_count());
    rebuild_all_components();
}

void IncrementalConnectivityCache::initialize(
    const VoxelObject& object,
    const AnchorPredicate& anchorPredicate) {
    AnchorMaskProvider provider;
    if (anchorPredicate) provider = [&](BrickKey key) { return build_anchor_mask(object, key, anchorPredicate); };
    initialize_with_anchor_masks(object, provider);
}

ConnectivityUpdateStats IncrementalConnectivityCache::update_with_anchor_masks(
    const VoxelObject& object,
    std::span<const AppliedBrickEdit> edits,
    const AnchorMaskProvider& anchorMaskProvider) {
    ConnectivityUpdateStats stats;
    const std::size_t graphGrowthBefore = graph_.growth_events();
    const std::size_t scratchGrowthBefore = scratchGrowthEvents_;

    if (!initialized_) {
        initialize_with_anchor_masks(object, anchorMaskProvider);
        stats.fullRebuild = true;
        stats.bricksRecomputed = brickData_.size();
        stats.graphNodesVisited = graph_.node_count();
        stats.componentsRebuilt = componentsById_.size();
        stats.graphCapacityGrowthEvents = graph_.growth_events() - graphGrowthBefore;
        stats.scratchCapacityGrowthEvents = scratchGrowthEvents_ - scratchGrowthBefore;
        return stats;
    }

    dirtyKeysScratch_.clear();
    for (const AppliedBrickEdit& edit : edits) {
        if (edit.changedMask.any()) dirtyKeysScratch_.push_back(edit.key);
    }
    std::sort(dirtyKeysScratch_.begin(), dirtyKeysScratch_.end());
    dirtyKeysScratch_.erase(std::unique(dirtyKeysScratch_.begin(), dirtyKeysScratch_.end()), dirtyKeysScratch_.end());
    stats.dirtyBricks = dirtyKeysScratch_.size();
    if (dirtyKeysScratch_.empty()) return stats;

    newDataScratch_.clear();
    for (const BrickKey key : dirtyKeysScratch_) {
        const Brick* brick = object.find_brick(key);
        if (brick == nullptr || brick->empty()) {
            BrickConnectivity empty;
            empty.key = key;
            empty.generation = brick == nullptr ? 0U : brick->generation();
            newDataScratch_.push_back(std::move(empty));
        } else {
            const Bitset512 anchorMask = anchorMaskProvider ? anchorMaskProvider(key) : Bitset512{};
            newDataScratch_.push_back(build_brick_connectivity_from_mask(object, key, anchorMask));
        }
        ++stats.bricksRecomputed;
    }

    stableKeysScratch_.clear();
    unstableKeysScratch_.clear();
    for (const BrickKey key : dirtyKeysScratch_) {
        const auto oldIt = brickData_.find(key);
        const BrickConnectivity* fresh = new_data_for(key);
        if (oldIt == brickData_.end() || fresh == nullptr || oldIt->second.components.size() != 1 ||
            fresh->components.size() != 1 || !component_id_for({key, 0}).has_value()) {
            unstableKeysScratch_.push_back(key);
            continue;
        }
        collect_boundary_neighbors(key, oldIt->second, false, oldNeighborScratch_);
        collect_boundary_neighbors(key, *fresh, true, newNeighborScratch_);
        if (oldNeighborScratch_ == newNeighborScratch_) {
            stableKeysScratch_.push_back(key);
            ++stats.topologyStableBricks;
        } else {
            unstableKeysScratch_.push_back(key);
        }
    }

    if (!unstableKeysScratch_.empty()) {
        // Publish compact local data, then rebuild the retained CSR graph. The
        // common stable path above avoids this entirely.
        for (const BrickKey key : dirtyKeysScratch_) {
            BrickConnectivity* fresh = new_data_for(key);
            assert(fresh != nullptr);
            const auto oldIt = brickData_.find(key);
            if (fresh->components.empty()) {
                if (oldIt != brickData_.end()) brickData_.erase(oldIt);
            } else if (oldIt != brickData_.end()) {
                oldIt->second = std::move(*fresh);
            } else {
                brickData_.emplace(key, std::move(*fresh));
            }
        }
        graph_.rebuild(brickData_);
        stats.boundaryPairsRebuilt = unstableKeysScratch_.size() * kBrickFaceCount;
        rebuild_all_components(&stats);
    } else {
        componentIdsScratch_.clear();
        for (const BrickKey key : stableKeysScratch_) {
            const auto oldIt = brickData_.find(key);
            BrickConnectivity* fresh = new_data_for(key);
            assert(oldIt != brickData_.end() && fresh != nullptr);
            const auto id = component_id_for({key, 0});
            assert(id.has_value());
            StoredComponent* stored = stored_component(*id);
            assert(stored != nullptr);
            ObjectComponent& objectComponent = stored->component;
            const LocalComponent& oldLocal = oldIt->second.components[0];
            const LocalComponent& newLocal = fresh->components[0];

            if (newLocal.voxelCount >= oldLocal.voxelCount) {
                objectComponent.voxelCount += newLocal.voxelCount - oldLocal.voxelCount;
            } else {
                objectComponent.voxelCount -= oldLocal.voxelCount - newLocal.voxelCount;
            }

            const Int3 oldMin = global_from_local(key, oldLocal.minLocal);
            const Int3 oldMax = global_from_local(key, oldLocal.maxLocal);
            const Int3 newMin = global_from_local(key, newLocal.minLocal);
            const Int3 newMax = global_from_local(key, newLocal.maxLocal);
            const bool shrankRecordedExtreme =
                (oldMin.x == objectComponent.minVoxel.x && newMin.x > oldMin.x) ||
                (oldMin.y == objectComponent.minVoxel.y && newMin.y > oldMin.y) ||
                (oldMin.z == objectComponent.minVoxel.z && newMin.z > oldMin.z) ||
                (oldMax.x == objectComponent.maxVoxel.x && newMax.x < oldMax.x) ||
                (oldMax.y == objectComponent.maxVoxel.y && newMax.y < oldMax.y) ||
                (oldMax.z == objectComponent.maxVoxel.z && newMax.z < oldMax.z);
            if (shrankRecordedExtreme || (oldLocal.anchored && !newLocal.anchored)) {
                componentIdsScratch_.push_back(*id);
            } else {
                objectComponent.minVoxel = min_components(objectComponent.minVoxel, newMin);
                objectComponent.maxVoxel = max_components(objectComponent.maxVoxel, newMax);
                objectComponent.anchored = objectComponent.anchored || newLocal.anchored;
            }
            oldIt->second = std::move(*fresh);
        }

        std::sort(componentIdsScratch_.begin(), componentIdsScratch_.end());
        componentIdsScratch_.erase(
            std::unique(componentIdsScratch_.begin(), componentIdsScratch_.end()), componentIdsScratch_.end());
        for (const ComponentId id : componentIdsScratch_) {
            StoredComponent* stored = stored_component(id);
            if (stored == nullptr) continue;
            stats.metadataNodesVisited += stored->component.nodes.size();
            stored->component = aggregate_component(stored->component.nodes);
            ++stats.componentsReaggregated;
        }
    }

    stats.graphCapacityGrowthEvents = graph_.growth_events() - graphGrowthBefore;
    stats.scratchCapacityGrowthEvents = scratchGrowthEvents_ - scratchGrowthBefore;
    return stats;
}

ConnectivityUpdateStats IncrementalConnectivityCache::update(
    const VoxelObject& object,
    std::span<const AppliedBrickEdit> edits,
    const AnchorPredicate& anchorPredicate) {
    AnchorMaskProvider provider;
    if (anchorPredicate) provider = [&](BrickKey key) { return build_anchor_mask(object, key, anchorPredicate); };
    return update_with_anchor_masks(object, edits, provider);
}

std::vector<ObjectComponent> IncrementalConnectivityCache::components() const {
    std::vector<ObjectComponent> result;
    result.reserve(componentsById_.size());
    for (const StoredComponent& stored : componentsById_) result.push_back(stored.component);
    std::sort(result.begin(), result.end(), component_less);
    return result;
}

ConnectivitySnapshot IncrementalConnectivityCache::snapshot() const {
    ConnectivitySnapshot result;
    result.brickData = brickData_;
    result.components = components();
    return result;
}

std::size_t IncrementalConnectivityCache::graph_node_count() const noexcept {
    return graph_.node_count();
}

std::size_t IncrementalConnectivityCache::graph_edge_count() const noexcept {
    return graph_.edge_count();
}

ConnectivityStorageStats IncrementalConnectivityCache::storage_stats() const noexcept {
    ConnectivityStorageStats stats;
    stats.brickCount = brickData_.size();
    stats.graphNodeCount = graph_.node_count();
    stats.graphEdgeCount = graph_.edge_count();
    stats.brickHeaderBytes = brickData_.size() * sizeof(BrickConnectivity);
    for (const auto& [key, data] : brickData_) {
        (void)key;
        stats.localComponentCount += data.components.size();
        stats.localComponentLiveBytes += data.components.size() * sizeof(LocalComponent);
        stats.localComponentCapacityBytes += data.components.capacity() * sizeof(LocalComponent);
    }
    stats.graphLiveBytes = graph_.live_bytes();
    stats.graphCapacityBytes = graph_.capacity_bytes();
    stats.objectComponentLiveBytes = nodeAssignments_.size() * sizeof(NodeAssignment) +
                                     componentsById_.size() * sizeof(StoredComponent);
    stats.objectComponentCapacityBytes = nodeAssignments_.capacity() * sizeof(NodeAssignment) +
                                         componentsById_.capacity() * sizeof(StoredComponent);
    for (const StoredComponent& stored : componentsById_) {
        stats.objectComponentLiveBytes += live_bytes(stored.component.nodes);
        stats.objectComponentCapacityBytes += capacity_bytes(stored.component.nodes);
    }
    stats.totalLiveBytes = stats.brickHeaderBytes + stats.localComponentLiveBytes + stats.graphLiveBytes +
                           stats.objectComponentLiveBytes;
    stats.totalCapacityBytes = stats.brickHeaderBytes + stats.localComponentCapacityBytes + stats.graphCapacityBytes +
                               stats.objectComponentCapacityBytes;
    stats.cumulativeCapacityGrowthEvents = graph_.growth_events() + scratchGrowthEvents_;
    return stats;
}

bool IncrementalConnectivityCache::validate(const VoxelObject& object) const {
    if (!initialized_ || !graph_.validate()) return false;
    for (const auto& [key, brick] : object.bricks()) {
        if (brick.empty()) continue;
        const auto cached = brickData_.find(key);
        if (cached == brickData_.end() || cached->second.generation != brick.generation()) return false;
    }
    for (const auto& [key, data] : brickData_) {
        const Brick* brick = object.find_brick(key);
        if (brick == nullptr || brick->empty() || brick->generation() != data.generation) return false;
        if (data.components.empty()) return false;
    }
    if (nodeAssignments_.size() != graph_.node_count()) return false;
    if (!std::is_sorted(nodeAssignments_.begin(), nodeAssignments_.end(), [](const NodeAssignment& a, const NodeAssignment& b) {
            return a.node < b.node;
        })) {
        return false;
    }

    std::size_t assignedNodes = 0;
    for (const StoredComponent& stored : componentsById_) {
        for (const ComponentNodeRef node : stored.component.nodes) {
            const auto id = component_id_for(node);
            if (!id.has_value() || *id != stored.id || !graph_.contains(node)) return false;
            ++assignedNodes;
        }
    }
    return assignedNodes == graph_.node_count();
}

std::optional<SplitPlan> build_split_plan(
    const VoxelObject& object,
    const ConnectivitySnapshot& snapshot,
    std::size_t componentIndex) {
    if (componentIndex >= snapshot.components.size()) return std::nullopt;
    const ObjectComponent& component = snapshot.components[componentIndex];
    SplitPlan plan;
    plan.sourceObjectId = object.id();
    plan.voxelCount = component.voxelCount;

    std::map<BrickKey, std::vector<std::uint16_t>> localComponentsByBrick;
    for (const ComponentNodeRef node : component.nodes) {
        localComponentsByBrick[node.brick].push_back(node.localComponent);
    }

    for (auto& [key, localIds] : localComponentsByBrick) {
        const auto dataIt = snapshot.brickData.find(key);
        const Brick* brick = object.find_brick(key);
        if (dataIt == snapshot.brickData.end() || brick == nullptr || brick->generation() != dataIt->second.generation) {
            return std::nullopt;
        }
        LocalLabelBuild labeled = label_brick(object, key, {});
        if (labeled.components.size() != dataIt->second.components.size()) return std::nullopt;
        std::sort(localIds.begin(), localIds.end());
        localIds.erase(std::unique(localIds.begin(), localIds.end()), localIds.end());

        SplitBrickPlan brickPlan;
        brickPlan.key = key;
        brickPlan.sourceGeneration = dataIt->second.generation;
        for (std::uint16_t index = 0; index < kBrickVoxelCount; ++index) {
            const std::uint16_t label = labeled.labels[index];
            if (label == kNoComponent || !std::binary_search(localIds.begin(), localIds.end(), label)) continue;
            const MaterialId materialId = brick->material(index);
            if (materialId == kAirMaterial) continue;
            brickPlan.removeMask.set(index);
            brickPlan.voxels.push_back({index, materialId});
        }
        if (!brickPlan.voxels.empty()) plan.bricks.push_back(std::move(brickPlan));
    }
    return plan;
}

std::optional<VoxelObject> commit_split_plan(
    VoxelObject& source,
    const SplitPlan& plan,
    std::uint64_t newObjectId) {
    if (source.id() != plan.sourceObjectId) return std::nullopt;
    for (const SplitBrickPlan& brickPlan : plan.bricks) {
        const Brick* sourceBrick = source.find_brick(brickPlan.key);
        if (sourceBrick == nullptr || sourceBrick->generation() != brickPlan.sourceGeneration) return std::nullopt;
    }

    VoxelObject detached(newObjectId);
    detached.reserve_bricks(plan.bricks.size());
    for (const SplitBrickPlan& brickPlan : plan.bricks) {
        BrickMutation writeMutation;
        writeMutation.writes.reserve(brickPlan.voxels.size());
        for (const SplitVoxel& voxel : brickPlan.voxels) writeMutation.writes.push_back({voxel.index, voxel.material});
        detached.apply(brickPlan.key, writeMutation);
    }
    if (!detached.validate() || detached.occupied_voxel_count() != plan.voxelCount) return std::nullopt;

    for (const SplitBrickPlan& brickPlan : plan.bricks) {
        BrickMutation removeMutation;
        removeMutation.removeMask = brickPlan.removeMask;
        source.apply(brickPlan.key, removeMutation);
    }
    return detached;
}

} // namespace dve
