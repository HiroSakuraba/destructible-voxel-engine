#include "dve/polygon_bvh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace dve {
namespace {

constexpr std::uint32_t kLeafTriangles = 8U;
constexpr float kGeometryEpsilon = 1.0e-7F;

[[nodiscard]] Float3 min3(Float3 a, Float3 b) noexcept {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
[[nodiscard]] Float3 max3(Float3 a, Float3 b) noexcept {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}
[[nodiscard]] PolygonBounds merge_bounds(PolygonBounds a, PolygonBounds b) noexcept {
    return {min3(a.minimum, b.minimum), max3(a.maximum, b.maximum)};
}
[[nodiscard]] float component(Float3 value, std::uint32_t axis) noexcept {
    return axis == 0U ? value.x : (axis == 1U ? value.y : value.z);
}
[[nodiscard]] Float3 cross3(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] bool finite3(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
[[nodiscard]] bool ray_box(
    Float3 origin, Float3 direction, const PolygonBounds& bounds, float maximum,
    float* nearOut = nullptr) noexcept {
    float nearValue = 0.0F;
    float farValue = maximum;
    for (std::uint32_t axis = 0; axis < 3U; ++axis) {
        const float o = component(origin, axis);
        const float d = component(direction, axis);
        const float lo = component(bounds.minimum, axis);
        const float hi = component(bounds.maximum, axis);
        if (std::abs(d) < 1.0e-8F) {
            if (o < lo || o > hi) return false;
            continue;
        }
        const float inverse = 1.0F / d;
        float a = (lo - o) * inverse;
        float b = (hi - o) * inverse;
        if (a > b) std::swap(a, b);
        nearValue = std::max(nearValue, a);
        farValue = std::min(farValue, b);
        if (nearValue > farValue) return false;
    }
    if (nearOut != nullptr) *nearOut = nearValue;
    return true;
}
[[nodiscard]] bool ray_triangle(
    Float3 origin, Float3 direction, Float3 a, Float3 b, Float3 c, float maximum,
    float* distance, Float3* normal) noexcept {
    const Float3 edge1 = subtract(b, a);
    const Float3 edge2 = subtract(c, a);
    const Float3 p = cross3(direction, edge2);
    const float determinant = dot(edge1, p);
    if (std::abs(determinant) < 1.0e-8F) return false;
    const float inverse = 1.0F / determinant;
    const Float3 t = subtract(origin, a);
    const float u = dot(t, p) * inverse;
    if (u < 0.0F || u > 1.0F) return false;
    const Float3 q = cross3(t, edge1);
    const float v = dot(direction, q) * inverse;
    if (v < 0.0F || u + v > 1.0F) return false;
    const float hitDistance = dot(edge2, q) * inverse;
    if (hitDistance < 0.0F || hitDistance > maximum || !std::isfinite(hitDistance)) return false;
    if (distance != nullptr) *distance = hitDistance;
    if (normal != nullptr) *normal = normalize(cross3(edge1, edge2));
    return true;
}
[[nodiscard]] bool bounds_overlap(const PolygonBounds& a, const PolygonBounds& b) noexcept {
    return a.maximum.x >= b.minimum.x && a.minimum.x <= b.maximum.x &&
           a.maximum.y >= b.minimum.y && a.minimum.y <= b.maximum.y &&
           a.maximum.z >= b.minimum.z && a.minimum.z <= b.maximum.z;
}
[[nodiscard]] bool sphere_box(Float3 center, float radius, const PolygonBounds& bounds) noexcept {
    const Float3 closest{
        std::clamp(center.x, bounds.minimum.x, bounds.maximum.x),
        std::clamp(center.y, bounds.minimum.y, bounds.maximum.y),
        std::clamp(center.z, bounds.minimum.z, bounds.maximum.z)};
    return length_squared(subtract(center, closest)) <= radius * radius;
}
[[nodiscard]] PolygonBounds capsule_bounds(const Capsule& capsule) noexcept {
    const Float3 radius{capsule.radius, capsule.radius, capsule.radius};
    return {subtract(min3(capsule.pointA, capsule.pointB), radius),
            add(max3(capsule.pointA, capsule.pointB), radius)};
}
[[nodiscard]] PolygonBounds swept_capsule_bounds(const Capsule& capsule, Float3 displacement) noexcept {
    const PolygonBounds start = capsule_bounds(capsule);
    Capsule moved = capsule;
    moved.pointA = add(moved.pointA, displacement);
    moved.pointB = add(moved.pointB, displacement);
    return merge_bounds(start, capsule_bounds(moved));
}

struct SegmentClosestPoints {
    Float3 first{};
    Float3 second{};
    float distanceSquared{};
};

[[nodiscard]] SegmentClosestPoints closest_segments(
    Float3 p1, Float3 q1, Float3 p2, Float3 q2) noexcept {
    const Float3 d1 = subtract(q1, p1);
    const Float3 d2 = subtract(q2, p2);
    const Float3 r = subtract(p1, p2);
    const float a = dot(d1, d1);
    const float e = dot(d2, d2);
    const float f = dot(d2, r);
    float s{};
    float t{};
    if (a <= kGeometryEpsilon && e <= kGeometryEpsilon) {
        return {p1, p2, length_squared(subtract(p1, p2))};
    }
    if (a <= kGeometryEpsilon) {
        s = 0.0F;
        t = std::clamp(f / e, 0.0F, 1.0F);
    } else {
        const float c = dot(d1, r);
        if (e <= kGeometryEpsilon) {
            t = 0.0F;
            s = std::clamp(-c / a, 0.0F, 1.0F);
        } else {
            const float b = dot(d1, d2);
            const float denominator = a * e - b * b;
            s = denominator > kGeometryEpsilon
                ? std::clamp((b * f - c * e) / denominator, 0.0F, 1.0F)
                : 0.0F;
            t = (b * s + f) / e;
            if (t < 0.0F) {
                t = 0.0F;
                s = std::clamp(-c / a, 0.0F, 1.0F);
            } else if (t > 1.0F) {
                t = 1.0F;
                s = std::clamp((b - c) / a, 0.0F, 1.0F);
            }
        }
    }
    const Float3 first = add(p1, multiply(d1, s));
    const Float3 second = add(p2, multiply(d2, t));
    return {first, second, length_squared(subtract(first, second))};
}

[[nodiscard]] Float3 closest_point_triangle(Float3 point, Float3 a, Float3 b, Float3 c) noexcept {
    const Float3 ab = subtract(b, a);
    const Float3 ac = subtract(c, a);
    const Float3 ap = subtract(point, a);
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    if (d1 <= 0.0F && d2 <= 0.0F) return a;

    const Float3 bp = subtract(point, b);
    const float d3 = dot(ab, bp);
    const float d4 = dot(ac, bp);
    if (d3 >= 0.0F && d4 <= d3) return b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
        const float v = d1 / (d1 - d3);
        return add(a, multiply(ab, v));
    }

    const Float3 cp = subtract(point, c);
    const float d5 = dot(ab, cp);
    const float d6 = dot(ac, cp);
    if (d6 >= 0.0F && d5 <= d6) return c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
        const float w = d2 / (d2 - d6);
        return add(a, multiply(ac, w));
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0F && d4 - d3 >= 0.0F && d5 - d6 >= 0.0F) {
        const Float3 bc = subtract(c, b);
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return add(b, multiply(bc, w));
    }

    const float denominator = 1.0F / (va + vb + vc);
    const float v = vb * denominator;
    const float w = vc * denominator;
    return add(a, add(multiply(ab, v), multiply(ac, w)));
}

[[nodiscard]] bool segment_triangle_intersection(
    Float3 p, Float3 q, Float3 a, Float3 b, Float3 c, Float3& point) noexcept {
    const Float3 direction = subtract(q, p);
    const float segmentLength = length(direction);
    if (!(segmentLength > kGeometryEpsilon)) return false;
    float distance{};
    if (!ray_triangle(p, multiply(direction, 1.0F / segmentLength), a, b, c,
                      segmentLength, &distance, nullptr)) return false;
    point = add(p, multiply(direction, distance / segmentLength));
    return true;
}

struct SegmentTriangleDistance {
    Float3 segmentPoint{};
    Float3 trianglePoint{};
    float distanceSquared{std::numeric_limits<float>::infinity()};
};

[[nodiscard]] SegmentTriangleDistance segment_triangle_distance(
    Float3 p, Float3 q, Float3 a, Float3 b, Float3 c) noexcept {
    Float3 intersection{};
    if (segment_triangle_intersection(p, q, a, b, c, intersection)) {
        return {intersection, intersection, 0.0F};
    }

    SegmentTriangleDistance best;
    const auto consider = [&](Float3 segmentPoint, Float3 trianglePoint) {
        const float distanceSquared = length_squared(subtract(segmentPoint, trianglePoint));
        if (distanceSquared < best.distanceSquared) {
            best = {segmentPoint, trianglePoint, distanceSquared};
        }
    };
    consider(p, closest_point_triangle(p, a, b, c));
    consider(q, closest_point_triangle(q, a, b, c));
    const std::array<std::pair<Float3, Float3>, 3> edges{{{a, b}, {b, c}, {c, a}}};
    for (const auto& [edgeA, edgeB] : edges) {
        const SegmentClosestPoints points = closest_segments(p, q, edgeA, edgeB);
        if (points.distanceSquared < best.distanceSquared) {
            best = {points.first, points.second, points.distanceSquared};
        }
    }
    return best;
}

[[nodiscard]] Float3 stable_contact_normal(
    const SegmentTriangleDistance& distance, Float3 a, Float3 b, Float3 c,
    Float3 capsuleMidpoint, Float3 preferredOpposition = {}) noexcept {
    const Float3 delta = subtract(distance.segmentPoint, distance.trianglePoint);
    if (length_squared(delta) > kGeometryEpsilon * kGeometryEpsilon) return normalize(delta);
    Float3 normal = normalize(cross3(subtract(b, a), subtract(c, a)));
    if (length_squared(normal) <= kGeometryEpsilon * kGeometryEpsilon) {
        normal = length_squared(preferredOpposition) > kGeometryEpsilon * kGeometryEpsilon
            ? normalize(preferredOpposition) : Float3{0.0F, 0.0F, 1.0F};
    }
    const float side = dot(subtract(capsuleMidpoint, a), normal);
    if (side < 0.0F || (std::abs(side) <= kGeometryEpsilon &&
        dot(normal, preferredOpposition) < 0.0F)) normal = multiply(normal, -1.0F);
    return normal;
}

} // namespace

bool PolygonBvh::build(const CookedPolygonAsset& asset, std::string* error) {
    triangles_.clear();
    nodes_.clear();
    stats_ = {};
    const auto valid = validate_polygon_asset(asset);
    if (!valid) {
        if (error != nullptr) *error = valid.message;
        return false;
    }
    std::uint32_t triangleIndex = 0U;
    for (const auto& submesh : asset.submeshes) {
        for (std::uint32_t offset = 0; offset < submesh.indexCount; offset += 3U) {
            const std::uint32_t base = submesh.firstIndex + offset;
            const Float3 a = asset.vertices[asset.indices[base]].position;
            const Float3 b = asset.vertices[asset.indices[base + 1U]].position;
            const Float3 c = asset.vertices[asset.indices[base + 2U]].position;
            const Float3 minimum = min3(a, min3(b, c));
            const Float3 maximum = max3(a, max3(b, c));
            triangles_.push_back({a, b, c, {minimum, maximum},
                                  multiply(add(add(a, b), c), 1.0F / 3.0F),
                                  triangleIndex++, submesh.materialIndex});
        }
    }
    if (triangles_.empty()) {
        if (error != nullptr) *error = "polygon BVH has no triangles";
        return false;
    }
    nodes_.reserve(triangles_.size() * 2U);
    build_node(0U, static_cast<std::uint32_t>(triangles_.size()), 0U);
    stats_.nodes = static_cast<std::uint32_t>(nodes_.size());
    stats_.triangles = static_cast<std::uint32_t>(triangles_.size());
    return true;
}

std::uint32_t PolygonBvh::build_node(
    std::uint32_t first, std::uint32_t count, std::uint32_t depth) {
    const std::uint32_t nodeIndex = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back({});
    Node& node = nodes_.back();
    node.first = first;
    node.count = count;
    node.bounds = triangles_[first].bounds;
    PolygonBounds centroidBounds{triangles_[first].centroid, triangles_[first].centroid};
    for (std::uint32_t index = 1U; index < count; ++index) {
        node.bounds = merge_bounds(node.bounds, triangles_[first + index].bounds);
        centroidBounds.minimum = min3(centroidBounds.minimum, triangles_[first + index].centroid);
        centroidBounds.maximum = max3(centroidBounds.maximum, triangles_[first + index].centroid);
    }
    stats_.maximumDepth = std::max(stats_.maximumDepth, depth);
    if (count <= kLeafTriangles) {
        ++stats_.leaves;
        return nodeIndex;
    }
    const Float3 extent = subtract(centroidBounds.maximum, centroidBounds.minimum);
    const std::uint32_t axis = extent.y > extent.x
        ? (extent.z > extent.y ? 2U : 1U)
        : (extent.z > extent.x ? 2U : 0U);
    const std::uint32_t middle = first + count / 2U;
    std::nth_element(
        triangles_.begin() + first, triangles_.begin() + middle,
        triangles_.begin() + first + count,
        [axis](const Triangle& a, const Triangle& b) {
            return component(a.centroid, axis) < component(b.centroid, axis);
        });
    const std::uint32_t left = build_node(first, middle - first, depth + 1U);
    const std::uint32_t right = build_node(middle, first + count - middle, depth + 1U);
    nodes_[nodeIndex].left = left;
    nodes_[nodeIndex].right = right;
    nodes_[nodeIndex].count = 0U;
    return nodeIndex;
}

std::optional<PolygonBvhHit> PolygonBvh::raycast(
    Float3 origin, Float3 direction, float maximumDistance) const noexcept {
    if (nodes_.empty() || !(maximumDistance > 0.0F) || !std::isfinite(maximumDistance))
        return std::nullopt;
    direction = normalize(direction);
    if (length_squared(direction) < 1.0e-8F) return std::nullopt;
    std::optional<PolygonBvhHit> best;
    std::vector<std::uint32_t> stack{0U};
    while (!stack.empty()) {
        const std::uint32_t nodeIndex = stack.back();
        stack.pop_back();
        const Node& node = nodes_[nodeIndex];
        const float limit = best ? best->distance : maximumDistance;
        float nearValue{};
        if (!ray_box(origin, direction, node.bounds, limit, &nearValue)) continue;
        if (node.leaf()) {
            for (std::uint32_t index = 0U; index < node.count; ++index) {
                const Triangle& triangle = triangles_[node.first + index];
                float distance{};
                Float3 normal{};
                if (ray_triangle(origin, direction, triangle.a, triangle.b, triangle.c,
                                 limit, &distance, &normal) &&
                    (!best || distance < best->distance)) {
                    best = PolygonBvhHit{distance, add(origin, multiply(direction, distance)),
                                         normal, triangle.triangleIndex,
                                         triangle.materialIndex};
                }
            }
        } else {
            float leftNear{};
            float rightNear{};
            const bool hitLeft = ray_box(origin, direction, nodes_[node.left].bounds,
                                         limit, &leftNear);
            const bool hitRight = ray_box(origin, direction, nodes_[node.right].bounds,
                                          limit, &rightNear);
            if (hitLeft && hitRight) {
                if (leftNear < rightNear) {
                    stack.push_back(node.right);
                    stack.push_back(node.left);
                } else {
                    stack.push_back(node.left);
                    stack.push_back(node.right);
                }
            } else if (hitLeft) {
                stack.push_back(node.left);
            } else if (hitRight) {
                stack.push_back(node.right);
            }
        }
    }
    return best;
}

bool PolygonBvh::sphere_overlap(Float3 center, float radius) const noexcept {
    if (nodes_.empty() || !(radius > 0.0F) || !std::isfinite(radius) || !finite3(center))
        return false;
    const float radiusSquared = radius * radius;
    std::vector<std::uint32_t> stack{0U};
    while (!stack.empty()) {
        const Node& node = nodes_[stack.back()];
        stack.pop_back();
        if (!sphere_box(center, radius, node.bounds)) continue;
        if (node.leaf()) {
            for (std::uint32_t index = 0U; index < node.count; ++index) {
                const Triangle& triangle = triangles_[node.first + index];
                const Float3 closest = closest_point_triangle(
                    center, triangle.a, triangle.b, triangle.c);
                if (length_squared(subtract(center, closest)) <= radiusSquared + 1.0e-7F)
                    return true;
            }
        } else {
            stack.push_back(node.left);
            stack.push_back(node.right);
        }
    }
    return false;
}

std::optional<PolygonCapsuleContact> PolygonBvh::capsule_overlap(
    const Capsule& capsule) const noexcept {
    if (nodes_.empty() || !(capsule.radius > 0.0F) || !std::isfinite(capsule.radius) ||
        !finite3(capsule.pointA) || !finite3(capsule.pointB)) return std::nullopt;
    const PolygonBounds queryBounds = capsule_bounds(capsule);
    const float radiusSquared = capsule.radius * capsule.radius;
    const Float3 midpoint = multiply(add(capsule.pointA, capsule.pointB), 0.5F);
    std::optional<PolygonCapsuleContact> best;
    std::vector<std::uint32_t> stack{0U};
    while (!stack.empty()) {
        const Node& node = nodes_[stack.back()];
        stack.pop_back();
        if (!bounds_overlap(queryBounds, node.bounds)) continue;
        if (node.leaf()) {
            for (std::uint32_t index = 0U; index < node.count; ++index) {
                const Triangle& triangle = triangles_[node.first + index];
                const SegmentTriangleDistance nearest = segment_triangle_distance(
                    capsule.pointA, capsule.pointB, triangle.a, triangle.b, triangle.c);
                if (nearest.distanceSquared > radiusSquared + 1.0e-6F) continue;
                const float distance = std::sqrt(std::max(0.0F, nearest.distanceSquared));
                const float penetration = std::max(0.0F, capsule.radius - distance);
                const Float3 normal = stable_contact_normal(
                    nearest, triangle.a, triangle.b, triangle.c, midpoint);
                if (!best || penetration > best->penetration + 1.0e-7F ||
                    (std::abs(penetration - best->penetration) <= 1.0e-7F &&
                     triangle.triangleIndex < best->triangleIndex)) {
                    best = PolygonCapsuleContact{
                        distance, penetration, nearest.segmentPoint, nearest.trianglePoint,
                        normal, triangle.triangleIndex, triangle.materialIndex};
                }
            }
        } else {
            stack.push_back(node.left);
            stack.push_back(node.right);
        }
    }
    return best;
}

std::optional<PolygonCapsuleSweepHit> PolygonBvh::sweep_capsule(
    const Capsule& capsule, Float3 displacement, float timeTolerance,
    std::uint32_t maximumIterations) const noexcept {
    if (nodes_.empty() || !(capsule.radius > 0.0F) || !std::isfinite(capsule.radius) ||
        !finite3(capsule.pointA) || !finite3(capsule.pointB) || !finite3(displacement) ||
        !(timeTolerance > 0.0F) || !std::isfinite(timeTolerance) ||
        maximumIterations == 0U) return std::nullopt;
    const float speed = length(displacement);
    if (!(speed > kGeometryEpsilon)) {
        const auto contact = capsule_overlap(capsule);
        if (!contact) return std::nullopt;
        return PolygonCapsuleSweepHit{0.0F, contact->capsulePoint, contact->polygonPoint,
                                      contact->normal, contact->triangleIndex,
                                      contact->materialIndex, 0U};
    }

    const PolygonBounds queryBounds = swept_capsule_bounds(capsule, displacement);
    std::vector<const Triangle*> candidates;
    std::vector<std::uint32_t> stack{0U};
    while (!stack.empty()) {
        const Node& node = nodes_[stack.back()];
        stack.pop_back();
        if (!bounds_overlap(queryBounds, node.bounds)) continue;
        if (node.leaf()) {
            for (std::uint32_t index = 0U; index < node.count; ++index)
                candidates.push_back(&triangles_[node.first + index]);
        } else {
            stack.push_back(node.left);
            stack.push_back(node.right);
        }
    }

    std::optional<PolygonCapsuleSweepHit> best;
    const float spatialTolerance = std::max(1.0e-6F, speed * timeTolerance);
    for (const Triangle* triangle : candidates) {
        float time = 0.0F;
        float previousSafeTime = 0.0F;
        SegmentTriangleDistance nearest{};
        std::uint32_t usedIterations = 0U;
        bool found = false;
        for (; usedIterations < maximumIterations; ++usedIterations) {
            const Float3 offset = multiply(displacement, time);
            nearest = segment_triangle_distance(
                add(capsule.pointA, offset), add(capsule.pointB, offset),
                triangle->a, triangle->b, triangle->c);
            const float distance = std::sqrt(std::max(0.0F, nearest.distanceSquared));
            const float clearance = distance - capsule.radius;
            if (clearance <= spatialTolerance) {
                found = true;
                if (clearance <= 0.0F && time > previousSafeTime + timeTolerance) {
                    float low = previousSafeTime;
                    float high = time;
                    for (std::uint32_t refine = 0U; refine < 24U &&
                         high - low > timeTolerance; ++refine) {
                        const float middle = (low + high) * 0.5F;
                        const Float3 middleOffset = multiply(displacement, middle);
                        const auto middleDistance = segment_triangle_distance(
                            add(capsule.pointA, middleOffset), add(capsule.pointB, middleOffset),
                            triangle->a, triangle->b, triangle->c);
                        if (middleDistance.distanceSquared <=
                            capsule.radius * capsule.radius) high = middle;
                        else low = middle;
                    }
                    time = high;
                    const Float3 refinedOffset = multiply(displacement, time);
                    nearest = segment_triangle_distance(
                        add(capsule.pointA, refinedOffset), add(capsule.pointB, refinedOffset),
                        triangle->a, triangle->b, triangle->c);
                }
                break;
            }
            const float safeAdvance = clearance / speed;
            if (time + safeAdvance > 1.0F + timeTolerance) break;
            previousSafeTime = time;
            const float advance = std::max(timeTolerance * 0.25F, safeAdvance * 0.9F);
            time = std::min(1.0F, time + advance);
            if (time >= 1.0F && previousSafeTime >= 1.0F - timeTolerance) break;
        }
        if (!found || time > 1.0F + timeTolerance) continue;
        time = std::clamp(time, 0.0F, 1.0F);
        const Float3 midpoint = add(
            multiply(add(capsule.pointA, capsule.pointB), 0.5F), multiply(displacement, time));
        const Float3 normal = stable_contact_normal(
            nearest, triangle->a, triangle->b, triangle->c, midpoint,
            multiply(displacement, -1.0F));
        PolygonCapsuleSweepHit hit{
            time, nearest.segmentPoint, nearest.trianglePoint, normal,
            triangle->triangleIndex, triangle->materialIndex, usedIterations + 1U};
        if (!best || hit.time < best->time - timeTolerance ||
            (std::abs(hit.time - best->time) <= timeTolerance &&
             hit.triangleIndex < best->triangleIndex)) best = hit;
    }
    return best;
}

} // namespace dve
