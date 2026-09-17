#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/sprite2d.hpp"

namespace dve {

using SpriteRigBoneId = std::uint64_t;
using SpriteRigPartId = std::uint64_t;
using SpriteRigConstraintId = std::uint64_t;
constexpr SpriteRigBoneId kInvalidSpriteRigBoneId = 0U;
constexpr SpriteRigPartId kInvalidSpriteRigPartId = 0U;

struct SpriteTransform2D {
    SpriteVec2 translation{};
    float rotationDegrees{};
    SpriteVec2 scale{1.0F, 1.0F};
};

struct SpriteRigBone2D {
    SpriteRigBoneId id{};
    std::string name;
    SpriteRigBoneId parent{kInvalidSpriteRigBoneId};
    SpriteTransform2D bind;
    float lengthPixels{8.0F};
};

struct SpriteRigPart2D {
    SpriteRigPartId id{};
    std::string name;
    SpriteRigBoneId bone{kInvalidSpriteRigBoneId};
    std::string spriteAsset;
    std::string clip;
    SpriteTransform2D local;
    std::int32_t drawOrder{};
    std::uint32_t paletteBank{};
    bool visible{true};
    std::array<float, 4> tint{1.0F, 1.0F, 1.0F, 1.0F};
};

struct SpriteRigBoneKey2D {
    SpriteRigBoneId bone{kInvalidSpriteRigBoneId};
    float timeSeconds{};
    SpriteTransform2D transform;
};

struct SpriteRigPartKey2D {
    SpriteRigPartId part{kInvalidSpriteRigPartId};
    float timeSeconds{};
    std::optional<std::string> spriteAsset;
    std::optional<std::string> clip;
    std::optional<bool> visible;
    std::optional<std::int32_t> drawOrder;
};

struct SpriteRigClip2D {
    std::string name;
    float durationSeconds{1.0F};
    bool looping{true};
    std::vector<SpriteRigBoneKey2D> boneKeys;
    std::vector<SpriteRigPartKey2D> partKeys;
};

struct SpriteRigTwoBoneIk2D {
    SpriteRigConstraintId id{};
    std::string name;
    SpriteRigBoneId upper{kInvalidSpriteRigBoneId};
    SpriteRigBoneId lower{kInvalidSpriteRigBoneId};
    SpriteVec2 targetPixels{};
    float bendSign{1.0F};
    float weight{1.0F};
};

struct SpriteRigPartOverride2D {
    std::string partName;
    std::optional<std::string> spriteAsset;
    std::optional<std::string> clip;
    std::optional<bool> visible;
    std::optional<std::uint32_t> paletteBank;
};

struct SpriteRigVariant2D {
    std::string name;
    std::vector<SpriteRigPartOverride2D> overrides;
};

struct SpriteRig2DAsset {
    std::string name{"Untitled Sprite Rig"};
    float pixelsPerWorldUnit{16.0F};
    std::vector<SpriteRigBone2D> bones;
    std::vector<SpriteRigPart2D> parts;
    std::vector<SpriteRigClip2D> clips;
    std::vector<SpriteRigTwoBoneIk2D> constraints;
    std::vector<SpriteRigVariant2D> variants;
    std::uint64_t contentHash{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    void recompute_hash() noexcept;
};

struct SpriteRigBonePose2D {
    SpriteRigBoneId id{};
    SpriteTransform2D local;
    SpriteTransform2D world;
};

struct SpriteRigPartPose2D {
    SpriteRigPartId id{};
    std::string name;
    std::string spriteAsset;
    std::string clip;
    SpriteTransform2D world;
    std::int32_t drawOrder{};
    std::uint32_t paletteBank{};
    bool visible{true};
    std::array<float, 4> tint{1.0F, 1.0F, 1.0F, 1.0F};
};

struct SpriteRigPose2D {
    std::string clip;
    float timeSeconds{};
    std::vector<SpriteRigBonePose2D> bones;
    std::vector<SpriteRigPartPose2D> parts;
    std::uint64_t poseHash{};
};

struct SpriteRigBakeFrame2D {
    float timeSeconds{};
    SpriteVec2 minimumPixels{};
    SpriteVec2 maximumPixels{};
    std::uint64_t poseHash{};
};

[[nodiscard]] bool sample_sprite_rig2d(const SpriteRig2DAsset& asset,
                                        std::string_view clipName,
                                        float timeSeconds,
                                        std::string_view variantName,
                                        SpriteRigPose2D& out,
                                        std::string* error = nullptr);
[[nodiscard]] bool build_sprite_rig2d_bake_plan(const SpriteRig2DAsset& asset,
                                                 std::string_view clipName,
                                                 std::string_view variantName,
                                                 float framesPerSecond,
                                                 std::vector<SpriteRigBakeFrame2D>& out,
                                                 std::string* error = nullptr);

[[nodiscard]] bool write_dvespriterig(const std::filesystem::path& path,
                                      const SpriteRig2DAsset& asset,
                                      std::string* error = nullptr);
struct SpriteRig2DReadResult {
    SpriteRig2DAsset asset;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
[[nodiscard]] SpriteRig2DReadResult read_dvespriterig(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes = 16ULL * 1024ULL * 1024ULL);

namespace editor {

class SpriteRig2DAuthoringSession {
public:
    explicit SpriteRig2DAuthoringSession(SpriteRig2DAsset asset = {});

    [[nodiscard]] const SpriteRig2DAsset& asset() const noexcept { return asset_; }
    [[nodiscard]] bool dirty() const noexcept { return asset_.contentHash != savedHash_; }
    [[nodiscard]] bool open(const std::filesystem::path& path, std::string* error = nullptr);
    [[nodiscard]] bool save(const std::filesystem::path& path, std::string* error = nullptr);

    [[nodiscard]] bool add_bone(std::string name, SpriteRigBoneId parent,
                                SpriteTransform2D bind, float lengthPixels,
                                SpriteRigBoneId* id = nullptr, std::string* error = nullptr);
    [[nodiscard]] bool remove_bone(SpriteRigBoneId id, bool reparentChildren,
                                   std::string* error = nullptr);
    [[nodiscard]] bool reparent_bone(SpriteRigBoneId id, SpriteRigBoneId parent,
                                     std::string* error = nullptr);
    [[nodiscard]] bool set_bone_bind(SpriteRigBoneId id, SpriteTransform2D bind,
                                     std::string* error = nullptr);
    [[nodiscard]] bool add_part(SpriteRigPart2D part, SpriteRigPartId* id = nullptr,
                                std::string* error = nullptr);
    [[nodiscard]] bool update_part(SpriteRigPartId id, SpriteRigPart2D replacement,
                                   std::string* error = nullptr);
    [[nodiscard]] bool remove_part(SpriteRigPartId id, std::string* error = nullptr);
    [[nodiscard]] bool add_clip(SpriteRigClip2D clip, std::string* error = nullptr);
    [[nodiscard]] bool upsert_bone_key(std::string_view clip, SpriteRigBoneKey2D key,
                                       std::string* error = nullptr);
    [[nodiscard]] bool upsert_part_key(std::string_view clip, SpriteRigPartKey2D key,
                                       std::string* error = nullptr);
    [[nodiscard]] bool add_two_bone_ik(SpriteRigTwoBoneIk2D constraint,
                                       SpriteRigConstraintId* id = nullptr,
                                       std::string* error = nullptr);
    [[nodiscard]] bool set_ik_target(SpriteRigConstraintId id, SpriteVec2 target,
                                     float weight, std::string* error = nullptr);
    [[nodiscard]] bool add_variant(SpriteRigVariant2D variant, std::string* error = nullptr);
    [[nodiscard]] bool duplicate_variant(std::string_view source, std::string name,
                                         std::string* error = nullptr);

    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] bool undo() noexcept;
    [[nodiscard]] bool redo() noexcept;

private:
    struct Snapshot { std::string label; SpriteRig2DAsset asset; };
    [[nodiscard]] bool commit(std::string label, SpriteRig2DAsset replacement,
                              std::string* error);
    SpriteRig2DAsset asset_;
    std::uint64_t savedHash_{};
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
};

} // namespace editor
} // namespace dve
