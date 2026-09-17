#include "dve/ragdoll_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace dve {
namespace {

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool finite(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

RigidBodyConstraintKind constraint_kind(RagdollJointKind kind) noexcept {
    switch (kind) {
        case RagdollJointKind::Ball: return RigidBodyConstraintKind::Ball;
        case RagdollJointKind::Hinge: return RigidBodyConstraintKind::Hinge;
        case RagdollJointKind::ConeTwist: return RigidBodyConstraintKind::ConeTwist;
        case RagdollJointKind::Fixed: return RigidBodyConstraintKind::Fixed;
    }
    return RigidBodyConstraintKind::ConeTwist;
}

RigidBodyCreateDesc body_desc(
    const RagdollBodyDefinition& body, const RigidTransform& world,
    const RagdollActivationOptions& options) {
    const double mass = static_cast<double>(body.massKilograms);
    const double x = static_cast<double>(body.halfExtents.x);
    const double y = static_cast<double>(body.halfExtents.y);
    const double z = static_cast<double>(body.halfExtents.z);
    RigidBodyCreateDesc desc;
    desc.transform = world;
    desc.massKilograms = mass;
    desc.inertiaKilogramMetersSquared = {
        mass * (y * y + z * z) / 3.0,
        mass * (x * x + z * z) / 3.0,
        mass * (x * x + y * y) / 3.0,
        0.0, 0.0, 0.0};
    desc.boxes.push_back({{}, body.halfExtents});
    desc.linearVelocity = options.linearVelocity;
    desc.angularVelocity = options.angularVelocity;
    desc.allowSleeping = true;
    desc.useContinuousCollision = options.useContinuousCollision;
    return desc;
}

float speed(Float3 value) noexcept { return std::sqrt(length_squared(value)); }

RigidTransform inverse_rigid(const RigidTransform& value) noexcept {
    const Quaternion rotation = conjugate(normalize(value.rotation));
    return make_rigid_transform(rotate(rotation, multiply(value.position, -1.0F)), rotation);
}

} // namespace

RagdollRuntime::RagdollRuntime(
    SkeletalAnimationRuntime& animation, IRigidBodyWorld& physics) noexcept
    : animation_(&animation), physics_(&physics) {}

RagdollRuntime::~RagdollRuntime() {
    for (auto& [id, instance] : instances_) {
        (void)id;
        destroy_physics(instance);
    }
}

void RagdollRuntime::destroy_physics(Instance& instance) noexcept {
    for (auto it = instance.constraints.rbegin(); it != instance.constraints.rend(); ++it)
        (void)physics_->destroy_constraint(*it);
    instance.constraints.clear();
    for (auto it = instance.bodies.rbegin(); it != instance.bodies.rend(); ++it)
        (void)physics_->destroy_body(*it);
    instance.bodies.clear();
}

bool RagdollRuntime::bind(
    std::uint64_t objectId, RagdollDefinition definition,
    RagdollRuntimeConfig config, std::string* error) {
    const SkeletonAsset* skeleton = animation_->skeleton(objectId);
    if (!skeleton) return fail(error, "ragdoll object has no bound skeleton");
    const AnimationValidationResult validation = validate_ragdoll(*skeleton, definition);
    if (!validation) return fail(error, validation.message);
    if (!std::isfinite(config.blendInSeconds) || config.blendInSeconds < 0.0F ||
        !std::isfinite(config.settleLinearSpeed) || config.settleLinearSpeed < 0.0F ||
        !std::isfinite(config.settleAngularSpeed) || config.settleAngularSpeed < 0.0F ||
        !std::isfinite(config.settleSeconds) || config.settleSeconds < 0.0F)
        return fail(error, "ragdoll runtime configuration is invalid");
    if (auto existing = instances_.find(objectId); existing != instances_.end())
        destroy_physics(existing->second);
    Instance instance;
    instance.definition = std::move(definition);
    instance.config = config;
    instance.blend.blendRatePerSecond = config.blendInSeconds > 0.0F
        ? 1.0F / config.blendInSeconds : 0.0F;
    instances_.insert_or_assign(objectId, std::move(instance));
    return true;
}

bool RagdollRuntime::unbind(std::uint64_t objectId) noexcept {
    const auto found = instances_.find(objectId);
    if (found == instances_.end()) return false;
    destroy_physics(found->second);
    instances_.erase(found);
    return true;
}

bool RagdollRuntime::activate(
    std::uint64_t objectId, const RigidTransform& objectWorld,
    RagdollActivationOptions options, std::string* error) {
    auto found = instances_.find(objectId);
    if (found == instances_.end()) return fail(error, "ragdoll instance is not bound");
    Instance& instance = found->second;
    if (instance.state != RagdollRuntimeState::Animated)
        return fail(error, "ragdoll instance is already active");
    if (!finite(options.linearVelocity) || !finite(options.angularVelocity) || !finite(options.impulse) ||
        options.impulseBody >= instance.definition.bodies.size())
        return fail(error, "ragdoll activation options are invalid");
    const SkeletonAsset* skeleton = animation_->skeleton(objectId);
    const LocalPose* pose = animation_->local_pose(objectId);
    if (!skeleton || !pose) return fail(error, "ragdoll animation pose is unavailable");
    std::string poseError;
    const auto model = compute_model_pose(*skeleton, *pose, &poseError);
    if (model.empty()) return fail(error, poseError);
    std::vector<RigidTransform> initialBodyWorld;
    std::vector<RigidBodyCreateDesc> descs;
    initialBodyWorld.reserve(instance.definition.bodies.size());
    descs.reserve(instance.definition.bodies.size());
    for (const RagdollBodyDefinition& body : instance.definition.bodies) {
        const RigidTransform boneWorld = compose_rigid_transforms(objectWorld, model[body.bone]);
        const RigidTransform world = compose_rigid_transforms(boneWorld, body.bodyFromBone);
        initialBodyWorld.push_back(world);
        descs.push_back(body_desc(body, world, options));
    }
    instance.bodies = physics_->create_bodies(descs);
    if (instance.bodies.size() != descs.size()) {
        instance.bodies.clear();
        return fail(error, "physics backend rejected ragdoll body creation");
    }
    for (const RagdollJointDefinition& joint : instance.definition.joints) {
        const RagdollBodyDefinition& childDefinition = instance.definition.bodies[joint.childBody];
        const Float3 anchorWorld = compose_rigid_transforms(
            objectWorld, model[childDefinition.bone]).position;
        RigidBodyConstraintDesc desc;
        desc.parentBody = instance.bodies[joint.parentBody];
        desc.childBody = instance.bodies[joint.childBody];
        desc.kind = constraint_kind(joint.kind);
        desc.parentAnchorLocal = inverse_transform_point(initialBodyWorld[joint.parentBody], anchorWorld);
        desc.childAnchorLocal = inverse_transform_point(initialBodyWorld[joint.childBody], anchorWorld);
        desc.parentAxisLocal = normalize(joint.axis);
        desc.referenceRotation = normalize(multiply(
            conjugate(initialBodyWorld[joint.parentBody].rotation),
            initialBodyWorld[joint.childBody].rotation));
        desc.minimumRadians = joint.minimumRadians;
        desc.maximumRadians = joint.maximumRadians;
        desc.swingLimitRadians = joint.swingLimitRadians;
        const RigidBodyConstraintHandle handle = physics_->create_constraint(desc);
        if (handle == kInvalidRigidBodyConstraintHandle) {
            destroy_physics(instance);
            return fail(error, "physics backend rejected or does not support ragdoll constraints");
        }
        instance.constraints.push_back(handle);
    }
    if (length_squared(options.impulse) > 0.0F &&
        !physics_->apply_impulse(instance.bodies[options.impulseBody], options.impulse)) {
        destroy_physics(instance);
        return fail(error, "physics backend rejected the ragdoll activation impulse");
    }
    instance.blend.weight = instance.config.blendInSeconds == 0.0F ? 1.0F : 0.0F;
    instance.blend.targetWeight = 1.0F;
    instance.state = instance.blend.weight == 1.0F
        ? RagdollRuntimeState::Simulating : RagdollRuntimeState::BlendingIn;
    instance.quietSeconds = 0.0F;
    instance.settled = false;
    instance.facing.reset();
    return true;
}

bool RagdollRuntime::gather_body_world(
    const Instance& instance, std::vector<RigidTransform>& bodyWorld,
    std::string* error) const {
    bodyWorld.clear();
    bodyWorld.reserve(instance.bodies.size());
    for (RigidBodyHandle handle : instance.bodies) {
        const auto body = physics_->state(handle);
        if (!body) return fail(error, "ragdoll physics body is unavailable");
        bodyWorld.push_back(body->currentTransform);
    }
    return true;
}

bool RagdollRuntime::begin_recovery(
    std::uint64_t objectId, const RigidTransform& currentObjectWorld,
    const RagdollRecoveryOptions& options, RigidTransform* alignedObjectWorld,
    std::string* error) {
    auto found = instances_.find(objectId);
    if (found == instances_.end()) return fail(error, "ragdoll instance is not bound");
    Instance& instance = found->second;
    if ((instance.state != RagdollRuntimeState::Simulating &&
         instance.state != RagdollRuntimeState::BlendingIn) || instance.bodies.empty())
        return fail(error, "ragdoll instance is not physically active");
    if ((options.getUpClip.empty() && options.faceUpClip.empty() && options.faceDownClip.empty()) ||
        !std::isfinite(options.poseBlendSeconds) ||
        options.poseBlendSeconds < 0.0F || options.referenceBody >= instance.definition.bodies.size())
        return fail(error, "ragdoll recovery options are invalid");
    std::vector<RigidTransform> bodyWorld;
    if (!gather_body_world(instance, bodyWorld, error)) return false;
    const RagdollBodyDefinition& reference = instance.definition.bodies[options.referenceBody];
    const RigidTransform physicalBoneWorld = compose_rigid_transforms(
        bodyWorld[options.referenceBody], inverse_rigid(reference.bodyFromBone));
    const Float3 referenceUp = rotate(physicalBoneWorld.rotation, {0.0F, 1.0F, 0.0F});
    instance.facing = dot(referenceUp, {0.0F, 1.0F, 0.0F}) >= 0.0F
        ? RagdollRecoveryFacing::FaceUp : RagdollRecoveryFacing::FaceDown;
    const std::string& orientedClip = *instance.facing == RagdollRecoveryFacing::FaceUp
        ? options.faceUpClip : options.faceDownClip;
    const std::string& selectedClip = orientedClip.empty() ? options.getUpClip : orientedClip;
    if (selectedClip.empty()) return fail(error, "selected ragdoll orientation has no get-up clip");
    instance.resumePlaybackSpeed = animation_->playback_speed(objectId).value_or(1.0F);
    if (!animation_->play(objectId, selectedClip, true, error)) return false;
    if (!animation_->set_playback_speed(objectId, 0.0F, error)) return false;
    const SkeletonAsset* skeleton = animation_->skeleton(objectId);
    const LocalPose* target = animation_->local_pose(objectId);
    if (!skeleton || !target) return fail(error, "get-up pose is unavailable");
    std::string poseError;
    const auto targetModel = compute_model_pose(*skeleton, *target, &poseError);
    if (targetModel.empty()) return fail(error, poseError);
    RigidTransform aligned = currentObjectWorld;
    if (options.alignRotation) {
        if (options.uprightRotation) {
            Float3 physicalForward = rotate(physicalBoneWorld.rotation, {0.0F, 0.0F, 1.0F});
            Float3 targetForward = rotate(targetModel[reference.bone].rotation, {0.0F, 0.0F, 1.0F});
            physicalForward.y = 0.0F;
            targetForward.y = 0.0F;
            if (length_squared(physicalForward) < 1.0e-8F)
                physicalForward = rotate(currentObjectWorld.rotation, {0.0F, 0.0F, 1.0F});
            if (length_squared(targetForward) < 1.0e-8F) targetForward = {0.0F, 0.0F, 1.0F};
            const float physicalYaw = std::atan2(physicalForward.x, physicalForward.z);
            const float targetYaw = std::atan2(targetForward.x, targetForward.z);
            aligned.rotation = quaternion_from_axis_angle(
                {0.0F, 1.0F, 0.0F}, physicalYaw - targetYaw);
        } else {
            aligned.rotation = compose_rigid_transforms(
                physicalBoneWorld, inverse_rigid(targetModel[reference.bone])).rotation;
        }
    }
    if (options.alignPosition)
        aligned.position = subtract(
            physicalBoneWorld.position,
            rotate(aligned.rotation, targetModel[reference.bone].position));
    std::vector<float> weights;
    weights.reserve(instance.definition.bodies.size());
    for (const auto& body : instance.definition.bodies) weights.push_back(body.simulationWeight);
    instance.recoveryStart = ragdoll_pose_from_body_world_weighted(
        *skeleton, instance.definition, aligned, bodyWorld, *target, 1.0F, weights, error);
    if (instance.recoveryStart.empty()) return false;
    instance.recoveryTarget = *target;
    destroy_physics(instance);
    instance.recoveryElapsed = 0.0F;
    instance.recoveryDuration = options.poseBlendSeconds;
    instance.state = RagdollRuntimeState::Recovering;
    instance.blend.weight = 1.0F;
    instance.blend.targetWeight = 0.0F;
    if (alignedObjectWorld) *alignedObjectWorld = aligned;
    return true;
}

bool RagdollRuntime::apply_impulse(
    std::uint64_t objectId, std::size_t bodyIndex, Float3 worldImpulse) noexcept {
    const auto found = instances_.find(objectId);
    return found != instances_.end() && bodyIndex < found->second.bodies.size() && finite(worldImpulse) &&
        physics_->apply_impulse(found->second.bodies[bodyIndex], worldImpulse);
}

bool RagdollRuntime::tick(
    std::uint64_t objectId, float deltaSeconds, const RigidTransform& objectWorld,
    std::string* error) {
    auto found = instances_.find(objectId);
    if (found == instances_.end()) return false;
    Instance& instance = found->second;
    if (!(deltaSeconds > 0.0F) || !std::isfinite(deltaSeconds))
        return fail(error, "ragdoll tick delta is invalid");
    if (instance.state == RagdollRuntimeState::Animated) return true;
    if (instance.state == RagdollRuntimeState::Recovering) {
        instance.recoveryElapsed += deltaSeconds;
        const float linear = instance.recoveryDuration == 0.0F ? 1.0F :
            std::clamp(instance.recoveryElapsed / instance.recoveryDuration, 0.0F, 1.0F);
        const float eased = linear * linear * (3.0F - 2.0F * linear);
        const LocalPose pose = blend_local_poses(instance.recoveryStart, instance.recoveryTarget, eased);
        if (!animation_->publish_local_pose(objectId, pose, error)) return false;
        instance.blend.weight = 1.0F - eased;
        if (linear >= 1.0F) {
            if (!animation_->set_playback_speed(objectId, instance.resumePlaybackSpeed, error)) return false;
            instance.state = RagdollRuntimeState::Animated;
            instance.blend.weight = instance.blend.targetWeight = 0.0F;
        }
        return true;
    }
    instance.blend.tick(deltaSeconds);
    if (instance.state == RagdollRuntimeState::BlendingIn && instance.blend.weight >= 1.0F)
        instance.state = RagdollRuntimeState::Simulating;
    std::vector<RigidTransform> bodyWorld;
    if (!gather_body_world(instance, bodyWorld, error)) return false;
    const SkeletonAsset* skeleton = animation_->skeleton(objectId);
    const LocalPose* animationPose = animation_->local_pose(objectId);
    if (!skeleton || !animationPose) return fail(error, "ragdoll animation instance disappeared");
    std::vector<float> weights;
    weights.reserve(instance.definition.bodies.size());
    for (const auto& body : instance.definition.bodies) weights.push_back(body.simulationWeight);
    const LocalPose pose = ragdoll_pose_from_body_world_weighted(
        *skeleton, instance.definition, objectWorld, bodyWorld, *animationPose,
        instance.blend.weight, weights, error);
    if (pose.empty() || !animation_->publish_local_pose(objectId, pose, error)) return false;
    bool quiet = true;
    for (RigidBodyHandle handle : instance.bodies) {
        const auto body = physics_->state(handle);
        if (!body || (speed(body->linearVelocity) > instance.config.settleLinearSpeed) ||
            (speed(body->angularVelocity) > instance.config.settleAngularSpeed)) {
            quiet = false;
            break;
        }
    }
    instance.quietSeconds = quiet ? instance.quietSeconds + deltaSeconds : 0.0F;
    instance.settled = instance.quietSeconds >= instance.config.settleSeconds;
    return true;
}

bool RagdollRuntime::has_instance(std::uint64_t objectId) const noexcept {
    return instances_.contains(objectId);
}

bool RagdollRuntime::owns_pose(std::uint64_t objectId) const noexcept {
    const auto found = instances_.find(objectId);
    return found != instances_.end() && found->second.state != RagdollRuntimeState::Animated;
}

bool RagdollRuntime::settled(std::uint64_t objectId) const noexcept {
    const auto found = instances_.find(objectId);
    return found != instances_.end() && found->second.settled;
}

RagdollRuntimeState RagdollRuntime::state(std::uint64_t objectId) const noexcept {
    const auto found = instances_.find(objectId);
    return found == instances_.end() ? RagdollRuntimeState::Animated : found->second.state;
}

float RagdollRuntime::blend_weight(std::uint64_t objectId) const noexcept {
    const auto found = instances_.find(objectId);
    return found == instances_.end() ? 0.0F : found->second.blend.weight;
}

std::optional<RagdollRecoveryFacing> RagdollRuntime::recovery_facing(
    std::uint64_t objectId) const noexcept {
    const auto found = instances_.find(objectId);
    return found == instances_.end() ? std::nullopt : found->second.facing;
}

std::span<const RigidBodyHandle> RagdollRuntime::body_handles(
    std::uint64_t objectId) const noexcept {
    const auto found = instances_.find(objectId);
    return found == instances_.end() ? std::span<const RigidBodyHandle>{} :
        std::span<const RigidBodyHandle>(found->second.bodies);
}

std::span<const RigidBodyConstraintHandle> RagdollRuntime::constraint_handles(
    std::uint64_t objectId) const noexcept {
    const auto found = instances_.find(objectId);
    return found == instances_.end() ? std::span<const RigidBodyConstraintHandle>{} :
        std::span<const RigidBodyConstraintHandle>(found->second.constraints);
}

std::vector<std::uint64_t> RagdollRuntime::object_ids() const {
    std::vector<std::uint64_t> result;
    result.reserve(instances_.size());
    for (const auto& [id, instance] : instances_) {
        (void)instance;
        result.push_back(id);
    }
    return result;
}

} // namespace dve
