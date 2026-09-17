#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/animation_controller.hpp"
#include "dve/rigid_body_adapter.hpp"

namespace dve {

enum class RagdollRuntimeState : std::uint8_t { Animated, BlendingIn, Simulating, Recovering };
enum class RagdollRecoveryFacing : std::uint8_t { FaceUp, FaceDown };

struct RagdollRuntimeConfig {
    float blendInSeconds{0.15F};
    float settleLinearSpeed{0.08F};
    float settleAngularSpeed{0.12F};
    float settleSeconds{0.4F};
};

struct RagdollActivationOptions {
    Float3 linearVelocity{};
    Float3 angularVelocity{};
    Float3 impulse{};
    std::size_t impulseBody{};
    bool useContinuousCollision{true};
};

struct RagdollRecoveryOptions {
    std::string getUpClip; // fallback when an orientation-specific clip is empty
    std::string faceUpClip;
    std::string faceDownClip;
    float poseBlendSeconds{0.25F};
    std::size_t referenceBody{};
    bool alignPosition{true};
    bool alignRotation{true};
    bool uprightRotation{true};
};

// Owns live physics bodies/constraints for animation instances. Physics is stepped by the host;
// tick() runs afterward and publishes the physics/recovery pose into SkeletalAnimationRuntime.
class RagdollRuntime {
public:
    RagdollRuntime(SkeletalAnimationRuntime& animation, IRigidBodyWorld& physics) noexcept;
    ~RagdollRuntime();
    RagdollRuntime(const RagdollRuntime&) = delete;
    RagdollRuntime& operator=(const RagdollRuntime&) = delete;

    [[nodiscard]] bool bind(
        std::uint64_t objectId, RagdollDefinition definition,
        RagdollRuntimeConfig config = {}, std::string* error = nullptr);
    [[nodiscard]] bool unbind(std::uint64_t objectId) noexcept;
    [[nodiscard]] bool activate(
        std::uint64_t objectId, const RigidTransform& objectWorld,
        RagdollActivationOptions options = {}, std::string* error = nullptr);
    [[nodiscard]] bool begin_recovery(
        std::uint64_t objectId, const RigidTransform& currentObjectWorld,
        const RagdollRecoveryOptions& options, RigidTransform* alignedObjectWorld = nullptr,
        std::string* error = nullptr);
    [[nodiscard]] bool apply_impulse(
        std::uint64_t objectId, std::size_t bodyIndex, Float3 worldImpulse) noexcept;
    [[nodiscard]] bool tick(
        std::uint64_t objectId, float deltaSeconds, const RigidTransform& objectWorld,
        std::string* error = nullptr);

    [[nodiscard]] bool has_instance(std::uint64_t objectId) const noexcept;
    [[nodiscard]] bool owns_pose(std::uint64_t objectId) const noexcept;
    [[nodiscard]] bool settled(std::uint64_t objectId) const noexcept;
    [[nodiscard]] RagdollRuntimeState state(std::uint64_t objectId) const noexcept;
    [[nodiscard]] float blend_weight(std::uint64_t objectId) const noexcept;
    [[nodiscard]] std::optional<RagdollRecoveryFacing> recovery_facing(
        std::uint64_t objectId) const noexcept;
    [[nodiscard]] std::span<const RigidBodyHandle> body_handles(
        std::uint64_t objectId) const noexcept;
    [[nodiscard]] std::span<const RigidBodyConstraintHandle> constraint_handles(
        std::uint64_t objectId) const noexcept;
    [[nodiscard]] std::vector<std::uint64_t> object_ids() const;

private:
    struct Instance {
        RagdollDefinition definition;
        RagdollRuntimeConfig config;
        RagdollRuntimeState state{RagdollRuntimeState::Animated};
        RagdollBlendState blend;
        std::vector<RigidBodyHandle> bodies;
        std::vector<RigidBodyConstraintHandle> constraints;
        float quietSeconds{};
        bool settled{};
        LocalPose recoveryStart;
        LocalPose recoveryTarget;
        float recoveryElapsed{};
        float recoveryDuration{};
        float resumePlaybackSpeed{1.0F};
        std::optional<RagdollRecoveryFacing> facing;
    };

    void destroy_physics(Instance& instance) noexcept;
    [[nodiscard]] bool gather_body_world(
        const Instance& instance, std::vector<RigidTransform>& bodyWorld,
        std::string* error) const;

    SkeletalAnimationRuntime* animation_{};
    IRigidBodyWorld* physics_{};
    std::map<std::uint64_t, Instance> instances_;
};

} // namespace dve
