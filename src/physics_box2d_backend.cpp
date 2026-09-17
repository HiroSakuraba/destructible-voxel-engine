#include "dve/physics_box2d_backend.hpp"

#include <box2d/box2d.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dve {
namespace {

struct BodyRecord {
    Physics2DBodyHandle handle{};
    b2BodyId id{};
    std::uint64_t userTag{};
    float dropThroughRemaining{};
    b2Vec2 preStepLinearVelocity{};
    b2AABB preStepAabb{};
    bool hasPreStepState{false};
};

struct ShapeRecord {
    Physics2DColliderHandle handle{};
    Physics2DBodyHandle body{};
    b2ShapeId id{};
    std::uint64_t userTag{};
    float tangentSpeedPixelsPerSecond{};
    bool sensor{false};
    bool oneWay{false};
    float oneWayTopMeters{};
};

struct JointRecord {
    Physics2DJointHandle handle{};
    b2JointId id{};
    Physics2DJointDef def{};
    bool broken{false};
};

struct TileChunkRecord {
    b2BodyId body{};
    std::vector<std::unique_ptr<ShapeRecord>> shapes;
};

class Box2DPhysicsWorld final : public Physics2DWorld {
public:
    explicit Box2DPhysicsWorld(Physics2DWorldSettings settings) : settings_(std::move(settings)) {
        b2WorldDef worldDef = b2DefaultWorldDef();
        worldDef.gravity = to_meters(settings_.gravityPixelsPerSecondSquared);
        worldDef.enableSleep = settings_.enableSleep;
        worldDef.enableContinuous = settings_.enableContinuousCollision;
        worldDef.workerCount = 1; // serial default; Box2D requires task callbacks for larger worker counts.
        worldDef.userData = this;
        world_ = b2CreateWorld(&worldDef);
        if (B2_IS_NON_NULL(world_)) {
            b2World_SetPreSolveCallback(world_, &Box2DPhysicsWorld::pre_solve, this);
        }
    }

    ~Box2DPhysicsWorld() override {
        if (B2_IS_NON_NULL(world_)) b2DestroyWorld(world_);
    }

    [[nodiscard]] bool valid() const noexcept { return B2_IS_NON_NULL(world_); }
    [[nodiscard]] Physics2DBackend backend() const noexcept override { return Physics2DBackend::Box2D; }
    [[nodiscard]] Physics2DCapabilities capabilities() const noexcept override {
        Physics2DCapabilities c;
        c.rigidBodyDynamics = true;
        c.bodyBodyCollision = true;
        c.circles = true;
        c.capsules = true;
        c.convexPolygons = true;
        c.sensors = true;
        c.rayCasts = true;
        c.oneWayPlatforms = true;
        c.continuousCollision = true;
        c.slopeTiles = true;
        c.kinematicPlatforms = true;
        c.incrementalTileRecook = true;
        c.aabbQueries = true;
        c.shapeOverlapQueries = true;
        c.shapeCasts = true;
        c.joints = true;
        return c;
    }
    [[nodiscard]] const Physics2DWorldSettings& settings() const noexcept override { return settings_; }

    bool set_tile_map(const TileMap& map, std::string* error) override {
        std::string validationError;
        if (!map.validate(&validationError)) {
            if (error != nullptr) *error = "invalid tile map: " + validationError;
            return false;
        }
        clear_tile_map();
        tileGrid_ = build_collision_grid(map);
        hasTileGrid_ = true;
        const std::uint32_t chunkColumns = (tileGrid_.width + kTileChunkSize - 1U) / kTileChunkSize;
        const std::uint32_t chunkRows = (tileGrid_.height + kTileChunkSize - 1U) / kTileChunkSize;
        for (std::uint32_t chunkRow = 0; chunkRow < chunkRows; ++chunkRow) {
            for (std::uint32_t chunkCol = 0; chunkCol < chunkColumns; ++chunkCol) {
                if (!cook_tile_chunk(chunkCol, chunkRow)) {
                    clear_tile_map();
                    if (error != nullptr) *error = "Box2D exhausted body/shape capacity while cooking tile chunks";
                    return false;
                }
            }
        }
        return true;
    }

    bool update_tile_map_region(const TileMap& map, Physics2DTileRegion region,
                                std::string* error) override {
        std::string validationError;
        if (!map.validate(&validationError)) {
            if (error != nullptr) *error = "invalid tile map: " + validationError;
            return false;
        }
        if (!hasTileGrid_ || region.minCol > region.maxCol || region.minRow > region.maxRow ||
            region.maxCol >= tileGrid_.width || region.maxRow >= tileGrid_.height ||
            !update_collision_grid_region(tileGrid_, map, region.minCol, region.minRow,
                                          region.maxCol, region.maxRow)) {
            if (error != nullptr) *error = "tile update region is outside the active Box2D tile grid";
            return false;
        }
        const std::uint32_t minChunkCol = region.minCol / kTileChunkSize;
        const std::uint32_t maxChunkCol = region.maxCol / kTileChunkSize;
        const std::uint32_t minChunkRow = region.minRow / kTileChunkSize;
        const std::uint32_t maxChunkRow = region.maxRow / kTileChunkSize;
        for (std::uint32_t chunkRow = minChunkRow; chunkRow <= maxChunkRow; ++chunkRow) {
            for (std::uint32_t chunkCol = minChunkCol; chunkCol <= maxChunkCol; ++chunkCol) {
                destroy_tile_chunk(chunkCol, chunkRow);
                if (!cook_tile_chunk(chunkCol, chunkRow)) {
                    if (error != nullptr) *error = "Box2D could not recook an edited tile chunk";
                    return false;
                }
            }
        }
        return true;
    }

    void clear_tile_map() override {
        for (auto& [key, chunk] : tileChunks_) {
            static_cast<void>(key);
            for (const auto& record : chunk.shapes) {
                shapeByBoxId_.erase(b2StoreShapeId(record->id));
            }
            if (b2Body_IsValid(chunk.body)) b2DestroyBody(chunk.body);
        }
        tileChunks_.clear();
        tileGrid_ = {};
        hasTileGrid_ = false;
    }

    [[nodiscard]] Physics2DBodyHandle create_body(const Physics2DBodyDef& def,
                                                   std::string* error) override {
        if (!finite(def.positionPixels) || !finite(def.linearVelocityPixelsPerSecond) ||
            !std::isfinite(def.angleRadians) || !std::isfinite(def.angularVelocityRadiansPerSecond) ||
            !std::isfinite(def.linearDamping) || !std::isfinite(def.angularDamping) ||
            !std::isfinite(def.gravityScale)) {
            if (error != nullptr) *error = "body definition contains a non-finite value";
            return {};
        }
        auto record = std::make_unique<BodyRecord>();
        record->handle = Physics2DBodyHandle{nextHandle_++};
        record->userTag = def.userTag;

        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = to_box_type(def.type);
        bodyDef.position = to_meters(def.positionPixels);
        bodyDef.rotation = b2MakeRot(def.angleRadians);
        bodyDef.linearVelocity = to_meters(def.linearVelocityPixelsPerSecond);
        bodyDef.angularVelocity = def.angularVelocityRadiansPerSecond;
        bodyDef.linearDamping = std::max(0.0F, def.linearDamping);
        bodyDef.angularDamping = std::max(0.0F, def.angularDamping);
        bodyDef.gravityScale = def.gravityScale;
#if defined(DVE_BOX2D_API_32)
        bodyDef.motionLocks.angularZ = def.fixedRotation;
        // Box2D 3.2 contact recycling may retain a resting one-way contact without
        // re-running the pre-solve callback after drop-through begins.
        bodyDef.enableContactRecycling = false;
#else
        bodyDef.fixedRotation = def.fixedRotation;
#endif
        bodyDef.isBullet = def.bullet;
        bodyDef.isEnabled = def.enabled;
        bodyDef.enableSleep = def.allowSleep;
        bodyDef.name = def.debugName.empty() ? nullptr : def.debugName.c_str();
        bodyDef.userData = record.get();
        record->id = b2CreateBody(world_, &bodyDef);
        if (!b2Body_IsValid(record->id)) {
            if (error != nullptr) *error = "Box2D could not allocate the body";
            return {};
        }

        const auto handle = record->handle;
        bodies_.emplace(handle.value, std::move(record));
        return handle;
    }

    bool destroy_body(Physics2DBodyHandle body) override {
        const auto it = bodies_.find(body.value);
        if (it == bodies_.end()) return false;
        std::vector<std::uint64_t> ownedJoints;
        for (const auto& [handle, joint] : joints_) {
            if (joint->def.bodyA == body || joint->def.bodyB == body) ownedJoints.push_back(handle);
        }
        for (const std::uint64_t handle : ownedJoints) destroy_joint(Physics2DJointHandle{handle});
        std::vector<std::uint64_t> owned;
        for (const auto& [handle, shape] : shapes_) {
            if (shape->body == body) owned.push_back(handle);
        }
        for (const std::uint64_t handle : owned) destroy_collider(Physics2DColliderHandle{handle});
        b2DestroyBody(it->second->id);
        bodies_.erase(it);
        return true;
    }

    [[nodiscard]] Physics2DColliderHandle add_collider(Physics2DBodyHandle body,
                                                        const Physics2DColliderDef& def,
                                                        std::string* error) override {
        const auto bodyIt = bodies_.find(body.value);
        if (bodyIt == bodies_.end()) {
            if (error != nullptr) *error = "body handle is invalid";
            return {};
        }
        if (!validate_collider(def, error)) return {};

        auto record = std::make_unique<ShapeRecord>();
        record->handle = Physics2DColliderHandle{nextHandle_++};
        record->body = body;
        record->userTag = def.userTag;
        record->tangentSpeedPixelsPerSecond = def.tangentSpeedPixelsPerSecond;
        record->sensor = def.sensor;
        record->oneWay = def.oneWayPlatform;

        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.userData = record.get();
        shapeDef.material.friction = std::max(0.0F, def.friction);
        shapeDef.material.restitution = std::clamp(def.restitution, 0.0F, 1.0F);
        shapeDef.material.rollingResistance = std::max(0.0F, def.rollingResistance);
        shapeDef.material.tangentSpeed = def.tangentSpeedPixelsPerSecond / settings_.pixelsPerMeter;
        shapeDef.material.userMaterialId = def.materialId;
        shapeDef.density = std::max(0.0F, def.density);
        shapeDef.filter.categoryBits = def.categoryBits;
        shapeDef.filter.maskBits = def.maskBits;
        shapeDef.filter.groupIndex = def.groupIndex;
        shapeDef.isSensor = def.sensor;
        shapeDef.enableSensorEvents = true;
        shapeDef.enableContactEvents = def.enableContactEvents;
        shapeDef.enableHitEvents = def.enableHitEvents;
        // Required so the world's one-way callback can reject contacts against one-way tile shapes.
        shapeDef.enablePreSolveEvents = !def.sensor;

        const b2BodyId boxBody = bodyIt->second->id;
        switch (def.shape) {
        case Physics2DShapeType::Box: {
            const b2Polygon polygon = b2MakeOffsetBox(def.halfExtentsPixels.x / settings_.pixelsPerMeter,
                                                      def.halfExtentsPixels.y / settings_.pixelsPerMeter,
                                                      to_meters(def.localCenterPixels), b2Rot_identity);
            record->id = b2CreatePolygonShape(boxBody, &shapeDef, &polygon);
            break;
        }
        case Physics2DShapeType::Circle: {
            const b2Circle circle{to_meters(def.localCenterPixels), def.radiusPixels / settings_.pixelsPerMeter};
            record->id = b2CreateCircleShape(boxBody, &shapeDef, &circle);
            break;
        }
        case Physics2DShapeType::Capsule: {
            const b2Capsule capsule{
                to_meters({def.capsulePoint1Pixels.x + def.localCenterPixels.x,
                           def.capsulePoint1Pixels.y + def.localCenterPixels.y}),
                to_meters({def.capsulePoint2Pixels.x + def.localCenterPixels.x,
                           def.capsulePoint2Pixels.y + def.localCenterPixels.y}),
                def.radiusPixels / settings_.pixelsPerMeter};
            record->id = b2CreateCapsuleShape(boxBody, &shapeDef, &capsule);
            break;
        }
        case Physics2DShapeType::ConvexPolygon: {
            std::vector<b2Vec2> points;
            points.reserve(def.verticesPixels.size());
            for (const TileVec2 point : def.verticesPixels) {
                points.push_back(to_meters({point.x + def.localCenterPixels.x,
                                            point.y + def.localCenterPixels.y}));
            }
            const b2Hull hull = b2ComputeHull(points.data(), static_cast<int>(points.size()));
            if (hull.count < 3) {
                if (error != nullptr) *error = "Box2D could not construct a convex hull from collider vertices";
                return {};
            }
            const b2Polygon polygon = b2MakePolygon(&hull, 0.0F);
            record->id = b2CreatePolygonShape(boxBody, &shapeDef, &polygon);
            break;
        }
        }

        if (!b2Shape_IsValid(record->id)) {
            if (error != nullptr) *error = "Box2D could not allocate the collider shape";
            return {};
        }
        const auto handle = record->handle;
        shapeByBoxId_.emplace(b2StoreShapeId(record->id), record.get());
        shapes_.emplace(handle.value, std::move(record));
        return handle;
    }

    bool destroy_collider(Physics2DColliderHandle collider) override {
        const auto it = shapes_.find(collider.value);
        if (it == shapes_.end()) return false;
        shapeByBoxId_.erase(b2StoreShapeId(it->second->id));
        b2DestroyShape(it->second->id, true);
        shapes_.erase(it);
        return true;
    }

    [[nodiscard]] Physics2DJointHandle create_joint(const Physics2DJointDef& def,
                                                     std::string* error) override {
        const BodyRecord* bodyA = find_body(def.bodyA);
        const BodyRecord* bodyB = find_body(def.bodyB);
        const auto fail = [error](const char* text) {
            if (error != nullptr) *error = text;
            return Physics2DJointHandle{};
        };
        if (bodyA == nullptr || bodyB == nullptr || def.bodyA == def.bodyB) {
            return fail("joint requires two different valid bodies");
        }
        const auto finiteVec = [](TileVec2 value) {
            return std::isfinite(value.x) && std::isfinite(value.y);
        };
        const std::array<float, 22> values{
            def.referenceAngleRadians, def.lowerTranslationPixels, def.upperTranslationPixels,
            def.lowerAngleRadians, def.upperAngleRadians, def.motorSpeed, def.maxMotorForce,
            def.maxMotorTorque, def.springHertz, def.springDampingRatio, def.lengthPixels,
            def.minLengthPixels, def.maxLengthPixels, def.angularOffsetRadians,
            def.angularVelocityRadiansPerSecond, def.maxVelocityForce, def.maxVelocityTorque,
            def.correctionFactor, def.breakForce, def.breakTorque,
            def.localAxisA.x, def.localAxisA.y};
        if (!finiteVec(def.localAnchorAPixels) || !finiteVec(def.localAnchorBPixels) ||
            !finiteVec(def.localAxisA) || !finiteVec(def.linearOffsetPixels) ||
            !finiteVec(def.linearVelocityPixelsPerSecond) ||
            std::any_of(values.begin(), values.end(), [](float value) { return !std::isfinite(value); })) {
            return fail("joint definition contains a non-finite value");
        }
        const float axisLength = std::hypot(def.localAxisA.x, def.localAxisA.y);
        if ((def.type == Physics2DJointType::Prismatic || def.type == Physics2DJointType::Wheel) &&
            axisLength <= 0.0001F) return fail("prismatic and wheel joints require a non-zero local axis");
        if (def.springHertz < 0.0F || def.springDampingRatio < 0.0F ||
            def.maxMotorForce < 0.0F || def.maxMotorTorque < 0.0F ||
            def.maxVelocityForce < 0.0F || def.maxVelocityTorque < 0.0F ||
            def.breakForce < 0.0F || def.breakTorque < 0.0F) {
            return fail("joint forces, torque, and spring values must be non-negative");
        }

        auto record = std::make_unique<JointRecord>();
        record->handle = Physics2DJointHandle{nextHandle_++};
        record->def = def;
        const float invAxis = axisLength > 0.0001F ? 1.0F / axisLength : 1.0F;
        const TileVec2 axis{def.localAxisA.x * invAxis, def.localAxisA.y * invAxis};
#if defined(DVE_BOX2D_API_32)
        const float axisAngle = std::atan2(axis.y, axis.x);
        const auto configureBase = [&](b2JointDef& base, bool oriented) {
            base.userData = record.get();
            base.bodyIdA = bodyA->id;
            base.bodyIdB = bodyB->id;
            base.localFrameA.p = to_meters(def.localAnchorAPixels);
            base.localFrameB.p = to_meters(def.localAnchorBPixels);
            base.localFrameA.q = oriented ? b2MakeRot(axisAngle) : b2Rot_identity;
            base.localFrameB.q = oriented ? b2MakeRot(axisAngle + def.referenceAngleRadians)
                                          : b2MakeRot(def.referenceAngleRadians);
            base.forceThreshold = def.breakForce;
            base.torqueThreshold = def.breakTorque;
            base.collideConnected = def.collideConnected;
        };
        switch (def.type) {
        case Physics2DJointType::Distance: {
            b2DistanceJointDef joint = b2DefaultDistanceJointDef();
            configureBase(joint.base, false);
            joint.length = std::max(0.001F, def.lengthPixels / settings_.pixelsPerMeter);
            joint.enableSpring = def.enableSpring;
            joint.hertz = std::max(0.0F, def.springHertz);
            joint.dampingRatio = std::max(0.0F, def.springDampingRatio);
            joint.enableLimit = def.enableLimit;
            joint.minLength = std::max(0.001F, def.minLengthPixels / settings_.pixelsPerMeter);
            joint.maxLength = std::max(joint.minLength, def.maxLengthPixels / settings_.pixelsPerMeter);
            joint.enableMotor = def.enableMotor;
            joint.motorSpeed = def.motorSpeed / settings_.pixelsPerMeter;
            joint.maxMotorForce = std::max(0.0F, def.maxMotorForce);
            record->id = b2CreateDistanceJoint(world_, &joint);
            break;
        }
        case Physics2DJointType::Motor: {
            b2MotorJointDef joint = b2DefaultMotorJointDef();
            configureBase(joint.base, false);
            joint.linearVelocity = to_meters(def.linearVelocityPixelsPerSecond);
            joint.angularVelocity = def.angularVelocityRadiansPerSecond;
            joint.maxVelocityForce = std::max(0.0F, def.maxVelocityForce);
            joint.maxVelocityTorque = std::max(0.0F, def.maxVelocityTorque);
            joint.linearHertz = def.enableSpring ? std::max(0.0F, def.springHertz) : 0.0F;
            joint.angularHertz = joint.linearHertz;
            joint.linearDampingRatio = std::max(0.0F, def.springDampingRatio);
            joint.angularDampingRatio = joint.linearDampingRatio;
            joint.maxSpringForce = std::max(0.0F, def.maxMotorForce);
            joint.maxSpringTorque = std::max(0.0F, def.maxMotorTorque);
            record->id = b2CreateMotorJoint(world_, &joint);
            break;
        }
        case Physics2DJointType::Prismatic: {
            b2PrismaticJointDef joint = b2DefaultPrismaticJointDef();
            configureBase(joint.base, true);
            joint.enableSpring = def.enableSpring;
            joint.hertz = std::max(0.0F, def.springHertz);
            joint.dampingRatio = std::max(0.0F, def.springDampingRatio);
            joint.enableLimit = def.enableLimit;
            joint.lowerTranslation = def.lowerTranslationPixels / settings_.pixelsPerMeter;
            joint.upperTranslation = def.upperTranslationPixels / settings_.pixelsPerMeter;
            joint.enableMotor = def.enableMotor;
            joint.motorSpeed = def.motorSpeed / settings_.pixelsPerMeter;
            joint.maxMotorForce = std::max(0.0F, def.maxMotorForce);
            record->id = b2CreatePrismaticJoint(world_, &joint);
            break;
        }
        case Physics2DJointType::Revolute: {
            b2RevoluteJointDef joint = b2DefaultRevoluteJointDef();
            configureBase(joint.base, false);
            joint.enableSpring = def.enableSpring;
            joint.hertz = std::max(0.0F, def.springHertz);
            joint.dampingRatio = std::max(0.0F, def.springDampingRatio);
            joint.enableLimit = def.enableLimit;
            joint.lowerAngle = def.lowerAngleRadians;
            joint.upperAngle = def.upperAngleRadians;
            joint.enableMotor = def.enableMotor;
            joint.motorSpeed = def.motorSpeed;
            joint.maxMotorTorque = std::max(0.0F, def.maxMotorTorque);
            record->id = b2CreateRevoluteJoint(world_, &joint);
            break;
        }
        case Physics2DJointType::Weld: {
            b2WeldJointDef joint = b2DefaultWeldJointDef();
            configureBase(joint.base, false);
            joint.linearHertz = def.enableSpring ? std::max(0.0F, def.springHertz) : 0.0F;
            joint.angularHertz = joint.linearHertz;
            joint.linearDampingRatio = std::max(0.0F, def.springDampingRatio);
            joint.angularDampingRatio = joint.linearDampingRatio;
            record->id = b2CreateWeldJoint(world_, &joint);
            break;
        }
        case Physics2DJointType::Wheel: {
            b2WheelJointDef joint = b2DefaultWheelJointDef();
            configureBase(joint.base, true);
            joint.enableSpring = def.enableSpring;
            joint.hertz = std::max(0.0F, def.springHertz);
            joint.dampingRatio = std::max(0.0F, def.springDampingRatio);
            joint.enableLimit = def.enableLimit;
            joint.lowerTranslation = def.lowerTranslationPixels / settings_.pixelsPerMeter;
            joint.upperTranslation = def.upperTranslationPixels / settings_.pixelsPerMeter;
            joint.enableMotor = def.enableMotor;
            joint.motorSpeed = def.motorSpeed;
            joint.maxMotorTorque = std::max(0.0F, def.maxMotorTorque);
            record->id = b2CreateWheelJoint(world_, &joint);
            break;
        }
        }
#else
        switch (def.type) {
        case Physics2DJointType::Distance: {
            b2DistanceJointDef joint = b2DefaultDistanceJointDef();
            joint.bodyIdA = bodyA->id; joint.bodyIdB = bodyB->id;
            joint.localAnchorA = to_meters(def.localAnchorAPixels);
            joint.localAnchorB = to_meters(def.localAnchorBPixels);
            joint.length = std::max(0.001F, def.lengthPixels / settings_.pixelsPerMeter);
            joint.enableSpring = def.enableSpring; joint.hertz = std::max(0.0F, def.springHertz);
            joint.dampingRatio = std::max(0.0F, def.springDampingRatio);
            joint.enableLimit = def.enableLimit;
            joint.minLength = std::max(0.001F, def.minLengthPixels / settings_.pixelsPerMeter);
            joint.maxLength = std::max(joint.minLength, def.maxLengthPixels / settings_.pixelsPerMeter);
            joint.enableMotor = def.enableMotor; joint.motorSpeed = def.motorSpeed / settings_.pixelsPerMeter;
            joint.maxMotorForce = std::max(0.0F, def.maxMotorForce);
            joint.collideConnected = def.collideConnected; joint.userData = record.get();
            record->id = b2CreateDistanceJoint(world_, &joint); break;
        }
        case Physics2DJointType::Motor: {
            b2MotorJointDef joint = b2DefaultMotorJointDef();
            joint.bodyIdA = bodyA->id; joint.bodyIdB = bodyB->id;
            joint.linearOffset = to_meters(def.linearOffsetPixels);
            joint.angularOffset = def.angularOffsetRadians;
            joint.maxForce = std::max(0.0F, def.maxVelocityForce);
            joint.maxTorque = std::max(0.0F, def.maxVelocityTorque);
            joint.correctionFactor = std::clamp(def.correctionFactor, 0.0F, 1.0F);
            joint.collideConnected = def.collideConnected; joint.userData = record.get();
            record->id = b2CreateMotorJoint(world_, &joint); break;
        }
        case Physics2DJointType::Prismatic: {
            b2PrismaticJointDef joint = b2DefaultPrismaticJointDef();
            joint.bodyIdA = bodyA->id; joint.bodyIdB = bodyB->id;
            joint.localAnchorA = to_meters(def.localAnchorAPixels); joint.localAnchorB = to_meters(def.localAnchorBPixels);
            joint.localAxisA = {axis.x, axis.y}; joint.referenceAngle = def.referenceAngleRadians;
            joint.enableSpring = def.enableSpring; joint.hertz = std::max(0.0F, def.springHertz);
            joint.dampingRatio = std::max(0.0F, def.springDampingRatio);
            joint.enableLimit = def.enableLimit; joint.lowerTranslation = def.lowerTranslationPixels / settings_.pixelsPerMeter;
            joint.upperTranslation = def.upperTranslationPixels / settings_.pixelsPerMeter;
            joint.enableMotor = def.enableMotor; joint.motorSpeed = def.motorSpeed / settings_.pixelsPerMeter;
            joint.maxMotorForce = std::max(0.0F, def.maxMotorForce);
            joint.collideConnected = def.collideConnected; joint.userData = record.get();
            record->id = b2CreatePrismaticJoint(world_, &joint); break;
        }
        case Physics2DJointType::Revolute: {
            b2RevoluteJointDef joint = b2DefaultRevoluteJointDef();
            joint.bodyIdA = bodyA->id; joint.bodyIdB = bodyB->id;
            joint.localAnchorA = to_meters(def.localAnchorAPixels); joint.localAnchorB = to_meters(def.localAnchorBPixels);
            joint.referenceAngle = def.referenceAngleRadians; joint.enableSpring = def.enableSpring;
            joint.hertz = std::max(0.0F, def.springHertz); joint.dampingRatio = std::max(0.0F, def.springDampingRatio);
            joint.enableLimit = def.enableLimit; joint.lowerAngle = def.lowerAngleRadians; joint.upperAngle = def.upperAngleRadians;
            joint.enableMotor = def.enableMotor; joint.motorSpeed = def.motorSpeed;
            joint.maxMotorTorque = std::max(0.0F, def.maxMotorTorque);
            joint.collideConnected = def.collideConnected; joint.userData = record.get();
            record->id = b2CreateRevoluteJoint(world_, &joint); break;
        }
        case Physics2DJointType::Weld: {
            b2WeldJointDef joint = b2DefaultWeldJointDef();
            joint.bodyIdA = bodyA->id; joint.bodyIdB = bodyB->id;
            joint.localAnchorA = to_meters(def.localAnchorAPixels); joint.localAnchorB = to_meters(def.localAnchorBPixels);
            joint.referenceAngle = def.referenceAngleRadians;
            joint.linearHertz = def.enableSpring ? std::max(0.0F, def.springHertz) : 0.0F;
            joint.angularHertz = joint.linearHertz; joint.linearDampingRatio = std::max(0.0F, def.springDampingRatio);
            joint.angularDampingRatio = joint.linearDampingRatio;
            joint.collideConnected = def.collideConnected; joint.userData = record.get();
            record->id = b2CreateWeldJoint(world_, &joint); break;
        }
        case Physics2DJointType::Wheel: {
            b2WheelJointDef joint = b2DefaultWheelJointDef();
            joint.bodyIdA = bodyA->id; joint.bodyIdB = bodyB->id;
            joint.localAnchorA = to_meters(def.localAnchorAPixels); joint.localAnchorB = to_meters(def.localAnchorBPixels);
            joint.localAxisA = {axis.x, axis.y}; joint.enableSpring = def.enableSpring;
            joint.hertz = std::max(0.0F, def.springHertz); joint.dampingRatio = std::max(0.0F, def.springDampingRatio);
            joint.enableLimit = def.enableLimit; joint.lowerTranslation = def.lowerTranslationPixels / settings_.pixelsPerMeter;
            joint.upperTranslation = def.upperTranslationPixels / settings_.pixelsPerMeter;
            joint.enableMotor = def.enableMotor; joint.motorSpeed = def.motorSpeed;
            joint.maxMotorTorque = std::max(0.0F, def.maxMotorTorque);
            joint.collideConnected = def.collideConnected; joint.userData = record.get();
            record->id = b2CreateWheelJoint(world_, &joint); break;
        }
        }
#endif
        if (!b2Joint_IsValid(record->id)) return fail("Box2D could not allocate the joint");
        const Physics2DJointHandle handle = record->handle;
        joints_.emplace(handle.value, std::move(record));
        return handle;
    }

    bool destroy_joint(Physics2DJointHandle joint) override {
        const auto it = joints_.find(joint.value);
        if (it == joints_.end()) return false;
        if (b2Joint_IsValid(it->second->id)) b2DestroyJoint(it->second->id, true);
        joints_.erase(it);
        return true;
    }

    [[nodiscard]] bool joint_state(Physics2DJointHandle joint,
                                   Physics2DJointState& out) const override {
        const auto it = joints_.find(joint.value);
        if (it == joints_.end()) return false;
        const JointRecord& record = *it->second;
        out = {};
        out.type = record.def.type;
        out.bodyA = record.def.bodyA;
        out.bodyB = record.def.bodyB;
        out.userTag = record.def.userTag;
        out.broken = record.broken;
        out.enabled = b2Joint_IsValid(record.id) && !record.broken;
        if (!out.enabled) return true;
        const b2Vec2 constraintForce = b2Joint_GetConstraintForce(record.id);
        out.motorForce = std::hypot(constraintForce.x, constraintForce.y);
        out.motorTorque = b2Joint_GetConstraintTorque(record.id);
        switch (record.def.type) {
        case Physics2DJointType::Distance:
            out.translationPixels = b2DistanceJoint_GetCurrentLength(record.id) * settings_.pixelsPerMeter;
            out.motorSpeed = b2DistanceJoint_GetMotorSpeed(record.id) * settings_.pixelsPerMeter;
            out.motorForce = b2DistanceJoint_GetMotorForce(record.id);
            break;
        case Physics2DJointType::Prismatic:
            out.translationPixels = b2PrismaticJoint_GetTranslation(record.id) * settings_.pixelsPerMeter;
            out.speedPixelsPerSecond = b2PrismaticJoint_GetSpeed(record.id) * settings_.pixelsPerMeter;
            out.motorSpeed = b2PrismaticJoint_GetMotorSpeed(record.id) * settings_.pixelsPerMeter;
            out.motorForce = b2PrismaticJoint_GetMotorForce(record.id);
            break;
        case Physics2DJointType::Revolute:
            out.angleRadians = b2RevoluteJoint_GetAngle(record.id);
            out.motorSpeed = b2RevoluteJoint_GetMotorSpeed(record.id);
            out.motorTorque = b2RevoluteJoint_GetMotorTorque(record.id);
            break;
        case Physics2DJointType::Wheel:
            out.motorSpeed = b2WheelJoint_GetMotorSpeed(record.id);
            out.motorTorque = b2WheelJoint_GetMotorTorque(record.id);
            break;
        case Physics2DJointType::Motor:
        case Physics2DJointType::Weld:
            break;
        }
        return true;
    }

    void step(float frameDeltaSeconds) override {
        events_.clear();
        if (!(frameDeltaSeconds > 0.0F) || !std::isfinite(frameDeltaSeconds)) return;
        const float maxDelta = settings_.fixedTimeStep * static_cast<float>(settings_.maxFrameSteps);
        accumulator_ += std::min(frameDeltaSeconds, maxDelta);
        std::uint32_t stepCount = 0;
        while (accumulator_ + std::numeric_limits<float>::epsilon() >= settings_.fixedTimeStep &&
               stepCount < settings_.maxFrameSteps) {
            for (auto& [shapeHandle, shape] : shapes_) {
                static_cast<void>(shapeHandle);
                if (shape->oneWay && b2Shape_IsValid(shape->id)) {
                    shape->oneWayTopMeters = b2Shape_GetAABB(shape->id).lowerBound.y;
                }
            }
            for (auto& [handle, body] : bodies_) {
                static_cast<void>(handle);
                body->dropThroughRemaining = std::max(0.0F, body->dropThroughRemaining - settings_.fixedTimeStep);
                if (b2Body_IsValid(body->id)) {
                    body->preStepLinearVelocity = b2Body_GetLinearVelocity(body->id);
                    body->preStepAabb = b2Body_ComputeAABB(body->id);
                    body->hasPreStepState = true;
                } else {
                    body->hasPreStepState = false;
                }
            }
            b2World_Step(world_, settings_.fixedTimeStep, static_cast<int>(settings_.subStepCount));
            collect_events();
            collect_joint_breaks();
            accumulator_ -= settings_.fixedTimeStep;
            ++stepCount;
        }
    }

    [[nodiscard]] bool body_state(Physics2DBodyHandle body, Physics2DBodyState& out) const override {
        const auto it = bodies_.find(body.value);
        if (it == bodies_.end() || !b2Body_IsValid(it->second->id)) return false;
        const b2Vec2 position = b2Body_GetPosition(it->second->id);
        const b2Rot rotation = b2Body_GetRotation(it->second->id);
        const b2Vec2 velocity = b2Body_GetLinearVelocity(it->second->id);
        out.positionPixels = to_pixels(position);
        out.angleRadians = std::atan2(rotation.s, rotation.c);
        out.linearVelocityPixelsPerSecond = to_pixels(velocity);
        out.angularVelocityRadiansPerSecond = b2Body_GetAngularVelocity(it->second->id);
        out.awake = b2Body_IsAwake(it->second->id);
        out.enabled = b2Body_IsEnabled(it->second->id);
        return true;
    }

    bool set_body_transform(Physics2DBodyHandle body, TileVec2 positionPixels,
                            float angleRadians) override {
        const auto it = bodies_.find(body.value);
        if (it == bodies_.end() || !finite(positionPixels) || !std::isfinite(angleRadians)) return false;
        b2Body_SetTransform(it->second->id, to_meters(positionPixels), b2MakeRot(angleRadians));
        return true;
    }

    bool set_body_linear_velocity(Physics2DBodyHandle body,
                                  TileVec2 velocityPixelsPerSecond) override {
        const auto it = bodies_.find(body.value);
        if (it == bodies_.end() || !finite(velocityPixelsPerSecond)) return false;
        b2Body_SetLinearVelocity(it->second->id, to_meters(velocityPixelsPerSecond));
        return true;
    }

    bool set_body_gravity_scale(Physics2DBodyHandle body, float gravityScale) override {
        const auto it = bodies_.find(body.value);
        if (it == bodies_.end() || !std::isfinite(gravityScale) || gravityScale < 0.0F) return false;
        b2Body_SetGravityScale(it->second->id, gravityScale);
        return true;
    }

    bool apply_linear_impulse(Physics2DBodyHandle body,
                              TileVec2 impulsePixelKilogramsPerSecond) override {
        const auto it = bodies_.find(body.value);
        if (it == bodies_.end() || !finite(impulsePixelKilogramsPerSecond)) return false;
        b2Body_ApplyLinearImpulseToCenter(it->second->id, to_meters(impulsePixelKilogramsPerSecond), true);
        return true;
    }

    bool set_body_enabled(Physics2DBodyHandle body, bool enabled) override {
        const auto it = bodies_.find(body.value);
        if (it == bodies_.end()) return false;
        if (enabled) b2Body_Enable(it->second->id);
        else b2Body_Disable(it->second->id);
        return true;
    }

    bool drop_through_one_way(Physics2DBodyHandle body, float seconds) override {
        const auto it = bodies_.find(body.value);
        if (it == bodies_.end() || !std::isfinite(seconds)) return false;
        it->second->dropThroughRemaining = std::max(0.0F, seconds);
        return true;
    }

    [[nodiscard]] Physics2DRayCastHit ray_cast(TileVec2 originPixels,
                                                TileVec2 translationPixels,
                                                std::uint64_t categoryBits,
                                                std::uint64_t maskBits) const override {
        Physics2DRayCastHit out;
        if (!finite(originPixels) || !finite(translationPixels)) return out;
        b2QueryFilter filter = b2DefaultQueryFilter();
        filter.categoryBits = categoryBits;
        filter.maskBits = maskBits;
        const b2RayResult result = b2World_CastRayClosest(world_, to_meters(originPixels),
                                                          to_meters(translationPixels), filter);
        if (!result.hit) return out;
        out.hit = true;
        out.pointPixels = to_pixels(result.point);
        out.normal = {result.normal.x, result.normal.y};
        out.fraction = result.fraction;
        if (const ShapeRecord* shape = find_shape(result.shapeId); shape != nullptr) {
            TileVec2 tangent{-out.normal.y, out.normal.x};
            if (tangent.x < 0.0F) { tangent.x = -tangent.x; tangent.y = -tangent.y; }
            out.surfaceVelocityPixelsPerSecond = {tangent.x * shape->tangentSpeedPixelsPerSecond,
                                                   tangent.y * shape->tangentSpeedPixelsPerSecond};
            out.collider = shape->handle;
            out.colliderUserTag = shape->userTag;
            out.body = shape->body;
            if (const BodyRecord* body = find_body(out.body); body != nullptr) out.bodyUserTag = body->userTag;
        }
        return out;
    }

    [[nodiscard]] std::size_t query_aabb(TileAabb bounds,
                                         std::span<Physics2DOverlapHit> out,
                                         std::uint64_t categoryBits,
                                         std::uint64_t maskBits) const override {
        if (!finite(bounds.min) || !finite(bounds.size) ||
            bounds.size.x < 0.0F || bounds.size.y < 0.0F) return 0;
        const b2AABB query{to_meters(bounds.min), to_meters({bounds.max_x(), bounds.max_y()})};
        std::size_t count = 0;
        for (const auto& [handle, record] : shapes_) {
            static_cast<void>(handle);
            const b2Filter filter = b2Shape_GetFilter(record->id);
            if ((categoryBits & filter.maskBits) == 0U || (filter.categoryBits & maskBits) == 0U) continue;
            const b2AABB shapeBounds = b2Shape_GetAABB(record->id);
            const bool overlaps = query.lowerBound.x < shapeBounds.upperBound.x &&
                                  query.upperBound.x > shapeBounds.lowerBound.x &&
                                  query.lowerBound.y < shapeBounds.upperBound.y &&
                                  query.upperBound.y > shapeBounds.lowerBound.y;
            if (!overlaps) continue;
            if (count < out.size()) {
                Physics2DOverlapHit hit;
                hit.body = record->body;
                hit.collider = record->handle;
                hit.colliderUserTag = record->userTag;
                if (const BodyRecord* body = find_body(record->body); body != nullptr) {
                    hit.bodyUserTag = body->userTag;
                }
                out[count] = hit;
            }
            ++count;
        }
        return count;
    }

    [[nodiscard]] std::size_t query_shape(const Physics2DQueryShape& shape,
                                          std::span<Physics2DOverlapHit> out,
                                          const Physics2DQueryFilter& filter) const override {
        b2ShapeProxy proxy{};
        if (!make_query_proxy(shape, proxy) || filter.categoryBits == 0U || filter.maskBits == 0U) return 0U;
        OverlapQueryContext context{this, &filter, {}};
        b2QueryFilter queryFilter = b2DefaultQueryFilter();
        queryFilter.categoryBits = filter.categoryBits;
        queryFilter.maskBits = filter.maskBits;
#if defined(DVE_BOX2D_API_32)
        static_cast<void>(b2World_OverlapShape(world_, b2Pos_zero, &proxy, queryFilter,
                                                &Box2DPhysicsWorld::overlap_query_callback, &context));
#else
        static_cast<void>(b2World_OverlapShape(world_, &proxy, queryFilter,
                                                &Box2DPhysicsWorld::overlap_query_callback, &context));
#endif
        std::sort(context.hits.begin(), context.hits.end(), [](const Physics2DOverlapHit& a,
                                                               const Physics2DOverlapHit& b) {
            if (a.body.value != b.body.value) return a.body.value < b.body.value;
            return a.collider.value < b.collider.value;
        });
        const std::size_t writeCount = std::min(out.size(), context.hits.size());
        for (std::size_t index = 0U; index < writeCount; ++index) out[index] = context.hits[index];
        return context.hits.size();
    }

    [[nodiscard]] Physics2DShapeCastHit cast_shape(
        const Physics2DQueryShape& shape, TileVec2 translationPixels,
        const Physics2DQueryFilter& filter) const override {
        Physics2DShapeCastHit result;
        b2ShapeProxy proxy{};
        if (!make_query_proxy(shape, proxy) || !finite(translationPixels) ||
            filter.categoryBits == 0U || filter.maskBits == 0U) return result;
        ShapeCastContext context{this, &filter, {}};
        b2QueryFilter queryFilter = b2DefaultQueryFilter();
        queryFilter.categoryBits = filter.categoryBits;
        queryFilter.maskBits = filter.maskBits;
#if defined(DVE_BOX2D_API_32)
        static_cast<void>(b2World_CastShape(world_, b2Pos_zero, &proxy, to_meters(translationPixels),
                                             queryFilter, &Box2DPhysicsWorld::shape_cast_callback, &context));
#else
        static_cast<void>(b2World_CastShape(world_, &proxy, to_meters(translationPixels),
                                             queryFilter, &Box2DPhysicsWorld::shape_cast_callback, &context));
#endif
        return context.hit;
    }

    [[nodiscard]] bool overlaps_tile_map(TileAabb bounds) const override {
        if (!hasTileGrid_ || !finite(bounds.min) || !finite(bounds.size) ||
            bounds.size.x < 0.0F || bounds.size.y < 0.0F) return false;
        constexpr float epsilon = 0.001F;
        const auto minCol = static_cast<std::int64_t>(std::floor(
            bounds.min.x / static_cast<float>(tileGrid_.tileWidth)));
        const auto maxCol = static_cast<std::int64_t>(std::floor(
            (bounds.max_x() - epsilon) / static_cast<float>(tileGrid_.tileWidth)));
        const auto minRow = static_cast<std::int64_t>(std::floor(
            bounds.min.y / static_cast<float>(tileGrid_.tileHeight)));
        const auto maxRow = static_cast<std::int64_t>(std::floor(
            (bounds.max_y() - epsilon) / static_cast<float>(tileGrid_.tileHeight)));
        for (std::int64_t row = minRow; row <= maxRow; ++row) {
            for (std::int64_t col = minCol; col <= maxCol; ++col) {
                if (tileGrid_.at(col, row) != TileCollision::Empty) return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::span<const Physics2DEvent> events() const noexcept override { return events_; }

private:
    [[nodiscard]] static b2BodyType to_box_type(Physics2DBodyType type) noexcept {
        switch (type) {
        case Physics2DBodyType::Static: return b2_staticBody;
        case Physics2DBodyType::Kinematic: return b2_kinematicBody;
        case Physics2DBodyType::Dynamic: return b2_dynamicBody;
        }
        return b2_dynamicBody;
    }

    [[nodiscard]] bool finite(TileVec2 value) const noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y);
    }
    [[nodiscard]] b2Vec2 to_meters(TileVec2 pixels) const noexcept {
        return {pixels.x / settings_.pixelsPerMeter, pixels.y / settings_.pixelsPerMeter};
    }
    [[nodiscard]] TileVec2 to_pixels(b2Vec2 meters) const noexcept {
        return {meters.x * settings_.pixelsPerMeter, meters.y * settings_.pixelsPerMeter};
    }

    struct OverlapQueryContext {
        const Box2DPhysicsWorld* world{};
        const Physics2DQueryFilter* filter{};
        std::vector<Physics2DOverlapHit> hits;
    };
    struct ShapeCastContext {
        const Box2DPhysicsWorld* world{};
        const Physics2DQueryFilter* filter{};
        Physics2DShapeCastHit hit{};
    };

    [[nodiscard]] bool make_query_proxy(const Physics2DQueryShape& shape,
                                        b2ShapeProxy& proxy) const {
        if (!finite(shape.centerPixels) || !finite(shape.halfExtentsPixels) ||
            !finite(shape.capsulePoint1Pixels) || !finite(shape.capsulePoint2Pixels) ||
            !std::isfinite(shape.angleRadians) || !std::isfinite(shape.radiusPixels)) return false;
        proxy = {};
        const float c = std::cos(shape.angleRadians);
        const float sn = std::sin(shape.angleRadians);
        const auto world_point = [&](TileVec2 local) {
            const TileVec2 rotated{c * local.x - sn * local.y, sn * local.x + c * local.y};
            return to_meters({shape.centerPixels.x + rotated.x, shape.centerPixels.y + rotated.y});
        };
        switch (shape.shape) {
        case Physics2DShapeType::Box: {
            if (!(shape.halfExtentsPixels.x > 0.0F && shape.halfExtentsPixels.y > 0.0F)) return false;
            const TileVec2 h = shape.halfExtentsPixels;
            proxy.points[0] = world_point({-h.x, -h.y});
            proxy.points[1] = world_point({ h.x, -h.y});
            proxy.points[2] = world_point({ h.x,  h.y});
            proxy.points[3] = world_point({-h.x,  h.y});
            proxy.count = 4;
            break;
        }
        case Physics2DShapeType::Circle:
            if (!(shape.radiusPixels > 0.0F)) return false;
            proxy.points[0] = to_meters(shape.centerPixels);
            proxy.count = 1;
            proxy.radius = shape.radiusPixels / settings_.pixelsPerMeter;
            break;
        case Physics2DShapeType::Capsule:
            if (!(shape.radiusPixels > 0.0F)) return false;
            proxy.points[0] = world_point(shape.capsulePoint1Pixels);
            proxy.points[1] = world_point(shape.capsulePoint2Pixels);
            proxy.count = 2;
            proxy.radius = shape.radiusPixels / settings_.pixelsPerMeter;
            break;
        case Physics2DShapeType::ConvexPolygon:
            if (shape.verticesPixels.size() < 3U ||
                shape.verticesPixels.size() > B2_MAX_POLYGON_VERTICES) return false;
            proxy.count = static_cast<int>(shape.verticesPixels.size());
            for (int index = 0; index < proxy.count; ++index) {
                const TileVec2 point = shape.verticesPixels[static_cast<std::size_t>(index)];
                if (!finite(point)) return false;
                proxy.points[index] = world_point(point);
            }
            break;
        }
        return true;
    }

    static bool overlap_query_callback(b2ShapeId shapeId, void* rawContext) {
        auto& context = *static_cast<OverlapQueryContext*>(rawContext);
        const ShapeRecord* shape = context.world->find_shape(shapeId);
        if (shape == nullptr) return true;
        if (shape->body == context.filter->ignoredBody ||
            (!context.filter->includeSensors && shape->sensor) ||
            (!context.filter->includeTileMap && !shape->body)) return true;
        Physics2DOverlapHit hit;
        hit.body = shape->body;
        hit.collider = shape->handle;
        hit.colliderUserTag = shape->userTag;
        if (const BodyRecord* body = context.world->find_body(shape->body); body != nullptr) {
            hit.bodyUserTag = body->userTag;
        }
        context.hits.push_back(hit);
        return true;
    }

#if defined(DVE_BOX2D_API_32)
    static float shape_cast_callback(b2ShapeId shapeId, b2Pos point, b2Vec2 normal,
                                     float fraction, void* rawContext) {
        const TileVec2 pointPixels{static_cast<float>(point.x) *
                                      static_cast<ShapeCastContext*>(rawContext)->world->settings_.pixelsPerMeter,
                                  static_cast<float>(point.y) *
                                      static_cast<ShapeCastContext*>(rawContext)->world->settings_.pixelsPerMeter};
#else
    static float shape_cast_callback(b2ShapeId shapeId, b2Vec2 point, b2Vec2 normal,
                                     float fraction, void* rawContext) {
        const TileVec2 pointPixels = static_cast<ShapeCastContext*>(rawContext)->world->to_pixels(point);
#endif
        auto& context = *static_cast<ShapeCastContext*>(rawContext);
        const ShapeRecord* shape = context.world->find_shape(shapeId);
        if (shape == nullptr || shape->body == context.filter->ignoredBody ||
            (!context.filter->includeSensors && shape->sensor) ||
            (!context.filter->includeTileMap && !shape->body)) return -1.0F;
        Physics2DShapeCastHit hit;
        hit.hit = true;
        hit.startedOverlapping = fraction <= 0.0F && std::fabs(normal.x) + std::fabs(normal.y) < 0.001F;
        hit.body = shape->body;
        hit.collider = shape->handle;
        hit.colliderUserTag = shape->userTag;
        hit.pointPixels = pointPixels;
        hit.normal = {normal.x, normal.y};
        hit.fraction = fraction;
        if (const BodyRecord* body = context.world->find_body(shape->body); body != nullptr) {
            hit.bodyUserTag = body->userTag;
            hit.surfaceVelocityPixelsPerSecond = context.world->to_pixels(b2Body_GetLinearVelocity(body->id));
        }
        TileVec2 tangent{-normal.y, normal.x};
        if (tangent.x < 0.0F) tangent = {-tangent.x, -tangent.y};
        hit.surfaceVelocityPixelsPerSecond.x += tangent.x * shape->tangentSpeedPixelsPerSecond;
        hit.surfaceVelocityPixelsPerSecond.y += tangent.y * shape->tangentSpeedPixelsPerSecond;
        context.hit = hit;
        return fraction;
    }

    [[nodiscard]] bool validate_collider(const Physics2DColliderDef& def, std::string* error) const {
        const auto fail = [error](const char* text) {
            if (error != nullptr) *error = text;
            return false;
        };
        if (!finite(def.localCenterPixels) || !finite(def.halfExtentsPixels) ||
            !finite(def.capsulePoint1Pixels) || !finite(def.capsulePoint2Pixels) ||
            !std::isfinite(def.radiusPixels) || !std::isfinite(def.density) ||
            !std::isfinite(def.friction) || !std::isfinite(def.restitution) ||
            !std::isfinite(def.rollingResistance) || !std::isfinite(def.tangentSpeedPixelsPerSecond)) {
            return fail("collider definition contains a non-finite value");
        }
        if (def.shape == Physics2DShapeType::Box &&
            (!(def.halfExtentsPixels.x > 0.0F) || !(def.halfExtentsPixels.y > 0.0F))) {
            return fail("box half extents must be positive");
        }
        if ((def.shape == Physics2DShapeType::Circle || def.shape == Physics2DShapeType::Capsule) &&
            !(def.radiusPixels > 0.0F)) {
            return fail("circle/capsule radius must be positive");
        }
        if (def.shape == Physics2DShapeType::ConvexPolygon) {
            if (def.verticesPixels.size() < 3U || def.verticesPixels.size() > B2_MAX_POLYGON_VERTICES) {
                return fail("convex polygon must have 3..B2_MAX_POLYGON_VERTICES vertices");
            }
            for (const TileVec2 point : def.verticesPixels) {
                if (!finite(point)) return fail("convex polygon contains a non-finite vertex");
            }
        }
        return true;
    }

    static constexpr std::uint32_t kTileChunkSize = 16U;

    [[nodiscard]] static std::uint64_t tile_chunk_key(std::uint32_t col,
                                                       std::uint32_t row) noexcept {
        return (static_cast<std::uint64_t>(row) << 32U) | col;
    }

    void destroy_tile_chunk(std::uint32_t chunkCol, std::uint32_t chunkRow) {
        const auto it = tileChunks_.find(tile_chunk_key(chunkCol, chunkRow));
        if (it == tileChunks_.end()) return;
        for (const auto& record : it->second.shapes) {
            shapeByBoxId_.erase(b2StoreShapeId(record->id));
        }
        if (b2Body_IsValid(it->second.body)) b2DestroyBody(it->second.body);
        tileChunks_.erase(it);
    }

    [[nodiscard]] bool create_tile_box(TileChunkRecord& chunk, TileVec2 centerPixels,
                                       TileVec2 halfExtentsPixels, bool oneWay,
                                       float topPixels) {
        auto record = std::make_unique<ShapeRecord>();
        record->oneWay = oneWay;
        record->oneWayTopMeters = topPixels / settings_.pixelsPerMeter;
        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.userData = record.get();
        shapeDef.material.friction = 0.6F;
        shapeDef.material.restitution = 0.0F;
        shapeDef.enableContactEvents = false;
        shapeDef.enableHitEvents = false;
        shapeDef.enablePreSolveEvents = oneWay;
        const b2Polygon polygon = b2MakeOffsetBox(halfExtentsPixels.x / settings_.pixelsPerMeter,
                                                  halfExtentsPixels.y / settings_.pixelsPerMeter,
                                                  to_meters(centerPixels), b2Rot_identity);
        record->id = b2CreatePolygonShape(chunk.body, &shapeDef, &polygon);
        if (!b2Shape_IsValid(record->id)) return false;
        shapeByBoxId_.emplace(b2StoreShapeId(record->id), record.get());
        chunk.shapes.push_back(std::move(record));
        return true;
    }

    [[nodiscard]] bool create_tile_slope(TileChunkRecord& chunk, std::uint32_t col,
                                         std::uint32_t row, TileCollision collision) {
        const float left = static_cast<float>(col * tileGrid_.tileWidth);
        const float right = left + static_cast<float>(tileGrid_.tileWidth);
        const float top = static_cast<float>(row * tileGrid_.tileHeight);
        const float bottom = top + static_cast<float>(tileGrid_.tileHeight);
        b2Vec2 points[3];
        if (collision == TileCollision::SlopeUpRight) {
            points[0] = to_meters({left, bottom});
            points[1] = to_meters({right, top});
            points[2] = to_meters({right, bottom});
        } else {
            points[0] = to_meters({left, top});
            points[1] = to_meters({right, bottom});
            points[2] = to_meters({left, bottom});
        }
        const b2Hull hull = b2ComputeHull(points, 3);
        if (hull.count < 3) return false;
        const b2Polygon polygon = b2MakePolygon(&hull, 0.0F);
        auto record = std::make_unique<ShapeRecord>();
        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.userData = record.get();
        shapeDef.material.friction = 0.6F;
        shapeDef.enableContactEvents = false;
        shapeDef.enableHitEvents = false;
        record->id = b2CreatePolygonShape(chunk.body, &shapeDef, &polygon);
        if (!b2Shape_IsValid(record->id)) return false;
        shapeByBoxId_.emplace(b2StoreShapeId(record->id), record.get());
        chunk.shapes.push_back(std::move(record));
        return true;
    }

    [[nodiscard]] bool cook_tile_chunk(std::uint32_t chunkCol, std::uint32_t chunkRow) {
        const std::uint32_t minCol = chunkCol * kTileChunkSize;
        const std::uint32_t minRow = chunkRow * kTileChunkSize;
        if (minCol >= tileGrid_.width || minRow >= tileGrid_.height) return true;
        const std::uint32_t maxCol = std::min(tileGrid_.width, minCol + kTileChunkSize);
        const std::uint32_t maxRow = std::min(tileGrid_.height, minRow + kTileChunkSize);

        TileChunkRecord chunk;
        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_staticBody;
        bodyDef.name = "DVE tile chunk";
        chunk.body = b2CreateBody(world_, &bodyDef);
        if (!b2Body_IsValid(chunk.body)) return false;

        const std::uint32_t width = maxCol - minCol;
        const std::uint32_t height = maxRow - minRow;
        std::vector<std::uint8_t> consumed(static_cast<std::size_t>(width) * height, 0U);
        const auto local_index = [width, minCol, minRow](std::uint32_t col, std::uint32_t row) {
            return static_cast<std::size_t>(row - minRow) * width + (col - minCol);
        };

        for (std::uint32_t row = minRow; row < maxRow; ++row) {
            for (std::uint32_t col = minCol; col < maxCol; ++col) {
                if (consumed[local_index(col, row)] != 0U ||
                    tileGrid_.at(col, row) != TileCollision::Solid) continue;
                std::uint32_t rectangleWidth = 1U;
                while (col + rectangleWidth < maxCol &&
                       consumed[local_index(col + rectangleWidth, row)] == 0U &&
                       tileGrid_.at(col + rectangleWidth, row) == TileCollision::Solid) {
                    ++rectangleWidth;
                }
                std::uint32_t rectangleHeight = 1U;
                bool canGrow = true;
                while (row + rectangleHeight < maxRow && canGrow) {
                    for (std::uint32_t x = 0; x < rectangleWidth; ++x) {
                        if (consumed[local_index(col + x, row + rectangleHeight)] != 0U ||
                            tileGrid_.at(col + x, row + rectangleHeight) != TileCollision::Solid) {
                            canGrow = false;
                            break;
                        }
                    }
                    if (canGrow) ++rectangleHeight;
                }
                for (std::uint32_t y = 0; y < rectangleHeight; ++y) {
                    for (std::uint32_t x = 0; x < rectangleWidth; ++x) {
                        consumed[local_index(col + x, row + y)] = 1U;
                    }
                }
                const float pixelWidth = static_cast<float>(rectangleWidth * tileGrid_.tileWidth);
                const float pixelHeight = static_cast<float>(rectangleHeight * tileGrid_.tileHeight);
                const TileVec2 center{static_cast<float>(col * tileGrid_.tileWidth) + pixelWidth * 0.5F,
                                      static_cast<float>(row * tileGrid_.tileHeight) + pixelHeight * 0.5F};
                if (!create_tile_box(chunk, center, {pixelWidth * 0.5F, pixelHeight * 0.5F},
                                     false, static_cast<float>(row * tileGrid_.tileHeight))) {
                    b2DestroyBody(chunk.body);
                    return false;
                }
            }
        }

        for (std::uint32_t row = minRow; row < maxRow; ++row) {
            std::uint32_t col = minCol;
            while (col < maxCol) {
                const TileCollision collision = tileGrid_.at(col, row);
                if (collision == TileCollision::SlopeUpRight || collision == TileCollision::SlopeUpLeft) {
                    if (!create_tile_slope(chunk, col, row, collision)) {
                        b2DestroyBody(chunk.body);
                        return false;
                    }
                    ++col;
                    continue;
                }
                if (collision != TileCollision::OneWayTop) {
                    ++col;
                    continue;
                }
                const std::uint32_t start = col;
                while (col < maxCol && tileGrid_.at(col, row) == TileCollision::OneWayTop) ++col;
                const std::uint32_t run = col - start;
                const float thickness = std::max(0.5F, std::min(2.0F,
                    static_cast<float>(tileGrid_.tileHeight) * 0.125F));
                const float pixelWidth = static_cast<float>(run * tileGrid_.tileWidth);
                const float top = static_cast<float>(row * tileGrid_.tileHeight);
                const TileVec2 center{static_cast<float>(start * tileGrid_.tileWidth) + pixelWidth * 0.5F,
                                      top + thickness * 0.5F};
                if (!create_tile_box(chunk, center, {pixelWidth * 0.5F, thickness * 0.5F}, true, top)) {
                    b2DestroyBody(chunk.body);
                    return false;
                }
            }
        }

        tileChunks_.emplace(tile_chunk_key(chunkCol, chunkRow), std::move(chunk));
        return true;
    }

    [[nodiscard]] const ShapeRecord* find_shape(b2ShapeId id) const noexcept {
        const auto it = shapeByBoxId_.find(b2StoreShapeId(id));
        return it == shapeByBoxId_.end() ? nullptr : it->second;
    }
    [[nodiscard]] ShapeRecord* find_shape(b2ShapeId id) noexcept {
        const auto it = shapeByBoxId_.find(b2StoreShapeId(id));
        return it == shapeByBoxId_.end() ? nullptr : it->second;
    }
    [[nodiscard]] const BodyRecord* find_body(Physics2DBodyHandle handle) const noexcept {
        const auto it = bodies_.find(handle.value);
        return it == bodies_.end() ? nullptr : it->second.get();
    }
    [[nodiscard]] BodyRecord* find_body(Physics2DBodyHandle handle) noexcept {
        const auto it = bodies_.find(handle.value);
        return it == bodies_.end() ? nullptr : it->second.get();
    }

#if defined(DVE_BOX2D_API_32)
    static bool pre_solve(b2ShapeId shapeIdA, b2ShapeId shapeIdB, b2Pos point,
                          b2Vec2 normal, void* context) {
        static_cast<void>(point);
#else
    static bool pre_solve(b2ShapeId shapeIdA, b2ShapeId shapeIdB, b2Manifold* manifold,
                          void* context) {
        const b2Vec2 normal = manifold->normal;
#endif
        auto* self = static_cast<Box2DPhysicsWorld*>(context);
        ShapeRecord* shapeA = self->find_shape(shapeIdA);
        ShapeRecord* shapeB = self->find_shape(shapeIdB);
        ShapeRecord* platform = nullptr;
        b2ShapeId actorShape = b2_nullShapeId;
        bool platformIsA = false;
        if (shapeA != nullptr && shapeA->oneWay) {
            platform = shapeA;
            actorShape = shapeIdB;
            platformIsA = true;
        } else if (shapeB != nullptr && shapeB->oneWay) {
            platform = shapeB;
            actorShape = shapeIdA;
        } else {
            return true;
        }

        // Only retain a contact whose normal pushes the actor upward in DVE's y-down coordinates.
        const b2Vec2 n = platformIsA ? normal : b2Vec2{-normal.x, -normal.y};
        if (n.y > -0.5F) return false;

        const ShapeRecord* actor = self->find_shape(actorShape);
        BodyRecord* body = actor == nullptr ? nullptr : self->find_body(actor->body);
        if (body == nullptr || !body->hasPreStepState) return false;
        if (body->dropThroughRemaining > 0.0F) return false;
        if (body->preStepLinearVelocity.y < -0.01F) return false; // moving upward through the platform
        const float slop = self->settings_.oneWaySlopPixels / self->settings_.pixelsPerMeter;
        return body->preStepAabb.upperBound.y <= platform->oneWayTopMeters + slop;
    }

    void collect_joint_breaks() {
        std::vector<Physics2DJointHandle> broken;
        for (const auto& [handle, record] : joints_) {
            if (!b2Joint_IsValid(record->id) || record->broken) continue;
            const b2Vec2 force = b2Joint_GetConstraintForce(record->id);
            const float forceMagnitude = std::hypot(force.x, force.y);
            const float torqueMagnitude = std::fabs(b2Joint_GetConstraintTorque(record->id));
            const bool forceBreak = record->def.breakForce > 0.0F && forceMagnitude > record->def.breakForce;
            const bool torqueBreak = record->def.breakTorque > 0.0F && torqueMagnitude > record->def.breakTorque;
            if (!forceBreak && !torqueBreak) continue;
            Physics2DEvent event;
            event.type = Physics2DEventType::JointBreak;
            event.joint = record->handle;
            event.jointUserTag = record->def.userTag;
            event.bodyA = record->def.bodyA;
            event.bodyB = record->def.bodyB;
            event.approachSpeedPixelsPerSecond = forceMagnitude;
            events_.push_back(event);
            broken.push_back(Physics2DJointHandle{handle});
        }
        for (const Physics2DJointHandle handle : broken) {
            static_cast<void>(destroy_joint(handle));
        }
    }

    void collect_events() {
        const b2ContactEvents contacts = b2World_GetContactEvents(world_);
        for (int i = 0; i < contacts.beginCount; ++i) {
            append_pair_event(Physics2DEventType::ContactBegin, contacts.beginEvents[i].shapeIdA,
                              contacts.beginEvents[i].shapeIdB, {}, {}, 0.0F);
        }
        for (int i = 0; i < contacts.endCount; ++i) {
            append_pair_event(Physics2DEventType::ContactEnd, contacts.endEvents[i].shapeIdA,
                              contacts.endEvents[i].shapeIdB, {}, {}, 0.0F);
        }
        for (int i = 0; i < contacts.hitCount; ++i) {
            const auto& hit = contacts.hitEvents[i];
            append_pair_event(Physics2DEventType::ContactHit, hit.shapeIdA, hit.shapeIdB,
                              to_pixels(hit.point), {hit.normal.x, hit.normal.y},
                              hit.approachSpeed * settings_.pixelsPerMeter);
        }
        const b2SensorEvents sensors = b2World_GetSensorEvents(world_);
        for (int i = 0; i < sensors.beginCount; ++i) {
            append_pair_event(Physics2DEventType::SensorBegin, sensors.beginEvents[i].sensorShapeId,
                              sensors.beginEvents[i].visitorShapeId, {}, {}, 0.0F);
        }
        for (int i = 0; i < sensors.endCount; ++i) {
            append_pair_event(Physics2DEventType::SensorEnd, sensors.endEvents[i].sensorShapeId,
                              sensors.endEvents[i].visitorShapeId, {}, {}, 0.0F);
        }
    }

    void append_pair_event(Physics2DEventType type, b2ShapeId shapeIdA, b2ShapeId shapeIdB,
                           TileVec2 point, TileVec2 normal, float speed) {
        const ShapeRecord* a = find_shape(shapeIdA);
        const ShapeRecord* b = find_shape(shapeIdB);
        Physics2DEvent event;
        event.type = type;
        event.pointPixels = point;
        event.normal = normal;
        event.approachSpeedPixelsPerSecond = speed;
        if (a != nullptr) {
            event.bodyA = a->body;
            event.colliderA = a->handle;
            event.colliderUserTagA = a->userTag;
            if (const BodyRecord* body = find_body(a->body); body != nullptr) event.bodyUserTagA = body->userTag;
        }
        if (b != nullptr) {
            event.bodyB = b->body;
            event.colliderB = b->handle;
            event.colliderUserTagB = b->userTag;
            if (const BodyRecord* body = find_body(b->body); body != nullptr) event.bodyUserTagB = body->userTag;
        }
        events_.push_back(event);
    }

    Physics2DWorldSettings settings_;
    b2WorldId world_{};
    CollisionGrid tileGrid_{};
    bool hasTileGrid_{false};
    float accumulator_{};
    std::uint64_t nextHandle_{1};
    std::unordered_map<std::uint64_t, std::unique_ptr<BodyRecord>> bodies_;
    std::unordered_map<std::uint64_t, std::unique_ptr<ShapeRecord>> shapes_;
    std::unordered_map<std::uint64_t, std::unique_ptr<JointRecord>> joints_;
    std::unordered_map<std::uint64_t, ShapeRecord*> shapeByBoxId_;
    std::unordered_map<std::uint64_t, TileChunkRecord> tileChunks_;
    std::vector<Physics2DEvent> events_;
};

} // namespace

std::unique_ptr<Physics2DWorld> create_box2d_physics_world(const Physics2DWorldSettings& settings,
                                                           std::string* error) {
    if (!(settings.pixelsPerMeter > 0.0F) || !std::isfinite(settings.pixelsPerMeter)) {
        if (error != nullptr) *error = "pixelsPerMeter must be finite and positive";
        return nullptr;
    }
    auto world = std::make_unique<Box2DPhysicsWorld>(settings);
    if (!world->valid()) {
        if (error != nullptr) *error = "Box2D could not allocate a world";
        return nullptr;
    }
    return world;
}

} // namespace dve
