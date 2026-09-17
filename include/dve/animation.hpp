#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/transform.hpp"

namespace dve {

using BoneIndex = std::uint16_t;
inline constexpr BoneIndex kInvalidBoneIndex = std::numeric_limits<BoneIndex>::max();
inline constexpr std::size_t kMaximumSkeletonBones = 1024U;
inline constexpr std::size_t kMaximumSkinInfluences = 4U;

struct SkeletonBone {
    std::string name;
    std::int32_t parent{-1};
    RigidTransform bindLocal{};
};

struct SkeletalSocket {
    std::string name;
    BoneIndex bone{kInvalidBoneIndex};
    RigidTransform localTransform{};
};

struct SkeletonAsset {
    std::string name;
    std::vector<SkeletonBone> bones;
    std::vector<SkeletalSocket> sockets;
    std::uint64_t contentHash{};
};

struct TranslationKey {
    float timeSeconds{};
    Float3 value{};
};

struct RotationKey {
    float timeSeconds{};
    Quaternion value{};
};

struct BoneAnimationTrack {
    BoneIndex bone{kInvalidBoneIndex};
    std::vector<TranslationKey> translations;
    std::vector<RotationKey> rotations;
};

struct AnimationEventKey {
    float timeSeconds{};
    std::string name;
};

struct AnimationClipAsset {
    std::string name;
    float durationSeconds{};
    bool looping{true};
    std::vector<BoneAnimationTrack> tracks;
    std::vector<AnimationEventKey> events;
    std::uint64_t contentHash{};
};

struct AnimationValidationResult {
    bool valid{};
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept { return valid; }
};

struct SkeletonReadResult {
    SkeletonAsset asset;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

struct AnimationClipReadResult {
    AnimationClipAsset asset;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

using LocalPose = std::vector<RigidTransform>;

struct SkinInfluences {
    std::array<BoneIndex, kMaximumSkinInfluences> bones{
        kInvalidBoneIndex, kInvalidBoneIndex, kInvalidBoneIndex, kInvalidBoneIndex};
    std::array<float, kMaximumSkinInfluences> weights{};
};

struct SkinVertex {
    Float3 position{};
    Float3 normal{};
    SkinInfluences influences{};
};

struct CpuSkinnedVertex {
    Float3 position{};
    Float3 normal{};
};

struct RootMotionDelta {
    Float3 translation{};
    Quaternion rotation{};
};

[[nodiscard]] AnimationValidationResult validate_skeleton(const SkeletonAsset& asset) noexcept;
[[nodiscard]] AnimationValidationResult validate_animation_clip(
    const AnimationClipAsset& clip, const SkeletonAsset* skeleton = nullptr) noexcept;
[[nodiscard]] std::uint64_t skeleton_content_hash(const SkeletonAsset& asset) noexcept;
[[nodiscard]] std::uint64_t animation_clip_content_hash(const AnimationClipAsset& clip) noexcept;

[[nodiscard]] bool write_dveskeleton(
    const std::filesystem::path& path, const SkeletonAsset& asset, std::string* error = nullptr);
[[nodiscard]] SkeletonReadResult read_dveskeleton(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes = 16ULL * 1024ULL * 1024ULL);
[[nodiscard]] bool write_dveanim(
    const std::filesystem::path& path, const AnimationClipAsset& clip, std::string* error = nullptr);
[[nodiscard]] AnimationClipReadResult read_dveanim(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes = 64ULL * 1024ULL * 1024ULL);

[[nodiscard]] LocalPose make_bind_pose(const SkeletonAsset& skeleton);
[[nodiscard]] LocalPose sample_animation_clip(
    const SkeletonAsset& skeleton, const AnimationClipAsset& clip, float timeSeconds);
[[nodiscard]] LocalPose blend_local_poses(
    std::span<const RigidTransform> from, std::span<const RigidTransform> to, float weight);
[[nodiscard]] LocalPose blend_local_pose_stack(
    std::span<const LocalPose> poses, std::span<const float> weights,
    std::string* error = nullptr);
[[nodiscard]] std::vector<RigidTransform> compute_model_pose(
    const SkeletonAsset& skeleton, std::span<const RigidTransform> localPose,
    std::string* error = nullptr);
[[nodiscard]] bool skin_vertices_cpu(
    const SkeletonAsset& skeleton, std::span<const RigidTransform> localPose,
    std::span<const SkinVertex> vertices, std::vector<CpuSkinnedVertex>& output,
    std::string* error = nullptr);
[[nodiscard]] std::optional<RigidTransform> resolve_skeletal_socket(
    const SkeletonAsset& skeleton, std::span<const RigidTransform> localPose,
    std::string_view socketName, const RigidTransform& objectWorldTransform,
    std::string* error = nullptr);

// CPU reference animation service. It owns deterministic copies of skeletons and clips,
// evaluates local poses, and supplies socket transforms to GameWorld attachments. Rendering
// backends may consume its poses, but this foundation makes no GPU-skinning claim.
class SkeletalAnimationRuntime {
public:
    SkeletalAnimationRuntime();
    ~SkeletalAnimationRuntime();
    SkeletalAnimationRuntime(const SkeletalAnimationRuntime&) = delete;
    SkeletalAnimationRuntime& operator=(const SkeletalAnimationRuntime&) = delete;

    [[nodiscard]] bool bind_skeleton(
        std::uint64_t objectId, SkeletonAsset skeleton, std::string* error = nullptr);
    [[nodiscard]] bool unbind(std::uint64_t objectId) noexcept;
    [[nodiscard]] bool add_clip(
        std::uint64_t objectId, AnimationClipAsset clip, std::string* error = nullptr);
    [[nodiscard]] bool play(
        std::uint64_t objectId, std::string_view clipName, bool restart = true,
        std::string* error = nullptr);
    [[nodiscard]] bool crossfade(
        std::uint64_t objectId, std::string_view clipName, float durationSeconds,
        std::string* error = nullptr);
    [[nodiscard]] bool set_playback_speed(
        std::uint64_t objectId, float speed, std::string* error = nullptr);
    [[nodiscard]] bool set_root_motion_enabled(
        std::uint64_t objectId, bool enabled) noexcept;
    [[nodiscard]] std::optional<RootMotionDelta> consume_root_motion(
        std::uint64_t objectId) noexcept;
    [[nodiscard]] std::vector<std::uint64_t> root_motion_objects() const;
    [[nodiscard]] bool publish_local_pose(
        std::uint64_t objectId, std::span<const RigidTransform> pose,
        std::string* error = nullptr);
    void tick(float deltaSeconds);

    [[nodiscard]] bool has_instance(std::uint64_t objectId) const noexcept;
    [[nodiscard]] const SkeletonAsset* skeleton(std::uint64_t objectId) const noexcept;
    [[nodiscard]] const LocalPose* local_pose(std::uint64_t objectId) const noexcept;
    [[nodiscard]] std::string_view active_clip(std::uint64_t objectId) const noexcept;
    [[nodiscard]] std::optional<float> playback_speed(std::uint64_t objectId) const noexcept;
    [[nodiscard]] std::optional<RigidTransform> socket_world_transform(
        std::uint64_t objectId, std::string_view socketName,
        const RigidTransform& objectWorldTransform) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dve
