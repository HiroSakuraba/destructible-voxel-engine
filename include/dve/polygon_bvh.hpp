#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "dve/polygon_asset.hpp"
#include "dve/query.hpp"
#include "dve/transform.hpp"

namespace dve {

struct PolygonBvhHit {
    float distance{};
    Float3 position{};
    Float3 normal{};
    std::uint32_t triangleIndex{};
    std::uint32_t materialIndex{};
};


struct PolygonCapsuleContact {
    float distance{};
    float penetration{};
    Float3 capsulePoint{};
    Float3 polygonPoint{};
    Float3 normal{}; // points from polygon surface toward the capsule
    std::uint32_t triangleIndex{};
    std::uint32_t materialIndex{};
};

struct PolygonCapsuleSweepHit {
    float time{}; // normalized [0,1] along displacement
    Float3 capsulePoint{};
    Float3 polygonPoint{};
    Float3 normal{};
    std::uint32_t triangleIndex{};
    std::uint32_t materialIndex{};
    std::uint32_t conservativeAdvancementIterations{};
};

struct PolygonBvhStats {
    std::uint32_t nodes{};
    std::uint32_t leaves{};
    std::uint32_t maximumDepth{};
    std::uint32_t triangles{};
};

class PolygonBvh {
public:
    bool build(const CookedPolygonAsset& asset, std::string* error = nullptr);
    [[nodiscard]] std::optional<PolygonBvhHit> raycast(
        Float3 origin, Float3 direction, float maximumDistance) const noexcept;
    [[nodiscard]] bool sphere_overlap(Float3 center, float radius) const noexcept;
    // Exact triangle-level capsule overlap. The BVH is used only for candidate pruning;
    // every reported contact is produced by a segment-to-triangle closest-point query.
    [[nodiscard]] std::optional<PolygonCapsuleContact> capsule_overlap(
        const Capsule& capsule) const noexcept;
    // Continuous capsule cast using triangle-level conservative advancement. The distance
    // oracle is exact and the returned time is refined to `timeTolerance`; it does not use
    // the former sampled-sphere approximation.
    [[nodiscard]] std::optional<PolygonCapsuleSweepHit> sweep_capsule(
        const Capsule& capsule, Float3 displacement, float timeTolerance = 1.0e-5F,
        std::uint32_t maximumIterations = 64U) const noexcept;
    [[nodiscard]] const PolygonBvhStats& stats() const noexcept { return stats_; }
    [[nodiscard]] bool empty() const noexcept { return triangles_.empty(); }

private:
    struct Triangle {
        Float3 a{}, b{}, c{};
        PolygonBounds bounds{};
        Float3 centroid{};
        std::uint32_t triangleIndex{};
        std::uint32_t materialIndex{};
    };
    struct Node {
        PolygonBounds bounds{};
        std::uint32_t first{};
        std::uint32_t count{};
        std::uint32_t left{0xFFFFFFFFU};
        std::uint32_t right{0xFFFFFFFFU};
        [[nodiscard]] bool leaf() const noexcept { return left == 0xFFFFFFFFU; }
    };

    std::uint32_t build_node(std::uint32_t first, std::uint32_t count, std::uint32_t depth);
    std::vector<Triangle> triangles_;
    std::vector<Node> nodes_;
    PolygonBvhStats stats_{};
};

} // namespace dve
