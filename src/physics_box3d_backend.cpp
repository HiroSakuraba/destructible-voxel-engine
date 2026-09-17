#include "dve/physics_box3d_backend.hpp"

#include <box3d/box3d.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dve {
namespace {

constexpr std::uint64_t kStaticCategory = 1ULL << 0U;
constexpr std::uint64_t kFullCategory = 1ULL << 1U;
constexpr std::uint64_t kDebrisCategory = 1ULL << 2U;
constexpr std::uint32_t kMaximumBox3DWorkers = 32U;

[[nodiscard]] bool finite(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] b3Vec3 to_b3_vec3(Float3 value) noexcept {
    return {value.x, value.y, value.z};
}

[[nodiscard]] b3Pos to_b3_pos(Float3 value) noexcept {
    b3Pos result{};
    result.x = value.x;
    result.y = value.y;
    result.z = value.z;
    return result;
}

[[nodiscard]] Float3 from_b3(b3Vec3 value) noexcept {
    return {value.x, value.y, value.z};
}

[[nodiscard]] Float3 from_b3_pos(b3Pos value) noexcept {
    return {
        static_cast<float>(value.x),
        static_cast<float>(value.y),
        static_cast<float>(value.z)};
}

[[nodiscard]] b3Quat to_b3_quat(Quaternion value) noexcept {
    const Quaternion normalized = normalize(value);
    return {{normalized.x, normalized.y, normalized.z}, normalized.w};
}

[[nodiscard]] Quaternion from_b3(b3Quat value) noexcept {
    return normalize({value.v.x, value.v.y, value.v.z, value.s});
}

[[nodiscard]] b3Matrix3 to_b3_inertia(const SolverInertiaTensor& inertia) noexcept {
    return {
        {static_cast<float>(inertia.xx), static_cast<float>(inertia.xy), static_cast<float>(inertia.xz)},
        {static_cast<float>(inertia.xy), static_cast<float>(inertia.yy), static_cast<float>(inertia.yz)},
        {static_cast<float>(inertia.xz), static_cast<float>(inertia.yz), static_cast<float>(inertia.zz)}};
}

[[nodiscard]] Float3 cross(Float3 a, Float3 b) noexcept {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}

[[nodiscard]] Float3 stable_perpendicular(Float3 axis) noexcept {
    const Float3 n = normalize(axis);
    const Float3 helper = std::fabs(n.x) < 0.7F ? Float3{1.0F, 0.0F, 0.0F}
                                                 : Float3{0.0F, 1.0F, 0.0F};
    return normalize(cross(helper, n));
}

[[nodiscard]] Quaternion quaternion_from_basis(Float3 xAxis, Float3 yAxis, Float3 zAxis) noexcept {
    const float m00 = xAxis.x;
    const float m01 = yAxis.x;
    const float m02 = zAxis.x;
    const float m10 = xAxis.y;
    const float m11 = yAxis.y;
    const float m12 = zAxis.y;
    const float m20 = xAxis.z;
    const float m21 = yAxis.z;
    const float m22 = zAxis.z;

    Quaternion q;
    const float trace = m00 + m11 + m22;
    if (trace > 0.0F) {
        const float s = std::sqrt(trace + 1.0F) * 2.0F;
        q.w = 0.25F * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0F + m00 - m11 - m22) * 2.0F;
        q.w = (m21 - m12) / s;
        q.x = 0.25F * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0F + m11 - m00 - m22) * 2.0F;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25F * s;
        q.z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0F + m22 - m00 - m11) * 2.0F;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25F * s;
    }
    return normalize(q);
}

[[nodiscard]] Quaternion frame_rotation(Float3 zAxis, Float3 xHint) noexcept {
    const Float3 z = normalize(zAxis);
    Float3 x = normalize(xHint);
    if (length_squared(x) < 0.5F || std::fabs(dot(x, z)) > 0.99F) x = stable_perpendicular(z);
    x = normalize(subtract(x, multiply(z, dot(x, z))));
    const Float3 y = normalize(cross(z, x));
    return quaternion_from_basis(x, y, z);
}

[[nodiscard]] b3Filter collision_filter(RigidBodyCollisionClass collisionClass, bool isStatic) noexcept {
    b3Filter filter = b3DefaultFilter();
    if (isStatic) {
        filter.categoryBits = kStaticCategory;
        filter.maskBits = kFullCategory | kDebrisCategory;
    } else if (collisionClass == RigidBodyCollisionClass::DebrisNoSelf) {
        filter.categoryBits = kDebrisCategory;
        filter.maskBits = kStaticCategory | kFullCategory;
    } else {
        filter.categoryBits = kFullCategory;
        filter.maskBits = kStaticCategory | kFullCategory | kDebrisCategory;
    }
    return filter;
}

[[nodiscard]] int capacity_to_int(std::uint64_t value) noexcept {
    return static_cast<int>(std::min<std::uint64_t>(
        value, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
}

[[nodiscard]] void* encode_handle(RigidBodyHandle handle) noexcept {
    const std::uintptr_t value = static_cast<std::uintptr_t>(handle) + 1U;
    return reinterpret_cast<void*>(value);
}

[[nodiscard]] RigidBodyHandle decode_handle(void* value) noexcept {
    const std::uintptr_t encoded = reinterpret_cast<std::uintptr_t>(value);
    if (encoded == 0U || encoded - 1U > static_cast<std::uintptr_t>(kInvalidRigidBodyHandle - 1U)) {
        return kInvalidRigidBodyHandle;
    }
    return static_cast<RigidBodyHandle>(encoded - 1U);
}

[[nodiscard]] bool box_is_centered(const SolverBox& box) noexcept {
    return box.center.x == 0.0F && box.center.y == 0.0F && box.center.z == 0.0F;
}

[[nodiscard]] b3ShapeId attach_box_shape(
    b3BodyId body,
    const SolverBox& box,
    const b3ShapeDef& shapeDefinition) {
    const b3BoxHull hull = b3MakeBoxHull(
        box.halfExtents.x, box.halfExtents.y, box.halfExtents.z);
    if (box_is_centered(box)) {
        return b3CreateHullShape(body, &shapeDefinition, &hull.base);
    }
    b3Transform local = b3Transform_identity;
    local.p = to_b3_vec3(box.center);
    return b3CreateTransformedHullShape(
        body, &shapeDefinition, &hull.base, local, b3Vec3_one);
}

} // namespace

struct Box3DRigidBodyWorld::Impl {
    struct Slot {
        bool alive{};
        bool isStatic{};
        b3BodyId bodyId{};
        RigidTransform previousTransform{};
        RigidTransform currentTransform{};
        std::uint16_t material{};
        b3MeshData* meshData{};
    };

    struct ConstraintSlot {
        bool alive{};
        RigidBodyHandle parentBody{kInvalidRigidBodyHandle};
        RigidBodyHandle childBody{kInvalidRigidBodyHandle};
        b3JointId jointId{};
    };

    explicit Impl(const Box3DWorldConfig& requestedConfig) : config(requestedConfig) {
        config.workerThreads = std::clamp(config.workerThreads, 1U, kMaximumBox3DWorkers);
        config.subStepCount = std::max(config.subStepCount, 1U);
        config.expectedBodyCount = std::max(config.expectedBodyCount, 1U);
        config.expectedDynamicBodyCount = std::min(
            std::max(config.expectedDynamicBodyCount, 1U), config.expectedBodyCount);
        config.expectedContactCount = std::max(config.expectedContactCount, 1U);
        if (!(config.hitEventThresholdMetersPerSecond >= 0.0F) ||
            !std::isfinite(config.hitEventThresholdMetersPerSecond)) {
            config.hitEventThresholdMetersPerSecond = 0.25F;
        }

        const b3Version version = b3GetVersion();
        if (version.major != 0 || version.minor != 1) {
            throw std::runtime_error("DVE requires Box3D 0.1.x");
        }
        telemetry.versionMajor = static_cast<std::uint32_t>(version.major);
        telemetry.versionMinor = static_cast<std::uint32_t>(version.minor);
        telemetry.versionRevision = static_cast<std::uint32_t>(version.revision);
        telemetry.configuredWorkerThreads = config.workerThreads;
        telemetry.configuredSubStepCount = config.subStepCount;

        b3WorldDef definition = b3DefaultWorldDef();
        definition.gravity = {0.0F, -9.81F, 0.0F};
        definition.workerCount = config.workerThreads;
        definition.enableSleep = config.enableSleeping;
        definition.enableContinuous = config.enableContinuousCollision;
        definition.hitEventThreshold = config.hitEventThresholdMetersPerSecond;
        const std::uint32_t staticBodies = config.expectedBodyCount - config.expectedDynamicBodyCount;
        definition.capacity.staticBodyCount = capacity_to_int(staticBodies);
        definition.capacity.dynamicBodyCount = capacity_to_int(config.expectedDynamicBodyCount);
        definition.capacity.staticShapeCount = capacity_to_int(static_cast<std::uint64_t>(staticBodies) * 4U);
        definition.capacity.dynamicShapeCount = capacity_to_int(
            static_cast<std::uint64_t>(config.expectedDynamicBodyCount) * 4U);
        definition.capacity.contactCount = capacity_to_int(config.expectedContactCount);
        worldId = b3CreateWorld(&definition);
        if (B3_IS_NULL(worldId)) throw std::runtime_error("Box3D world creation failed");
    }

    ~Impl() {
        if (B3_IS_NON_NULL(worldId) && b3World_IsValid(worldId)) b3DestroyWorld(worldId);
        for (Slot& slot : slots) {
            if (slot.meshData != nullptr) {
                b3DestroyMesh(slot.meshData);
                slot.meshData = nullptr;
            }
        }
    }

    Box3DWorldConfig config{};
    b3WorldId worldId{};
    std::vector<Slot> slots{};
    std::vector<RigidBodyHandle> freeHandles{};
    std::vector<ConstraintSlot> constraints{};
    std::vector<RigidBodyConstraintHandle> freeConstraintHandles{};
    std::atomic<IPhysicsContactSink*> contactSink{};
    Box3DPhysicsTelemetry telemetry{};
    double simulatedSeconds{};

    [[nodiscard]] RigidBodyHandle allocate_handle() {
        if (!freeHandles.empty()) {
            const RigidBodyHandle handle = freeHandles.back();
            freeHandles.pop_back();
            return handle;
        }
        if (slots.size() >= static_cast<std::size_t>(kInvalidRigidBodyHandle)) {
            return kInvalidRigidBodyHandle;
        }
        slots.emplace_back();
        return static_cast<RigidBodyHandle>(slots.size() - 1U);
    }

    void release_handle(RigidBodyHandle handle) {
        if (handle >= slots.size()) return;
        slots[handle] = {};
        freeHandles.push_back(handle);
    }

    [[nodiscard]] bool valid(RigidBodyHandle handle) const noexcept {
        return handle != kInvalidRigidBodyHandle && handle < slots.size() && slots[handle].alive &&
            B3_IS_NON_NULL(slots[handle].bodyId) && b3Body_IsValid(slots[handle].bodyId);
    }

    [[nodiscard]] RigidBodyConstraintHandle allocate_constraint_handle() {
        if (!freeConstraintHandles.empty()) {
            const RigidBodyConstraintHandle handle = freeConstraintHandles.back();
            freeConstraintHandles.pop_back();
            return handle;
        }
        if (constraints.size() >= static_cast<std::size_t>(kInvalidRigidBodyConstraintHandle)) {
            return kInvalidRigidBodyConstraintHandle;
        }
        constraints.emplace_back();
        return static_cast<RigidBodyConstraintHandle>(constraints.size() - 1U);
    }

    [[nodiscard]] bool valid_constraint(RigidBodyConstraintHandle handle) const noexcept {
        return handle != kInvalidRigidBodyConstraintHandle && handle < constraints.size() &&
            constraints[handle].alive && B3_IS_NON_NULL(constraints[handle].jointId) &&
            b3Joint_IsValid(constraints[handle].jointId);
    }

    [[nodiscard]] RigidBodyHandle handle_for_body(b3BodyId bodyId) const noexcept {
        if (B3_IS_NULL(bodyId) || !b3Body_IsValid(bodyId)) return kInvalidRigidBodyHandle;
        const RigidBodyHandle handle = decode_handle(b3Body_GetUserData(bodyId));
        return valid(handle) && B3_ID_EQUALS(slots[handle].bodyId, bodyId)
            ? handle
            : kInvalidRigidBodyHandle;
    }

    [[nodiscard]] RigidTransform body_transform(b3BodyId bodyId) const noexcept {
        const b3WorldTransform transform = b3Body_GetTransform(bodyId);
        return make_rigid_transform(from_b3_pos(transform.p), from_b3(transform.q));
    }

    [[nodiscard]] b3ShapeDef shape_definition(
        RigidBodyCollisionClass collisionClass,
        bool isStatic,
        std::uint16_t material,
        bool updateMass) const noexcept {
        b3ShapeDef definition = b3DefaultShapeDef();
        definition.filter = collision_filter(collisionClass, isStatic);
        definition.updateBodyMass = updateMass;
        definition.enableHitEvents = !isStatic;
        definition.baseMaterial.userMaterialId = material;
        return definition;
    }

    void publish_hit_events() {
        const b3ContactEvents events = b3World_GetContactEvents(worldId);
        telemetry.contactHitEvents += static_cast<std::uint64_t>(std::max(events.hitCount, 0));
        IPhysicsContactSink* sink = contactSink.load(std::memory_order_acquire);
        if (sink == nullptr) return;

        for (int i = 0; i < events.hitCount; ++i) {
            const b3ContactHitEvent& hit = events.hitEvents[i];
            if (!b3Shape_IsValid(hit.shapeIdA) || !b3Shape_IsValid(hit.shapeIdB)) continue;
            const b3BodyId bodyA = b3Shape_GetBody(hit.shapeIdA);
            const b3BodyId bodyB = b3Shape_GetBody(hit.shapeIdB);
            const RigidBodyHandle handleA = handle_for_body(bodyA);
            const RigidBodyHandle handleB = handle_for_body(bodyB);
            if (handleA == kInvalidRigidBodyHandle || handleB == kInvalidRigidBodyHandle) continue;

            const b3Vec3 velocityA = b3Body_GetWorldPointVelocity(bodyA, hit.point);
            const b3Vec3 velocityB = b3Body_GetWorldPointVelocity(bodyB, hit.point);
            const float inverseMassSum = b3Body_GetInverseMass(bodyA) + b3Body_GetInverseMass(bodyB);
            const float effectiveMass = inverseMassSum > 0.0F ? 1.0F / inverseMassSum : 0.0F;

            PhysicsContactEvent event;
            event.bodyA = handleA;
            event.bodyB = handleB;
            event.materialA = static_cast<std::uint16_t>(std::min<std::uint64_t>(
                hit.userMaterialIdA, std::numeric_limits<std::uint16_t>::max()));
            event.materialB = static_cast<std::uint16_t>(std::min<std::uint64_t>(
                hit.userMaterialIdB, std::numeric_limits<std::uint16_t>::max()));
            event.position = from_b3_pos(hit.point);
            event.relativeVelocity = subtract(from_b3(velocityB), from_b3(velocityA));
            event.contactNormal = from_b3(hit.normal);
            event.normalImpulse = effectiveMass * hit.approachSpeed;
            event.effectiveMass = effectiveMass;
            event.persistent = false;
            event.timeSeconds = simulatedSeconds;
            if (sink->record_contact(event)) ++telemetry.contactEventsQueued;
            else ++telemetry.contactEventsRejected;
        }
    }
};

Box3DRigidBodyWorld::Box3DRigidBodyWorld() : Box3DRigidBodyWorld(Box3DWorldConfig{}) {}

Box3DRigidBodyWorld::Box3DRigidBodyWorld(const Box3DWorldConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

Box3DRigidBodyWorld::~Box3DRigidBodyWorld() = default;

RigidBodyHandle Box3DRigidBodyWorld::create_body(const RigidBodyCreateDesc& desc) {
    const RigidBodyValidationResult validation = validate_rigid_body_desc(desc);
    if (!validation) return kInvalidRigidBodyHandle;

    const RigidBodyHandle handle = impl_->allocate_handle();
    if (handle == kInvalidRigidBodyHandle) return handle;

    b3BodyDef bodyDefinition = b3DefaultBodyDef();
    bodyDefinition.type = b3_dynamicBody;
    bodyDefinition.position = to_b3_pos(validation.normalizedTransform.position);
    bodyDefinition.rotation = to_b3_quat(validation.normalizedTransform.rotation);
    bodyDefinition.linearVelocity = to_b3_vec3(desc.linearVelocity);
    bodyDefinition.angularVelocity = to_b3_vec3(desc.angularVelocity);
    bodyDefinition.enableSleep = desc.allowSleeping;
    bodyDefinition.isBullet = desc.useContinuousCollision;
    bodyDefinition.userData = encode_handle(handle);
    const b3BodyId bodyId = b3CreateBody(impl_->worldId, &bodyDefinition);
    if (B3_IS_NULL(bodyId)) {
        impl_->release_handle(handle);
        return kInvalidRigidBodyHandle;
    }

    const b3ShapeDef shapeDefinition = impl_->shape_definition(
        desc.collisionClass, false, 0U, false);
    for (const SolverBox& box : desc.boxes) {
        const b3ShapeId shapeId = attach_box_shape(bodyId, box, shapeDefinition);
        if (B3_IS_NULL(shapeId)) {
            b3DestroyBody(bodyId);
            impl_->release_handle(handle);
            return kInvalidRigidBodyHandle;
        }
    }

    b3MassData massData{};
    massData.mass = validation.massKilogramsFloat;
    massData.center = b3Vec3_zero;
    massData.inertia = to_b3_inertia(validation.inertiaAfterFloatConversion);
    b3Body_SetMassData(bodyId, massData);

    Impl::Slot& slot = impl_->slots[handle];
    slot.alive = true;
    slot.isStatic = false;
    slot.bodyId = bodyId;
    slot.previousTransform = validation.normalizedTransform;
    slot.currentTransform = validation.normalizedTransform;
    return handle;
}

std::vector<RigidBodyHandle> Box3DRigidBodyWorld::create_bodies(
    std::span<const RigidBodyCreateDesc> descs) {
    return IRigidBodyWorld::create_bodies(descs);
}

RigidBodyHandle Box3DRigidBodyWorld::create_static_body(
    const StaticRigidBodyCreateDesc& desc) {
    if (!validate_static_rigid_body_desc(desc)) return kInvalidRigidBodyHandle;
    const RigidTransform transform = make_rigid_transform(desc.transform.position, desc.transform.rotation);
    const RigidBodyHandle handle = impl_->allocate_handle();
    if (handle == kInvalidRigidBodyHandle) return handle;

    b3BodyDef bodyDefinition = b3DefaultBodyDef();
    bodyDefinition.type = b3_staticBody;
    bodyDefinition.position = to_b3_pos(transform.position);
    bodyDefinition.rotation = to_b3_quat(transform.rotation);
    bodyDefinition.userData = encode_handle(handle);
    const b3BodyId bodyId = b3CreateBody(impl_->worldId, &bodyDefinition);
    if (B3_IS_NULL(bodyId)) {
        impl_->release_handle(handle);
        return kInvalidRigidBodyHandle;
    }

    const b3ShapeDef shapeDefinition = impl_->shape_definition(
        desc.collisionClass, true, 0U, true);
    for (const SolverBox& box : desc.boxes) {
        const b3ShapeId shapeId = attach_box_shape(bodyId, box, shapeDefinition);
        if (B3_IS_NULL(shapeId)) {
            b3DestroyBody(bodyId);
            impl_->release_handle(handle);
            return kInvalidRigidBodyHandle;
        }
    }

    Impl::Slot& slot = impl_->slots[handle];
    slot.alive = true;
    slot.isStatic = true;
    slot.bodyId = bodyId;
    slot.previousTransform = transform;
    slot.currentTransform = transform;
    return handle;
}

std::vector<RigidBodyHandle> Box3DRigidBodyWorld::create_static_bodies(
    std::span<const StaticRigidBodyCreateDesc> descs) {
    return IRigidBodyWorld::create_static_bodies(descs);
}

bool Box3DRigidBodyWorld::destroy_body(RigidBodyHandle handle) {
    if (!impl_->valid(handle)) return false;

    std::vector<RigidBodyConstraintHandle> attached;
    for (RigidBodyConstraintHandle constraintHandle = 0U;
         constraintHandle < impl_->constraints.size(); ++constraintHandle) {
        const Impl::ConstraintSlot& constraint = impl_->constraints[constraintHandle];
        if (constraint.alive &&
            (constraint.parentBody == handle || constraint.childBody == handle)) {
            attached.push_back(constraintHandle);
        }
    }
    for (const RigidBodyConstraintHandle constraintHandle : attached) {
        (void)destroy_constraint(constraintHandle);
    }

    IPhysicsContactSink* sink = impl_->contactSink.load(std::memory_order_acquire);
    if (sink != nullptr) sink->body_removed(handle);
    Impl::Slot& slot = impl_->slots[handle];
    b3DestroyBody(slot.bodyId);
    if (slot.meshData != nullptr) {
        b3DestroyMesh(slot.meshData);
        slot.meshData = nullptr;
    }
    impl_->release_handle(handle);
    return true;
}

std::optional<RigidBodyState> Box3DRigidBodyWorld::state(RigidBodyHandle handle) const {
    if (!impl_->valid(handle)) return std::nullopt;
    const Impl::Slot& slot = impl_->slots[handle];
    RigidBodyState result;
    result.previousTransform = slot.previousTransform;
    result.currentTransform = slot.currentTransform;
    if (slot.isStatic) {
        result.sleeping = true;
    } else {
        result.linearVelocity = from_b3(b3Body_GetLinearVelocity(slot.bodyId));
        result.angularVelocity = from_b3(b3Body_GetAngularVelocity(slot.bodyId));
        result.sleeping = !b3Body_IsAwake(slot.bodyId);
    }
    return result;
}

bool Box3DRigidBodyWorld::set_state(
    RigidBodyHandle handle, const RigidBodyState& stateValue) {
    if (!impl_->valid(handle) || !rigid_body_state_is_finite(stateValue)) return false;
    Impl::Slot& slot = impl_->slots[handle];
    const RigidTransform previous = make_rigid_transform(
        stateValue.previousTransform.position, stateValue.previousTransform.rotation);
    const RigidTransform current = make_rigid_transform(
        stateValue.currentTransform.position, stateValue.currentTransform.rotation);
    b3Body_SetTransform(slot.bodyId, to_b3_pos(current.position), to_b3_quat(current.rotation));
    if (!slot.isStatic) {
        b3Body_SetLinearVelocity(slot.bodyId, to_b3_vec3(stateValue.linearVelocity));
        b3Body_SetAngularVelocity(slot.bodyId, to_b3_vec3(stateValue.angularVelocity));
        b3Body_SetAwake(slot.bodyId, !stateValue.sleeping);
    }
    slot.previousTransform = previous;
    slot.currentTransform = current;
    return true;
}

bool Box3DRigidBodyWorld::apply_impulse(RigidBodyHandle handle, Float3 worldImpulse) {
    if (!impl_->valid(handle) || impl_->slots[handle].isStatic || !finite(worldImpulse)) return false;
    b3Body_ApplyLinearImpulseToCenter(
        impl_->slots[handle].bodyId, to_b3_vec3(worldImpulse), true);
    return true;
}

bool Box3DRigidBodyWorld::apply_force(RigidBodyHandle handle, Float3 worldForce) {
    if (!impl_->valid(handle) || impl_->slots[handle].isStatic || !finite(worldForce)) return false;
    b3Body_ApplyForceToCenter(impl_->slots[handle].bodyId, to_b3_vec3(worldForce), true);
    return true;
}

RigidBodyConstraintHandle Box3DRigidBodyWorld::create_constraint(
    const RigidBodyConstraintDesc& desc) {
    if (!validate_rigid_body_constraint_desc(desc) || !impl_->valid(desc.parentBody) ||
        !impl_->valid(desc.childBody) || impl_->slots[desc.parentBody].isStatic ||
        impl_->slots[desc.childBody].isStatic) {
        return kInvalidRigidBodyConstraintHandle;
    }

    const RigidBodyConstraintHandle handle = impl_->allocate_constraint_handle();
    if (handle == kInvalidRigidBodyConstraintHandle) return handle;

    const Float3 parentAxis = normalize(desc.parentAxisLocal);
    const Quaternion reference = normalize(desc.referenceRotation);
    const Float3 childAxis = rotate(conjugate(reference), parentAxis);
    const Float3 parentNormal = stable_perpendicular(parentAxis);
    const Float3 childNormal = rotate(conjugate(reference), parentNormal);
    const Quaternion parentFrameRotation = frame_rotation(parentAxis, parentNormal);
    const Quaternion childFrameRotation = frame_rotation(childAxis, childNormal);

    b3Transform frameA = b3Transform_identity;
    frameA.p = to_b3_vec3(desc.parentAnchorLocal);
    frameA.q = to_b3_quat(parentFrameRotation);
    b3Transform frameB = b3Transform_identity;
    frameB.p = to_b3_vec3(desc.childAnchorLocal);
    frameB.q = to_b3_quat(childFrameRotation);

    const b3BodyId parentId = impl_->slots[desc.parentBody].bodyId;
    const b3BodyId childId = impl_->slots[desc.childBody].bodyId;
    b3JointId jointId = b3_nullJointId;

    switch (desc.kind) {
    case RigidBodyConstraintKind::Ball: {
        b3SphericalJointDef definition = b3DefaultSphericalJointDef();
        definition.base.bodyIdA = parentId;
        definition.base.bodyIdB = childId;
        definition.base.localFrameA = frameA;
        definition.base.localFrameB = frameB;
        definition.base.userData = encode_handle(handle);
        jointId = b3CreateSphericalJoint(impl_->worldId, &definition);
        break;
    }
    case RigidBodyConstraintKind::Hinge: {
        b3RevoluteJointDef definition = b3DefaultRevoluteJointDef();
        definition.base.bodyIdA = parentId;
        definition.base.bodyIdB = childId;
        definition.base.localFrameA = frameA;
        definition.base.localFrameB = frameB;
        definition.base.userData = encode_handle(handle);
        definition.enableLimit = true;
        definition.lowerAngle = desc.minimumRadians;
        definition.upperAngle = desc.maximumRadians;
        jointId = b3CreateRevoluteJoint(impl_->worldId, &definition);
        break;
    }
    case RigidBodyConstraintKind::ConeTwist: {
        b3SphericalJointDef definition = b3DefaultSphericalJointDef();
        definition.base.bodyIdA = parentId;
        definition.base.bodyIdB = childId;
        definition.base.localFrameA = frameA;
        definition.base.localFrameB = frameB;
        definition.base.userData = encode_handle(handle);
        definition.enableConeLimit = true;
        definition.coneAngle = desc.swingLimitRadians;
        definition.enableTwistLimit = true;
        definition.lowerTwistAngle = desc.minimumRadians;
        definition.upperTwistAngle = desc.maximumRadians;
        jointId = b3CreateSphericalJoint(impl_->worldId, &definition);
        break;
    }
    case RigidBodyConstraintKind::Fixed: {
        b3WeldJointDef definition = b3DefaultWeldJointDef();
        definition.base.bodyIdA = parentId;
        definition.base.bodyIdB = childId;
        definition.base.localFrameA = frameA;
        definition.base.localFrameB = frameB;
        definition.base.userData = encode_handle(handle);
        definition.linearHertz = 0.0F;
        definition.angularHertz = 0.0F;
        definition.linearDampingRatio = 1.0F;
        definition.angularDampingRatio = 1.0F;
        jointId = b3CreateWeldJoint(impl_->worldId, &definition);
        break;
    }
    }

    if (B3_IS_NULL(jointId)) {
        impl_->freeConstraintHandles.push_back(handle);
        return kInvalidRigidBodyConstraintHandle;
    }
    Impl::ConstraintSlot& slot = impl_->constraints[handle];
    slot.alive = true;
    slot.parentBody = desc.parentBody;
    slot.childBody = desc.childBody;
    slot.jointId = jointId;
    return handle;
}

bool Box3DRigidBodyWorld::destroy_constraint(RigidBodyConstraintHandle handle) {
    if (!impl_->valid_constraint(handle)) return false;
    b3DestroyJoint(impl_->constraints[handle].jointId, true);
    impl_->constraints[handle] = {};
    impl_->freeConstraintHandles.push_back(handle);
    return true;
}

void Box3DRigidBodyWorld::step(float fixedDeltaSeconds) {
    if (!(fixedDeltaSeconds > 0.0F) || !std::isfinite(fixedDeltaSeconds)) {
        ++impl_->telemetry.invalidStepInputs;
        return;
    }
    for (Impl::Slot& slot : impl_->slots) {
        if (!slot.alive || slot.isStatic) continue;
        slot.previousTransform = slot.currentTransform;
    }
    b3World_Step(
        impl_->worldId, fixedDeltaSeconds, static_cast<int>(impl_->config.subStepCount));
    impl_->simulatedSeconds += fixedDeltaSeconds;
    ++impl_->telemetry.stepCalls;
    for (Impl::Slot& slot : impl_->slots) {
        if (!slot.alive || slot.isStatic) continue;
        slot.currentTransform = impl_->body_transform(slot.bodyId);
    }
    impl_->publish_hit_events();
}

bool Box3DRigidBodyWorld::teleport_body(
    RigidBodyHandle handle,
    const RigidTransform& transform,
    Float3 linearVelocity,
    Float3 angularVelocity) {
    RigidBodyState target;
    target.previousTransform = transform;
    target.currentTransform = transform;
    target.linearVelocity = linearVelocity;
    target.angularVelocity = angularVelocity;
    target.sleeping = false;
    return set_state(handle, target);
}

void Box3DRigidBodyWorld::set_gravity(Float3 acceleration) noexcept {
    if (finite(acceleration)) b3World_SetGravity(impl_->worldId, to_b3_vec3(acceleration));
}

bool Box3DRigidBodyWorld::set_damping(
    RigidBodyHandle handle, float linearDamping, float angularDamping) {
    if (!impl_->valid(handle) || impl_->slots[handle].isStatic ||
        !(linearDamping >= 0.0F) || !(angularDamping >= 0.0F) ||
        !std::isfinite(linearDamping) || !std::isfinite(angularDamping)) {
        return false;
    }
    b3Body_SetLinearDamping(impl_->slots[handle].bodyId, linearDamping);
    b3Body_SetAngularDamping(impl_->slots[handle].bodyId, angularDamping);
    return true;
}

bool Box3DRigidBodyWorld::apply_impulse_at_point(
    RigidBodyHandle handle, Float3 impulse, Float3 worldPoint) {
    if (!impl_->valid(handle) || impl_->slots[handle].isStatic ||
        !finite(impulse) || !finite(worldPoint)) {
        return false;
    }
    b3Body_ApplyLinearImpulse(
        impl_->slots[handle].bodyId,
        to_b3_vec3(impulse),
        to_b3_pos(worldPoint),
        true);
    return true;
}

bool Box3DRigidBodyWorld::apply_force_at_point(
    RigidBodyHandle handle, Float3 force, Float3 worldPoint) {
    if (!impl_->valid(handle) || impl_->slots[handle].isStatic ||
        !finite(force) || !finite(worldPoint)) {
        return false;
    }
    b3Body_ApplyForce(
        impl_->slots[handle].bodyId,
        to_b3_vec3(force),
        to_b3_pos(worldPoint),
        true);
    return true;
}

void Box3DRigidBodyWorld::set_contact_sink(IPhysicsContactSink* sink) noexcept {
    impl_->contactSink.store(sink, std::memory_order_release);
}

bool Box3DRigidBodyWorld::set_contact_material(
    RigidBodyHandle handle, std::uint16_t material) noexcept {
    if (!impl_->valid(handle)) return false;
    const b3BodyId bodyId = impl_->slots[handle].bodyId;
    const int count = b3Body_GetShapeCount(bodyId);
    if (count < 0) return false;
    std::vector<b3ShapeId> shapes(static_cast<std::size_t>(count));
    const int written = count > 0 ? b3Body_GetShapes(bodyId, shapes.data(), count) : 0;
    if (written != count) return false;
    for (const b3ShapeId shapeId : shapes) {
        b3SurfaceMaterial surface = b3Shape_GetSurfaceMaterial(shapeId);
        surface.userMaterialId = material;
        b3Shape_SetSurfaceMaterial(shapeId, surface);
    }
    impl_->slots[handle].material = material;
    return true;
}

RigidBodyHandle Box3DRigidBodyWorld::create_static_box(
    const RigidTransform& transform, Float3 halfExtents) {
    StaticRigidBodyCreateDesc desc;
    desc.transform = transform;
    desc.boxes.push_back({{}, halfExtents});
    return create_static_body(desc);
}

RigidBodyHandle Box3DRigidBodyWorld::create_static_triangle_mesh(
    const RigidTransform& requestedTransform,
    std::span<const Float3> vertices,
    std::span<const std::uint32_t> triangleIndices,
    Float3 scale,
    std::uint16_t material) {
    if (vertices.size() < 3U || triangleIndices.empty() || triangleIndices.size() % 3U != 0U ||
        vertices.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        triangleIndices.size() / 3U > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        !finite(requestedTransform.position) || !finite(scale) ||
        !(scale.x > 0.0F) || !(scale.y > 0.0F) || !(scale.z > 0.0F)) {
        return kInvalidRigidBodyHandle;
    }
    for (const Float3 vertex : vertices) {
        if (!finite(vertex)) return kInvalidRigidBodyHandle;
    }

    std::vector<b3Vec3> meshVertices;
    meshVertices.reserve(vertices.size());
    for (const Float3 vertex : vertices) meshVertices.push_back(to_b3_vec3(vertex));
    std::vector<std::int32_t> meshIndices;
    meshIndices.reserve(triangleIndices.size());
    for (const std::uint32_t index : triangleIndices) {
        if (index >= vertices.size() || index > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
            return kInvalidRigidBodyHandle;
        }
        meshIndices.push_back(static_cast<std::int32_t>(index));
    }

    b3MeshDef meshDefinition{};
    meshDefinition.vertices = meshVertices.data();
    meshDefinition.indices = meshIndices.data();
    meshDefinition.vertexCount = static_cast<int>(meshVertices.size());
    meshDefinition.triangleCount = static_cast<int>(meshIndices.size() / 3U);
    meshDefinition.weldTolerance = 1.0e-5F;
    meshDefinition.weldVertices = true;
    meshDefinition.identifyEdges = true;
    b3MeshData* meshData = b3CreateMesh(&meshDefinition, nullptr, 0);
    if (meshData == nullptr) return kInvalidRigidBodyHandle;

    const RigidBodyHandle handle = impl_->allocate_handle();
    if (handle == kInvalidRigidBodyHandle) {
        b3DestroyMesh(meshData);
        return handle;
    }
    const RigidTransform transform = make_rigid_transform(
        requestedTransform.position, requestedTransform.rotation);
    b3BodyDef bodyDefinition = b3DefaultBodyDef();
    bodyDefinition.type = b3_staticBody;
    bodyDefinition.position = to_b3_pos(transform.position);
    bodyDefinition.rotation = to_b3_quat(transform.rotation);
    bodyDefinition.userData = encode_handle(handle);
    const b3BodyId bodyId = b3CreateBody(impl_->worldId, &bodyDefinition);
    if (B3_IS_NULL(bodyId)) {
        b3DestroyMesh(meshData);
        impl_->release_handle(handle);
        return kInvalidRigidBodyHandle;
    }
    b3ShapeDef shapeDefinition = impl_->shape_definition(
        RigidBodyCollisionClass::Full, true, material, true);
    const b3ShapeId shapeId = b3CreateMeshShape(
        bodyId, &shapeDefinition, meshData, to_b3_vec3(scale));
    if (B3_IS_NULL(shapeId)) {
        b3DestroyBody(bodyId);
        b3DestroyMesh(meshData);
        impl_->release_handle(handle);
        return kInvalidRigidBodyHandle;
    }

    Impl::Slot& slot = impl_->slots[handle];
    slot.alive = true;
    slot.isStatic = true;
    slot.bodyId = bodyId;
    slot.previousTransform = transform;
    slot.currentTransform = transform;
    slot.material = material;
    slot.meshData = meshData;
    return handle;
}

std::optional<Box3DRayHit> Box3DRigidBodyWorld::ray_cast_closest(
    Float3 origin, Float3 translation, std::uint64_t maskBits) const {
    if (!finite(origin) || !finite(translation) || length_squared(translation) <= 0.0F) {
        return std::nullopt;
    }
    b3QueryFilter filter = b3DefaultQueryFilter();
    filter.maskBits = maskBits;
    const b3RayResult result = b3World_CastRayClosest(
        impl_->worldId, to_b3_pos(origin), to_b3_vec3(translation), filter);
    if (!result.hit || !b3Shape_IsValid(result.shapeId)) return std::nullopt;
    const RigidBodyHandle body = impl_->handle_for_body(b3Shape_GetBody(result.shapeId));
    if (body == kInvalidRigidBodyHandle) return std::nullopt;
    Box3DRayHit hit;
    hit.body = body;
    hit.point = from_b3_pos(result.point);
    hit.normal = from_b3(result.normal);
    hit.fraction = result.fraction;
    hit.material = static_cast<std::uint16_t>(std::min<std::uint64_t>(
        result.userMaterialId, std::numeric_limits<std::uint16_t>::max()));
    hit.triangleIndex = result.triangleIndex;
    hit.childIndex = result.childIndex;
    return hit;
}

std::size_t Box3DRigidBodyWorld::query_aabb(
    const RigidBodyWorldBounds& bounds,
    std::span<Box3DOverlapHit> output,
    std::uint64_t maskBits) const {
    if (!finite(bounds.minimum) || !finite(bounds.maximum) ||
        bounds.minimum.x > bounds.maximum.x || bounds.minimum.y > bounds.maximum.y ||
        bounds.minimum.z > bounds.maximum.z || output.empty()) {
        return 0U;
    }
    struct Context {
        const Impl* impl{};
        std::span<Box3DOverlapHit> output{};
        std::size_t count{};
    } context{impl_.get(), output, 0U};
    const auto callback = [](b3ShapeId shapeId, void* rawContext) -> bool {
        auto& query = *static_cast<Context*>(rawContext);
        if (!b3Shape_IsValid(shapeId)) return true;
        const RigidBodyHandle body = query.impl->handle_for_body(b3Shape_GetBody(shapeId));
        if (body == kInvalidRigidBodyHandle) return true;
        for (std::size_t i = 0; i < query.count; ++i) {
            if (query.output[i].body == body) return true;
        }
        if (query.count >= query.output.size()) return false;
        const b3SurfaceMaterial surface = b3Shape_GetSurfaceMaterial(shapeId);
        query.output[query.count++] = {
            body, static_cast<std::uint16_t>(std::min<std::uint64_t>(
                surface.userMaterialId, std::numeric_limits<std::uint16_t>::max()))};
        return query.count < query.output.size();
    };
    b3AABB box{};
    box.lowerBound = to_b3_vec3(bounds.minimum);
    box.upperBound = to_b3_vec3(bounds.maximum);
    b3QueryFilter filter = b3DefaultQueryFilter();
    filter.maskBits = maskBits;
    (void)b3World_OverlapAABB(impl_->worldId, box, filter, callback, &context);
    return context.count;
}

std::optional<bool> Box3DRigidBodyWorld::is_active(RigidBodyHandle handle) const {
    if (!impl_->valid(handle)) return std::nullopt;
    if (impl_->slots[handle].isStatic) return false;
    return b3Body_IsAwake(impl_->slots[handle].bodyId);
}

RigidTransform Box3DRigidBodyWorld::interpolated_transform(
    RigidBodyHandle handle, float alpha) const {
    if (!impl_->valid(handle)) return {};
    const Impl::Slot& slot = impl_->slots[handle];
    return interpolate_rigid_transform(slot.previousTransform, slot.currentTransform, alpha);
}

RigidBodyCounts Box3DRigidBodyWorld::body_counts() const {
    RigidBodyCounts counts;
    for (const Impl::Slot& slot : impl_->slots) {
        if (!slot.alive) continue;
        ++counts.total;
        if (slot.isStatic) {
            ++counts.staticBodies;
        } else {
            ++counts.dynamicBodies;
            if (b3Body_IsAwake(slot.bodyId)) ++counts.awakeDynamicBodies;
            else ++counts.sleepingDynamicBodies;
        }
    }
    return counts;
}

std::size_t Box3DRigidBodyWorld::body_count() const { return body_counts().total; }
std::size_t Box3DRigidBodyWorld::dynamic_body_count() const { return body_counts().dynamicBodies; }
std::size_t Box3DRigidBodyWorld::awake_body_count() const { return body_counts().awakeDynamicBodies; }
std::size_t Box3DRigidBodyWorld::sleeping_body_count() const { return body_counts().sleepingDynamicBodies; }

std::size_t Box3DRigidBodyWorld::constraint_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        impl_->constraints.begin(), impl_->constraints.end(),
        [](const Impl::ConstraintSlot& slot) { return slot.alive; }));
}

Box3DPhysicsTelemetry Box3DRigidBodyWorld::telemetry() const {
    Box3DPhysicsTelemetry result = impl_->telemetry;
    result.box3dAllocatedBytes = static_cast<std::size_t>(std::max(b3GetByteCount(), 0));
    result.bodyCounts = body_counts();
    result.constraintCount = constraint_count();
    return result;
}

std::optional<Float3> Box3DRigidBodyWorld::solver_shape_center_of_mass_local(
    RigidBodyHandle handle) const {
    if (!impl_->valid(handle)) return std::nullopt;
    return from_b3(b3Body_GetLocalCenter(impl_->slots[handle].bodyId));
}

std::optional<RigidBodyWorldBounds> Box3DRigidBodyWorld::solver_world_bounds(
    RigidBodyHandle handle) const {
    if (!impl_->valid(handle)) return std::nullopt;
    const b3AABB bounds = b3Body_ComputeAABB(impl_->slots[handle].bodyId);
    return RigidBodyWorldBounds{from_b3(bounds.lowerBound), from_b3(bounds.upperBound)};
}

} // namespace dve
