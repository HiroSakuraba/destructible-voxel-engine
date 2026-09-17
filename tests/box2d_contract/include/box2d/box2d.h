#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define B2_MAX_POLYGON_VERTICES 8

typedef struct b2WorldId { uint16_t index1; uint16_t generation; } b2WorldId;
typedef struct b2BodyId { int32_t index1; uint16_t world0; uint16_t generation; } b2BodyId;
typedef struct b2ShapeId { int32_t index1; uint16_t world0; uint16_t generation; } b2ShapeId;
typedef struct b2JointId { int32_t index1; uint16_t world0; uint16_t generation; } b2JointId;

static const b2WorldId b2_nullWorldId = {0, 0};
static const b2BodyId b2_nullBodyId = {0, 0, 0};
static const b2ShapeId b2_nullShapeId = {0, 0, 0};
static const b2JointId b2_nullJointId = {0, 0, 0};
#define B2_IS_NULL(id) ((id).index1 == 0)
#define B2_IS_NON_NULL(id) ((id).index1 != 0)

static inline uint32_t b2StoreWorldId(b2WorldId id) {
    return ((uint32_t)id.index1 << 16) | (uint32_t)id.generation;
}
static inline uint64_t b2StoreBodyId(b2BodyId id) {
    return ((uint64_t)(uint32_t)id.index1 << 32) | ((uint64_t)id.world0 << 16) | (uint64_t)id.generation;
}
static inline uint64_t b2StoreShapeId(b2ShapeId id) {
    return ((uint64_t)(uint32_t)id.index1 << 32) | ((uint64_t)id.world0 << 16) | (uint64_t)id.generation;
}
static inline uint64_t b2StoreJointId(b2JointId id) {
    return ((uint64_t)(uint32_t)id.index1 << 32) | ((uint64_t)id.world0 << 16) | (uint64_t)id.generation;
}

typedef struct b2Vec2 { float x, y; } b2Vec2;
typedef struct b2Rot { float c, s; } b2Rot;
typedef struct b2Transform { b2Vec2 p; b2Rot q; } b2Transform;
typedef struct b2AABB { b2Vec2 lowerBound; b2Vec2 upperBound; } b2AABB;
static const b2Rot b2Rot_identity = {1.0f, 0.0f};

typedef enum b2BodyType {
    b2_staticBody = 0,
    b2_kinematicBody = 1,
    b2_dynamicBody = 2
} b2BodyType;

typedef struct b2WorldDef {
    b2Vec2 gravity;
    float restitutionThreshold;
    float hitEventThreshold;
    float contactHertz;
    float contactDampingRatio;
    float maxContactPushSpeed;
    float maximumLinearSpeed;
    void* frictionCallback;
    void* restitutionCallback;
    bool enableSleep;
    bool enableContinuous;
    int workerCount;
    void* enqueueTask;
    void* finishTask;
    void* userTaskContext;
    void* userData;
    int internalValue;
} b2WorldDef;

typedef struct b2BodyDef {
    b2BodyType type;
    b2Vec2 position;
    b2Rot rotation;
    b2Vec2 linearVelocity;
    float angularVelocity;
    float linearDamping;
    float angularDamping;
    float gravityScale;
    float sleepThreshold;
    const char* name;
    void* userData;
    bool enableSleep;
    bool isAwake;
    bool fixedRotation;
    bool isBullet;
    bool isEnabled;
    bool allowFastRotation;
    int internalValue;
} b2BodyDef;

typedef struct b2Filter {
    uint64_t categoryBits;
    uint64_t maskBits;
    int groupIndex;
} b2Filter;

typedef struct b2QueryFilter {
    uint64_t categoryBits;
    uint64_t maskBits;
} b2QueryFilter;

typedef struct b2SurfaceMaterial {
    float friction;
    float restitution;
    float rollingResistance;
    float tangentSpeed;
    int userMaterialId;
    uint32_t customColor;
} b2SurfaceMaterial;

typedef struct b2ShapeDef {
    void* userData;
    b2SurfaceMaterial material;
    float density;
    b2Filter filter;
    bool isSensor;
    bool enableSensorEvents;
    bool enableContactEvents;
    bool enableHitEvents;
    bool enablePreSolveEvents;
    bool invokeContactCreation;
    bool updateBodyMass;
    int internalValue;
} b2ShapeDef;

typedef struct b2Circle { b2Vec2 center; float radius; } b2Circle;
typedef struct b2Capsule { b2Vec2 center1; b2Vec2 center2; float radius; } b2Capsule;
typedef struct b2Polygon {
    b2Vec2 vertices[B2_MAX_POLYGON_VERTICES];
    b2Vec2 normals[B2_MAX_POLYGON_VERTICES];
    b2Vec2 centroid;
    float radius;
    int count;
} b2Polygon;
typedef struct b2Hull { b2Vec2 points[B2_MAX_POLYGON_VERTICES]; int count; } b2Hull;
typedef struct b2ShapeProxy { b2Vec2 points[B2_MAX_POLYGON_VERTICES]; int count; float radius; } b2ShapeProxy;
typedef struct b2TreeStats { int nodeVisits; int leafVisits; } b2TreeStats;
typedef bool b2OverlapResultFcn(b2ShapeId shapeId, void* context);
typedef float b2CastResultFcn(b2ShapeId shapeId, b2Vec2 point, b2Vec2 normal, float fraction, void* context);


typedef struct b2DistanceJointDef {
    b2BodyId bodyIdA, bodyIdB; b2Vec2 localAnchorA, localAnchorB; float length;
    bool enableSpring; float hertz, dampingRatio; bool enableLimit; float minLength, maxLength;
    bool enableMotor; float maxMotorForce, motorSpeed; bool collideConnected; void* userData; int internalValue;
} b2DistanceJointDef;
typedef struct b2MotorJointDef {
    b2BodyId bodyIdA, bodyIdB; b2Vec2 linearOffset; float angularOffset; float maxForce, maxTorque;
    float correctionFactor; bool collideConnected; void* userData; int internalValue;
} b2MotorJointDef;
typedef struct b2PrismaticJointDef {
    b2BodyId bodyIdA, bodyIdB; b2Vec2 localAnchorA, localAnchorB, localAxisA; float referenceAngle;
    float targetTranslation; bool enableSpring; float hertz, dampingRatio; bool enableLimit;
    float lowerTranslation, upperTranslation; bool enableMotor; float maxMotorForce, motorSpeed;
    bool collideConnected; void* userData; int internalValue;
} b2PrismaticJointDef;
typedef struct b2RevoluteJointDef {
    b2BodyId bodyIdA, bodyIdB; b2Vec2 localAnchorA, localAnchorB; float referenceAngle, targetAngle;
    bool enableSpring; float hertz, dampingRatio; bool enableLimit; float lowerAngle, upperAngle;
    bool enableMotor; float maxMotorTorque, motorSpeed, drawSize; bool collideConnected; void* userData; int internalValue;
} b2RevoluteJointDef;
typedef struct b2WeldJointDef {
    b2BodyId bodyIdA, bodyIdB; b2Vec2 localAnchorA, localAnchorB; float referenceAngle;
    float linearHertz, angularHertz, linearDampingRatio, angularDampingRatio;
    bool collideConnected; void* userData; int internalValue;
} b2WeldJointDef;
typedef struct b2WheelJointDef {
    b2BodyId bodyIdA, bodyIdB; b2Vec2 localAnchorA, localAnchorB, localAxisA;
    bool enableSpring; float hertz, dampingRatio; bool enableLimit; float lowerTranslation, upperTranslation;
    bool enableMotor; float maxMotorTorque, motorSpeed; bool collideConnected; void* userData; int internalValue;
} b2WheelJointDef;

typedef struct b2Manifold { b2Vec2 normal; } b2Manifold;
typedef bool b2PreSolveFcn(b2ShapeId shapeIdA, b2ShapeId shapeIdB, b2Manifold* manifold, void* context);

typedef struct b2ContactBeginTouchEvent {
    b2ShapeId shapeIdA;
    b2ShapeId shapeIdB;
    b2Manifold manifold;
} b2ContactBeginTouchEvent;
typedef struct b2ContactEndTouchEvent { b2ShapeId shapeIdA; b2ShapeId shapeIdB; } b2ContactEndTouchEvent;
typedef struct b2ContactHitEvent {
    b2ShapeId shapeIdA;
    b2ShapeId shapeIdB;
    b2Vec2 point;
    b2Vec2 normal;
    float approachSpeed;
} b2ContactHitEvent;
typedef struct b2ContactEvents {
    b2ContactBeginTouchEvent* beginEvents;
    b2ContactEndTouchEvent* endEvents;
    b2ContactHitEvent* hitEvents;
    int beginCount;
    int endCount;
    int hitCount;
} b2ContactEvents;

typedef struct b2SensorBeginTouchEvent { b2ShapeId sensorShapeId; b2ShapeId visitorShapeId; } b2SensorBeginTouchEvent;
typedef struct b2SensorEndTouchEvent { b2ShapeId sensorShapeId; b2ShapeId visitorShapeId; } b2SensorEndTouchEvent;
typedef struct b2SensorEvents {
    b2SensorBeginTouchEvent* beginEvents;
    b2SensorEndTouchEvent* endEvents;
    int beginCount;
    int endCount;
} b2SensorEvents;

typedef struct b2RayResult {
    b2ShapeId shapeId;
    b2Vec2 point;
    b2Vec2 normal;
    float fraction;
    int nodeVisits;
    int leafVisits;
    bool hit;
} b2RayResult;

b2WorldDef b2DefaultWorldDef(void);
b2BodyDef b2DefaultBodyDef(void);
b2ShapeDef b2DefaultShapeDef(void);
b2QueryFilter b2DefaultQueryFilter(void);
b2DistanceJointDef b2DefaultDistanceJointDef(void);
b2MotorJointDef b2DefaultMotorJointDef(void);
b2PrismaticJointDef b2DefaultPrismaticJointDef(void);
b2RevoluteJointDef b2DefaultRevoluteJointDef(void);
b2WeldJointDef b2DefaultWeldJointDef(void);
b2WheelJointDef b2DefaultWheelJointDef(void);
b2Rot b2MakeRot(float angle);

b2WorldId b2CreateWorld(const b2WorldDef* def);
void b2DestroyWorld(b2WorldId worldId);
void b2World_SetPreSolveCallback(b2WorldId worldId, b2PreSolveFcn* callback, void* context);
void b2World_Step(b2WorldId worldId, float timeStep, int subStepCount);
b2ContactEvents b2World_GetContactEvents(b2WorldId worldId);
b2SensorEvents b2World_GetSensorEvents(b2WorldId worldId);
b2RayResult b2World_CastRayClosest(b2WorldId worldId, b2Vec2 origin, b2Vec2 translation, b2QueryFilter filter);
b2TreeStats b2World_OverlapShape(b2WorldId worldId, const b2ShapeProxy* proxy, b2QueryFilter filter, b2OverlapResultFcn* fcn, void* context);
b2TreeStats b2World_CastShape(b2WorldId worldId, const b2ShapeProxy* proxy, b2Vec2 translation, b2QueryFilter filter, b2CastResultFcn* fcn, void* context);

b2BodyId b2CreateBody(b2WorldId worldId, const b2BodyDef* def);
void b2DestroyBody(b2BodyId bodyId);
bool b2Body_IsValid(b2BodyId bodyId);
b2Vec2 b2Body_GetPosition(b2BodyId bodyId);
b2Rot b2Body_GetRotation(b2BodyId bodyId);
b2Vec2 b2Body_GetLinearVelocity(b2BodyId bodyId);
float b2Body_GetAngularVelocity(b2BodyId bodyId);
void b2Body_SetTransform(b2BodyId bodyId, b2Vec2 position, b2Rot rotation);
void b2Body_SetLinearVelocity(b2BodyId bodyId, b2Vec2 velocity);
void b2Body_SetGravityScale(b2BodyId bodyId, float gravityScale);
void b2Body_ApplyLinearImpulseToCenter(b2BodyId bodyId, b2Vec2 impulse, bool wake);
bool b2Body_IsAwake(b2BodyId bodyId);
bool b2Body_IsEnabled(b2BodyId bodyId);
void b2Body_Enable(b2BodyId bodyId);
void b2Body_Disable(b2BodyId bodyId);
void* b2Body_GetUserData(b2BodyId bodyId);
b2AABB b2Body_ComputeAABB(b2BodyId bodyId);

b2ShapeId b2CreateCircleShape(b2BodyId bodyId, const b2ShapeDef* def, const b2Circle* circle);
b2ShapeId b2CreateCapsuleShape(b2BodyId bodyId, const b2ShapeDef* def, const b2Capsule* capsule);
b2ShapeId b2CreatePolygonShape(b2BodyId bodyId, const b2ShapeDef* def, const b2Polygon* polygon);
void b2DestroyShape(b2ShapeId shapeId, bool updateBodyMass);
bool b2Shape_IsValid(b2ShapeId shapeId);
b2BodyId b2Shape_GetBody(b2ShapeId shapeId);
b2Filter b2Shape_GetFilter(b2ShapeId shapeId);
b2AABB b2Shape_GetAABB(b2ShapeId shapeId);


b2JointId b2CreateDistanceJoint(b2WorldId worldId, const b2DistanceJointDef* def);
b2JointId b2CreateMotorJoint(b2WorldId worldId, const b2MotorJointDef* def);
b2JointId b2CreatePrismaticJoint(b2WorldId worldId, const b2PrismaticJointDef* def);
b2JointId b2CreateRevoluteJoint(b2WorldId worldId, const b2RevoluteJointDef* def);
b2JointId b2CreateWeldJoint(b2WorldId worldId, const b2WeldJointDef* def);
b2JointId b2CreateWheelJoint(b2WorldId worldId, const b2WheelJointDef* def);
void b2DestroyJoint(b2JointId jointId, bool wakeAttached);
bool b2Joint_IsValid(b2JointId jointId);
b2Vec2 b2Joint_GetConstraintForce(b2JointId jointId);
float b2Joint_GetConstraintTorque(b2JointId jointId);
float b2DistanceJoint_GetCurrentLength(b2JointId jointId);
float b2DistanceJoint_GetMotorSpeed(b2JointId jointId);
float b2DistanceJoint_GetMotorForce(b2JointId jointId);
float b2PrismaticJoint_GetTranslation(b2JointId jointId);
float b2PrismaticJoint_GetSpeed(b2JointId jointId);
float b2PrismaticJoint_GetMotorSpeed(b2JointId jointId);
float b2PrismaticJoint_GetMotorForce(b2JointId jointId);
float b2RevoluteJoint_GetAngle(b2JointId jointId);
float b2RevoluteJoint_GetMotorSpeed(b2JointId jointId);
float b2RevoluteJoint_GetMotorTorque(b2JointId jointId);
float b2WheelJoint_GetMotorSpeed(b2JointId jointId);
float b2WheelJoint_GetMotorTorque(b2JointId jointId);

b2Hull b2ComputeHull(const b2Vec2* points, int count);
b2Polygon b2MakePolygon(const b2Hull* hull, float radius);
b2Polygon b2MakeOffsetBox(float halfWidth, float halfHeight, b2Vec2 center, b2Rot rotation);

#ifdef __cplusplus
}
#endif
