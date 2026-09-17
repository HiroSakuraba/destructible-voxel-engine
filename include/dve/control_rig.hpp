#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/animation_controller.hpp"

namespace dve {

using ControlRigControlId = std::uint64_t;
inline constexpr ControlRigControlId kInvalidControlRigControlId = 0U;
using ControlRigNodeId = std::uint64_t;
inline constexpr ControlRigNodeId kInvalidControlRigNodeId = 0U;

enum class ControlRigControlKind : std::uint8_t { Transform, Translation, Rotation };
enum class ControlRigSpace : std::uint8_t { Model, Bone, Control };
enum class ControlRigSolvePhase : std::uint8_t { PreSolve, ForwardSolve, PostSolve };
enum class ControlRigNodeKind : std::uint8_t {
    SetBoneTransform,
    CopyBoneTransform,
    ParentConstraint,
    AimConstraint,
    TwoBoneIk,
    Fabrik
};

struct ControlRigLimits {
    bool translationEnabled{};
    Float3 minimumTranslation{-100000.0F, -100000.0F, -100000.0F};
    Float3 maximumTranslation{100000.0F, 100000.0F, 100000.0F};
    bool rotationEnabled{};
    Float3 minimumRotationRadians{-3.1415927F, -3.1415927F, -3.1415927F};
    Float3 maximumRotationRadians{3.1415927F, 3.1415927F, 3.1415927F};
};

struct ControlRigControl {
    ControlRigControlId id{kInvalidControlRigControlId};
    std::string name;
    ControlRigControlKind kind{ControlRigControlKind::Transform};
    ControlRigSpace space{ControlRigSpace::Model};
    BoneIndex spaceBone{kInvalidBoneIndex};
    ControlRigControlId parentControl{kInvalidControlRigControlId};
    RigidTransform defaultLocal{};
    ControlRigLimits limits{};
};

// One compact node record keeps assets inspectable and deterministic. Fields irrelevant to a
// node kind are ignored; validate_control_rig() checks every field that participates in execution.
struct ControlRigNode {
    ControlRigNodeId id{kInvalidControlRigNodeId};
    std::string name;
    ControlRigNodeKind kind{ControlRigNodeKind::SetBoneTransform};
    ControlRigSolvePhase phase{ControlRigSolvePhase::ForwardSolve};
    bool enabled{true};
    float weight{1.0F};
    BoneIndex bone{kInvalidBoneIndex};
    BoneIndex sourceBone{kInvalidBoneIndex};
    BoneIndex middleBone{kInvalidBoneIndex};
    BoneIndex endBone{kInvalidBoneIndex};
    std::vector<BoneIndex> chain;
    ControlRigControlId targetControl{kInvalidControlRigControlId};
    ControlRigControlId poleControl{kInvalidControlRigControlId};
    RigidTransform offset{};
    Float3 localAimAxis{1.0F, 0.0F, 0.0F};
    bool affectTranslation{true};
    bool affectRotation{true};
    bool allowStretch{};
    float maximumStretch{1.0F};
    std::uint32_t maximumIterations{12U};
    float toleranceMeters{0.001F};
};

struct ControlRigAsset {
    std::string name;
    std::vector<ControlRigControl> controls;
    std::vector<ControlRigNode> nodes;
    std::uint64_t contentHash{};
};

struct ControlRigReadResult {
    ControlRigAsset asset;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

struct ControlRigTraceEntry {
    ControlRigNodeId node{kInvalidControlRigNodeId};
    ControlRigSolvePhase phase{ControlRigSolvePhase::ForwardSolve};
    float weight{};
    float maximumLocalTranslationDelta{};
    bool succeeded{};
};

struct ControlRigEvaluationTrace {
    std::map<ControlRigControlId, RigidTransform> controlModels;
    std::vector<ControlRigTraceEntry> nodes;
    std::string error;
};

[[nodiscard]] AnimationValidationResult validate_control_rig(
    const SkeletonAsset& skeleton, const ControlRigAsset& rig) noexcept;
[[nodiscard]] std::uint64_t control_rig_content_hash(const ControlRigAsset& rig) noexcept;
[[nodiscard]] bool write_dvecontrolrig(
    const std::filesystem::path& path, const SkeletonAsset& skeleton,
    const ControlRigAsset& rig, std::string* error = nullptr);
[[nodiscard]] ControlRigReadResult read_dvecontrolrig(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes = 16ULL * 1024ULL * 1024ULL);

// Evaluates a rig without owning animation state. controlLocals may omit controls, in which case
// their authored defaults are used. outControlModels receives the resolved model-space controls.
[[nodiscard]] bool evaluate_control_rig(
    const SkeletonAsset& skeleton, const ControlRigAsset& rig,
    std::span<const RigidTransform> inputPose,
    const std::map<ControlRigControlId, RigidTransform>& controlLocals,
    LocalPose& outputPose,
    std::map<ControlRigControlId, RigidTransform>* outControlModels = nullptr,
    std::string* error = nullptr);
[[nodiscard]] bool evaluate_control_rig_traced(
    const SkeletonAsset& skeleton, const ControlRigAsset& rig,
    std::span<const RigidTransform> inputPose,
    const std::map<ControlRigControlId, RigidTransform>& controlLocals,
    LocalPose& outputPose, ControlRigEvaluationTrace& trace,
    std::map<ControlRigControlId, RigidTransform>* outControlModels = nullptr,
    std::string* error = nullptr);

[[nodiscard]] bool bake_control_rig_clip(
    const SkeletonAsset& skeleton, const AnimationClipAsset& source,
    const ControlRigAsset& rig,
    const std::map<ControlRigControlId, RigidTransform>& controlLocals,
    float samplesPerSecond, std::string outputName,
    AnimationClipAsset& output, std::string* error = nullptr);

class ControlRigRuntime {
public:
    explicit ControlRigRuntime(SkeletalAnimationRuntime& animation) noexcept;

    [[nodiscard]] bool bind(
        std::uint64_t objectId, ControlRigAsset rig, std::string* error = nullptr);
    [[nodiscard]] bool unbind(std::uint64_t objectId) noexcept;
    [[nodiscard]] bool has_instance(std::uint64_t objectId) const noexcept;
    [[nodiscard]] bool set_enabled(std::uint64_t objectId, bool enabled) noexcept;
    [[nodiscard]] bool set_control_local(
        std::uint64_t objectId, ControlRigControlId control, RigidTransform value,
        std::string* error = nullptr);
    [[nodiscard]] bool set_control_local(
        std::uint64_t objectId, std::string_view controlName, RigidTransform value,
        std::string* error = nullptr);
    [[nodiscard]] bool reset_control(
        std::uint64_t objectId, ControlRigControlId control, std::string* error = nullptr);
    [[nodiscard]] const RigidTransform* control_local(
        std::uint64_t objectId, ControlRigControlId control) const noexcept;
    [[nodiscard]] const RigidTransform* control_model(
        std::uint64_t objectId, ControlRigControlId control) const noexcept;
    [[nodiscard]] std::string_view last_error(std::uint64_t objectId) const noexcept;
    [[nodiscard]] bool set_debug_tracing(std::uint64_t objectId, bool enabled) noexcept;
    [[nodiscard]] const ControlRigEvaluationTrace* evaluation_trace(
        std::uint64_t objectId) const noexcept;
    [[nodiscard]] bool evaluate(std::uint64_t objectId, std::string* error = nullptr);
    void evaluate_all();

private:
    struct Instance {
        ControlRigAsset rig;
        std::map<ControlRigControlId, RigidTransform> controlLocals;
        std::map<ControlRigControlId, RigidTransform> controlModels;
        std::string lastError;
        ControlRigEvaluationTrace trace;
        bool enabled{true};
        bool debugTracing{};
    };

    SkeletalAnimationRuntime* animation_{};
    std::map<std::uint64_t, Instance> instances_;
};

} // namespace dve
