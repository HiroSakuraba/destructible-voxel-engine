#include "dve/marionette_runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <utility>

namespace dve {
namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

constexpr ControlRigControlId kPelvisControl = 1001U;
constexpr ControlRigControlId kChestControl = 1002U;
constexpr ControlRigControlId kHeadControl = 1003U;
constexpr ControlRigControlId kLeftHandControl = 1004U;
constexpr ControlRigControlId kRightHandControl = 1005U;
constexpr ControlRigControlId kLeftFootControl = 1006U;
constexpr ControlRigControlId kRightFootControl = 1007U;
constexpr ControlRigControlId kLeftElbowPoleControl = 1008U;
constexpr ControlRigControlId kRightElbowPoleControl = 1009U;
constexpr ControlRigControlId kLeftKneePoleControl = 1010U;
constexpr ControlRigControlId kRightKneePoleControl = 1011U;

[[nodiscard]] std::size_t role_index(MarionetteStringRole role) noexcept {
    return static_cast<std::size_t>(role);
}

[[nodiscard]] Float3 cross(Float3 a, Float3 b) noexcept {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

[[nodiscard]] bool finite(float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] bool finite(Float3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] float clamp_axis(float value, float deadZone) noexcept {
    if (!finite(value)) return 0.0F;
    const float magnitude = std::abs(value);
    if (magnitude <= deadZone) return 0.0F;
    const float sign = value < 0.0F ? -1.0F : 1.0F;
    return sign * std::clamp((magnitude - deadZone) / std::max(1.0F - deadZone, 0.001F), 0.0F, 1.0F);
}

[[nodiscard]] Quaternion quaternion_from_basis(Float3 right, Float3 up, Float3 forward) noexcept {
    right = normalize(right);
    up = normalize(up);
    forward = normalize(forward);
    const float trace = right.x + up.y + forward.z;
    Quaternion result;
    if (trace > 0.0F) {
        const float s = std::sqrt(trace + 1.0F) * 2.0F;
        result.w = 0.25F * s;
        result.x = (up.z - forward.y) / s;
        result.y = (forward.x - right.z) / s;
        result.z = (right.y - up.x) / s;
    } else if (right.x > up.y && right.x > forward.z) {
        const float s = std::sqrt(1.0F + right.x - up.y - forward.z) * 2.0F;
        result.w = (up.z - forward.y) / s;
        result.x = 0.25F * s;
        result.y = (up.x + right.y) / s;
        result.z = (forward.x + right.z) / s;
    } else if (up.y > forward.z) {
        const float s = std::sqrt(1.0F + up.y - right.x - forward.z) * 2.0F;
        result.w = (forward.x - right.z) / s;
        result.x = (up.x + right.y) / s;
        result.y = 0.25F * s;
        result.z = (forward.y + up.z) / s;
    } else {
        const float s = std::sqrt(1.0F + forward.z - right.x - up.y) * 2.0F;
        result.w = (right.y - up.x) / s;
        result.x = (forward.x + right.z) / s;
        result.y = (forward.y + up.z) / s;
        result.z = 0.25F * s;
    }
    return normalize(result);
}

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
}

void hash_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    hash_bytes(hash, &value, sizeof(value));
}

void hash_float(std::uint64_t& hash, float value) noexcept {
    hash_u64(hash, std::bit_cast<std::uint32_t>(value));
}

void hash_float3(std::uint64_t& hash, Float3 value) noexcept {
    hash_float(hash, value.x);
    hash_float(hash, value.y);
    hash_float(hash, value.z);
}

[[nodiscard]] bool valid_bone(const SkeletonAsset& skeleton, BoneIndex bone) noexcept {
    return bone != kInvalidBoneIndex && static_cast<std::size_t>(bone) < skeleton.bones.size();
}

[[nodiscard]] ControlRigControl model_control(
    ControlRigControlId id, std::string name, RigidTransform defaultTransform) {
    ControlRigControl control;
    control.id = id;
    control.name = std::move(name);
    control.kind = ControlRigControlKind::Transform;
    control.space = ControlRigSpace::Model;
    control.defaultLocal = defaultTransform;
    return control;
}

[[nodiscard]] ControlRigNode set_bone_node(
    ControlRigNodeId id, std::string name, BoneIndex bone, ControlRigControlId control) {
    ControlRigNode node;
    node.id = id;
    node.name = std::move(name);
    node.kind = ControlRigNodeKind::SetBoneTransform;
    node.phase = ControlRigSolvePhase::ForwardSolve;
    node.bone = bone;
    node.targetControl = control;
    return node;
}

[[nodiscard]] ControlRigNode two_bone_node(
    ControlRigNodeId id, std::string name, BoneIndex upper, BoneIndex lower,
    BoneIndex end, ControlRigControlId target, ControlRigControlId pole) {
    ControlRigNode node;
    node.id = id;
    node.name = std::move(name);
    node.kind = ControlRigNodeKind::TwoBoneIk;
    node.phase = ControlRigSolvePhase::ForwardSolve;
    node.bone = upper;
    node.middleBone = lower;
    node.endBone = end;
    node.targetControl = target;
    node.poleControl = pole;
    node.maximumStretch = 1.05F;
    node.allowStretch = true;
    return node;
}

[[nodiscard]] RagdollBodyDefinition ragdoll_body(
    BoneIndex bone, float mass, Float3 halfExtents, float weight = 1.0F) noexcept {
    RagdollBodyDefinition body;
    body.bone = bone;
    body.massKilograms = mass;
    body.halfExtents = halfExtents;
    body.simulationWeight = weight;
    return body;
}

[[nodiscard]] RagdollJointDefinition ragdoll_joint(
    std::uint16_t parent, std::uint16_t child, RagdollJointKind kind,
    float minimum, float maximum, float swing) noexcept {
    RagdollJointDefinition joint;
    joint.parentBody = parent;
    joint.childBody = child;
    joint.kind = kind;
    joint.minimumRadians = minimum;
    joint.maximumRadians = maximum;
    joint.swingLimitRadians = swing;
    return joint;
}

[[nodiscard]] Float3 default_anchor(MarionetteStringRole role) noexcept {
    switch (role) {
    case MarionetteStringRole::Head: return {0.0F, 0.0F, 0.0F};
    case MarionetteStringRole::Pelvis: return {0.0F, 0.0F, 0.28F};
    case MarionetteStringRole::LeftHand: return {-0.62F, 0.0F, 0.0F};
    case MarionetteStringRole::RightHand: return {0.62F, 0.0F, 0.0F};
    case MarionetteStringRole::LeftFoot: return {-0.38F, 0.0F, 0.38F};
    case MarionetteStringRole::RightFoot: return {0.38F, 0.0F, 0.38F};
    case MarionetteStringRole::Count: break;
    }
    return {};
}

[[nodiscard]] Float3 target_from_string(
    const MarionetteStringDefinition& string, const MarionetteInputFrame& input) noexcept {
    const Float3 anchor = transform_point(input.crossbarWorld, string.crossbarAnchorLocal);
    const float pull = std::clamp(
        input.stringPullMeters[role_index(string.role)], 0.0F,
        std::max(string.restLengthMeters, 0.0F));
    const float lengthMeters = std::max(0.02F, string.restLengthMeters - pull);
    return add(anchor, {0.0F, -lengthMeters, 0.0F});
}

[[nodiscard]] RigidTransform model_target(
    const RigidTransform& objectWorld, Float3 worldPosition, Quaternion worldRotation) noexcept {
    return relative_rigid_transform(
        objectWorld, make_rigid_transform(worldPosition, worldRotation));
}

[[nodiscard]] std::optional<std::uint16_t> body_index_for_bone(
    const RagdollDefinition& ragdoll, BoneIndex bone) noexcept {
    for (std::size_t index = 0; index < ragdoll.bodies.size(); ++index) {
        if (ragdoll.bodies[index].bone == bone && index <= std::numeric_limits<std::uint16_t>::max())
            return static_cast<std::uint16_t>(index);
    }
    return std::nullopt;
}

} // namespace

std::string_view marionette_string_role_name(MarionetteStringRole role) noexcept {
    switch (role) {
    case MarionetteStringRole::Head: return "Head";
    case MarionetteStringRole::Pelvis: return "Pelvis";
    case MarionetteStringRole::LeftHand: return "Left Hand";
    case MarionetteStringRole::RightHand: return "Right Hand";
    case MarionetteStringRole::LeftFoot: return "Left Foot";
    case MarionetteStringRole::RightFoot: return "Right Foot";
    case MarionetteStringRole::Count: return "Count";
    }
    return "Unknown";
}

std::string_view marionette_control_mode_name(MarionetteControlMode mode) noexcept {
    switch (mode) {
    case MarionetteControlMode::AssistedRig: return "Assisted Rig";
    case MarionetteControlMode::Hybrid: return "Hybrid";
    case MarionetteControlMode::Physical: return "Physical";
    }
    return "Unknown";
}

AnimationValidationResult validate_humanoid_marionette_bones(
    const SkeletonAsset& skeleton, const HumanoidMarionetteBones& bones) noexcept {
    const auto skeletonValidation = validate_skeleton(skeleton);
    if (!skeletonValidation) return skeletonValidation;
    const std::array<BoneIndex, 17U> required{
        bones.pelvis, bones.spine, bones.chest, bones.neck, bones.head,
        bones.leftUpperArm, bones.leftLowerArm, bones.leftHand,
        bones.rightUpperArm, bones.rightLowerArm, bones.rightHand,
        bones.leftThigh, bones.leftCalf, bones.leftFoot,
        bones.rightThigh, bones.rightCalf, bones.rightFoot,
    };
    std::set<BoneIndex> unique;
    for (const BoneIndex bone : required) {
        if (!valid_bone(skeleton, bone))
            return {false, "humanoid marionette mapping contains an invalid bone"};
        if (!unique.insert(bone).second)
            return {false, "humanoid marionette mapping reuses a bone"};
    }
    return {true, {}};
}

std::uint64_t marionette_content_hash(const MarionetteAsset& asset) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_bytes(hash, asset.name.data(), asset.name.size());
    const std::array<ControlRigControlId, 11U> controls{
        asset.controls.pelvis, asset.controls.chest, asset.controls.head,
        asset.controls.leftHand, asset.controls.rightHand,
        asset.controls.leftFoot, asset.controls.rightFoot,
        asset.controls.leftElbowPole, asset.controls.rightElbowPole,
        asset.controls.leftKneePole, asset.controls.rightKneePole,
    };
    for (const auto control : controls) hash_u64(hash, control);
    for (const auto& string : asset.strings) {
        hash_u64(hash, static_cast<std::uint64_t>(string.role));
        hash_u64(hash, string.control);
        hash_u64(hash, string.ragdollBody);
        hash_float3(hash, string.crossbarAnchorLocal);
        hash_float3(hash, string.bodyAnchorLocal);
        hash_float(hash, string.restLengthMeters);
        hash_float(hash, string.stiffnessNewtonsPerMeter);
        hash_float(hash, string.dampingNewtonSecondsPerMeter);
        hash_float(hash, string.maximumForceNewtons);
    }
    hash_float(hash, asset.maximumStringPullMeters);
    hash_float(hash, asset.maximumBodyLeanMeters);
    hash_float(hash, asset.maximumBodyYawRadians);
    return hash;
}

AnimationValidationResult validate_marionette_asset(
    const ControlRigAsset& rig, const RagdollDefinition& ragdoll,
    const MarionetteAsset& asset) noexcept {
    std::set<ControlRigControlId> controls;
    for (const auto& control : rig.controls) controls.insert(control.id);
    const auto hasControl = [&controls](ControlRigControlId control) {
        return control != kInvalidControlRigControlId && controls.contains(control);
    };
    const std::array<ControlRigControlId, 11U> required{
        asset.controls.pelvis, asset.controls.chest, asset.controls.head,
        asset.controls.leftHand, asset.controls.rightHand,
        asset.controls.leftFoot, asset.controls.rightFoot,
        asset.controls.leftElbowPole, asset.controls.rightElbowPole,
        asset.controls.leftKneePole, asset.controls.rightKneePole,
    };
    for (const auto control : required) {
        if (!hasControl(control)) return {false, "marionette references a missing control-rig control"};
    }
    std::array<bool, kMarionetteStringCount> roles{};
    for (const auto& string : asset.strings) {
        const std::size_t index = role_index(string.role);
        if (index >= roles.size() || roles[index])
            return {false, "marionette string roles must be unique and complete"};
        roles[index] = true;
        if (!hasControl(string.control)) return {false, "marionette string references a missing control"};
        if (string.ragdollBody >= ragdoll.bodies.size())
            return {false, "marionette string references a missing ragdoll body"};
        if (!finite(string.crossbarAnchorLocal) || !finite(string.bodyAnchorLocal) ||
            !finite(string.restLengthMeters) || string.restLengthMeters <= 0.0F ||
            !finite(string.stiffnessNewtonsPerMeter) || string.stiffnessNewtonsPerMeter < 0.0F ||
            !finite(string.dampingNewtonSecondsPerMeter) || string.dampingNewtonSecondsPerMeter < 0.0F ||
            !finite(string.maximumForceNewtons) || string.maximumForceNewtons <= 0.0F)
            return {false, "marionette string contains invalid physical parameters"};
    }
    if (std::find(roles.begin(), roles.end(), false) != roles.end())
        return {false, "marionette does not define all six strings"};
    if (!finite(asset.maximumStringPullMeters) || asset.maximumStringPullMeters < 0.0F ||
        !finite(asset.maximumBodyLeanMeters) || asset.maximumBodyLeanMeters < 0.0F ||
        !finite(asset.maximumBodyYawRadians) || asset.maximumBodyYawRadians < 0.0F)
        return {false, "marionette limits are invalid"};
    if (asset.contentHash != 0U && asset.contentHash != marionette_content_hash(asset))
        return {false, "marionette content hash is stale"};
    return {true, {}};
}

bool build_humanoid_marionette_setup(
    const SkeletonAsset& skeleton, const HumanoidMarionetteBones& bones,
    HumanoidMarionetteSetup& output, std::string* error) {
    const auto validation = validate_humanoid_marionette_bones(skeleton, bones);
    if (!validation) {
        if (error) *error = validation.message;
        return false;
    }
    std::string poseError;
    const LocalPose bind = make_bind_pose(skeleton);
    const auto model = compute_model_pose(skeleton, bind, &poseError);
    if (model.size() != skeleton.bones.size()) {
        if (error) *error = poseError.empty() ? "could not compute bind model pose" : poseError;
        return false;
    }

    ControlRigAsset rig;
    rig.name = "Fully Articulated Marionette Rig";
    rig.controls = {
        model_control(kPelvisControl, "Marionette Pelvis", model[bones.pelvis]),
        model_control(kChestControl, "Marionette Chest", model[bones.chest]),
        model_control(kHeadControl, "Marionette Head", model[bones.head]),
        model_control(kLeftHandControl, "Marionette Left Hand", model[bones.leftHand]),
        model_control(kRightHandControl, "Marionette Right Hand", model[bones.rightHand]),
        model_control(kLeftFootControl, "Marionette Left Foot", model[bones.leftFoot]),
        model_control(kRightFootControl, "Marionette Right Foot", model[bones.rightFoot]),
        model_control(kLeftElbowPoleControl, "Marionette Left Elbow Pole",
                      make_rigid_transform(add(model[bones.leftLowerArm].position, {0.0F, 0.0F, -0.35F}), {})),
        model_control(kRightElbowPoleControl, "Marionette Right Elbow Pole",
                      make_rigid_transform(add(model[bones.rightLowerArm].position, {0.0F, 0.0F, -0.35F}), {})),
        model_control(kLeftKneePoleControl, "Marionette Left Knee Pole",
                      make_rigid_transform(add(model[bones.leftCalf].position, {0.0F, 0.0F, 0.45F}), {})),
        model_control(kRightKneePoleControl, "Marionette Right Knee Pole",
                      make_rigid_transform(add(model[bones.rightCalf].position, {0.0F, 0.0F, 0.45F}), {})),
    };
    rig.nodes = {
        set_bone_node(2001U, "Drive Pelvis", bones.pelvis, kPelvisControl),
        set_bone_node(2002U, "Drive Chest", bones.chest, kChestControl),
        set_bone_node(2003U, "Drive Head", bones.head, kHeadControl),
        two_bone_node(2004U, "Left Arm String IK", bones.leftUpperArm, bones.leftLowerArm,
                      bones.leftHand, kLeftHandControl, kLeftElbowPoleControl),
        two_bone_node(2005U, "Right Arm String IK", bones.rightUpperArm, bones.rightLowerArm,
                      bones.rightHand, kRightHandControl, kRightElbowPoleControl),
        two_bone_node(2006U, "Left Leg String IK", bones.leftThigh, bones.leftCalf,
                      bones.leftFoot, kLeftFootControl, kLeftKneePoleControl),
        two_bone_node(2007U, "Right Leg String IK", bones.rightThigh, bones.rightCalf,
                      bones.rightFoot, kRightFootControl, kRightKneePoleControl),
    };
    rig.contentHash = control_rig_content_hash(rig);

    RagdollDefinition ragdoll;
    ragdoll.name = "Fully Articulated Marionette Ragdoll";
    ragdoll.bodies = {
        ragdoll_body(bones.pelvis, 8.0F, {0.16F, 0.12F, 0.12F}),
        ragdoll_body(bones.chest, 7.0F, {0.18F, 0.18F, 0.12F}),
        ragdoll_body(bones.head, 3.0F, {0.12F, 0.14F, 0.12F}),
        ragdoll_body(bones.leftUpperArm, 1.5F, {0.16F, 0.06F, 0.06F}),
        ragdoll_body(bones.leftLowerArm, 1.2F, {0.15F, 0.05F, 0.05F}),
        ragdoll_body(bones.leftHand, 0.7F, {0.07F, 0.04F, 0.08F}),
        ragdoll_body(bones.rightUpperArm, 1.5F, {0.16F, 0.06F, 0.06F}),
        ragdoll_body(bones.rightLowerArm, 1.2F, {0.15F, 0.05F, 0.05F}),
        ragdoll_body(bones.rightHand, 0.7F, {0.07F, 0.04F, 0.08F}),
        ragdoll_body(bones.leftThigh, 3.5F, {0.07F, 0.21F, 0.07F}),
        ragdoll_body(bones.leftCalf, 2.2F, {0.06F, 0.20F, 0.06F}),
        ragdoll_body(bones.leftFoot, 1.0F, {0.07F, 0.05F, 0.13F}),
        ragdoll_body(bones.rightThigh, 3.5F, {0.07F, 0.21F, 0.07F}),
        ragdoll_body(bones.rightCalf, 2.2F, {0.06F, 0.20F, 0.06F}),
        ragdoll_body(bones.rightFoot, 1.0F, {0.07F, 0.05F, 0.13F}),
    };
    ragdoll.joints = {
        ragdoll_joint(0U, 1U, RagdollJointKind::ConeTwist, -0.45F, 0.45F, 0.55F),
        ragdoll_joint(1U, 2U, RagdollJointKind::ConeTwist, -0.55F, 0.55F, 0.55F),
        ragdoll_joint(1U, 3U, RagdollJointKind::ConeTwist, -0.8F, 0.8F, 1.25F),
        ragdoll_joint(3U, 4U, RagdollJointKind::Hinge, 0.0F, 2.55F, 0.2F),
        ragdoll_joint(4U, 5U, RagdollJointKind::ConeTwist, -0.55F, 0.55F, 0.6F),
        ragdoll_joint(1U, 6U, RagdollJointKind::ConeTwist, -0.8F, 0.8F, 1.25F),
        ragdoll_joint(6U, 7U, RagdollJointKind::Hinge, 0.0F, 2.55F, 0.2F),
        ragdoll_joint(7U, 8U, RagdollJointKind::ConeTwist, -0.55F, 0.55F, 0.6F),
        ragdoll_joint(0U, 9U, RagdollJointKind::ConeTwist, -0.55F, 0.55F, 0.85F),
        ragdoll_joint(9U, 10U, RagdollJointKind::Hinge, 0.0F, 2.7F, 0.2F),
        ragdoll_joint(10U, 11U, RagdollJointKind::ConeTwist, -0.45F, 0.45F, 0.45F),
        ragdoll_joint(0U, 12U, RagdollJointKind::ConeTwist, -0.55F, 0.55F, 0.85F),
        ragdoll_joint(12U, 13U, RagdollJointKind::Hinge, 0.0F, 2.7F, 0.2F),
        ragdoll_joint(13U, 14U, RagdollJointKind::ConeTwist, -0.45F, 0.45F, 0.45F),
    };

    MarionetteAsset asset;
    asset.name = "Six String Fully Articulated Marionette";
    asset.controls = {
        kPelvisControl, kChestControl, kHeadControl,
        kLeftHandControl, kRightHandControl, kLeftFootControl, kRightFootControl,
        kLeftElbowPoleControl, kRightElbowPoleControl,
        kLeftKneePoleControl, kRightKneePoleControl,
    };
    const std::array<std::pair<MarionetteStringRole, BoneIndex>, kMarionetteStringCount> stringBones{
        std::pair{MarionetteStringRole::Head, bones.head},
        std::pair{MarionetteStringRole::Pelvis, bones.pelvis},
        std::pair{MarionetteStringRole::LeftHand, bones.leftHand},
        std::pair{MarionetteStringRole::RightHand, bones.rightHand},
        std::pair{MarionetteStringRole::LeftFoot, bones.leftFoot},
        std::pair{MarionetteStringRole::RightFoot, bones.rightFoot},
    };
    const std::array<ControlRigControlId, kMarionetteStringCount> stringControls{
        kHeadControl, kPelvisControl, kLeftHandControl, kRightHandControl,
        kLeftFootControl, kRightFootControl,
    };
    for (std::size_t index = 0; index < asset.strings.size(); ++index) {
        const auto bodyIndex = body_index_for_bone(ragdoll, stringBones[index].second);
        if (!bodyIndex) {
            if (error) *error = "could not map a marionette string to its ragdoll body";
            return false;
        }
        auto& string = asset.strings[index];
        string.role = stringBones[index].first;
        string.control = stringControls[index];
        string.ragdollBody = *bodyIndex;
        string.crossbarAnchorLocal = default_anchor(string.role);
        string.restLengthMeters =
            string.role == MarionetteStringRole::Pelvis ? 1.15F :
            (string.role == MarionetteStringRole::Head ? 0.75F : 1.35F);
    }
    asset.contentHash = marionette_content_hash(asset);

    const auto rigValidation = validate_control_rig(skeleton, rig);
    if (!rigValidation) {
        if (error) *error = rigValidation.message;
        return false;
    }
    const auto ragdollValidation = validate_ragdoll(skeleton, ragdoll);
    if (!ragdollValidation) {
        if (error) *error = ragdollValidation.message;
        return false;
    }
    const auto assetValidation = validate_marionette_asset(rig, ragdoll, asset);
    if (!assetValidation) {
        if (error) *error = assetValidation.message;
        return false;
    }
    output = {std::move(rig), std::move(ragdoll), std::move(asset)};
    if (error) error->clear();
    return true;
}

MarionetteInputRouter::MarionetteInputRouter(MarionetteGamepadSettings settings) noexcept
    : settings_(settings) {}

void MarionetteInputRouter::reset(const RigidTransform& referenceWorld) noexcept {
    gamepadCrossbar_ = referenceWorld;
    axes_.fill(0.0F);
    buttons_.fill(false);
    modePulse_ = false;
    recoveryPulse_ = false;
    recenterPulse_ = false;
}

void MarionetteInputRouter::consume_gamepad_event(const platform::PlatformEvent& event) noexcept {
    if (event.type == platform::EventType::GamepadAxisMotion) {
        const std::size_t axis = static_cast<std::size_t>(event.gamepadAxis);
        if (axis > 0U && axis <= axes_.size()) axes_[axis - 1U] = std::clamp(event.gamepadValue, -1.0F, 1.0F);
        return;
    }
    if (event.type != platform::EventType::GamepadButtonDown &&
        event.type != platform::EventType::GamepadButtonUp) return;
    const bool down = event.type == platform::EventType::GamepadButtonDown;
    const std::size_t button = static_cast<std::size_t>(event.gamepadButton);
    if (button < buttons_.size()) buttons_[button] = down;
    if (down && event.gamepadButton == platform::GamepadButton::North) modePulse_ = true;
    if (down && event.gamepadButton == platform::GamepadButton::Touchpad) recoveryPulse_ = true;
    if (down && event.gamepadButton == platform::GamepadButton::Start) recenterPulse_ = true;
}

void MarionetteInputRouter::set_vr_input(MarionetteVrInput input) noexcept {
    vr_ = input;
}

MarionetteInputFrame MarionetteInputRouter::sample(
    float deltaSeconds, const RigidTransform& referenceWorld) noexcept {
    MarionetteInputFrame frame;
    const float delta = std::clamp(finite(deltaSeconds) ? deltaSeconds : 0.0F, 0.0F, 0.1F);
    frame.requestModeCycle = std::exchange(modePulse_, false);
    frame.requestRecovery = std::exchange(recoveryPulse_, false);
    frame.requestRecenter = std::exchange(recenterPulse_, false);
    if (frame.requestRecenter) gamepadCrossbar_ = referenceWorld;

    if (vr_.enabled && vr_.left.tracked && vr_.right.tracked) {
        frame.vrTracked = true;
        frame.crossbarWorld.position = multiply(add(vr_.left.world.position, vr_.right.world.position), 0.5F);
        Float3 right = subtract(vr_.right.world.position, vr_.left.world.position);
        if (length_squared(right) < 1.0e-6F) right = rotate(referenceWorld.rotation, {1.0F, 0.0F, 0.0F});
        right = normalize(right);
        Float3 up = add(rotate(vr_.left.world.rotation, {0.0F, 1.0F, 0.0F}),
                        rotate(vr_.right.world.rotation, {0.0F, 1.0F, 0.0F}));
        up = subtract(up, multiply(right, dot(up, right)));
        if (length_squared(up) < 1.0e-6F) up = {0.0F, 1.0F, 0.0F};
        up = normalize(up);
        const Float3 forward = normalize(cross(right, up));
        up = normalize(cross(forward, right));
        frame.crossbarWorld.rotation = quaternion_from_basis(right, up, forward);
        frame.stringPullMeters[role_index(MarionetteStringRole::LeftHand)] =
            std::clamp(vr_.left.trigger, 0.0F, 1.0F) * settings_.maximumPullMeters;
        frame.stringPullMeters[role_index(MarionetteStringRole::RightHand)] =
            std::clamp(vr_.right.trigger, 0.0F, 1.0F) * settings_.maximumPullMeters;
        frame.stringPullMeters[role_index(MarionetteStringRole::LeftFoot)] =
            std::clamp(-vr_.left.stickY, 0.0F, 1.0F) * settings_.maximumPullMeters;
        frame.stringPullMeters[role_index(MarionetteStringRole::RightFoot)] =
            std::clamp(-vr_.right.stickY, 0.0F, 1.0F) * settings_.maximumPullMeters;
        const float grip = 0.5F * (std::clamp(vr_.left.grip, 0.0F, 1.0F) +
                                   std::clamp(vr_.right.grip, 0.0F, 1.0F));
        frame.stringPullMeters[role_index(MarionetteStringRole::Head)] = 0.2F * grip;
        frame.stringPullMeters[role_index(MarionetteStringRole::Pelvis)] = 0.12F * grip;
        frame.bodyLeanLocal = {
            0.15F * 0.5F * (vr_.left.stickX + vr_.right.stickX),
            0.0F,
            0.15F * 0.5F * (vr_.left.stickY + vr_.right.stickY),
        };
        frame.leftWristTwistRadians = vr_.left.stickX * 0.8F;
        frame.rightWristTwistRadians = vr_.right.stickX * 0.8F;
        frame.tensionScale = 0.5F + grip;
        return frame;
    }

    const float leftX = clamp_axis(axes_[0], settings_.deadZone);
    const float leftY = clamp_axis(axes_[1], settings_.deadZone);
    const float rightX = clamp_axis(axes_[2], settings_.deadZone);
    const float rightY = clamp_axis(axes_[3], settings_.deadZone);
    const float leftTrigger = std::clamp(axes_[4], 0.0F, 1.0F);
    const float rightTrigger = std::clamp(axes_[5], 0.0F, 1.0F);
    const Float3 localMove{
        leftX * settings_.translationSpeedMetersPerSecond * delta,
        (rightTrigger - leftTrigger) * settings_.verticalSpeedMetersPerSecond * delta,
        -leftY * settings_.translationSpeedMetersPerSecond * delta,
    };
    gamepadCrossbar_.position = add(gamepadCrossbar_.position, rotate(referenceWorld.rotation, localMove));
    const Quaternion deltaRotation = quaternion_from_euler_xyz({
        -rightY * settings_.rotationSpeedRadiansPerSecond * delta,
        rightX * settings_.rotationSpeedRadiansPerSecond * delta,
        0.0F,
    });
    gamepadCrossbar_.rotation = normalize(multiply(gamepadCrossbar_.rotation, deltaRotation));
    frame.crossbarWorld = gamepadCrossbar_;

    const auto held = [this](platform::GamepadButton button) {
        const std::size_t index = static_cast<std::size_t>(button);
        return index < buttons_.size() && buttons_[index];
    };
    frame.stringPullMeters[role_index(MarionetteStringRole::LeftHand)] =
        held(platform::GamepadButton::West) ? settings_.maximumPullMeters : 0.0F;
    frame.stringPullMeters[role_index(MarionetteStringRole::RightHand)] =
        held(platform::GamepadButton::East) ? settings_.maximumPullMeters : 0.0F;
    frame.stringPullMeters[role_index(MarionetteStringRole::LeftFoot)] =
        held(platform::GamepadButton::LeftShoulder) ? settings_.maximumPullMeters : 0.0F;
    frame.stringPullMeters[role_index(MarionetteStringRole::RightFoot)] =
        held(platform::GamepadButton::RightShoulder) ? settings_.maximumPullMeters : 0.0F;
    frame.stringPullMeters[role_index(MarionetteStringRole::Head)] =
        held(platform::GamepadButton::DpadUp) ? 0.3F : 0.0F;
    frame.stringPullMeters[role_index(MarionetteStringRole::Pelvis)] =
        held(platform::GamepadButton::DpadDown) ? 0.25F : 0.0F;
    frame.bodyLeanLocal = {
        (held(platform::GamepadButton::DpadRight) ? 0.18F : 0.0F) -
            (held(platform::GamepadButton::DpadLeft) ? 0.18F : 0.0F),
        0.0F,
        0.0F,
    };
    frame.bodyYawRadians = rightX * 0.55F;
    frame.leftWristTwistRadians = held(platform::GamepadButton::LeftStick) ? -0.75F : 0.0F;
    frame.rightWristTwistRadians = held(platform::GamepadButton::RightStick) ? 0.75F : 0.0F;
    frame.tensionScale = held(platform::GamepadButton::South) ? 1.5F : 1.0F;
    return frame;
}

MarionetteRuntime::MarionetteRuntime(
    SkeletalAnimationRuntime& animation, ControlRigRuntime& controlRig,
    RagdollRuntime& ragdolls, IRigidBodyWorld& physics) noexcept
    : animation_(&animation), controlRig_(&controlRig), ragdolls_(&ragdolls), physics_(&physics) {}

bool MarionetteRuntime::bind(
    std::uint64_t objectId, MarionetteAsset asset,
    MarionetteRuntimeConfig config, std::string* error) {
    if (objectId == 0U || !animation_->has_instance(objectId) || !controlRig_->has_instance(objectId) ||
        !ragdolls_->has_instance(objectId)) {
        if (error) *error = "marionette binding requires animation, control rig, and ragdoll instances";
        return false;
    }
    if (asset.contentHash != 0U && asset.contentHash != marionette_content_hash(asset)) {
        if (error) *error = "marionette asset content hash is stale";
        return false;
    }
    if (!finite(config.maximumDeltaSeconds) || config.maximumDeltaSeconds <= 0.0F ||
        !finite(config.controlBlend) || config.controlBlend < 0.0F || config.controlBlend > 1.0F) {
        if (error) *error = "marionette runtime configuration is invalid";
        return false;
    }
    asset.contentHash = marionette_content_hash(asset);
    Instance instance;
    instance.asset = std::move(asset);
    instance.config = config;
    instance.mode = config.initialMode;
    instance.telemetry.mode = config.initialMode;
    instances_.insert_or_assign(objectId, std::move(instance));
    if (error) error->clear();
    return true;
}

bool MarionetteRuntime::unbind(std::uint64_t objectId) noexcept {
    return instances_.erase(objectId) != 0U;
}

bool MarionetteRuntime::has_instance(std::uint64_t objectId) const noexcept {
    return instances_.contains(objectId);
}

MarionetteControlMode MarionetteRuntime::mode(std::uint64_t objectId) const noexcept {
    const auto found = instances_.find(objectId);
    return found == instances_.end() ? MarionetteControlMode::AssistedRig : found->second.mode;
}

bool MarionetteRuntime::set_mode(
    std::uint64_t objectId, MarionetteControlMode modeValue,
    const RigidTransform& objectWorld, std::string* error) {
    auto found = instances_.find(objectId);
    if (found == instances_.end()) {
        if (error) *error = "unknown marionette instance";
        return false;
    }
    if ((modeValue == MarionetteControlMode::Physical || modeValue == MarionetteControlMode::Hybrid) &&
        found->second.config.autoActivateRagdollForPhysicalMode && !ragdolls_->owns_pose(objectId)) {
        if (!ragdolls_->activate(objectId, objectWorld, {}, error)) return false;
    }
    found->second.mode = modeValue;
    found->second.telemetry.mode = modeValue;
    if (error) error->clear();
    return true;
}

const MarionetteTelemetry* MarionetteRuntime::telemetry(std::uint64_t objectId) const noexcept {
    const auto found = instances_.find(objectId);
    return found == instances_.end() ? nullptr : &found->second.telemetry;
}

bool MarionetteRuntime::tick(
    std::uint64_t objectId, float deltaSeconds,
    const RigidTransform& objectWorld, const MarionetteInputFrame& input,
    std::string* error) {
    auto found = instances_.find(objectId);
    if (found == instances_.end()) {
        if (error) *error = "unknown marionette instance";
        return false;
    }
    Instance& instance = found->second;
    if (!finite(deltaSeconds) || deltaSeconds < 0.0F) {
        if (error) *error = "marionette delta time is invalid";
        return false;
    }
    if (input.requestModeCycle) {
        const MarionetteControlMode next =
            instance.mode == MarionetteControlMode::AssistedRig ? MarionetteControlMode::Hybrid :
            (instance.mode == MarionetteControlMode::Hybrid ? MarionetteControlMode::Physical :
                                                            MarionetteControlMode::AssistedRig);
        if (!set_mode(objectId, next, objectWorld, error)) return false;
    }
    instance.telemetry = {};
    instance.telemetry.mode = instance.mode;
    instance.telemetry.crossbarWorld = input.crossbarWorld;
    instance.telemetry.ragdollActive = ragdolls_->owns_pose(objectId);
    bool ok = true;
    if (instance.mode != MarionetteControlMode::Physical)
        ok = apply_assisted_pose(objectId, instance, objectWorld, input, error) && ok;
    if (instance.mode != MarionetteControlMode::AssistedRig)
        ok = apply_physical_strings(
                 objectId, instance, std::min(deltaSeconds, instance.config.maximumDeltaSeconds),
                 objectWorld, input, error) && ok;
    if (!ok && error) instance.telemetry.lastError = *error;
    return ok;
}

bool MarionetteRuntime::apply_assisted_pose(
    std::uint64_t objectId, Instance& instance,
    const RigidTransform& objectWorld, const MarionetteInputFrame& input,
    std::string* error) {
    std::array<Float3, kMarionetteStringCount> targets{};
    for (const auto& string : instance.asset.strings)
        targets[role_index(string.role)] = target_from_string(string, input);

    const Quaternion bodyYaw = quaternion_from_axis_angle(
        {0.0F, 1.0F, 0.0F},
        std::clamp(input.bodyYawRadians, -instance.asset.maximumBodyYawRadians,
                   instance.asset.maximumBodyYawRadians));
    const Float3 leanLocal{
        std::clamp(input.bodyLeanLocal.x, -instance.asset.maximumBodyLeanMeters,
                   instance.asset.maximumBodyLeanMeters),
        std::clamp(input.bodyLeanLocal.y, -instance.asset.maximumBodyLeanMeters,
                   instance.asset.maximumBodyLeanMeters),
        std::clamp(input.bodyLeanLocal.z, -instance.asset.maximumBodyLeanMeters,
                   instance.asset.maximumBodyLeanMeters),
    };
    const Float3 leanWorld = transform_vector(objectWorld, leanLocal);
    targets[role_index(MarionetteStringRole::Pelvis)] =
        add(targets[role_index(MarionetteStringRole::Pelvis)], leanWorld);

    const Quaternion worldBodyRotation = normalize(multiply(objectWorld.rotation, bodyYaw));
    const auto setTarget = [&](ControlRigControlId control, Float3 position, Quaternion rotation) {
        return controlRig_->set_control_local(
            objectId, control, model_target(objectWorld, position, rotation), error);
    };
    if (!setTarget(instance.asset.controls.pelvis,
                   targets[role_index(MarionetteStringRole::Pelvis)], worldBodyRotation)) return false;
    const Float3 chestPosition = add(
        targets[role_index(MarionetteStringRole::Pelvis)],
        add({0.0F, 0.42F, 0.0F}, multiply(leanWorld, 0.35F)));
    if (!setTarget(instance.asset.controls.chest, chestPosition, worldBodyRotation)) return false;
    if (!setTarget(instance.asset.controls.head,
                   targets[role_index(MarionetteStringRole::Head)], input.crossbarWorld.rotation)) return false;
    if (!setTarget(instance.asset.controls.leftHand,
                   targets[role_index(MarionetteStringRole::LeftHand)],
                   normalize(multiply(input.crossbarWorld.rotation,
                                      quaternion_from_axis_angle({0.0F, 1.0F, 0.0F}, input.leftWristTwistRadians)))))
        return false;
    if (!setTarget(instance.asset.controls.rightHand,
                   targets[role_index(MarionetteStringRole::RightHand)],
                   normalize(multiply(input.crossbarWorld.rotation,
                                      quaternion_from_axis_angle({0.0F, 1.0F, 0.0F}, input.rightWristTwistRadians)))))
        return false;
    if (!setTarget(instance.asset.controls.leftFoot,
                   targets[role_index(MarionetteStringRole::LeftFoot)], worldBodyRotation)) return false;
    if (!setTarget(instance.asset.controls.rightFoot,
                   targets[role_index(MarionetteStringRole::RightFoot)], worldBodyRotation)) return false;

    const Float3 leftHand = targets[role_index(MarionetteStringRole::LeftHand)];
    const Float3 rightHand = targets[role_index(MarionetteStringRole::RightHand)];
    const Float3 leftFoot = targets[role_index(MarionetteStringRole::LeftFoot)];
    const Float3 rightFoot = targets[role_index(MarionetteStringRole::RightFoot)];
    if (!setTarget(instance.asset.controls.leftElbowPole, add(leftHand, {-0.2F, 0.12F, -0.35F}), {})) return false;
    if (!setTarget(instance.asset.controls.rightElbowPole, add(rightHand, {0.2F, 0.12F, -0.35F}), {})) return false;
    if (!setTarget(instance.asset.controls.leftKneePole, add(leftFoot, {-0.05F, 0.3F, 0.45F}), {})) return false;
    if (!setTarget(instance.asset.controls.rightKneePole, add(rightFoot, {0.05F, 0.3F, 0.45F}), {})) return false;

    if (!controlRig_->evaluate(objectId, error)) return false;
    instance.telemetry.rigEvaluated = true;
    for (const auto& string : instance.asset.strings) {
        auto& telemetry = instance.telemetry.strings[role_index(string.role)];
        telemetry.role = string.role;
        telemetry.anchorWorld = transform_point(input.crossbarWorld, string.crossbarAnchorLocal);
        telemetry.targetWorld = targets[role_index(string.role)];
        telemetry.desiredLengthMeters = std::max(
            0.02F, string.restLengthMeters -
                       std::clamp(input.stringPullMeters[role_index(string.role)],
                                  0.0F, instance.asset.maximumStringPullMeters));
    }
    return true;
}

bool MarionetteRuntime::apply_physical_strings(
    std::uint64_t objectId, Instance& instance, float deltaSeconds,
    const RigidTransform& objectWorld, const MarionetteInputFrame& input,
    std::string* error) {
    (void)deltaSeconds;
    if (!ragdolls_->owns_pose(objectId) && instance.config.autoActivateRagdollForPhysicalMode) {
        if (!ragdolls_->activate(objectId, objectWorld, {}, error)) return false;
    }
    const auto handles = ragdolls_->body_handles(objectId);
    if (handles.empty()) {
        if (error) *error = "physical marionette mode requires an active ragdoll";
        return false;
    }
    instance.telemetry.ragdollActive = true;
    for (const auto& string : instance.asset.strings) {
        auto& telemetry = instance.telemetry.strings[role_index(string.role)];
        telemetry.role = string.role;
        telemetry.anchorWorld = transform_point(input.crossbarWorld, string.crossbarAnchorLocal);
        telemetry.desiredLengthMeters = std::max(
            0.02F, string.restLengthMeters -
                       std::clamp(input.stringPullMeters[role_index(string.role)],
                                  0.0F, instance.asset.maximumStringPullMeters));
        if (string.ragdollBody >= handles.size()) continue;
        const RigidBodyHandle handle = handles[string.ragdollBody];
        const auto state = physics_->state(handle);
        if (!state) continue;
        telemetry.bodyAvailable = true;
        telemetry.bodyWorld = transform_point(state->currentTransform, string.bodyAnchorLocal);
        const Float3 bodyToAnchor = subtract(telemetry.anchorWorld, telemetry.bodyWorld);
        telemetry.currentLengthMeters = length(bodyToAnchor);
        telemetry.extensionMeters = std::max(
            0.0F, telemetry.currentLengthMeters - telemetry.desiredLengthMeters);
        Float3 direction{};
        if (telemetry.currentLengthMeters > 1.0e-5F)
            direction = multiply(bodyToAnchor, 1.0F / telemetry.currentLengthMeters);
        const Float3 anchorOffset = subtract(
            telemetry.bodyWorld, state->currentTransform.position);
        const Float3 anchorVelocity = add(
            state->linearVelocity, cross(state->angularVelocity, anchorOffset));
        const float dampingSpeed = dot(anchorVelocity, direction);
        const float rawForce = string.stiffnessNewtonsPerMeter * telemetry.extensionMeters -
                               string.dampingNewtonSecondsPerMeter * dampingSpeed;
        telemetry.forceNewtons = std::clamp(
            rawForce * std::clamp(input.tensionScale, 0.0F, 2.0F),
            0.0F, string.maximumForceNewtons);
        telemetry.targetWorld = add(
            telemetry.anchorWorld, multiply(direction, -telemetry.desiredLengthMeters));
        if (telemetry.forceNewtons > 0.0F &&
            !physics_->apply_force_at_point(
                handle,
                multiply(direction, telemetry.forceNewtons),
                telemetry.bodyWorld)) {
            if (error) *error = "physics backend rejected a marionette string force";
            return false;
        }
    }
    return true;
}

} // namespace dve
