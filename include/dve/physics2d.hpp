#pragma once

#include "dve/tilemap2d.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dve {

// Sprite/2D physics is deliberately backend-neutral. The built-in NativeTile backend is the
// deterministic platformer-oriented reference. Box2D is an optional rigid-body backend selected
// per world/scene. Authoring coordinates remain pixels in DVE's y-down gameplay plane; backends
// perform their own unit conversion.
enum class Physics2DBackend : std::uint8_t { NativeTile, Box2D };
enum class Physics2DBodyType : std::uint8_t { Static, Kinematic, Dynamic };
enum class Physics2DShapeType : std::uint8_t { Box, Circle, Capsule, ConvexPolygon };
enum class Physics2DEventType : std::uint8_t { ContactBegin, ContactEnd, ContactHit, SensorBegin, SensorEnd, JointBreak };
enum class Physics2DJointType : std::uint8_t { Revolute, Prismatic, Distance, Weld, Wheel, Motor };

struct Physics2DHandle {
    std::uint64_t value{};
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return value != 0; }
    friend constexpr bool operator==(Physics2DHandle, Physics2DHandle) noexcept = default;
};
using Physics2DBodyHandle = Physics2DHandle;
using Physics2DColliderHandle = Physics2DHandle;
using Physics2DJointHandle = Physics2DHandle;

struct Physics2DWorldSettings {
    Physics2DBackend backend{Physics2DBackend::NativeTile};
    TileVec2 gravityPixelsPerSecondSquared{0.0F, 980.0F};
    float pixelsPerMeter{32.0F};
    float fixedTimeStep{1.0F / 60.0F};
    std::uint32_t subStepCount{4};
    std::uint32_t maxFrameSteps{8};
    bool enableSleep{true};
    bool enableContinuousCollision{true};
    float oneWaySlopPixels{2.0F};
};

struct Physics2DCapabilities {
    bool rigidBodyDynamics{false};
    bool bodyBodyCollision{false};
    bool circles{false};
    bool capsules{false};
    bool convexPolygons{false};
    bool sensors{false};
    bool rayCasts{false};
    bool oneWayPlatforms{true};
    bool continuousCollision{false};
    bool slopeTiles{false};
    bool kinematicPlatforms{false};
    bool incrementalTileRecook{false};
    bool aabbQueries{false};
    bool shapeOverlapQueries{false};
    bool shapeCasts{false};
    bool joints{false};
};

struct Physics2DBodyDef {
    Physics2DBodyType type{Physics2DBodyType::Dynamic};
    TileVec2 positionPixels{}; // body origin
    float angleRadians{};
    TileVec2 linearOffsetPixels{}; // Box2D 3.1 motor-joint position target
    float angularOffsetRadians{};
    TileVec2 linearVelocityPixelsPerSecond{}; // Box2D 3.2 velocity target
    float angularVelocityRadiansPerSecond{};
    float linearDamping{};
    float angularDamping{};
    float gravityScale{1.0F};
    bool fixedRotation{true};
    bool bullet{false};
    bool enabled{true};
    bool allowSleep{true};
    std::uint64_t userTag{};
    std::string debugName;
};

struct Physics2DColliderDef {
    Physics2DShapeType shape{Physics2DShapeType::Box};
    TileVec2 localCenterPixels{};
    TileVec2 halfExtentsPixels{8.0F, 8.0F};
    float radiusPixels{8.0F};
    // Capsule endpoints in local pixel coordinates. Radius is radiusPixels.
    TileVec2 capsulePoint1Pixels{0.0F, -4.0F};
    TileVec2 capsulePoint2Pixels{0.0F, 4.0F};
    // Convex, counter-clockwise in the selected gameplay plane. Box2D currently accepts at most
    // B2_MAX_POLYGON_VERTICES; DVE validates at runtime instead of hard-coding the upstream limit.
    std::vector<TileVec2> verticesPixels;
    float density{1.0F};
    float friction{0.4F};
    float restitution{};
    float rollingResistance{};
    float tangentSpeedPixelsPerSecond{}; // conveyor surface
    bool sensor{false};
    bool oneWayPlatform{false}; // body collider only; accepts contacts from its local top
    bool enableContactEvents{true};
    bool enableHitEvents{true};
    std::uint64_t categoryBits{1};
    std::uint64_t maskBits{~std::uint64_t{0}};
    std::int32_t groupIndex{};
    std::int32_t materialId{};
    std::uint64_t userTag{};
};

struct Physics2DBodyState {
    TileVec2 positionPixels{};
    float angleRadians{};
    TileVec2 linearOffsetPixels{}; // Box2D 3.1 motor-joint position target
    float angularOffsetRadians{};
    TileVec2 linearVelocityPixelsPerSecond{}; // Box2D 3.2 velocity target
    float angularVelocityRadiansPerSecond{};
    bool awake{true};
    bool enabled{true};
};

struct Physics2DEvent {
    Physics2DEventType type{Physics2DEventType::ContactBegin};
    Physics2DBodyHandle bodyA{};
    Physics2DBodyHandle bodyB{};
    Physics2DColliderHandle colliderA{};
    Physics2DColliderHandle colliderB{};
    std::uint64_t bodyUserTagA{};
    std::uint64_t bodyUserTagB{};
    std::uint64_t colliderUserTagA{};
    std::uint64_t colliderUserTagB{};
    TileVec2 pointPixels{};
    TileVec2 normal{};
    float approachSpeedPixelsPerSecond{};
    Physics2DJointHandle joint{};
    std::uint64_t jointUserTag{};
};

struct Physics2DTileRegion {
    std::uint32_t minCol{};
    std::uint32_t minRow{};
    std::uint32_t maxCol{};
    std::uint32_t maxRow{};
};

struct Physics2DOverlapHit {
    Physics2DBodyHandle body{};
    Physics2DColliderHandle collider{};
    std::uint64_t bodyUserTag{};
    std::uint64_t colliderUserTag{};
};

struct Physics2DQueryShape {
    Physics2DShapeType shape{Physics2DShapeType::Box};
    TileVec2 centerPixels{};
    float angleRadians{};
    TileVec2 halfExtentsPixels{8.0F, 8.0F};
    float radiusPixels{8.0F};
    TileVec2 capsulePoint1Pixels{0.0F, -4.0F};
    TileVec2 capsulePoint2Pixels{0.0F, 4.0F};
    std::vector<TileVec2> verticesPixels;
};

struct Physics2DQueryFilter {
    std::uint64_t categoryBits{1};
    std::uint64_t maskBits{~std::uint64_t{0}};
    Physics2DBodyHandle ignoredBody{};
    bool includeSensors{true};
    bool includeTileMap{true};
};

struct Physics2DShapeCastHit {
    bool hit{false};
    bool startedOverlapping{false};
    Physics2DBodyHandle body{};
    Physics2DColliderHandle collider{};
    std::uint64_t bodyUserTag{};
    std::uint64_t colliderUserTag{};
    TileVec2 pointPixels{};
    TileVec2 normal{};
    TileVec2 surfaceVelocityPixelsPerSecond{};
    float fraction{1.0F};
};

struct Physics2DJointDef {
    Physics2DJointType type{Physics2DJointType::Revolute};
    Physics2DBodyHandle bodyA{};
    Physics2DBodyHandle bodyB{};
    TileVec2 localAnchorAPixels{};
    TileVec2 localAnchorBPixels{};
    TileVec2 localAxisA{1.0F, 0.0F};
    float referenceAngleRadians{};
    bool collideConnected{false};

    bool enableLimit{false};
    float lowerTranslationPixels{};
    float upperTranslationPixels{};
    float lowerAngleRadians{};
    float upperAngleRadians{};

    bool enableMotor{false};
    float motorSpeed{};
    float maxMotorForce{};
    float maxMotorTorque{};

    bool enableSpring{false};
    float springHertz{};
    float springDampingRatio{0.7F};

    float lengthPixels{};
    float minLengthPixels{};
    float maxLengthPixels{};

    TileVec2 linearOffsetPixels{}; // Box2D 3.1 motor-joint position target
    float angularOffsetRadians{};
    TileVec2 linearVelocityPixelsPerSecond{}; // Box2D 3.2 velocity target
    float angularVelocityRadiansPerSecond{};
    float maxVelocityForce{};
    float maxVelocityTorque{};
    float correctionFactor{0.3F};

    float breakForce{};
    float breakTorque{};
    std::uint64_t userTag{};
};

struct Physics2DJointState {
    Physics2DJointType type{Physics2DJointType::Revolute};
    Physics2DBodyHandle bodyA{};
    Physics2DBodyHandle bodyB{};
    float translationPixels{};
    float speedPixelsPerSecond{};
    float angleRadians{};
    float motorSpeed{};
    float motorForce{};
    float motorTorque{};
    bool enabled{true};
    bool broken{false};
    std::uint64_t userTag{};
};

struct Physics2DRayCastHit {
    bool hit{false};
    Physics2DBodyHandle body{};
    Physics2DColliderHandle collider{};
    std::uint64_t bodyUserTag{};
    std::uint64_t colliderUserTag{};
    TileVec2 pointPixels{};
    TileVec2 normal{};
    // Authored surface motion at the hit point (for conveyors). Zero for ordinary surfaces.
    TileVec2 surfaceVelocityPixelsPerSecond{};
    float fraction{1.0F};
};

class Physics2DWorld {
public:
    virtual ~Physics2DWorld() = default;

    [[nodiscard]] virtual Physics2DBackend backend() const noexcept = 0;
    [[nodiscard]] virtual Physics2DCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual const Physics2DWorldSettings& settings() const noexcept = 0;

    // Replaces static tile collision. The native backend stores a CollisionGrid. The Box2D backend
    // cooks solid regions into merged static rectangles and one-way regions into thin platforms.
    virtual bool set_tile_map(const TileMap& map, std::string* error = nullptr) = 0;
    // Updates an inclusive tile rectangle. Backends that do not provide incremental recooking may
    // fall back to a complete set_tile_map rebuild; inspect capabilities().incrementalTileRecook.
    virtual bool update_tile_map_region(const TileMap& map, Physics2DTileRegion region,
                                        std::string* error = nullptr) {
        static_cast<void>(region);
        return set_tile_map(map, error);
    }
    virtual void clear_tile_map() = 0;

    [[nodiscard]] virtual Physics2DBodyHandle create_body(const Physics2DBodyDef& def,
                                                          std::string* error = nullptr) = 0;
    virtual bool destroy_body(Physics2DBodyHandle body) = 0;
    [[nodiscard]] virtual Physics2DColliderHandle add_collider(Physics2DBodyHandle body,
                                                               const Physics2DColliderDef& def,
                                                               std::string* error = nullptr) = 0;
    virtual bool destroy_collider(Physics2DColliderHandle collider) = 0;

    [[nodiscard]] virtual Physics2DJointHandle create_joint(const Physics2DJointDef& def,
                                                            std::string* error = nullptr) {
        static_cast<void>(def);
        if (error != nullptr) *error = "selected 2D physics backend does not support joints";
        return {};
    }
    virtual bool destroy_joint(Physics2DJointHandle joint) {
        static_cast<void>(joint);
        return false;
    }
    [[nodiscard]] virtual bool joint_state(Physics2DJointHandle joint,
                                           Physics2DJointState& out) const {
        static_cast<void>(joint);
        static_cast<void>(out);
        return false;
    }

    virtual void step(float frameDeltaSeconds) = 0;
    [[nodiscard]] virtual bool body_state(Physics2DBodyHandle body, Physics2DBodyState& out) const = 0;
    virtual bool set_body_transform(Physics2DBodyHandle body, TileVec2 positionPixels,
                                    float angleRadians) = 0;
    virtual bool set_body_linear_velocity(Physics2DBodyHandle body,
                                          TileVec2 velocityPixelsPerSecond) = 0;
    virtual bool set_body_gravity_scale(Physics2DBodyHandle body, float gravityScale) = 0;
    virtual bool apply_linear_impulse(Physics2DBodyHandle body,
                                      TileVec2 impulsePixelKilogramsPerSecond) = 0;
    virtual bool set_body_enabled(Physics2DBodyHandle body, bool enabled) = 0;
    // Temporarily ignores one-way platforms for this body; intended for down+jump/drop-through.
    virtual bool drop_through_one_way(Physics2DBodyHandle body, float seconds) = 0;

    [[nodiscard]] virtual Physics2DRayCastHit ray_cast(TileVec2 originPixels,
                                                       TileVec2 translationPixels,
                                                       std::uint64_t categoryBits = 1,
                                                       std::uint64_t maskBits = ~std::uint64_t{0}) const = 0;
    // Returns the total number of matching body colliders. Up to out.size() records are written.
    [[nodiscard]] virtual std::size_t query_aabb(TileAabb bounds,
                                                std::span<Physics2DOverlapHit> out,
                                                std::uint64_t categoryBits = 1,
                                                std::uint64_t maskBits = ~std::uint64_t{0}) const {
        static_cast<void>(bounds);
        static_cast<void>(out);
        static_cast<void>(categoryBits);
        static_cast<void>(maskBits);
        return 0;
    }
    // Exact overlap against authored query geometry. Results are stable-sorted by body/collider
    // handle. The total match count is returned even when out is smaller.
    [[nodiscard]] virtual std::size_t query_shape(const Physics2DQueryShape& shape,
                                                  std::span<Physics2DOverlapHit> out,
                                                  const Physics2DQueryFilter& filter = {}) const {
        static_cast<void>(shape);
        static_cast<void>(out);
        static_cast<void>(filter);
        return 0;
    }
    // Returns the closest translation hit. Initial overlap is reported separately and does not
    // invent a penetration normal.
    [[nodiscard]] virtual Physics2DShapeCastHit cast_shape(
        const Physics2DQueryShape& shape, TileVec2 translationPixels,
        const Physics2DQueryFilter& filter = {}) const {
        static_cast<void>(shape);
        static_cast<void>(translationPixels);
        static_cast<void>(filter);
        return {};
    }
    [[nodiscard]] virtual bool overlaps_tile_map(TileAabb bounds) const {
        static_cast<void>(bounds);
        return false;
    }
    [[nodiscard]] virtual std::span<const Physics2DEvent> events() const noexcept = 0;
};

// Returns nullptr and a useful error when an optional backend was not compiled in.
[[nodiscard]] std::unique_ptr<Physics2DWorld> create_physics2d_world(
    const Physics2DWorldSettings& settings, std::string* error = nullptr);
[[nodiscard]] bool box2d_physics_available() noexcept;

} // namespace dve
