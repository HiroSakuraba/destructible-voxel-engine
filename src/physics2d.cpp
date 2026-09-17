#include "dve/physics2d.hpp"

#if defined(DVE_HAVE_BOX2D)
#include "dve/physics_box2d_backend.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>

namespace dve {
namespace {

constexpr float kEpsilon = 0.001F;

[[nodiscard]] bool finite_vec(TileVec2 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] bool aabb_overlap(const TileAabb& a, const TileAabb& b) noexcept {
    return a.min.x < b.max_x() && a.max_x() > b.min.x &&
           a.min.y < b.max_y() && a.max_y() > b.min.y;
}

struct NativeBody {
    Physics2DBodyDef def;
    Physics2DBodyState state;
    TileVec2 previousPositionPixels{};
    Physics2DColliderHandle collider{};
    Physics2DColliderDef colliderDef{};
    bool hasCollider{false};
    float dropThroughRemaining{};
};

[[nodiscard]] TileAabb native_body_aabb(const NativeBody& body,
                                        TileVec2 position) noexcept {
    const TileVec2 half = body.colliderDef.halfExtentsPixels;
    const TileVec2 center{position.x + body.colliderDef.localCenterPixels.x,
                          position.y + body.colliderDef.localCenterPixels.y};
    return {{center.x - half.x, center.y - half.y}, {half.x * 2.0F, half.y * 2.0F}};
}

struct RayAabbResult {
    bool hit{false};
    float fraction{1.0F};
    TileVec2 normal{};
};

[[nodiscard]] RayAabbResult ray_aabb(TileVec2 origin, TileVec2 translation,
                                     const TileAabb& box) noexcept {
    // Match Box2D ray semantics: initial overlap is not reported as a hit. This also prevents
    // character foot probes from selecting their own collider.
    if (origin.x >= box.min.x && origin.x <= box.max_x() &&
        origin.y >= box.min.y && origin.y <= box.max_y()) return {};
    float tMin = 0.0F;
    float tMax = 1.0F;
    TileVec2 normal{};

    const auto axis = [&](float originValue, float delta, float minValue, float maxValue,
                          TileVec2 nearNormal, float& ioMin, float& ioMax,
                          TileVec2& ioNormal) -> bool {
        if (std::fabs(delta) <= kEpsilon) {
            return originValue >= minValue && originValue <= maxValue;
        }
        float nearT = (minValue - originValue) / delta;
        float farT = (maxValue - originValue) / delta;
        TileVec2 candidate = nearNormal;
        if (nearT > farT) {
            std::swap(nearT, farT);
            candidate = {-nearNormal.x, -nearNormal.y};
        }
        if (nearT > ioMin) {
            ioMin = nearT;
            ioNormal = candidate;
        }
        ioMax = std::min(ioMax, farT);
        return ioMin <= ioMax;
    };

    if (!axis(origin.x, translation.x, box.min.x, box.max_x(), {-1.0F, 0.0F},
              tMin, tMax, normal)) return {};
    if (!axis(origin.y, translation.y, box.min.y, box.max_y(), {0.0F, -1.0F},
              tMin, tMax, normal)) return {};
    if (tMin < 0.0F || tMin > 1.0F) return {};
    return {true, tMin, normal};
}


[[nodiscard]] TileVec2 add(TileVec2 a, TileVec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
[[nodiscard]] TileVec2 subtract(TileVec2 a, TileVec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
[[nodiscard]] TileVec2 multiply(TileVec2 a, float scalar) noexcept { return {a.x * scalar, a.y * scalar}; }
[[nodiscard]] float dot(TileVec2 a, TileVec2 b) noexcept { return a.x * b.x + a.y * b.y; }
[[nodiscard]] float length_squared(TileVec2 value) noexcept { return dot(value, value); }

[[nodiscard]] TileVec2 rotate(TileVec2 value, float angle) noexcept {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {c * value.x - s * value.y, s * value.x + c * value.y};
}

[[nodiscard]] float point_aabb_distance_squared(TileVec2 point, const TileAabb& box) noexcept {
    const float dx = std::max({box.min.x - point.x, 0.0F, point.x - box.max_x()});
    const float dy = std::max({box.min.y - point.y, 0.0F, point.y - box.max_y()});
    return dx * dx + dy * dy;
}

[[nodiscard]] float point_segment_distance_squared(TileVec2 point, TileVec2 a, TileVec2 b) noexcept {
    const TileVec2 ab = subtract(b, a);
    const float denominator = length_squared(ab);
    const float t = denominator > kEpsilon
        ? std::clamp(dot(subtract(point, a), ab) / denominator, 0.0F, 1.0F)
        : 0.0F;
    return length_squared(subtract(point, add(a, multiply(ab, t))));
}

[[nodiscard]] bool segment_intersects_aabb(TileVec2 a, TileVec2 b, const TileAabb& box) noexcept {
    return ray_aabb(a, subtract(b, a), box).hit ||
        (a.x >= box.min.x && a.x <= box.max_x() && a.y >= box.min.y && a.y <= box.max_y()) ||
        (b.x >= box.min.x && b.x <= box.max_x() && b.y >= box.min.y && b.y <= box.max_y());
}

[[nodiscard]] float segment_aabb_distance_squared(TileVec2 a, TileVec2 b,
                                                   const TileAabb& box) noexcept {
    if (segment_intersects_aabb(a, b, box)) return 0.0F;
    float result = std::min(point_aabb_distance_squared(a, box),
                            point_aabb_distance_squared(b, box));
    const TileVec2 corners[4]{
        box.min, {box.max_x(), box.min.y}, {box.max_x(), box.max_y()}, {box.min.x, box.max_y()}};
    for (const TileVec2 corner : corners) {
        result = std::min(result, point_segment_distance_squared(corner, a, b));
    }
    return result;
}

[[nodiscard]] std::vector<TileVec2> query_polygon(const Physics2DQueryShape& shape) {
    std::vector<TileVec2> vertices;
    if (shape.shape == Physics2DShapeType::Box) {
        vertices = {
            {-shape.halfExtentsPixels.x, -shape.halfExtentsPixels.y},
            { shape.halfExtentsPixels.x, -shape.halfExtentsPixels.y},
            { shape.halfExtentsPixels.x,  shape.halfExtentsPixels.y},
            {-shape.halfExtentsPixels.x,  shape.halfExtentsPixels.y},
        };
    } else if (shape.shape == Physics2DShapeType::ConvexPolygon) {
        vertices = shape.verticesPixels;
    }
    for (TileVec2& point : vertices) point = add(shape.centerPixels, rotate(point, shape.angleRadians));
    return vertices;
}

[[nodiscard]] TileAabb query_shape_aabb(const Physics2DQueryShape& shape) {
    if (shape.shape == Physics2DShapeType::Circle) {
        return {{shape.centerPixels.x - shape.radiusPixels, shape.centerPixels.y - shape.radiusPixels},
                {shape.radiusPixels * 2.0F, shape.radiusPixels * 2.0F}};
    }
    if (shape.shape == Physics2DShapeType::Capsule) {
        const TileVec2 a = add(shape.centerPixels, rotate(shape.capsulePoint1Pixels, shape.angleRadians));
        const TileVec2 b = add(shape.centerPixels, rotate(shape.capsulePoint2Pixels, shape.angleRadians));
        const float minX = std::min(a.x, b.x) - shape.radiusPixels;
        const float minY = std::min(a.y, b.y) - shape.radiusPixels;
        const float maxX = std::max(a.x, b.x) + shape.radiusPixels;
        const float maxY = std::max(a.y, b.y) + shape.radiusPixels;
        return {{minX, minY}, {maxX - minX, maxY - minY}};
    }
    const std::vector<TileVec2> vertices = query_polygon(shape);
    if (vertices.empty()) return {};
    float minX = vertices.front().x;
    float minY = vertices.front().y;
    float maxX = minX;
    float maxY = minY;
    for (const TileVec2 point : vertices) {
        minX = std::min(minX, point.x);
        minY = std::min(minY, point.y);
        maxX = std::max(maxX, point.x);
        maxY = std::max(maxY, point.y);
    }
    return {{minX, minY}, {maxX - minX, maxY - minY}};
}

[[nodiscard]] bool polygon_aabb_overlap(std::span<const TileVec2> polygon,
                                        const TileAabb& box) noexcept {
    if (polygon.size() < 3U) return false;
    const TileVec2 boxCenter = box.center();
    const TileVec2 boxHalf{box.size.x * 0.5F, box.size.y * 0.5F};
    const auto separated = [&](TileVec2 axis) {
        const float axisLengthSquared = length_squared(axis);
        if (axisLengthSquared <= kEpsilon) return false;
        float minimum = dot(polygon.front(), axis);
        float maximum = minimum;
        for (const TileVec2 point : polygon.subspan(1)) {
            const float projection = dot(point, axis);
            minimum = std::min(minimum, projection);
            maximum = std::max(maximum, projection);
        }
        const float centerProjection = dot(boxCenter, axis);
        const float radius = std::fabs(axis.x) * boxHalf.x + std::fabs(axis.y) * boxHalf.y;
        return maximum < centerProjection - radius || minimum > centerProjection + radius;
    };
    if (separated({1.0F, 0.0F}) || separated({0.0F, 1.0F})) return false;
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        const TileVec2 edge = subtract(polygon[(index + 1U) % polygon.size()], polygon[index]);
        if (separated({-edge.y, edge.x})) return false;
    }
    return true;
}

[[nodiscard]] bool query_shape_overlaps_aabb(const Physics2DQueryShape& shape,
                                              const TileAabb& box) {
    if (!aabb_overlap(query_shape_aabb(shape), box)) return false;
    switch (shape.shape) {
    case Physics2DShapeType::Box:
    case Physics2DShapeType::ConvexPolygon: {
        const std::vector<TileVec2> polygon = query_polygon(shape);
        return polygon_aabb_overlap(polygon, box);
    }
    case Physics2DShapeType::Circle:
        return point_aabb_distance_squared(shape.centerPixels, box) <=
               shape.radiusPixels * shape.radiusPixels;
    case Physics2DShapeType::Capsule: {
        const TileVec2 a = add(shape.centerPixels, rotate(shape.capsulePoint1Pixels, shape.angleRadians));
        const TileVec2 b = add(shape.centerPixels, rotate(shape.capsulePoint2Pixels, shape.angleRadians));
        return segment_aabb_distance_squared(a, b, box) <= shape.radiusPixels * shape.radiusPixels;
    }
    }
    return false;
}

[[nodiscard]] bool validate_query_shape(const Physics2DQueryShape& shape) noexcept {
    if (!finite_vec(shape.centerPixels) || !finite_vec(shape.halfExtentsPixels) ||
        !finite_vec(shape.capsulePoint1Pixels) || !finite_vec(shape.capsulePoint2Pixels) ||
        !std::isfinite(shape.angleRadians) || !std::isfinite(shape.radiusPixels)) return false;
    switch (shape.shape) {
    case Physics2DShapeType::Box:
        return shape.halfExtentsPixels.x > 0.0F && shape.halfExtentsPixels.y > 0.0F;
    case Physics2DShapeType::Circle:
    case Physics2DShapeType::Capsule:
        return shape.radiusPixels > 0.0F;
    case Physics2DShapeType::ConvexPolygon:
        if (shape.verticesPixels.size() < 3U || shape.verticesPixels.size() > 64U) return false;
        return std::all_of(shape.verticesPixels.begin(), shape.verticesPixels.end(), finite_vec);
    }
    return false;
}

class NativeTilePhysicsWorld final : public Physics2DWorld {
public:
    explicit NativeTilePhysicsWorld(Physics2DWorldSettings settings) : settings_(std::move(settings)) {}

    [[nodiscard]] Physics2DBackend backend() const noexcept override { return Physics2DBackend::NativeTile; }
    [[nodiscard]] Physics2DCapabilities capabilities() const noexcept override {
        Physics2DCapabilities c;
        c.rigidBodyDynamics = true;
        c.rayCasts = true;
        c.oneWayPlatforms = true;
        c.slopeTiles = true;
        c.kinematicPlatforms = true;
        c.incrementalTileRecook = true;
        c.aabbQueries = true;
        c.shapeOverlapQueries = true;
        c.shapeCasts = true;
        return c;
    }
    [[nodiscard]] const Physics2DWorldSettings& settings() const noexcept override { return settings_; }

    bool set_tile_map(const TileMap& map, std::string* error) override {
        std::string validationError;
        if (!map.validate(&validationError)) {
            if (error != nullptr) *error = "invalid tile map: " + validationError;
            return false;
        }
        grid_ = build_collision_grid(map);
        hasGrid_ = true;
        return true;
    }

    bool update_tile_map_region(const TileMap& map, Physics2DTileRegion region,
                                std::string* error) override {
        std::string validationError;
        if (!map.validate(&validationError)) {
            if (error != nullptr) *error = "invalid tile map: " + validationError;
            return false;
        }
        if (!hasGrid_ || region.minCol > region.maxCol || region.minRow > region.maxRow ||
            region.maxCol >= grid_.width || region.maxRow >= grid_.height ||
            !update_collision_grid_region(grid_, map, region.minCol, region.minRow,
                                          region.maxCol, region.maxRow)) {
            if (error != nullptr) *error = "tile update region is outside the active collision grid";
            return false;
        }
        return true;
    }

    void clear_tile_map() override {
        grid_ = {};
        hasGrid_ = false;
    }

    [[nodiscard]] Physics2DBodyHandle create_body(const Physics2DBodyDef& def,
                                                   std::string* error) override {
        if (!finite_vec(def.positionPixels) || !finite_vec(def.linearVelocityPixelsPerSecond) ||
            !std::isfinite(def.angleRadians) || !std::isfinite(def.angularVelocityRadiansPerSecond) ||
            !std::isfinite(def.linearDamping) || !std::isfinite(def.angularDamping) ||
            !std::isfinite(def.gravityScale) || def.gravityScale < 0.0F) {
            if (error != nullptr) *error = "body definition contains an invalid value";
            return {};
        }
        const Physics2DBodyHandle handle{nextHandle_++};
        NativeBody body;
        body.def = def;
        body.state.positionPixels = def.positionPixels;
        body.previousPositionPixels = def.positionPixels;
        body.state.angleRadians = def.angleRadians;
        body.state.linearVelocityPixelsPerSecond = def.linearVelocityPixelsPerSecond;
        body.state.angularVelocityRadiansPerSecond = def.angularVelocityRadiansPerSecond;
        body.state.enabled = def.enabled;
        bodies_.emplace(handle.value, std::move(body));
        return handle;
    }

    bool destroy_body(Physics2DBodyHandle body) override {
        const auto it = bodies_.find(body.value);
        if (it == bodies_.end()) return false;
        if (it->second.hasCollider) colliderOwners_.erase(it->second.collider.value);
        bodies_.erase(it);
        return true;
    }

    [[nodiscard]] Physics2DColliderHandle add_collider(Physics2DBodyHandle body,
                                                        const Physics2DColliderDef& def,
                                                        std::string* error) override {
        auto it = bodies_.find(body.value);
        if (it == bodies_.end()) {
            if (error != nullptr) *error = "body handle is invalid";
            return {};
        }
        if (def.sensor) {
            if (error != nullptr) *error = "NativeTile does not support sensor colliders; select Box2D";
            return {};
        }
        if (def.shape != Physics2DShapeType::Box) {
            if (error != nullptr) {
                *error = "NativeTile supports box colliders only; select Box2D for other shapes";
            }
            return {};
        }
        if (!(def.halfExtentsPixels.x > 0.0F && def.halfExtentsPixels.y > 0.0F) ||
            !finite_vec(def.localCenterPixels) || !finite_vec(def.halfExtentsPixels) ||
            !std::isfinite(def.tangentSpeedPixelsPerSecond)) {
            if (error != nullptr) *error = "box collider dimensions must be finite and positive";
            return {};
        }
        if (it->second.hasCollider) {
            if (error != nullptr) *error = "NativeTile supports one box collider per body";
            return {};
        }
        const Physics2DColliderHandle handle{nextHandle_++};
        it->second.collider = handle;
        it->second.colliderDef = def;
        it->second.hasCollider = true;
        colliderOwners_.emplace(handle.value, body.value);
        return handle;
    }

    bool destroy_collider(Physics2DColliderHandle collider) override {
        const auto ownerIt = colliderOwners_.find(collider.value);
        if (ownerIt == colliderOwners_.end()) return false;
        const auto bodyIt = bodies_.find(ownerIt->second);
        if (bodyIt != bodies_.end()) {
            bodyIt->second.collider = {};
            bodyIt->second.hasCollider = false;
        }
        colliderOwners_.erase(ownerIt);
        return true;
    }

    void step(float frameDeltaSeconds) override {
        events_.clear();
        if (!(frameDeltaSeconds > 0.0F) || !std::isfinite(frameDeltaSeconds)) return;
        accumulator_ += std::min(frameDeltaSeconds,
                                settings_.fixedTimeStep * static_cast<float>(settings_.maxFrameSteps));
        std::uint32_t steps = 0;
        while (accumulator_ + std::numeric_limits<float>::epsilon() >= settings_.fixedTimeStep &&
               steps < settings_.maxFrameSteps) {
            fixed_step(settings_.fixedTimeStep);
            accumulator_ -= settings_.fixedTimeStep;
            ++steps;
        }
    }

    [[nodiscard]] bool body_state(Physics2DBodyHandle body, Physics2DBodyState& out) const override {
        const auto it = bodies_.find(body.value);
        if (it == bodies_.end()) return false;
        out = it->second.state;
        return true;
    }

    bool set_body_transform(Physics2DBodyHandle body, TileVec2 positionPixels,
                            float angleRadians) override {
        auto it = bodies_.find(body.value);
        if (it == bodies_.end() || !finite_vec(positionPixels) || !std::isfinite(angleRadians)) return false;
        it->second.state.positionPixels = positionPixels;
        it->second.previousPositionPixels = positionPixels;
        it->second.state.angleRadians = angleRadians;
        return true;
    }

    bool set_body_linear_velocity(Physics2DBodyHandle body,
                                  TileVec2 velocityPixelsPerSecond) override {
        auto it = bodies_.find(body.value);
        if (it == bodies_.end() || !finite_vec(velocityPixelsPerSecond)) return false;
        it->second.state.linearVelocityPixelsPerSecond = velocityPixelsPerSecond;
        return true;
    }

    bool set_body_gravity_scale(Physics2DBodyHandle body, float gravityScale) override {
        auto it = bodies_.find(body.value);
        if (it == bodies_.end() || !std::isfinite(gravityScale) || gravityScale < 0.0F) return false;
        it->second.def.gravityScale = gravityScale;
        return true;
    }

    bool apply_linear_impulse(Physics2DBodyHandle body,
                              TileVec2 impulsePixelKilogramsPerSecond) override {
        auto it = bodies_.find(body.value);
        if (it == bodies_.end() || !finite_vec(impulsePixelKilogramsPerSecond)) return false;
        // The native reference has no mass model. Treat impulse as an immediate velocity delta.
        it->second.state.linearVelocityPixelsPerSecond.x += impulsePixelKilogramsPerSecond.x;
        it->second.state.linearVelocityPixelsPerSecond.y += impulsePixelKilogramsPerSecond.y;
        return true;
    }

    bool set_body_enabled(Physics2DBodyHandle body, bool enabled) override {
        auto it = bodies_.find(body.value);
        if (it == bodies_.end()) return false;
        it->second.state.enabled = enabled;
        return true;
    }

    bool drop_through_one_way(Physics2DBodyHandle body, float seconds) override {
        auto it = bodies_.find(body.value);
        if (it == bodies_.end() || !std::isfinite(seconds)) return false;
        it->second.dropThroughRemaining = std::max(0.0F, seconds);
        return true;
    }

    [[nodiscard]] Physics2DRayCastHit ray_cast(TileVec2 originPixels,
                                                TileVec2 translationPixels,
                                                std::uint64_t categoryBits,
                                                std::uint64_t maskBits) const override {
        Physics2DRayCastHit best;
        if (!finite_vec(originPixels) || !finite_vec(translationPixels)) return best;
        const float length = std::hypot(translationPixels.x, translationPixels.y);
        if (length <= 0.0001F) return best;

        if (hasGrid_) {
            const float spacing = std::max(0.5F, 0.125F * static_cast<float>(
                std::min(grid_.tileWidth, grid_.tileHeight)));
            const std::uint32_t samples = std::max<std::uint32_t>(1U,
                static_cast<std::uint32_t>(std::ceil(length / spacing)));
            for (std::uint32_t i = 0; i <= samples; ++i) {
                const float fraction = static_cast<float>(i) / static_cast<float>(samples);
                const TileVec2 point{originPixels.x + translationPixels.x * fraction,
                                     originPixels.y + translationPixels.y * fraction};
                const auto col = static_cast<std::int64_t>(std::floor(
                    point.x / static_cast<float>(grid_.tileWidth)));
                const auto row = static_cast<std::int64_t>(std::floor(
                    point.y / static_cast<float>(grid_.tileHeight)));
                const TileCollision collision = grid_.at(col, row);
                if (collision == TileCollision::Empty) continue;
                if (collision == TileCollision::OneWayTop && translationPixels.y <= 0.0F) continue;

                TileVec2 normal{};
                bool inside = true;
                if (collision == TileCollision::SlopeUpRight ||
                    collision == TileCollision::SlopeUpLeft) {
                    const float tw = static_cast<float>(grid_.tileWidth);
                    const float th = static_cast<float>(grid_.tileHeight);
                    const float local = std::clamp((point.x - static_cast<float>(col) * tw) / tw,
                                                   0.0F, 1.0F);
                    const float surface = static_cast<float>(row) * th +
                        (collision == TileCollision::SlopeUpRight ? th * (1.0F - local) : th * local);
                    inside = point.y >= surface - kEpsilon;
                    const float nx = collision == TileCollision::SlopeUpRight ? -th : th;
                    const float ny = -tw;
                    const float invLength = 1.0F / std::sqrt(nx * nx + ny * ny);
                    normal = {nx * invLength, ny * invLength};
                } else if (collision == TileCollision::OneWayTop) {
                    normal = {0.0F, -1.0F};
                    const float top = static_cast<float>(row * static_cast<std::int64_t>(grid_.tileHeight));
                    inside = point.y <= top + spacing;
                } else {
                    const float ax = std::fabs(translationPixels.x);
                    const float ay = std::fabs(translationPixels.y);
                    normal = ax > ay
                        ? TileVec2{translationPixels.x > 0.0F ? -1.0F : 1.0F, 0.0F}
                        : TileVec2{0.0F, translationPixels.y > 0.0F ? -1.0F : 1.0F};
                }
                if (!inside) continue;
                best.hit = true;
                best.pointPixels = point;
                best.normal = normal;
                best.fraction = fraction;
                break;
            }
        }

        for (const auto& [key, body] : bodies_) {
            if (!body.state.enabled || !body.hasCollider) continue;
            const auto& collider = body.colliderDef;
            if ((categoryBits & collider.maskBits) == 0U ||
                (collider.categoryBits & maskBits) == 0U) continue;
            if (collider.oneWayPlatform && translationPixels.y <= 0.0F) continue;
            const RayAabbResult result = ray_aabb(originPixels, translationPixels,
                                                   native_body_aabb(body, body.state.positionPixels));
            if (!result.hit || (collider.oneWayPlatform && result.normal.y > -0.5F) ||
                (best.hit && result.fraction >= best.fraction)) continue;
            best.hit = true;
            best.body = Physics2DBodyHandle{key};
            best.collider = body.collider;
            best.bodyUserTag = body.def.userTag;
            best.colliderUserTag = collider.userTag;
            best.fraction = result.fraction;
            best.normal = result.normal;
            best.pointPixels = {originPixels.x + translationPixels.x * result.fraction,
                                originPixels.y + translationPixels.y * result.fraction};
            best.surfaceVelocityPixelsPerSecond = body.state.linearVelocityPixelsPerSecond;
            if (std::fabs(result.normal.y) > 0.5F) {
                best.surfaceVelocityPixelsPerSecond.x += collider.tangentSpeedPixelsPerSecond;
            }
        }
        return best;
    }

    [[nodiscard]] std::size_t query_aabb(TileAabb bounds,
                                         std::span<Physics2DOverlapHit> out,
                                         std::uint64_t categoryBits,
                                         std::uint64_t maskBits) const override {
        if (!finite_vec(bounds.min) || !finite_vec(bounds.size) ||
            bounds.size.x < 0.0F || bounds.size.y < 0.0F) return 0;
        std::size_t count = 0;
        for (const auto& [key, body] : bodies_) {
            if (!body.state.enabled || !body.hasCollider) continue;
            const auto& collider = body.colliderDef;
            if ((categoryBits & collider.maskBits) == 0U ||
                (collider.categoryBits & maskBits) == 0U) continue;
            if (!aabb_overlap(bounds, native_body_aabb(body, body.state.positionPixels))) continue;
            if (count < out.size()) {
                out[count] = {Physics2DBodyHandle{key}, body.collider,
                              body.def.userTag, collider.userTag};
            }
            ++count;
        }
        return count;
    }


    [[nodiscard]] std::size_t query_shape(const Physics2DQueryShape& shape,
                                          std::span<Physics2DOverlapHit> out,
                                          const Physics2DQueryFilter& filter) const override {
        if (!validate_query_shape(shape) || filter.categoryBits == 0U || filter.maskBits == 0U) return 0;

        std::vector<Physics2DOverlapHit> hits;
        hits.reserve(bodies_.size() + 1U);
        for (const auto& [key, body] : bodies_) {
            const Physics2DBodyHandle handle{key};
            if (handle == filter.ignoredBody || !body.state.enabled || !body.hasCollider) continue;
            const auto& collider = body.colliderDef;
            if (!filter.includeSensors && collider.sensor) continue;
            if ((filter.categoryBits & collider.maskBits) == 0U ||
                (collider.categoryBits & filter.maskBits) == 0U) continue;
            if (!query_shape_overlaps_aabb(shape, native_body_aabb(body, body.state.positionPixels))) continue;
            hits.push_back({handle, body.collider, body.def.userTag, collider.userTag});
        }

        if (filter.includeTileMap && hasGrid_) {
            const TileAabb bounds = query_shape_aabb(shape);
            const auto minCol = static_cast<std::int64_t>(std::floor(
                bounds.min.x / static_cast<float>(grid_.tileWidth)));
            const auto maxCol = static_cast<std::int64_t>(std::floor(
                (bounds.max_x() - kEpsilon) / static_cast<float>(grid_.tileWidth)));
            const auto minRow = static_cast<std::int64_t>(std::floor(
                bounds.min.y / static_cast<float>(grid_.tileHeight)));
            const auto maxRow = static_cast<std::int64_t>(std::floor(
                (bounds.max_y() - kEpsilon) / static_cast<float>(grid_.tileHeight)));
            bool tileHit = false;
            for (std::int64_t row = minRow; row <= maxRow && !tileHit; ++row) {
                for (std::int64_t col = minCol; col <= maxCol; ++col) {
                    if (grid_.at(col, row) == TileCollision::Empty) continue;
                    const TileAabb tile{
                        {static_cast<float>(col * static_cast<std::int64_t>(grid_.tileWidth)),
                         static_cast<float>(row * static_cast<std::int64_t>(grid_.tileHeight))},
                        {static_cast<float>(grid_.tileWidth), static_cast<float>(grid_.tileHeight)}};
                    if (query_shape_overlaps_aabb(shape, tile)) {
                        tileHit = true;
                        break;
                    }
                }
            }
            if (tileHit) hits.insert(hits.begin(), Physics2DOverlapHit{});
        }

        std::sort(hits.begin(), hits.end(), [](const Physics2DOverlapHit& a,
                                               const Physics2DOverlapHit& b) {
            if (a.body.value != b.body.value) return a.body.value < b.body.value;
            return a.collider.value < b.collider.value;
        });
        const std::size_t writeCount = std::min(out.size(), hits.size());
        for (std::size_t index = 0; index < writeCount; ++index) out[index] = hits[index];
        return hits.size();
    }

    [[nodiscard]] Physics2DShapeCastHit cast_shape(
        const Physics2DQueryShape& shape, TileVec2 translationPixels,
        const Physics2DQueryFilter& filter) const override {
        Physics2DShapeCastHit result;
        if (!validate_query_shape(shape) || !finite_vec(translationPixels)) return result;

        Physics2DOverlapHit initial{};
        if (query_shape(shape, std::span<Physics2DOverlapHit>(&initial, 1U), filter) > 0U) {
            result.startedOverlapping = true;
            result.body = initial.body;
            result.collider = initial.collider;
            result.bodyUserTag = initial.bodyUserTag;
            result.colliderUserTag = initial.colliderUserTag;
            return result;
        }

        const float distance = std::hypot(translationPixels.x, translationPixels.y);
        if (distance <= kEpsilon) return result;
        const TileAabb bounds = query_shape_aabb(shape);
        const float feature = std::max(0.5F, std::min(bounds.size.x, bounds.size.y) * 0.25F);
        const std::uint32_t sampleCount = std::clamp<std::uint32_t>(
            static_cast<std::uint32_t>(std::ceil(distance / feature)), 1U, 4096U);

        float previousFraction = 0.0F;
        float hitFraction = 1.0F;
        Physics2DOverlapHit hit{};
        bool found = false;
        Physics2DQueryShape moved = shape;
        for (std::uint32_t sample = 1U; sample <= sampleCount; ++sample) {
            const float fraction = static_cast<float>(sample) / static_cast<float>(sampleCount);
            moved.centerPixels = add(shape.centerPixels, multiply(translationPixels, fraction));
            Physics2DOverlapHit candidate{};
            if (query_shape(moved, std::span<Physics2DOverlapHit>(&candidate, 1U), filter) > 0U) {
                hitFraction = fraction;
                hit = candidate;
                found = true;
                break;
            }
            previousFraction = fraction;
        }
        if (!found) return result;

        for (int iteration = 0; iteration < 18; ++iteration) {
            const float middle = (previousFraction + hitFraction) * 0.5F;
            moved.centerPixels = add(shape.centerPixels, multiply(translationPixels, middle));
            Physics2DOverlapHit candidate{};
            if (query_shape(moved, std::span<Physics2DOverlapHit>(&candidate, 1U), filter) > 0U) {
                hitFraction = middle;
                hit = candidate;
            } else {
                previousFraction = middle;
            }
        }

        result.hit = true;
        result.fraction = hitFraction;
        result.body = hit.body;
        result.collider = hit.collider;
        result.bodyUserTag = hit.bodyUserTag;
        result.colliderUserTag = hit.colliderUserTag;
        result.pointPixels = add(shape.centerPixels, multiply(translationPixels, hitFraction));
        if (std::fabs(translationPixels.x) > std::fabs(translationPixels.y)) {
            result.normal = {translationPixels.x > 0.0F ? -1.0F : 1.0F, 0.0F};
        } else {
            result.normal = {0.0F, translationPixels.y > 0.0F ? -1.0F : 1.0F};
        }
        if (hit.body) {
            const auto bodyIt = bodies_.find(hit.body.value);
            if (bodyIt != bodies_.end()) {
                result.surfaceVelocityPixelsPerSecond =
                    bodyIt->second.state.linearVelocityPixelsPerSecond;
                if (std::fabs(result.normal.y) > 0.5F) {
                    result.surfaceVelocityPixelsPerSecond.x +=
                        bodyIt->second.colliderDef.tangentSpeedPixelsPerSecond;
                }
            }
        }
        return result;
    }

    [[nodiscard]] bool overlaps_tile_map(TileAabb bounds) const override {
        if (!hasGrid_ || !finite_vec(bounds.min) || !finite_vec(bounds.size) ||
            bounds.size.x < 0.0F || bounds.size.y < 0.0F) return false;
        const auto minCol = static_cast<std::int64_t>(std::floor(
            bounds.min.x / static_cast<float>(grid_.tileWidth)));
        const auto maxCol = static_cast<std::int64_t>(std::floor(
            (bounds.max_x() - kEpsilon) / static_cast<float>(grid_.tileWidth)));
        const auto minRow = static_cast<std::int64_t>(std::floor(
            bounds.min.y / static_cast<float>(grid_.tileHeight)));
        const auto maxRow = static_cast<std::int64_t>(std::floor(
            (bounds.max_y() - kEpsilon) / static_cast<float>(grid_.tileHeight)));
        for (std::int64_t row = minRow; row <= maxRow; ++row) {
            for (std::int64_t col = minCol; col <= maxCol; ++col) {
                if (grid_.at(col, row) != TileCollision::Empty) return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::span<const Physics2DEvent> events() const noexcept override { return events_; }

private:
    void append_contact(std::uint64_t bodyKey, const NativeBody& body,
                        Physics2DBodyHandle otherBody, Physics2DColliderHandle otherCollider,
                        std::uint64_t otherBodyTag, std::uint64_t otherColliderTag,
                        TileVec2 point, TileVec2 normal, float speed) {
        Physics2DEvent event;
        event.type = Physics2DEventType::ContactHit;
        event.bodyA = Physics2DBodyHandle{bodyKey};
        event.colliderA = body.collider;
        event.bodyUserTagA = body.def.userTag;
        event.colliderUserTagA = body.colliderDef.userTag;
        event.bodyB = otherBody;
        event.colliderB = otherCollider;
        event.bodyUserTagB = otherBodyTag;
        event.colliderUserTagB = otherColliderTag;
        event.pointPixels = point;
        event.normal = normal;
        event.approachSpeedPixelsPerSecond = speed;
        events_.push_back(event);
    }

    void integrate_body(NativeBody& body, float dt, bool gravity) {
        body.previousPositionPixels = body.state.positionPixels;
        if (gravity) {
            body.state.linearVelocityPixelsPerSecond.x +=
                settings_.gravityPixelsPerSecondSquared.x * body.def.gravityScale * dt;
            body.state.linearVelocityPixelsPerSecond.y +=
                settings_.gravityPixelsPerSecondSquared.y * body.def.gravityScale * dt;
        }
        const float linearDamping = std::max(0.0F, 1.0F - body.def.linearDamping * dt);
        body.state.linearVelocityPixelsPerSecond.x *= linearDamping;
        body.state.linearVelocityPixelsPerSecond.y *= linearDamping;
        if (!body.def.fixedRotation) {
            const float angularDamping = std::max(0.0F, 1.0F - body.def.angularDamping * dt);
            body.state.angularVelocityRadiansPerSecond *= angularDamping;
            body.state.angleRadians += body.state.angularVelocityRadiansPerSecond * dt;
        }
    }

    void fixed_step(float dt) {
        for (auto& [key, body] : bodies_) {
            static_cast<void>(key);
            body.dropThroughRemaining = std::max(0.0F, body.dropThroughRemaining - dt);
        }

        // Kinematic platforms advance before dynamic bodies so landing and ground probes observe
        // their final pose for this fixed step.
        for (auto& [key, body] : bodies_) {
            static_cast<void>(key);
            if (!body.state.enabled || body.def.type != Physics2DBodyType::Kinematic) continue;
            integrate_body(body, dt, false);
            body.state.positionPixels.x += body.state.linearVelocityPixelsPerSecond.x * dt;
            body.state.positionPixels.y += body.state.linearVelocityPixelsPerSecond.y * dt;
        }

        for (auto& [bodyKey, body] : bodies_) {
            if (!body.state.enabled || body.def.type != Physics2DBodyType::Dynamic) continue;
            integrate_body(body, dt, true);
            const TileVec2 desired{body.state.linearVelocityPixelsPerSecond.x * dt,
                                   body.state.linearVelocityPixelsPerSecond.y * dt};
            const TileVec2 startPosition = body.previousPositionPixels;
            TileVec2 resolvedPosition{startPosition.x + desired.x, startPosition.y + desired.y};
            TileVec2 contactNormal{};
            bool contacted = false;

            if (body.hasCollider && hasGrid_) {
                const TileAabb startAabb = native_body_aabb(body, startPosition);
                AabbMoveOptions options;
                options.ignoreOneWayPlatforms = body.dropThroughRemaining > 0.0F;
                const AabbSweep result = move_aabb(grid_, startAabb, desired, options);
                const TileVec2 half = body.colliderDef.halfExtentsPixels;
                const TileVec2 resolvedCenter{result.position.x + half.x,
                                              result.position.y + half.y};
                resolvedPosition = {resolvedCenter.x - body.colliderDef.localCenterPixels.x,
                                    resolvedCenter.y - body.colliderDef.localCenterPixels.y};
                if (result.hitLeft || result.hitRight) body.state.linearVelocityPixelsPerSecond.x = 0.0F;
                if (result.hitTop || result.hitBottom) body.state.linearVelocityPixelsPerSecond.y = 0.0F;
                if (result.hitLeft || result.hitRight || result.hitTop || result.hitBottom) {
                    contacted = true;
                    if (result.hitBottom) contactNormal = result.groundNormal;
                    else if (result.hitTop) contactNormal = {0.0F, 1.0F};
                    else if (result.hitRight) contactNormal = {-1.0F, 0.0F};
                    else contactNormal = {1.0F, 0.0F};
                    append_contact(bodyKey, body, {}, {}, 0, 0,
                                   {result.position.x + body.colliderDef.halfExtentsPixels.x,
                                    result.position.y + body.colliderDef.halfExtentsPixels.y},
                                   contactNormal, std::hypot(desired.x, desired.y) / dt);
                }
            }

            body.state.positionPixels = resolvedPosition;
            if (!body.hasCollider) continue;

            // Collide dynamic AABBs against authored static/kinematic box bodies. Dynamic-dynamic
            // collision remains a Box2D feature; this path is intentionally platformer-focused.
            const TileAabb oldBox = native_body_aabb(body, startPosition);
            for (const auto& [otherKey, other] : bodies_) {
                if (otherKey == bodyKey || !other.state.enabled || !other.hasCollider ||
                    other.def.type == Physics2DBodyType::Dynamic) continue;
                const TileAabb oldObstacle = native_body_aabb(other, other.previousPositionPixels);
                const TileAabb currentObstacle = native_body_aabb(other, other.state.positionPixels);
                const TileVec2 obstacleDelta{
                    other.state.positionPixels.x - other.previousPositionPixels.x,
                    other.state.positionPixels.y - other.previousPositionPixels.y};
                const TileVec2 relative{desired.x - obstacleDelta.x, desired.y - obstacleDelta.y};
                TileAabb currentBox = native_body_aabb(body, body.state.positionPixels);

                bool hit = false;
                TileVec2 normal{};
                const bool oneWay = other.colliderDef.oneWayPlatform;
                const bool canLandOneWay = !oneWay || (body.dropThroughRemaining <= 0.0F && relative.y > 0.0F);
                if (canLandOneWay && relative.y > 0.0F && oldBox.max_y() <= oldObstacle.min.y + kEpsilon &&
                    oldBox.max_y() + relative.y >= oldObstacle.min.y - kEpsilon &&
                    currentBox.max_x() > currentObstacle.min.x + kEpsilon &&
                    currentBox.min.x < currentObstacle.max_x() - kEpsilon) {
                    body.state.positionPixels.y += currentObstacle.min.y - currentBox.max_y();
                    body.state.linearVelocityPixelsPerSecond.y = other.state.linearVelocityPixelsPerSecond.y;
                    normal = {0.0F, -1.0F};
                    hit = true;
                } else if (!oneWay && relative.y < 0.0F && oldBox.min.y >= oldObstacle.max_y() - kEpsilon &&
                           oldBox.min.y + relative.y <= oldObstacle.max_y() + kEpsilon &&
                           currentBox.max_x() > currentObstacle.min.x + kEpsilon &&
                           currentBox.min.x < currentObstacle.max_x() - kEpsilon) {
                    body.state.positionPixels.y += currentObstacle.max_y() - currentBox.min.y;
                    body.state.linearVelocityPixelsPerSecond.y = std::max(
                        body.state.linearVelocityPixelsPerSecond.y,
                        other.state.linearVelocityPixelsPerSecond.y);
                    normal = {0.0F, 1.0F};
                    hit = true;
                }

                currentBox = native_body_aabb(body, body.state.positionPixels);
                if (!oneWay && !hit && relative.x > 0.0F && oldBox.max_x() <= oldObstacle.min.x + kEpsilon &&
                    oldBox.max_x() + relative.x >= oldObstacle.min.x - kEpsilon &&
                    currentBox.max_y() > currentObstacle.min.y + kEpsilon &&
                    currentBox.min.y < currentObstacle.max_y() - kEpsilon) {
                    body.state.positionPixels.x += currentObstacle.min.x - currentBox.max_x();
                    body.state.linearVelocityPixelsPerSecond.x = other.state.linearVelocityPixelsPerSecond.x;
                    normal = {-1.0F, 0.0F};
                    hit = true;
                } else if (!oneWay && !hit && relative.x < 0.0F && oldBox.min.x >= oldObstacle.max_x() - kEpsilon &&
                           oldBox.min.x + relative.x <= oldObstacle.max_x() + kEpsilon &&
                           currentBox.max_y() > currentObstacle.min.y + kEpsilon &&
                           currentBox.min.y < currentObstacle.max_y() - kEpsilon) {
                    body.state.positionPixels.x += currentObstacle.max_x() - currentBox.min.x;
                    body.state.linearVelocityPixelsPerSecond.x = other.state.linearVelocityPixelsPerSecond.x;
                    normal = {1.0F, 0.0F};
                    hit = true;
                }

                if (hit) {
                    contacted = true;
                    append_contact(bodyKey, body, Physics2DBodyHandle{otherKey}, other.collider,
                                   other.def.userTag, other.colliderDef.userTag,
                                   native_body_aabb(body, body.state.positionPixels).center(), normal,
                                   std::hypot(relative.x, relative.y) / dt);
                }
            }
            body.state.awake = contacted || std::fabs(body.state.linearVelocityPixelsPerSecond.x) > kEpsilon ||
                               std::fabs(body.state.linearVelocityPixelsPerSecond.y) > kEpsilon;
        }
    }

    Physics2DWorldSettings settings_;
    CollisionGrid grid_{};
    bool hasGrid_{false};
    float accumulator_{};
    std::uint64_t nextHandle_{1};
    std::unordered_map<std::uint64_t, NativeBody> bodies_;
    std::unordered_map<std::uint64_t, std::uint64_t> colliderOwners_;
    std::vector<Physics2DEvent> events_;
};

} // namespace

std::unique_ptr<Physics2DWorld> create_physics2d_world(const Physics2DWorldSettings& settings,
                                                       std::string* error) {
    if (!(settings.pixelsPerMeter > 0.0F) || !std::isfinite(settings.pixelsPerMeter) ||
        !(settings.fixedTimeStep > 0.0F) || !std::isfinite(settings.fixedTimeStep) ||
        !std::isfinite(settings.gravityPixelsPerSecondSquared.x) ||
        !std::isfinite(settings.gravityPixelsPerSecondSquared.y) ||
        !std::isfinite(settings.oneWaySlopPixels) || settings.oneWaySlopPixels < 0.0F ||
        settings.subStepCount == 0U || settings.subStepCount > 128U ||
        settings.maxFrameSteps == 0U || settings.maxFrameSteps > 10000U) {
        if (error != nullptr) *error = "invalid Physics2DWorldSettings";
        return nullptr;
    }
    if (settings.backend == Physics2DBackend::NativeTile) {
        return std::make_unique<NativeTilePhysicsWorld>(settings);
    }
#if defined(DVE_HAVE_BOX2D)
    return create_box2d_physics_world(settings, error);
#else
    if (error != nullptr) {
        *error = "Box2D backend was requested but DVE was built without DVE_ENABLE_BOX2D=ON";
    }
    return nullptr;
#endif
}

bool box2d_physics_available() noexcept {
#if defined(DVE_HAVE_BOX2D)
    return true;
#else
    return false;
#endif
}

} // namespace dve
