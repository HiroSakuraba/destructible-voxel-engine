#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "dve/animation.hpp"

namespace dve {

using AnimationParameterValue = std::variant<bool, std::int64_t, double>;

enum class AnimationConditionOperator : std::uint8_t {
    Equal, NotEqual, Greater, GreaterOrEqual, Less, LessOrEqual, Triggered
};

struct AnimationCondition {
    std::string parameter;
    AnimationConditionOperator operation{AnimationConditionOperator::Equal};
    AnimationParameterValue value{false};
};

struct AnimationControllerState {
    std::string name;
    std::string clip;
    float playbackSpeed{1.0F};
    bool applyRootMotion{};
};

struct AnimationControllerTransition {
    std::string from;
    std::string to;
    float blendSeconds{0.15F};
    float minimumStateTimeSeconds{};
    std::int32_t priority{};
    std::vector<AnimationCondition> conditions;
};

struct AnimationControllerAsset {
    std::string name;
    std::string initialState;
    std::map<std::string, AnimationParameterValue, std::less<>> parameters;
    std::vector<AnimationControllerState> states;
    std::vector<AnimationControllerTransition> transitions;
};

[[nodiscard]] AnimationValidationResult validate_animation_controller(
    const AnimationControllerAsset& controller) noexcept;

class AnimationControllerRuntime {
public:
    explicit AnimationControllerRuntime(SkeletalAnimationRuntime& animation) noexcept;

    [[nodiscard]] bool bind(
        std::uint64_t objectId, AnimationControllerAsset controller,
        std::string* error = nullptr);
    [[nodiscard]] bool unbind(std::uint64_t objectId) noexcept;
    [[nodiscard]] bool set_parameter(
        std::uint64_t objectId, std::string_view name, AnimationParameterValue value,
        std::string* error = nullptr);
    [[nodiscard]] bool trigger(
        std::uint64_t objectId, std::string_view name, std::string* error = nullptr);
    [[nodiscard]] std::string_view state(std::uint64_t objectId) const noexcept;
    [[nodiscard]] float state_time(std::uint64_t objectId) const noexcept;
    void tick(float deltaSeconds);

private:
    struct Instance {
        AnimationControllerAsset asset;
        std::map<std::string, AnimationParameterValue, std::less<>> parameters;
        std::map<std::string, bool, std::less<>> triggers;
        std::string state;
        float stateTime{};
    };

    [[nodiscard]] const AnimationControllerState* find_state(
        const Instance& instance, std::string_view name) const noexcept;
    [[nodiscard]] bool enter_state(
        std::uint64_t objectId, Instance& instance, std::string_view state,
        float blendSeconds, std::string* error = nullptr);

    SkeletalAnimationRuntime* animation_{};
    std::map<std::uint64_t, Instance> instances_;
};

struct TwoBoneIkRequest {
    BoneIndex root{kInvalidBoneIndex};
    BoneIndex middle{kInvalidBoneIndex};
    BoneIndex end{kInvalidBoneIndex};
    Float3 targetModel{};
    Float3 poleModel{0.0F, 0.0F, 1.0F};
    float weight{1.0F};
    bool allowStretch{};
    float maximumStretch{1.0F};
};

[[nodiscard]] bool solve_two_bone_ik(
    const SkeletonAsset& skeleton, LocalPose& pose, const TwoBoneIkRequest& request,
    std::string* error = nullptr);

enum class RagdollJointKind : std::uint8_t { Ball, Hinge, ConeTwist, Fixed };

struct RagdollBodyDefinition {
    BoneIndex bone{kInvalidBoneIndex};
    float massKilograms{1.0F};
    Float3 halfExtents{0.1F, 0.1F, 0.1F};
    RigidTransform bodyFromBone{};
    float simulationWeight{1.0F};
};

struct RagdollJointDefinition {
    std::uint16_t parentBody{};
    std::uint16_t childBody{};
    RagdollJointKind kind{RagdollJointKind::ConeTwist};
    Float3 axis{1.0F, 0.0F, 0.0F};
    float minimumRadians{-0.785398F};
    float maximumRadians{0.785398F};
    float swingLimitRadians{0.785398F};
};

struct RagdollDefinition {
    std::string name;
    std::vector<RagdollBodyDefinition> bodies;
    std::vector<RagdollJointDefinition> joints;
};

[[nodiscard]] AnimationValidationResult validate_ragdoll(
    const SkeletonAsset& skeleton, const RagdollDefinition& ragdoll) noexcept;

// Converts externally simulated body transforms back into a local skeletal pose. The physics
// backend owns body creation and constraints; this bridge keeps animation and solver semantics
// separate and supplies deterministic animated/simulated blending.
[[nodiscard]] LocalPose ragdoll_pose_from_body_world(
    const SkeletonAsset& skeleton, const RagdollDefinition& ragdoll,
    const RigidTransform& objectWorld, std::span<const RigidTransform> bodyWorld,
    std::span<const RigidTransform> animationPose, float blendWeight,
    std::string* error = nullptr);
[[nodiscard]] LocalPose ragdoll_pose_from_body_world_weighted(
    const SkeletonAsset& skeleton, const RagdollDefinition& ragdoll,
    const RigidTransform& objectWorld, std::span<const RigidTransform> bodyWorld,
    std::span<const RigidTransform> animationPose, float globalBlendWeight,
    std::span<const float> bodyBlendWeights, std::string* error = nullptr);

struct RagdollBlendState {
    float weight{};
    float targetWeight{};
    float blendRatePerSecond{4.0F};

    void set_simulated(bool simulated) noexcept { targetWeight = simulated ? 1.0F : 0.0F; }
    void tick(float deltaSeconds) noexcept;
};

} // namespace dve
