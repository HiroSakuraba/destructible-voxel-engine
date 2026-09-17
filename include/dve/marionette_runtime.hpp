#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/control_rig.hpp"
#include "dve/platform/application_host.hpp"
#include "dve/ragdoll_runtime.hpp"

namespace dve {

enum class MarionetteStringRole : std::uint8_t {
    Head,
    Pelvis,
    LeftHand,
    RightHand,
    LeftFoot,
    RightFoot,
    Count,
};

inline constexpr std::size_t kMarionetteStringCount =
    static_cast<std::size_t>(MarionetteStringRole::Count);

enum class MarionetteControlMode : std::uint8_t {
    AssistedRig,
    Hybrid,
    Physical,
};

struct HumanoidMarionetteBones {
    BoneIndex pelvis{kInvalidBoneIndex};
    BoneIndex spine{kInvalidBoneIndex};
    BoneIndex chest{kInvalidBoneIndex};
    BoneIndex neck{kInvalidBoneIndex};
    BoneIndex head{kInvalidBoneIndex};
    BoneIndex leftUpperArm{kInvalidBoneIndex};
    BoneIndex leftLowerArm{kInvalidBoneIndex};
    BoneIndex leftHand{kInvalidBoneIndex};
    BoneIndex rightUpperArm{kInvalidBoneIndex};
    BoneIndex rightLowerArm{kInvalidBoneIndex};
    BoneIndex rightHand{kInvalidBoneIndex};
    BoneIndex leftThigh{kInvalidBoneIndex};
    BoneIndex leftCalf{kInvalidBoneIndex};
    BoneIndex leftFoot{kInvalidBoneIndex};
    BoneIndex rightThigh{kInvalidBoneIndex};
    BoneIndex rightCalf{kInvalidBoneIndex};
    BoneIndex rightFoot{kInvalidBoneIndex};
};

struct MarionetteControlBindings {
    ControlRigControlId pelvis{};
    ControlRigControlId chest{};
    ControlRigControlId head{};
    ControlRigControlId leftHand{};
    ControlRigControlId rightHand{};
    ControlRigControlId leftFoot{};
    ControlRigControlId rightFoot{};
    ControlRigControlId leftElbowPole{};
    ControlRigControlId rightElbowPole{};
    ControlRigControlId leftKneePole{};
    ControlRigControlId rightKneePole{};
};

struct MarionetteStringDefinition {
    MarionetteStringRole role{MarionetteStringRole::Head};
    ControlRigControlId control{};
    std::uint16_t ragdollBody{};
    Float3 crossbarAnchorLocal{};
    Float3 bodyAnchorLocal{};
    float restLengthMeters{1.0F};
    float stiffnessNewtonsPerMeter{180.0F};
    float dampingNewtonSecondsPerMeter{18.0F};
    float maximumForceNewtons{450.0F};
};

struct MarionetteAsset {
    std::string name;
    MarionetteControlBindings controls{};
    std::array<MarionetteStringDefinition, kMarionetteStringCount> strings{};
    float maximumStringPullMeters{0.65F};
    float maximumBodyLeanMeters{0.25F};
    float maximumBodyYawRadians{1.2F};
    std::uint64_t contentHash{};
};

struct HumanoidMarionetteSetup {
    ControlRigAsset controlRig;
    RagdollDefinition ragdoll;
    MarionetteAsset marionette;
};

struct MarionetteVrControllerState {
    bool tracked{};
    RigidTransform world{};
    float trigger{};
    float grip{};
    float stickX{};
    float stickY{};
};

struct MarionetteVrInput {
    MarionetteVrControllerState left;
    MarionetteVrControllerState right;
    bool enabled{};
};

struct MarionetteInputFrame {
    RigidTransform crossbarWorld{};
    std::array<float, kMarionetteStringCount> stringPullMeters{};
    Float3 bodyLeanLocal{};
    float bodyYawRadians{};
    float leftWristTwistRadians{};
    float rightWristTwistRadians{};
    float tensionScale{1.0F};
    bool requestModeCycle{};
    bool requestRecovery{};
    bool requestRecenter{};
    bool vrTracked{};
};

struct MarionetteGamepadSettings {
    float translationSpeedMetersPerSecond{1.5F};
    float verticalSpeedMetersPerSecond{1.0F};
    float rotationSpeedRadiansPerSecond{1.8F};
    float deadZone{0.12F};
    float maximumPullMeters{0.65F};
};

class MarionetteInputRouter {
public:
    explicit MarionetteInputRouter(MarionetteGamepadSettings settings = {}) noexcept;

    void reset(const RigidTransform& referenceWorld = {}) noexcept;
    void consume_gamepad_event(const platform::PlatformEvent& event) noexcept;
    void set_vr_input(MarionetteVrInput input) noexcept;
    [[nodiscard]] MarionetteInputFrame sample(
        float deltaSeconds, const RigidTransform& referenceWorld) noexcept;

private:
    MarionetteGamepadSettings settings_{};
    MarionetteVrInput vr_{};
    RigidTransform gamepadCrossbar_{};
    std::array<float, 6U> axes_{};
    std::array<bool, 32U> buttons_{};
    bool modePulse_{};
    bool recoveryPulse_{};
    bool recenterPulse_{};
};

struct MarionetteStringTelemetry {
    MarionetteStringRole role{MarionetteStringRole::Head};
    Float3 anchorWorld{};
    Float3 targetWorld{};
    Float3 bodyWorld{};
    float currentLengthMeters{};
    float desiredLengthMeters{};
    float extensionMeters{};
    float forceNewtons{};
    bool bodyAvailable{};
};

struct MarionetteTelemetry {
    MarionetteControlMode mode{MarionetteControlMode::AssistedRig};
    RigidTransform crossbarWorld{};
    std::array<MarionetteStringTelemetry, kMarionetteStringCount> strings{};
    bool rigEvaluated{};
    bool ragdollActive{};
    std::string lastError;
};

struct MarionetteRuntimeConfig {
    MarionetteControlMode initialMode{MarionetteControlMode::Hybrid};
    float maximumDeltaSeconds{1.0F / 15.0F};
    float controlBlend{1.0F};
    bool autoActivateRagdollForPhysicalMode{true};
};

[[nodiscard]] AnimationValidationResult validate_humanoid_marionette_bones(
    const SkeletonAsset& skeleton, const HumanoidMarionetteBones& bones) noexcept;
[[nodiscard]] AnimationValidationResult validate_marionette_asset(
    const ControlRigAsset& rig, const RagdollDefinition& ragdoll,
    const MarionetteAsset& asset) noexcept;
[[nodiscard]] std::uint64_t marionette_content_hash(const MarionetteAsset& asset) noexcept;
[[nodiscard]] bool build_humanoid_marionette_setup(
    const SkeletonAsset& skeleton, const HumanoidMarionetteBones& bones,
    HumanoidMarionetteSetup& output, std::string* error = nullptr);

class MarionetteRuntime {
public:
    MarionetteRuntime(
        SkeletalAnimationRuntime& animation, ControlRigRuntime& controlRig,
        RagdollRuntime& ragdolls, IRigidBodyWorld& physics) noexcept;

    [[nodiscard]] bool bind(
        std::uint64_t objectId, MarionetteAsset asset,
        MarionetteRuntimeConfig config = {}, std::string* error = nullptr);
    [[nodiscard]] bool unbind(std::uint64_t objectId) noexcept;
    [[nodiscard]] bool has_instance(std::uint64_t objectId) const noexcept;
    [[nodiscard]] bool set_mode(
        std::uint64_t objectId, MarionetteControlMode mode,
        const RigidTransform& objectWorld, std::string* error = nullptr);
    [[nodiscard]] MarionetteControlMode mode(std::uint64_t objectId) const noexcept;
    [[nodiscard]] bool tick(
        std::uint64_t objectId, float deltaSeconds,
        const RigidTransform& objectWorld, const MarionetteInputFrame& input,
        std::string* error = nullptr);
    [[nodiscard]] const MarionetteTelemetry* telemetry(std::uint64_t objectId) const noexcept;

private:
    struct Instance {
        MarionetteAsset asset;
        MarionetteRuntimeConfig config;
        MarionetteControlMode mode{MarionetteControlMode::Hybrid};
        MarionetteTelemetry telemetry;
    };

    [[nodiscard]] bool apply_assisted_pose(
        std::uint64_t objectId, Instance& instance,
        const RigidTransform& objectWorld, const MarionetteInputFrame& input,
        std::string* error);
    [[nodiscard]] bool apply_physical_strings(
        std::uint64_t objectId, Instance& instance, float deltaSeconds,
        const RigidTransform& objectWorld, const MarionetteInputFrame& input,
        std::string* error);

    SkeletalAnimationRuntime* animation_{};
    ControlRigRuntime* controlRig_{};
    RagdollRuntime* ragdolls_{};
    IRigidBodyWorld* physics_{};
    std::map<std::uint64_t, Instance> instances_;
};

[[nodiscard]] std::string_view marionette_string_role_name(MarionetteStringRole role) noexcept;
[[nodiscard]] std::string_view marionette_control_mode_name(MarionetteControlMode mode) noexcept;

} // namespace dve
