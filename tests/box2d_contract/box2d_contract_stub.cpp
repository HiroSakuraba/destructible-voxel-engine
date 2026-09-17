#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace {
struct World {
    b2WorldDef def{};
    b2PreSolveFcn* preSolve{};
    void* preSolveContext{};
};
struct Body {
    b2BodyId id{};
    b2WorldId world{};
    b2BodyDef def{};
};
enum class Geometry : std::uint8_t { Circle, Capsule, Polygon };
enum class JointKind : std::uint8_t { Distance, Motor, Prismatic, Revolute, Weld, Wheel };
struct Joint {
    b2JointId id{};
    b2WorldId world{};
    JointKind kind{JointKind::Distance};
    b2BodyId bodyA{};
    b2BodyId bodyB{};
    void* userData{};
    float translation{};
    float speed{};
    float motorSpeed{};
    float motorForce{};
    float motorTorque{};
};
struct Shape {
    b2ShapeId id{};
    b2BodyId body{};
    b2ShapeDef def{};
    Geometry geometry{Geometry::Polygon};
    b2Circle circle{};
    b2Capsule capsule{};
    b2Polygon polygon{};
};

std::unordered_map<std::uint32_t, World> worlds;
std::unordered_map<std::uint64_t, Body> bodies;
std::unordered_map<std::uint64_t, Shape> shapes;
std::unordered_map<std::uint64_t, Joint> joints;
std::uint16_t nextWorld = 1;
std::int32_t nextBody = 1;
std::int32_t nextShape = 1;
std::int32_t nextJoint = 1;

std::uint64_t body_key(b2BodyId id) { return b2StoreBodyId(id); }
std::uint64_t shape_key(b2ShapeId id) { return b2StoreShapeId(id); }
std::uint64_t joint_key(b2JointId id) { return b2StoreJointId(id); }
World* get_world(b2WorldId id) {
    const auto it = worlds.find(b2StoreWorldId(id));
    return it == worlds.end() ? nullptr : &it->second;
}
Body* get_body(b2BodyId id) {
    const auto it = bodies.find(body_key(id));
    return it == bodies.end() ? nullptr : &it->second;
}
const Body* get_body_const(b2BodyId id) {
    const auto it = bodies.find(body_key(id));
    return it == bodies.end() ? nullptr : &it->second;
}
Shape* get_shape(b2ShapeId id) {
    const auto it = shapes.find(shape_key(id));
    return it == shapes.end() ? nullptr : &it->second;
}

b2ShapeId create_shape(b2BodyId body, const b2ShapeDef* def, Geometry geometry) {
    if (get_body(body) == nullptr || def == nullptr) return b2_nullShapeId;
    b2ShapeId id{nextShape++, body.world0, 1};
    Shape shape;
    shape.id = id;
    shape.body = body;
    shape.def = *def;
    shape.geometry = geometry;
    shapes.emplace(shape_key(id), shape);
    return id;
}

b2JointId create_joint_record(b2WorldId world, JointKind kind, b2BodyId bodyA,
                              b2BodyId bodyB, void* userData, float translation,
                              float motorSpeed, float motorForce, float motorTorque) {
    if (get_world(world) == nullptr || get_body(bodyA) == nullptr || get_body(bodyB) == nullptr) return b2_nullJointId;
    b2JointId id{nextJoint++, static_cast<std::uint16_t>(world.index1 - 1U), 1};
    joints.emplace(joint_key(id), Joint{id, world, kind, bodyA, bodyB, userData,
                                        translation, 0.0F, motorSpeed, motorForce, motorTorque});
    return id;
}
const Joint* get_joint_const(b2JointId id) {
    const auto it = joints.find(joint_key(id));
    return it == joints.end() ? nullptr : &it->second;
}

b2AABB empty_aabb(b2Vec2 center) {
    return {{center.x, center.y}, {center.x, center.y}};
}
void include_point(b2AABB& box, b2Vec2 point) {
    box.lowerBound.x = std::min(box.lowerBound.x, point.x);
    box.lowerBound.y = std::min(box.lowerBound.y, point.y);
    box.upperBound.x = std::max(box.upperBound.x, point.x);
    box.upperBound.y = std::max(box.upperBound.y, point.y);
}
} // namespace

extern "C" {

b2WorldDef b2DefaultWorldDef(void) {
    b2WorldDef def{};
    def.gravity = {0.0F, -10.0F};
    def.enableSleep = true;
    def.enableContinuous = true;
    def.maximumLinearSpeed = 400.0F;
    def.internalValue = 1;
    return def;
}

b2BodyDef b2DefaultBodyDef(void) {
    b2BodyDef def{};
    def.type = b2_staticBody;
    def.rotation = b2Rot_identity;
    def.gravityScale = 1.0F;
    def.enableSleep = true;
    def.isAwake = true;
    def.isEnabled = true;
    def.internalValue = 1;
    return def;
}

b2ShapeDef b2DefaultShapeDef(void) {
    b2ShapeDef def{};
    def.material.friction = 0.6F;
    def.density = 1.0F;
    def.filter.categoryBits = 1;
    def.filter.maskBits = UINT64_MAX;
    def.updateBodyMass = true;
    def.internalValue = 1;
    return def;
}

b2QueryFilter b2DefaultQueryFilter(void) { return {1, UINT64_MAX}; }
b2DistanceJointDef b2DefaultDistanceJointDef(void) { b2DistanceJointDef d{}; d.length=1.0F; d.minLength=0.1F; d.maxLength=1.0F; d.internalValue=1; return d; }
b2MotorJointDef b2DefaultMotorJointDef(void) { b2MotorJointDef d{}; d.correctionFactor=0.3F; d.internalValue=1; return d; }
b2PrismaticJointDef b2DefaultPrismaticJointDef(void) { b2PrismaticJointDef d{}; d.localAxisA={1.0F,0.0F}; d.internalValue=1; return d; }
b2RevoluteJointDef b2DefaultRevoluteJointDef(void) { b2RevoluteJointDef d{}; d.internalValue=1; return d; }
b2WeldJointDef b2DefaultWeldJointDef(void) { b2WeldJointDef d{}; d.internalValue=1; return d; }
b2WheelJointDef b2DefaultWheelJointDef(void) { b2WheelJointDef d{}; d.localAxisA={1.0F,0.0F}; d.internalValue=1; return d; }

b2Rot b2MakeRot(float angle) { return {std::cos(angle), std::sin(angle)}; }

b2WorldId b2CreateWorld(const b2WorldDef* def) {
    if (def == nullptr) return b2_nullWorldId;
    b2WorldId id{nextWorld++, 1};
    worlds.emplace(b2StoreWorldId(id), World{*def, nullptr, nullptr});
    return id;
}

void b2DestroyWorld(b2WorldId worldId) {
    const std::uint16_t world0 = static_cast<std::uint16_t>(worldId.index1 - 1U);
    std::vector<std::uint64_t> removeShapes;
    for (const auto& [key, shape] : shapes) if (shape.id.world0 == world0) removeShapes.push_back(key);
    for (const auto key : removeShapes) shapes.erase(key);
    std::vector<std::uint64_t> removeJoints;
    for (const auto& [key, joint] : joints) if (joint.id.world0 == world0) removeJoints.push_back(key);
    for (const auto key : removeJoints) joints.erase(key);
    std::vector<std::uint64_t> removeBodies;
    for (const auto& [key, body] : bodies) if (body.id.world0 == world0) removeBodies.push_back(key);
    for (const auto key : removeBodies) bodies.erase(key);
    worlds.erase(b2StoreWorldId(worldId));
}

void b2World_SetPreSolveCallback(b2WorldId worldId, b2PreSolveFcn* callback, void* context) {
    if (World* world = get_world(worldId); world != nullptr) {
        world->preSolve = callback;
        world->preSolveContext = context;
    }
}

void b2World_Step(b2WorldId worldId, float timeStep, int subStepCount) {
    World* world = get_world(worldId);
    if (world == nullptr || !(timeStep > 0.0F)) return;
    const int steps = std::max(1, subStepCount);
    const float dt = timeStep / static_cast<float>(steps);
    const std::uint16_t world0 = static_cast<std::uint16_t>(worldId.index1 - 1U);
    for (int substep = 0; substep < steps; ++substep) {
        for (auto& [key, body] : bodies) {
            static_cast<void>(key);
            if (body.id.world0 != world0 || !body.def.isEnabled || body.def.type == b2_staticBody) continue;
            if (body.def.type == b2_dynamicBody) {
                body.def.linearVelocity.x += world->def.gravity.x * body.def.gravityScale * dt;
                body.def.linearVelocity.y += world->def.gravity.y * body.def.gravityScale * dt;
            }
            body.def.position.x += body.def.linearVelocity.x * dt;
            body.def.position.y += body.def.linearVelocity.y * dt;
            body.def.rotation = b2MakeRot(std::atan2(body.def.rotation.s, body.def.rotation.c) + body.def.angularVelocity * dt);
        }
    }
}

b2ContactEvents b2World_GetContactEvents(b2WorldId) { return {nullptr, nullptr, nullptr, 0, 0, 0}; }
b2SensorEvents b2World_GetSensorEvents(b2WorldId) { return {nullptr, nullptr, 0, 0}; }

b2RayResult b2World_CastRayClosest(b2WorldId worldId, b2Vec2 origin, b2Vec2 translation, b2QueryFilter filter) {
    b2RayResult result{};
    const std::uint16_t world0 = static_cast<std::uint16_t>(worldId.index1 - 1U);
    for (const auto& [key, shape] : shapes) {
        static_cast<void>(key);
        if (shape.id.world0 != world0) continue;
        if ((filter.maskBits & shape.def.filter.categoryBits) == 0U ||
            (shape.def.filter.maskBits & filter.categoryBits) == 0U) continue;
        result.shapeId = shape.id;
        result.point = {origin.x + translation.x * 0.5F, origin.y + translation.y * 0.5F};
        result.normal = translation.x != 0.0F ? b2Vec2{translation.x > 0.0F ? -1.0F : 1.0F, 0.0F}
                                                 : b2Vec2{0.0F, translation.y > 0.0F ? -1.0F : 1.0F};
        result.fraction = 0.5F;
        result.hit = true;
        return result;
    }
    return result;
}

b2TreeStats b2World_OverlapShape(b2WorldId worldId, const b2ShapeProxy* proxy,
                                       b2QueryFilter filter, b2OverlapResultFcn* fcn, void* context) {
    b2TreeStats stats{};
    if (proxy == nullptr || proxy->count <= 0 || fcn == nullptr) return stats;
    b2AABB query = empty_aabb(proxy->points[0]);
    for (int i = 0; i < proxy->count; ++i) {
        include_point(query, {proxy->points[i].x - proxy->radius, proxy->points[i].y - proxy->radius});
        include_point(query, {proxy->points[i].x + proxy->radius, proxy->points[i].y + proxy->radius});
    }
    const std::uint16_t world0 = static_cast<std::uint16_t>(worldId.index1 - 1U);
    for (const auto& [key, shape] : shapes) {
        static_cast<void>(key); ++stats.nodeVisits;
        if (shape.id.world0 != world0 || (filter.maskBits & shape.def.filter.categoryBits) == 0U ||
            (shape.def.filter.maskBits & filter.categoryBits) == 0U) continue;
        const b2AABB box = b2Shape_GetAABB(shape.id);
        if (query.lowerBound.x >= box.upperBound.x || query.upperBound.x <= box.lowerBound.x ||
            query.lowerBound.y >= box.upperBound.y || query.upperBound.y <= box.lowerBound.y) continue;
        ++stats.leafVisits;
        if (!fcn(shape.id, context)) break;
    }
    return stats;
}

b2TreeStats b2World_CastShape(b2WorldId worldId, const b2ShapeProxy* proxy, b2Vec2 translation,
                              b2QueryFilter filter, b2CastResultFcn* fcn, void* context) {
    b2TreeStats stats{};
    if (proxy == nullptr || proxy->count <= 0 || fcn == nullptr) return stats;
    const std::uint16_t world0 = static_cast<std::uint16_t>(worldId.index1 - 1U);
    for (const auto& [key, shape] : shapes) {
        static_cast<void>(key); ++stats.nodeVisits;
        if (shape.id.world0 != world0 || (filter.maskBits & shape.def.filter.categoryBits) == 0U ||
            (shape.def.filter.maskBits & filter.categoryBits) == 0U) continue;
        ++stats.leafVisits;
        const b2Vec2 point{proxy->points[0].x + translation.x * 0.5F,
                           proxy->points[0].y + translation.y * 0.5F};
        const b2Vec2 normal = std::fabs(translation.x) > std::fabs(translation.y)
            ? b2Vec2{translation.x > 0.0F ? -1.0F : 1.0F, 0.0F}
            : b2Vec2{0.0F, translation.y > 0.0F ? -1.0F : 1.0F};
        const float response = fcn(shape.id, point, normal, 0.5F, context);
        if (response == 0.0F) break;
    }
    return stats;
}

b2BodyId b2CreateBody(b2WorldId worldId, const b2BodyDef* def) {
    if (get_world(worldId) == nullptr || def == nullptr) return b2_nullBodyId;
    b2BodyId id{nextBody++, static_cast<std::uint16_t>(worldId.index1 - 1U), 1};
    bodies.emplace(body_key(id), Body{id, worldId, *def});
    return id;
}

void b2DestroyBody(b2BodyId bodyId) {
    std::vector<std::uint64_t> ownedJoints;
    for (const auto& [key, joint] : joints) {
        if (b2StoreBodyId(joint.bodyA) == b2StoreBodyId(bodyId) ||
            b2StoreBodyId(joint.bodyB) == b2StoreBodyId(bodyId)) ownedJoints.push_back(key);
    }
    for (const auto key : ownedJoints) joints.erase(key);
    std::vector<std::uint64_t> owned;
    for (const auto& [key, shape] : shapes) if (b2StoreBodyId(shape.body) == b2StoreBodyId(bodyId)) owned.push_back(key);
    for (const auto key : owned) shapes.erase(key);
    bodies.erase(body_key(bodyId));
}

bool b2Body_IsValid(b2BodyId bodyId) { return get_body_const(bodyId) != nullptr; }
b2Vec2 b2Body_GetPosition(b2BodyId bodyId) {
    const Body* body = get_body_const(bodyId);
    return body == nullptr ? b2Vec2{0.0F, 0.0F} : body->def.position;
}
b2Rot b2Body_GetRotation(b2BodyId bodyId) {
    const Body* body = get_body_const(bodyId);
    return body == nullptr ? b2Rot_identity : body->def.rotation;
}
b2Vec2 b2Body_GetLinearVelocity(b2BodyId bodyId) {
    const Body* body = get_body_const(bodyId);
    return body == nullptr ? b2Vec2{0.0F, 0.0F} : body->def.linearVelocity;
}
float b2Body_GetAngularVelocity(b2BodyId bodyId) {
    const Body* body = get_body_const(bodyId);
    return body == nullptr ? 0.0F : body->def.angularVelocity;
}
void b2Body_SetTransform(b2BodyId bodyId, b2Vec2 position, b2Rot rotation) {
    if (Body* body = get_body(bodyId); body != nullptr) { body->def.position = position; body->def.rotation = rotation; }
}
void b2Body_SetLinearVelocity(b2BodyId bodyId, b2Vec2 velocity) {
    if (Body* body = get_body(bodyId); body != nullptr) body->def.linearVelocity = velocity;
}
void b2Body_SetGravityScale(b2BodyId bodyId, float gravityScale) {
    if (Body* body = get_body(bodyId); body != nullptr) body->def.gravityScale = gravityScale;
}
void b2Body_ApplyLinearImpulseToCenter(b2BodyId bodyId, b2Vec2 impulse, bool wake) {
    if (Body* body = get_body(bodyId); body != nullptr) {
        body->def.linearVelocity.x += impulse.x;
        body->def.linearVelocity.y += impulse.y;
        if (wake) body->def.isAwake = true;
    }
}
bool b2Body_IsAwake(b2BodyId bodyId) {
    const Body* body = get_body_const(bodyId);
    return body != nullptr && body->def.isAwake;
}
bool b2Body_IsEnabled(b2BodyId bodyId) {
    const Body* body = get_body_const(bodyId);
    return body != nullptr && body->def.isEnabled;
}
void b2Body_Enable(b2BodyId bodyId) { if (Body* body = get_body(bodyId); body != nullptr) body->def.isEnabled = true; }
void b2Body_Disable(b2BodyId bodyId) { if (Body* body = get_body(bodyId); body != nullptr) body->def.isEnabled = false; }
void* b2Body_GetUserData(b2BodyId bodyId) {
    Body* body = get_body(bodyId);
    return body == nullptr ? nullptr : body->def.userData;
}

b2AABB b2Body_ComputeAABB(b2BodyId bodyId) {
    const Body* body = get_body_const(bodyId);
    if (body == nullptr) return {{0.0F, 0.0F}, {0.0F, 0.0F}};
    b2AABB result = empty_aabb(body->def.position);
    for (const auto& [key, shape] : shapes) {
        static_cast<void>(key);
        if (b2StoreBodyId(shape.body) != b2StoreBodyId(bodyId)) continue;
        if (shape.geometry == Geometry::Circle) {
            const b2Vec2 center{body->def.position.x + shape.circle.center.x, body->def.position.y + shape.circle.center.y};
            include_point(result, {center.x - shape.circle.radius, center.y - shape.circle.radius});
            include_point(result, {center.x + shape.circle.radius, center.y + shape.circle.radius});
        } else if (shape.geometry == Geometry::Capsule) {
            include_point(result, {body->def.position.x + shape.capsule.center1.x - shape.capsule.radius,
                                   body->def.position.y + shape.capsule.center1.y - shape.capsule.radius});
            include_point(result, {body->def.position.x + shape.capsule.center2.x + shape.capsule.radius,
                                   body->def.position.y + shape.capsule.center2.y + shape.capsule.radius});
        } else {
            for (int i = 0; i < shape.polygon.count; ++i) {
                include_point(result, {body->def.position.x + shape.polygon.vertices[i].x,
                                       body->def.position.y + shape.polygon.vertices[i].y});
            }
        }
    }
    return result;
}

b2ShapeId b2CreateCircleShape(b2BodyId bodyId, const b2ShapeDef* def, const b2Circle* circle) {
    const b2ShapeId id = create_shape(bodyId, def, Geometry::Circle);
    if (Shape* shape = get_shape(id); shape != nullptr && circle != nullptr) shape->circle = *circle;
    return id;
}
b2ShapeId b2CreateCapsuleShape(b2BodyId bodyId, const b2ShapeDef* def, const b2Capsule* capsule) {
    const b2ShapeId id = create_shape(bodyId, def, Geometry::Capsule);
    if (Shape* shape = get_shape(id); shape != nullptr && capsule != nullptr) shape->capsule = *capsule;
    return id;
}
b2ShapeId b2CreatePolygonShape(b2BodyId bodyId, const b2ShapeDef* def, const b2Polygon* polygon) {
    const b2ShapeId id = create_shape(bodyId, def, Geometry::Polygon);
    if (Shape* shape = get_shape(id); shape != nullptr && polygon != nullptr) shape->polygon = *polygon;
    return id;
}
void b2DestroyShape(b2ShapeId shapeId, bool) { shapes.erase(shape_key(shapeId)); }
bool b2Shape_IsValid(b2ShapeId shapeId) { return shapes.find(shape_key(shapeId)) != shapes.end(); }
b2BodyId b2Shape_GetBody(b2ShapeId shapeId) {
    const auto it = shapes.find(shape_key(shapeId));
    return it == shapes.end() ? b2_nullBodyId : it->second.body;
}

b2Filter b2Shape_GetFilter(b2ShapeId shapeId) {
    const auto it = shapes.find(shape_key(shapeId));
    return it == shapes.end() ? b2Filter{0, 0, 0} : it->second.def.filter;
}

b2AABB b2Shape_GetAABB(b2ShapeId shapeId) {
    const auto it = shapes.find(shape_key(shapeId));
    if (it == shapes.end()) return {{0.0F, 0.0F}, {0.0F, 0.0F}};
    const Shape& shape = it->second;
    const Body* body = get_body_const(shape.body);
    if (body == nullptr) return {{0.0F, 0.0F}, {0.0F, 0.0F}};
    b2AABB result = empty_aabb(body->def.position);
    if (shape.geometry == Geometry::Circle) {
        const b2Vec2 center{body->def.position.x + shape.circle.center.x,
                            body->def.position.y + shape.circle.center.y};
        include_point(result, {center.x - shape.circle.radius, center.y - shape.circle.radius});
        include_point(result, {center.x + shape.circle.radius, center.y + shape.circle.radius});
    } else if (shape.geometry == Geometry::Capsule) {
        include_point(result, {body->def.position.x + shape.capsule.center1.x - shape.capsule.radius,
                               body->def.position.y + shape.capsule.center1.y - shape.capsule.radius});
        include_point(result, {body->def.position.x + shape.capsule.center2.x + shape.capsule.radius,
                               body->def.position.y + shape.capsule.center2.y + shape.capsule.radius});
    } else {
        for (int i = 0; i < shape.polygon.count; ++i) {
            include_point(result, {body->def.position.x + shape.polygon.vertices[i].x,
                                   body->def.position.y + shape.polygon.vertices[i].y});
        }
    }
    return result;
}

b2JointId b2CreateDistanceJoint(b2WorldId worldId, const b2DistanceJointDef* def) {
    return def == nullptr ? b2_nullJointId : create_joint_record(worldId, JointKind::Distance, def->bodyIdA, def->bodyIdB, def->userData, def->length, def->motorSpeed, def->maxMotorForce, 0.0F);
}
b2JointId b2CreateMotorJoint(b2WorldId worldId, const b2MotorJointDef* def) {
    return def == nullptr ? b2_nullJointId : create_joint_record(worldId, JointKind::Motor, def->bodyIdA, def->bodyIdB, def->userData, 0.0F, 0.0F, def->maxForce, def->maxTorque);
}
b2JointId b2CreatePrismaticJoint(b2WorldId worldId, const b2PrismaticJointDef* def) {
    return def == nullptr ? b2_nullJointId : create_joint_record(worldId, JointKind::Prismatic, def->bodyIdA, def->bodyIdB, def->userData, 0.0F, def->motorSpeed, def->maxMotorForce, 0.0F);
}
b2JointId b2CreateRevoluteJoint(b2WorldId worldId, const b2RevoluteJointDef* def) {
    return def == nullptr ? b2_nullJointId : create_joint_record(worldId, JointKind::Revolute, def->bodyIdA, def->bodyIdB, def->userData, def->referenceAngle, def->motorSpeed, 0.0F, def->maxMotorTorque);
}
b2JointId b2CreateWeldJoint(b2WorldId worldId, const b2WeldJointDef* def) {
    return def == nullptr ? b2_nullJointId : create_joint_record(worldId, JointKind::Weld, def->bodyIdA, def->bodyIdB, def->userData, def->referenceAngle, 0.0F, 0.0F, 0.0F);
}
b2JointId b2CreateWheelJoint(b2WorldId worldId, const b2WheelJointDef* def) {
    return def == nullptr ? b2_nullJointId : create_joint_record(worldId, JointKind::Wheel, def->bodyIdA, def->bodyIdB, def->userData, 0.0F, def->motorSpeed, 0.0F, def->maxMotorTorque);
}
void b2DestroyJoint(b2JointId jointId, bool) { joints.erase(joint_key(jointId)); }
bool b2Joint_IsValid(b2JointId jointId) { return get_joint_const(jointId) != nullptr; }
b2Vec2 b2Joint_GetConstraintForce(b2JointId jointId) { const Joint* j=get_joint_const(jointId); return j == nullptr ? b2Vec2{} : b2Vec2{j->motorForce,0.0F}; }
float b2Joint_GetConstraintTorque(b2JointId jointId) { const Joint* j=get_joint_const(jointId); return j == nullptr ? 0.0F : j->motorTorque; }
float b2DistanceJoint_GetCurrentLength(b2JointId id) { const Joint* j=get_joint_const(id); return j == nullptr ? 0.0F : j->translation; }
float b2DistanceJoint_GetMotorSpeed(b2JointId id) { const Joint* j=get_joint_const(id); return j == nullptr ? 0.0F : j->motorSpeed; }
float b2DistanceJoint_GetMotorForce(b2JointId id) { const Joint* j=get_joint_const(id); return j == nullptr ? 0.0F : j->motorForce; }
float b2PrismaticJoint_GetTranslation(b2JointId id) { const Joint* j=get_joint_const(id); return j == nullptr ? 0.0F : j->translation; }
float b2PrismaticJoint_GetSpeed(b2JointId id) { const Joint* j=get_joint_const(id); return j == nullptr ? 0.0F : j->speed; }
float b2PrismaticJoint_GetMotorSpeed(b2JointId id) { const Joint* j=get_joint_const(id); return j == nullptr ? 0.0F : j->motorSpeed; }
float b2PrismaticJoint_GetMotorForce(b2JointId id) { const Joint* j=get_joint_const(id); return j == nullptr ? 0.0F : j->motorForce; }
float b2RevoluteJoint_GetAngle(b2JointId id) { const Joint* j=get_joint_const(id); return j == nullptr ? 0.0F : j->translation; }
float b2RevoluteJoint_GetMotorSpeed(b2JointId id) { const Joint* j=get_joint_const(id); return j == nullptr ? 0.0F : j->motorSpeed; }
float b2RevoluteJoint_GetMotorTorque(b2JointId id) { const Joint* j=get_joint_const(id); return j == nullptr ? 0.0F : j->motorTorque; }
float b2WheelJoint_GetMotorSpeed(b2JointId id) { const Joint* j=get_joint_const(id); return j == nullptr ? 0.0F : j->motorSpeed; }
float b2WheelJoint_GetMotorTorque(b2JointId id) { const Joint* j=get_joint_const(id); return j == nullptr ? 0.0F : j->motorTorque; }

b2Hull b2ComputeHull(const b2Vec2* points, int count) {
    b2Hull hull{};
    if (points == nullptr || count < 3 || count > B2_MAX_POLYGON_VERTICES) return hull;
    hull.count = count;
    for (int i = 0; i < count; ++i) hull.points[i] = points[i];
    return hull;
}

b2Polygon b2MakePolygon(const b2Hull* hull, float radius) {
    b2Polygon polygon{};
    if (hull == nullptr) return polygon;
    polygon.count = hull->count;
    polygon.radius = radius;
    for (int i = 0; i < hull->count; ++i) polygon.vertices[i] = hull->points[i];
    return polygon;
}

b2Polygon b2MakeOffsetBox(float halfWidth, float halfHeight, b2Vec2 center, b2Rot rotation) {
    const b2Vec2 local[4] = {{-halfWidth, -halfHeight}, {halfWidth, -halfHeight},
                             {halfWidth, halfHeight}, {-halfWidth, halfHeight}};
    b2Polygon polygon{};
    polygon.count = 4;
    polygon.centroid = center;
    for (int i = 0; i < 4; ++i) {
        polygon.vertices[i] = {center.x + rotation.c * local[i].x - rotation.s * local[i].y,
                               center.y + rotation.s * local[i].x + rotation.c * local[i].y};
    }
    return polygon;
}

} // extern "C"
