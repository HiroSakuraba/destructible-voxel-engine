#include "dve/sprite_rig2d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <type_traits>
#include <unordered_map>

namespace dve {
namespace {

constexpr std::array<char, 8> kRigMagic{'D','V','E','R','I','G','2','D'};
constexpr std::uint32_t kRigVersion = 1U;
constexpr std::size_t kMaximumBones = 1024U;
constexpr std::size_t kMaximumParts = 4096U;
constexpr std::size_t kMaximumClips = 1024U;
constexpr std::size_t kMaximumKeys = 1U << 20U;
constexpr std::size_t kMaximumHistory = 128U;
constexpr float kPi = 3.14159265358979323846F;

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

template <class T>
void hash_value(std::uint64_t& hash, const T& value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* bytes = reinterpret_cast<const std::byte*>(&value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        hash ^= static_cast<std::uint8_t>(bytes[i]);
        hash *= 1099511628211ULL;
    }
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_value(hash, static_cast<std::uint64_t>(value.size()));
    for (char character : value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ULL;
    }
}

float radians(float degrees) noexcept { return degrees * (kPi / 180.0F); }
float degrees(float radiansValue) noexcept { return radiansValue * (180.0F / kPi); }

SpriteVec2 rotate(SpriteVec2 value, float degreesValue) noexcept {
    const float angle = radians(degreesValue);
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {value.x * c - value.y * s, value.x * s + value.y * c};
}

SpriteTransform2D compose(SpriteTransform2D parent, SpriteTransform2D local) noexcept {
    const SpriteVec2 scaled{local.translation.x * parent.scale.x,
                            local.translation.y * parent.scale.y};
    const SpriteVec2 rotated = rotate(scaled, parent.rotationDegrees);
    return {{parent.translation.x + rotated.x, parent.translation.y + rotated.y},
            parent.rotationDegrees + local.rotationDegrees,
            {parent.scale.x * local.scale.x, parent.scale.y * local.scale.y}};
}

SpriteTransform2D lerp_transform(const SpriteTransform2D& a, const SpriteTransform2D& b,
                                 float t) noexcept {
    auto lerp = [t](float x, float y) { return x + (y - x) * t; };
    return {{lerp(a.translation.x, b.translation.x), lerp(a.translation.y, b.translation.y)},
            lerp(a.rotationDegrees, b.rotationDegrees),
            {lerp(a.scale.x, b.scale.x), lerp(a.scale.y, b.scale.y)}};
}

const SpriteRigBone2D* find_bone(const SpriteRig2DAsset& asset, SpriteRigBoneId id) noexcept {
    const auto it = std::find_if(asset.bones.begin(), asset.bones.end(),
        [id](const auto& bone) { return bone.id == id; });
    return it == asset.bones.end() ? nullptr : &*it;
}

SpriteRigBone2D* find_bone(SpriteRig2DAsset& asset, SpriteRigBoneId id) noexcept {
    const auto it = std::find_if(asset.bones.begin(), asset.bones.end(),
        [id](const auto& bone) { return bone.id == id; });
    return it == asset.bones.end() ? nullptr : &*it;
}

SpriteRigPart2D* find_part(SpriteRig2DAsset& asset, SpriteRigPartId id) noexcept {
    const auto it = std::find_if(asset.parts.begin(), asset.parts.end(),
        [id](const auto& part) { return part.id == id; });
    return it == asset.parts.end() ? nullptr : &*it;
}

const SpriteRigClip2D* find_clip(const SpriteRig2DAsset& asset, std::string_view name) noexcept {
    const auto it = std::find_if(asset.clips.begin(), asset.clips.end(),
        [name](const auto& clip) { return clip.name == name; });
    return it == asset.clips.end() ? nullptr : &*it;
}

SpriteRigClip2D* find_clip(SpriteRig2DAsset& asset, std::string_view name) noexcept {
    const auto it = std::find_if(asset.clips.begin(), asset.clips.end(),
        [name](const auto& clip) { return clip.name == name; });
    return it == asset.clips.end() ? nullptr : &*it;
}

SpriteRigConstraintId next_constraint_id(const SpriteRig2DAsset& asset) noexcept {
    SpriteRigConstraintId id = 1U;
    for (const auto& constraint : asset.constraints) id = std::max(id, constraint.id + 1U);
    return id;
}

SpriteRigBoneId next_bone_id(const SpriteRig2DAsset& asset) noexcept {
    SpriteRigBoneId id = 1U;
    for (const auto& bone : asset.bones) id = std::max(id, bone.id + 1U);
    return id;
}

SpriteRigPartId next_part_id(const SpriteRig2DAsset& asset) noexcept {
    SpriteRigPartId id = 1U;
    for (const auto& part : asset.parts) id = std::max(id, part.id + 1U);
    return id;
}

bool finite_transform(const SpriteTransform2D& transform) noexcept {
    return std::isfinite(transform.translation.x) && std::isfinite(transform.translation.y) &&
           std::isfinite(transform.rotationDegrees) &&
           std::isfinite(transform.scale.x) && std::isfinite(transform.scale.y) &&
           transform.scale.x != 0.0F && transform.scale.y != 0.0F;
}

template <class T>
bool write_scalar(std::ofstream& stream, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    stream.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(stream);
}

bool write_string(std::ofstream& stream, std::string_view value) {
    const auto count = static_cast<std::uint32_t>(value.size());
    if (!write_scalar(stream, count)) return false;
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    return static_cast<bool>(stream);
}

template <class T>
bool read_scalar(std::ifstream& stream, T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    stream.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(stream);
}

bool read_string(std::ifstream& stream, std::string& value, std::uint32_t maximum = 1U << 20U) {
    std::uint32_t count{};
    if (!read_scalar(stream, count) || count > maximum) return false;
    value.resize(count);
    stream.read(value.data(), static_cast<std::streamsize>(count));
    return static_cast<bool>(stream);
}

bool atomic_replace(const std::filesystem::path& temp, const std::filesystem::path& path,
                    std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    ec.clear();
    std::filesystem::rename(temp, path, ec);
    if (!ec) return true;
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(temp, path, ec);
    if (ec) return fail(error, "could not publish sprite rig: " + ec.message());
    return true;
}

void hash_transform(std::uint64_t& hash, const SpriteTransform2D& transform) noexcept {
    hash_value(hash, transform.translation.x);
    hash_value(hash, transform.translation.y);
    hash_value(hash, transform.rotationDegrees);
    hash_value(hash, transform.scale.x);
    hash_value(hash, transform.scale.y);
}

} // namespace

bool SpriteRig2DAsset::validate(std::string* error) const {
    if (name.empty()) return fail(error, "sprite rig name is empty");
    if (!std::isfinite(pixelsPerWorldUnit) || pixelsPerWorldUnit <= 0.0F)
        return fail(error, "sprite rig pixels-per-world-unit is invalid");
    if (bones.empty() || bones.size() > kMaximumBones || parts.size() > kMaximumParts ||
        clips.size() > kMaximumClips) return fail(error, "sprite rig count exceeds a production bound");
    std::set<SpriteRigBoneId> boneIds;
    std::set<std::string> boneNames;
    for (const auto& bone : bones) {
        if (bone.id == 0U || bone.name.empty() || !boneIds.insert(bone.id).second ||
            !boneNames.insert(bone.name).second || !finite_transform(bone.bind) ||
            !std::isfinite(bone.lengthPixels) || bone.lengthPixels <= 0.0F)
            return fail(error, "sprite rig contains an invalid or duplicate bone");
    }
    for (const auto& bone : bones) {
        if (bone.parent != 0U && !boneIds.contains(bone.parent))
            return fail(error, "sprite rig bone references a missing parent");
        std::set<SpriteRigBoneId> ancestors;
        const SpriteRigBone2D* current = &bone;
        while (current && current->parent != 0U) {
            if (!ancestors.insert(current->id).second)
                return fail(error, "sprite rig bone hierarchy contains a cycle");
            current = find_bone(*this, current->parent);
        }
    }
    std::set<SpriteRigPartId> partIds;
    std::set<std::string> partNames;
    for (const auto& part : parts) {
        if (part.id == 0U || part.name.empty() || !partIds.insert(part.id).second ||
            !partNames.insert(part.name).second || !boneIds.contains(part.bone) ||
            part.spriteAsset.empty() || !finite_transform(part.local))
            return fail(error, "sprite rig contains an invalid or duplicate part");
        for (float channel : part.tint)
            if (!std::isfinite(channel) || channel < 0.0F)
                return fail(error, "sprite rig part tint is invalid");
    }
    std::set<std::string> clipNames;
    std::size_t totalKeys{};
    for (const auto& clip : clips) {
        if (clip.name.empty() || !clipNames.insert(clip.name).second ||
            !std::isfinite(clip.durationSeconds) || clip.durationSeconds <= 0.0F)
            return fail(error, "sprite rig contains an invalid or duplicate clip");
        totalKeys += clip.boneKeys.size() + clip.partKeys.size();
        if (totalKeys > kMaximumKeys) return fail(error, "sprite rig key count exceeds the production bound");
        for (const auto& key : clip.boneKeys)
            if (!boneIds.contains(key.bone) || !std::isfinite(key.timeSeconds) || key.timeSeconds < 0.0F ||
                key.timeSeconds > clip.durationSeconds || !finite_transform(key.transform))
                return fail(error, "sprite rig bone key is invalid");
        for (const auto& key : clip.partKeys)
            if (!partIds.contains(key.part) || !std::isfinite(key.timeSeconds) || key.timeSeconds < 0.0F ||
                key.timeSeconds > clip.durationSeconds)
                return fail(error, "sprite rig part key is invalid");
    }
    std::set<SpriteRigConstraintId> constraintIds;
    for (const auto& constraint : constraints) {
        if (constraint.id == 0U || constraint.name.empty() || !constraintIds.insert(constraint.id).second ||
            !boneIds.contains(constraint.upper) || !boneIds.contains(constraint.lower) ||
            !std::isfinite(constraint.targetPixels.x) || !std::isfinite(constraint.targetPixels.y) ||
            !std::isfinite(constraint.bendSign) || !std::isfinite(constraint.weight) ||
            constraint.weight < 0.0F || constraint.weight > 1.0F)
            return fail(error, "sprite rig IK constraint is invalid");
        const auto* lower = find_bone(*this, constraint.lower);
        if (!lower || lower->parent != constraint.upper)
            return fail(error, "sprite rig two-bone IK lower bone must be parented to upper bone");
    }
    std::set<std::string> variantNames;
    for (const auto& variant : variants) {
        if (variant.name.empty() || !variantNames.insert(variant.name).second)
            return fail(error, "sprite rig contains an invalid or duplicate variant");
        std::set<std::string> overrideParts;
        for (const auto& overrideValue : variant.overrides)
            if (!partNames.contains(overrideValue.partName) || !overrideParts.insert(overrideValue.partName).second)
                return fail(error, "sprite rig variant references a missing or duplicate part");
    }
    return true;
}

void SpriteRig2DAsset::recompute_hash() noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    hash_string(hash, name);
    hash_value(hash, pixelsPerWorldUnit);
    for (const auto& bone : bones) {
        hash_value(hash, bone.id); hash_string(hash, bone.name); hash_value(hash, bone.parent);
        hash_transform(hash, bone.bind); hash_value(hash, bone.lengthPixels);
    }
    for (const auto& part : parts) {
        hash_value(hash, part.id); hash_string(hash, part.name); hash_value(hash, part.bone);
        hash_string(hash, part.spriteAsset); hash_string(hash, part.clip); hash_transform(hash, part.local);
        hash_value(hash, part.drawOrder); hash_value(hash, part.paletteBank); hash_value(hash, part.visible);
        for (float channel : part.tint) hash_value(hash, channel);
    }
    for (const auto& clip : clips) {
        hash_string(hash, clip.name); hash_value(hash, clip.durationSeconds); hash_value(hash, clip.looping);
        for (const auto& key : clip.boneKeys) {
            hash_value(hash, key.bone); hash_value(hash, key.timeSeconds); hash_transform(hash, key.transform);
        }
        for (const auto& key : clip.partKeys) {
            hash_value(hash, key.part); hash_value(hash, key.timeSeconds);
            hash_value(hash, key.spriteAsset.has_value()); if (key.spriteAsset) hash_string(hash, *key.spriteAsset);
            hash_value(hash, key.clip.has_value()); if (key.clip) hash_string(hash, *key.clip);
            hash_value(hash, key.visible.has_value()); if (key.visible) hash_value(hash, *key.visible);
            hash_value(hash, key.drawOrder.has_value()); if (key.drawOrder) hash_value(hash, *key.drawOrder);
        }
    }
    for (const auto& constraint : constraints) {
        hash_value(hash, constraint.id); hash_string(hash, constraint.name);
        hash_value(hash, constraint.upper); hash_value(hash, constraint.lower);
        hash_value(hash, constraint.targetPixels.x); hash_value(hash, constraint.targetPixels.y);
        hash_value(hash, constraint.bendSign); hash_value(hash, constraint.weight);
    }
    for (const auto& variant : variants) {
        hash_string(hash, variant.name);
        for (const auto& overrideValue : variant.overrides) {
            hash_string(hash, overrideValue.partName);
            hash_value(hash, overrideValue.spriteAsset.has_value()); if (overrideValue.spriteAsset) hash_string(hash, *overrideValue.spriteAsset);
            hash_value(hash, overrideValue.clip.has_value()); if (overrideValue.clip) hash_string(hash, *overrideValue.clip);
            hash_value(hash, overrideValue.visible.has_value()); if (overrideValue.visible) hash_value(hash, *overrideValue.visible);
            hash_value(hash, overrideValue.paletteBank.has_value()); if (overrideValue.paletteBank) hash_value(hash, *overrideValue.paletteBank);
        }
    }
    contentHash = hash;
}

bool sample_sprite_rig2d(const SpriteRig2DAsset& asset, std::string_view clipName,
                         float timeSeconds, std::string_view variantName,
                         SpriteRigPose2D& out, std::string* error) {
    if (!asset.validate(error)) return false;
    const SpriteRigClip2D* clip = clipName.empty() ? nullptr : find_clip(asset, clipName);
    if (!clipName.empty() && !clip) return fail(error, "sprite rig clip was not found");
    float sampleTime = std::max(0.0F, timeSeconds);
    if (clip) {
        if (clip->looping) sampleTime = std::fmod(sampleTime, clip->durationSeconds);
        else sampleTime = std::min(sampleTime, clip->durationSeconds);
    }
    out = {};
    out.clip = std::string(clipName);
    out.timeSeconds = sampleTime;
    out.bones.reserve(asset.bones.size());
    for (const auto& bone : asset.bones) {
        SpriteTransform2D local = bone.bind;
        if (clip) {
            std::vector<const SpriteRigBoneKey2D*> keys;
            for (const auto& key : clip->boneKeys) if (key.bone == bone.id) keys.push_back(&key);
            std::sort(keys.begin(), keys.end(), [](const auto* a, const auto* b) { return a->timeSeconds < b->timeSeconds; });
            if (!keys.empty()) {
                const SpriteRigBoneKey2D* before = keys.front();
                const SpriteRigBoneKey2D* after = keys.back();
                for (const auto* key : keys) {
                    if (key->timeSeconds <= sampleTime) before = key;
                    if (key->timeSeconds >= sampleTime) { after = key; break; }
                }
                const float denominator = after->timeSeconds - before->timeSeconds;
                const float factor = denominator > 0.0F ? (sampleTime - before->timeSeconds) / denominator : 0.0F;
                local = lerp_transform(before->transform, after->transform, std::clamp(factor, 0.0F, 1.0F));
            }
        }
        out.bones.push_back({bone.id, local, local});
    }
    auto poseFor = [&](SpriteRigBoneId id) -> SpriteRigBonePose2D* {
        const auto it = std::find_if(out.bones.begin(), out.bones.end(), [id](const auto& pose) { return pose.id == id; });
        return it == out.bones.end() ? nullptr : &*it;
    };
    for (std::size_t pass = 0; pass < asset.bones.size(); ++pass) {
        bool changed{};
        for (const auto& bone : asset.bones) {
            auto* pose = poseFor(bone.id);
            if (!pose) continue;
            if (bone.parent == 0U) { pose->world = pose->local; continue; }
            auto* parent = poseFor(bone.parent);
            if (!parent) continue;
            const auto world = compose(parent->world, pose->local);
            if (world.translation.x != pose->world.translation.x || world.translation.y != pose->world.translation.y ||
                world.rotationDegrees != pose->world.rotationDegrees) changed = true;
            pose->world = world;
        }
        if (!changed) break;
    }
    for (const auto& constraint : asset.constraints) {
        if (constraint.weight <= 0.0F) continue;
        auto* upperPose = poseFor(constraint.upper);
        auto* lowerPose = poseFor(constraint.lower);
        const auto* upperBone = find_bone(asset, constraint.upper);
        const auto* lowerBone = find_bone(asset, constraint.lower);
        if (!upperPose || !lowerPose || !upperBone || !lowerBone) continue;
        const SpriteVec2 origin = upperPose->world.translation;
        const float dx = constraint.targetPixels.x - origin.x;
        const float dy = constraint.targetPixels.y - origin.y;
        const float distance = std::max(0.0001F, std::sqrt(dx * dx + dy * dy));
        const float a = upperBone->lengthPixels;
        const float b = lowerBone->lengthPixels;
        const float clampedDistance = std::clamp(distance, std::abs(a - b) + 0.0001F, a + b - 0.0001F);
        const float targetAngle = std::atan2(dy, dx);
        const float shoulderOffset = std::acos(std::clamp((a * a + clampedDistance * clampedDistance - b * b) /
                                                          (2.0F * a * clampedDistance), -1.0F, 1.0F));
        const float elbow = std::acos(std::clamp((a * a + b * b - clampedDistance * clampedDistance) /
                                                 (2.0F * a * b), -1.0F, 1.0F));
        const float absoluteUpper = degrees(targetAngle - std::copysign(shoulderOffset, constraint.bendSign));
        const float parentRotation = upperBone->parent == 0U ? 0.0F : poseFor(upperBone->parent)->world.rotationDegrees;
        const float desiredUpperLocal = absoluteUpper - parentRotation;
        const float desiredLowerLocal = std::copysign(180.0F - degrees(elbow), constraint.bendSign);
        upperPose->local.rotationDegrees += (desiredUpperLocal - upperPose->local.rotationDegrees) * constraint.weight;
        lowerPose->local.rotationDegrees += (desiredLowerLocal - lowerPose->local.rotationDegrees) * constraint.weight;
        // Recompute the complete hierarchy after each constraint.
        for (std::size_t pass = 0; pass < asset.bones.size(); ++pass) {
            for (const auto& bone : asset.bones) {
                auto* pose = poseFor(bone.id);
                if (bone.parent == 0U) pose->world = pose->local;
                else if (auto* parent = poseFor(bone.parent)) pose->world = compose(parent->world, pose->local);
            }
        }
    }

    const SpriteRigVariant2D* variant = nullptr;
    if (!variantName.empty()) {
        const auto it = std::find_if(asset.variants.begin(), asset.variants.end(),
            [variantName](const auto& candidate) { return candidate.name == variantName; });
        if (it == asset.variants.end()) return fail(error, "sprite rig variant was not found");
        variant = &*it;
    }
    out.parts.reserve(asset.parts.size());
    for (const auto& part : asset.parts) {
        SpriteRigPartPose2D pose;
        pose.id = part.id; pose.name = part.name; pose.spriteAsset = part.spriteAsset; pose.clip = part.clip;
        pose.drawOrder = part.drawOrder; pose.paletteBank = part.paletteBank; pose.visible = part.visible; pose.tint = part.tint;
        if (clip) {
            std::vector<const SpriteRigPartKey2D*> keys;
            for (const auto& key : clip->partKeys) if (key.part == part.id && key.timeSeconds <= sampleTime) keys.push_back(&key);
            if (!keys.empty()) {
                const auto* key = *std::max_element(keys.begin(), keys.end(),
                    [](const auto* a, const auto* b) { return a->timeSeconds < b->timeSeconds; });
                if (key->spriteAsset) pose.spriteAsset = *key->spriteAsset;
                if (key->clip) pose.clip = *key->clip;
                if (key->visible) pose.visible = *key->visible;
                if (key->drawOrder) pose.drawOrder = *key->drawOrder;
            }
        }
        if (variant) {
            const auto overrideIt = std::find_if(variant->overrides.begin(), variant->overrides.end(),
                [&](const auto& overrideValue) { return overrideValue.partName == part.name; });
            if (overrideIt != variant->overrides.end()) {
                if (overrideIt->spriteAsset) pose.spriteAsset = *overrideIt->spriteAsset;
                if (overrideIt->clip) pose.clip = *overrideIt->clip;
                if (overrideIt->visible) pose.visible = *overrideIt->visible;
                if (overrideIt->paletteBank) pose.paletteBank = *overrideIt->paletteBank;
            }
        }
        const auto* bonePose = poseFor(part.bone);
        pose.world = bonePose ? compose(bonePose->world, part.local) : part.local;
        out.parts.push_back(std::move(pose));
    }
    std::stable_sort(out.parts.begin(), out.parts.end(), [](const auto& a, const auto& b) {
        if (a.drawOrder != b.drawOrder) return a.drawOrder < b.drawOrder;
        return a.id < b.id;
    });
    std::uint64_t hash = 1469598103934665603ULL;
    hash_string(hash, out.clip); hash_value(hash, out.timeSeconds);
    for (const auto& bone : out.bones) { hash_value(hash, bone.id); hash_transform(hash, bone.world); }
    for (const auto& part : out.parts) {
        hash_value(hash, part.id); hash_string(hash, part.spriteAsset); hash_string(hash, part.clip);
        hash_transform(hash, part.world); hash_value(hash, part.drawOrder); hash_value(hash, part.paletteBank); hash_value(hash, part.visible);
    }
    out.poseHash = hash;
    return true;
}

bool build_sprite_rig2d_bake_plan(const SpriteRig2DAsset& asset, std::string_view clipName,
                                  std::string_view variantName, float framesPerSecond,
                                  std::vector<SpriteRigBakeFrame2D>& out,
                                  std::string* error) {
    const auto* clip = find_clip(asset, clipName);
    if (!clip) return fail(error, "sprite rig bake clip was not found");
    if (!std::isfinite(framesPerSecond) || framesPerSecond <= 0.0F || framesPerSecond > 1000.0F)
        return fail(error, "sprite rig bake rate is invalid");
    const std::size_t frames = std::max<std::size_t>(1U, static_cast<std::size_t>(std::ceil(clip->durationSeconds * framesPerSecond)));
    if (frames > 100000U) return fail(error, "sprite rig bake plan exceeds 100000 frames");
    out.clear(); out.reserve(frames);
    for (std::size_t index = 0; index < frames; ++index) {
        const float time = static_cast<float>(index) / framesPerSecond;
        SpriteRigPose2D pose;
        if (!sample_sprite_rig2d(asset, clipName, time, variantName, pose, error)) return false;
        SpriteVec2 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        SpriteVec2 maximum{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
        bool any{};
        for (const auto& part : pose.parts) {
            if (!part.visible) continue;
            minimum.x = std::min(minimum.x, part.world.translation.x);
            minimum.y = std::min(minimum.y, part.world.translation.y);
            maximum.x = std::max(maximum.x, part.world.translation.x);
            maximum.y = std::max(maximum.y, part.world.translation.y);
            any = true;
        }
        if (!any) minimum = maximum = {};
        out.push_back({time, minimum, maximum, pose.poseHash});
    }
    return true;
}

bool write_dvespriterig(const std::filesystem::path& path, const SpriteRig2DAsset& source,
                        std::string* error) {
    SpriteRig2DAsset asset = source;
    asset.recompute_hash();
    if (!asset.validate(error)) return false;
    const auto temp = path.string() + ".tmp";
    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
    if (!stream) return fail(error, "could not open temporary sprite-rig output");
    stream.write(kRigMagic.data(), static_cast<std::streamsize>(kRigMagic.size()));
    if (!write_scalar(stream, kRigVersion) || !write_string(stream, asset.name) ||
        !write_scalar(stream, asset.pixelsPerWorldUnit)) return fail(error, "could not write sprite-rig header");
    auto writeCount = [&](std::size_t size) { return write_scalar(stream, static_cast<std::uint32_t>(size)); };
    if (!writeCount(asset.bones.size())) return fail(error, "could not write sprite-rig bone count");
    for (const auto& bone : asset.bones)
        if (!write_scalar(stream, bone.id) || !write_string(stream, bone.name) || !write_scalar(stream, bone.parent) ||
            !write_scalar(stream, bone.bind) || !write_scalar(stream, bone.lengthPixels)) return fail(error, "could not write sprite-rig bone");
    if (!writeCount(asset.parts.size())) return fail(error, "could not write sprite-rig part count");
    for (const auto& part : asset.parts)
        if (!write_scalar(stream, part.id) || !write_string(stream, part.name) || !write_scalar(stream, part.bone) ||
            !write_string(stream, part.spriteAsset) || !write_string(stream, part.clip) || !write_scalar(stream, part.local) ||
            !write_scalar(stream, part.drawOrder) || !write_scalar(stream, part.paletteBank) || !write_scalar(stream, part.visible) ||
            !write_scalar(stream, part.tint)) return fail(error, "could not write sprite-rig part");
    if (!writeCount(asset.clips.size())) return fail(error, "could not write sprite-rig clip count");
    for (const auto& clip : asset.clips) {
        if (!write_string(stream, clip.name) || !write_scalar(stream, clip.durationSeconds) || !write_scalar(stream, clip.looping) ||
            !writeCount(clip.boneKeys.size())) return fail(error, "could not write sprite-rig clip");
        for (const auto& key : clip.boneKeys)
            if (!write_scalar(stream, key.bone) || !write_scalar(stream, key.timeSeconds) || !write_scalar(stream, key.transform))
                return fail(error, "could not write sprite-rig bone key");
        if (!writeCount(clip.partKeys.size())) return fail(error, "could not write sprite-rig part-key count");
        for (const auto& key : clip.partKeys) {
            if (!write_scalar(stream, key.part) || !write_scalar(stream, key.timeSeconds)) return fail(error, "could not write sprite-rig part key");
            const bool hasAsset = key.spriteAsset.has_value(), hasClip = key.clip.has_value(),
                       hasVisible = key.visible.has_value(), hasOrder = key.drawOrder.has_value();
            if (!write_scalar(stream, hasAsset) || (hasAsset && !write_string(stream, *key.spriteAsset)) ||
                !write_scalar(stream, hasClip) || (hasClip && !write_string(stream, *key.clip)) ||
                !write_scalar(stream, hasVisible) || (hasVisible && !write_scalar(stream, *key.visible)) ||
                !write_scalar(stream, hasOrder) || (hasOrder && !write_scalar(stream, *key.drawOrder)))
                return fail(error, "could not write sprite-rig part key fields");
        }
    }
    if (!writeCount(asset.constraints.size())) return fail(error, "could not write sprite-rig constraint count");
    for (const auto& constraint : asset.constraints)
        if (!write_scalar(stream, constraint.id) || !write_string(stream, constraint.name) ||
            !write_scalar(stream, constraint.upper) || !write_scalar(stream, constraint.lower) ||
            !write_scalar(stream, constraint.targetPixels) || !write_scalar(stream, constraint.bendSign) ||
            !write_scalar(stream, constraint.weight)) return fail(error, "could not write sprite-rig constraint");
    if (!writeCount(asset.variants.size())) return fail(error, "could not write sprite-rig variant count");
    for (const auto& variant : asset.variants) {
        if (!write_string(stream, variant.name) || !writeCount(variant.overrides.size())) return fail(error, "could not write sprite-rig variant");
        for (const auto& overrideValue : variant.overrides) {
            if (!write_string(stream, overrideValue.partName)) return fail(error, "could not write sprite-rig override");
            const bool hasAsset = overrideValue.spriteAsset.has_value(), hasClip = overrideValue.clip.has_value(),
                       hasVisible = overrideValue.visible.has_value(), hasPalette = overrideValue.paletteBank.has_value();
            if (!write_scalar(stream, hasAsset) || (hasAsset && !write_string(stream, *overrideValue.spriteAsset)) ||
                !write_scalar(stream, hasClip) || (hasClip && !write_string(stream, *overrideValue.clip)) ||
                !write_scalar(stream, hasVisible) || (hasVisible && !write_scalar(stream, *overrideValue.visible)) ||
                !write_scalar(stream, hasPalette) || (hasPalette && !write_scalar(stream, *overrideValue.paletteBank)))
                return fail(error, "could not write sprite-rig override fields");
        }
    }
    if (!write_scalar(stream, asset.contentHash)) return fail(error, "could not write sprite-rig hash");
    stream.close();
    if (!stream) return fail(error, "could not finish sprite-rig output");
    return atomic_replace(temp, path, error);
}

SpriteRig2DReadResult read_dvespriterig(const std::filesystem::path& path, std::uint64_t maximumBytes) {
    SpriteRig2DReadResult result;
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes > maximumBytes) { result.error = "sprite-rig file is missing or exceeds the read limit"; return result; }
    std::ifstream stream(path, std::ios::binary);
    std::array<char, 8> magic{}; std::uint32_t version{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!stream || magic != kRigMagic || !read_scalar(stream, version) || version != kRigVersion) {
        result.error = "sprite-rig header is invalid"; return result;
    }
    auto readCount = [&](std::uint32_t& count, std::uint32_t maximum) {
        return read_scalar(stream, count) && count <= maximum;
    };
    if (!read_string(stream, result.asset.name) || !read_scalar(stream, result.asset.pixelsPerWorldUnit)) {
        result.error = "sprite-rig header is truncated"; return result;
    }
    std::uint32_t count{};
    if (!readCount(count, static_cast<std::uint32_t>(kMaximumBones))) { result.error = "sprite-rig bone count is invalid"; return result; }
    for (std::uint32_t i = 0; i < count; ++i) {
        SpriteRigBone2D bone;
        if (!read_scalar(stream, bone.id) || !read_string(stream, bone.name) || !read_scalar(stream, bone.parent) ||
            !read_scalar(stream, bone.bind) || !read_scalar(stream, bone.lengthPixels)) { result.error = "sprite-rig bone is truncated"; return result; }
        result.asset.bones.push_back(std::move(bone));
    }
    if (!readCount(count, static_cast<std::uint32_t>(kMaximumParts))) { result.error = "sprite-rig part count is invalid"; return result; }
    for (std::uint32_t i = 0; i < count; ++i) {
        SpriteRigPart2D part;
        if (!read_scalar(stream, part.id) || !read_string(stream, part.name) || !read_scalar(stream, part.bone) ||
            !read_string(stream, part.spriteAsset) || !read_string(stream, part.clip) || !read_scalar(stream, part.local) ||
            !read_scalar(stream, part.drawOrder) || !read_scalar(stream, part.paletteBank) || !read_scalar(stream, part.visible) ||
            !read_scalar(stream, part.tint)) { result.error = "sprite-rig part is truncated"; return result; }
        result.asset.parts.push_back(std::move(part));
    }
    if (!readCount(count, static_cast<std::uint32_t>(kMaximumClips))) { result.error = "sprite-rig clip count is invalid"; return result; }
    std::size_t totalKeys{};
    for (std::uint32_t i = 0; i < count; ++i) {
        SpriteRigClip2D clip; std::uint32_t keyCount{};
        if (!read_string(stream, clip.name) || !read_scalar(stream, clip.durationSeconds) || !read_scalar(stream, clip.looping) ||
            !readCount(keyCount, static_cast<std::uint32_t>(kMaximumKeys))) { result.error = "sprite-rig clip is truncated"; return result; }
        totalKeys += keyCount;
        if (totalKeys > kMaximumKeys) { result.error = "sprite-rig key count exceeds the production bound"; return result; }
        for (std::uint32_t keyIndex = 0; keyIndex < keyCount; ++keyIndex) {
            SpriteRigBoneKey2D key;
            if (!read_scalar(stream, key.bone) || !read_scalar(stream, key.timeSeconds) || !read_scalar(stream, key.transform)) {
                result.error = "sprite-rig bone key is truncated"; return result;
            }
            clip.boneKeys.push_back(key);
        }
        if (!readCount(keyCount, static_cast<std::uint32_t>(kMaximumKeys))) { result.error = "sprite-rig part-key count is invalid"; return result; }
        totalKeys += keyCount;
        if (totalKeys > kMaximumKeys) { result.error = "sprite-rig key count exceeds the production bound"; return result; }
        for (std::uint32_t keyIndex = 0; keyIndex < keyCount; ++keyIndex) {
            SpriteRigPartKey2D key; bool present{};
            if (!read_scalar(stream, key.part) || !read_scalar(stream, key.timeSeconds) || !read_scalar(stream, present)) {
                result.error = "sprite-rig part key is truncated"; return result;
            }
            if (present) { std::string value; if (!read_string(stream, value)) { result.error = "sprite-rig part asset is truncated"; return result; } key.spriteAsset = std::move(value); }
            if (!read_scalar(stream, present)) { result.error = "sprite-rig part clip flag is truncated"; return result; }
            if (present) { std::string value; if (!read_string(stream, value)) { result.error = "sprite-rig part clip is truncated"; return result; } key.clip = std::move(value); }
            if (!read_scalar(stream, present)) { result.error = "sprite-rig part visibility flag is truncated"; return result; }
            if (present) { bool value{}; if (!read_scalar(stream, value)) { result.error = "sprite-rig part visibility is truncated"; return result; } key.visible = value; }
            if (!read_scalar(stream, present)) { result.error = "sprite-rig part order flag is truncated"; return result; }
            if (present) { std::int32_t value{}; if (!read_scalar(stream, value)) { result.error = "sprite-rig part order is truncated"; return result; } key.drawOrder = value; }
            clip.partKeys.push_back(std::move(key));
        }
        result.asset.clips.push_back(std::move(clip));
    }
    if (!readCount(count, 4096U)) { result.error = "sprite-rig constraint count is invalid"; return result; }
    for (std::uint32_t i = 0; i < count; ++i) {
        SpriteRigTwoBoneIk2D constraint;
        if (!read_scalar(stream, constraint.id) || !read_string(stream, constraint.name) ||
            !read_scalar(stream, constraint.upper) || !read_scalar(stream, constraint.lower) ||
            !read_scalar(stream, constraint.targetPixels) || !read_scalar(stream, constraint.bendSign) ||
            !read_scalar(stream, constraint.weight)) { result.error = "sprite-rig constraint is truncated"; return result; }
        result.asset.constraints.push_back(std::move(constraint));
    }
    if (!readCount(count, 4096U)) { result.error = "sprite-rig variant count is invalid"; return result; }
    for (std::uint32_t i = 0; i < count; ++i) {
        SpriteRigVariant2D variant; std::uint32_t overrideCount{};
        if (!read_string(stream, variant.name) || !readCount(overrideCount, 4096U)) { result.error = "sprite-rig variant is truncated"; return result; }
        for (std::uint32_t overrideIndex = 0; overrideIndex < overrideCount; ++overrideIndex) {
            SpriteRigPartOverride2D overrideValue; bool present{};
            if (!read_string(stream, overrideValue.partName) || !read_scalar(stream, present)) { result.error = "sprite-rig override is truncated"; return result; }
            if (present) { std::string value; if (!read_string(stream, value)) { result.error = "sprite-rig override asset is truncated"; return result; } overrideValue.spriteAsset = std::move(value); }
            if (!read_scalar(stream, present)) { result.error = "sprite-rig override clip flag is truncated"; return result; }
            if (present) { std::string value; if (!read_string(stream, value)) { result.error = "sprite-rig override clip is truncated"; return result; } overrideValue.clip = std::move(value); }
            if (!read_scalar(stream, present)) { result.error = "sprite-rig override visibility flag is truncated"; return result; }
            if (present) { bool value{}; if (!read_scalar(stream, value)) { result.error = "sprite-rig override visibility is truncated"; return result; } overrideValue.visible = value; }
            if (!read_scalar(stream, present)) { result.error = "sprite-rig override palette flag is truncated"; return result; }
            if (present) { std::uint32_t value{}; if (!read_scalar(stream, value)) { result.error = "sprite-rig override palette is truncated"; return result; } overrideValue.paletteBank = value; }
            variant.overrides.push_back(std::move(overrideValue));
        }
        result.asset.variants.push_back(std::move(variant));
    }
    std::uint64_t storedHash{};
    if (!read_scalar(stream, storedHash)) { result.error = "sprite-rig content hash is missing"; return result; }
    result.asset.recompute_hash();
    if (result.asset.contentHash != storedHash) { result.error = "sprite-rig content hash mismatch"; return result; }
    if (!result.asset.validate(&result.error)) return result;
    return result;
}

namespace editor {

SpriteRig2DAuthoringSession::SpriteRig2DAuthoringSession(SpriteRig2DAsset asset)
    : asset_(std::move(asset)) {
    if (asset_.bones.empty()) {
        asset_.bones.push_back({1U, "root", 0U, {}, 8.0F});
        asset_.clips.push_back({"idle", 1.0F, true, {}, {}});
    }
    asset_.recompute_hash();
    savedHash_ = asset_.contentHash;
}

bool SpriteRig2DAuthoringSession::commit(std::string label, SpriteRig2DAsset replacement,
                                         std::string* error) {
    replacement.recompute_hash();
    if (!replacement.validate(error)) return false;
    undo_.push_back({std::move(label), asset_});
    if (undo_.size() > kMaximumHistory) undo_.erase(undo_.begin());
    asset_ = std::move(replacement);
    redo_.clear();
    return true;
}

bool SpriteRig2DAuthoringSession::open(const std::filesystem::path& path, std::string* error) {
    auto read = read_dvespriterig(path);
    if (!read) return fail(error, read.error);
    asset_ = std::move(read.asset); savedHash_ = asset_.contentHash; undo_.clear(); redo_.clear(); return true;
}

bool SpriteRig2DAuthoringSession::save(const std::filesystem::path& path, std::string* error) {
    asset_.recompute_hash();
    if (!write_dvespriterig(path, asset_, error)) return false;
    savedHash_ = asset_.contentHash; return true;
}

bool SpriteRig2DAuthoringSession::add_bone(std::string name, SpriteRigBoneId parent,
                                           SpriteTransform2D bind, float lengthPixels,
                                           SpriteRigBoneId* id, std::string* error) {
    auto replacement = asset_;
    SpriteRigBone2D bone{next_bone_id(replacement), std::move(name), parent, bind, lengthPixels};
    replacement.bones.push_back(bone);
    if (!commit("Add rig bone", std::move(replacement), error)) return false;
    if (id) *id = bone.id;
    return true;
}

bool SpriteRig2DAuthoringSession::remove_bone(SpriteRigBoneId id, bool reparentChildren, std::string* error) {
    auto replacement = asset_;
    const auto* bone = find_bone(replacement, id);
    if (!bone) return fail(error, "sprite rig bone was not found");
    const SpriteRigBoneId parent = bone->parent;
    const bool hasChildren = std::any_of(replacement.bones.begin(), replacement.bones.end(), [id](const auto& b) { return b.parent == id; });
    const bool hasParts = std::any_of(replacement.parts.begin(), replacement.parts.end(), [id](const auto& p) { return p.bone == id; });
    if ((hasChildren || hasParts) && !reparentChildren) return fail(error, "sprite rig bone still owns children or parts");
    for (auto& child : replacement.bones) if (child.parent == id) child.parent = parent;
    for (auto& part : replacement.parts) if (part.bone == id) part.bone = parent;
    replacement.bones.erase(std::remove_if(replacement.bones.begin(), replacement.bones.end(), [id](const auto& b) { return b.id == id; }), replacement.bones.end());
    for (auto& clip : replacement.clips)
        clip.boneKeys.erase(std::remove_if(clip.boneKeys.begin(), clip.boneKeys.end(), [id](const auto& key) { return key.bone == id; }), clip.boneKeys.end());
    replacement.constraints.erase(std::remove_if(replacement.constraints.begin(), replacement.constraints.end(),
        [id](const auto& constraint) { return constraint.upper == id || constraint.lower == id; }), replacement.constraints.end());
    return commit("Remove rig bone", std::move(replacement), error);
}

bool SpriteRig2DAuthoringSession::reparent_bone(SpriteRigBoneId id, SpriteRigBoneId parent, std::string* error) {
    auto replacement = asset_;
    auto* bone = find_bone(replacement, id);
    if (!bone) return fail(error, "sprite rig bone was not found");
    bone->parent = parent;
    return commit("Reparent rig bone", std::move(replacement), error);
}

bool SpriteRig2DAuthoringSession::set_bone_bind(SpriteRigBoneId id, SpriteTransform2D bind, std::string* error) {
    auto replacement = asset_;
    auto* bone = find_bone(replacement, id);
    if (!bone) return fail(error, "sprite rig bone was not found");
    bone->bind = bind;
    return commit("Edit rig bone", std::move(replacement), error);
}

bool SpriteRig2DAuthoringSession::add_part(SpriteRigPart2D part, SpriteRigPartId* id, std::string* error) {
    auto replacement = asset_;
    part.id = next_part_id(replacement);
    replacement.parts.push_back(part);
    if (!commit("Add rig part", std::move(replacement), error)) return false;
    if (id) *id = part.id;
    return true;
}

bool SpriteRig2DAuthoringSession::update_part(SpriteRigPartId id, SpriteRigPart2D replacementPart, std::string* error) {
    auto replacement = asset_;
    auto* part = find_part(replacement, id);
    if (!part) return fail(error, "sprite rig part was not found");
    replacementPart.id = id; *part = std::move(replacementPart);
    return commit("Edit rig part", std::move(replacement), error);
}

bool SpriteRig2DAuthoringSession::remove_part(SpriteRigPartId id, std::string* error) {
    auto replacement = asset_;
    if (!find_part(replacement, id)) return fail(error, "sprite rig part was not found");
    replacement.parts.erase(std::remove_if(replacement.parts.begin(), replacement.parts.end(), [id](const auto& part) { return part.id == id; }), replacement.parts.end());
    for (auto& clip : replacement.clips)
        clip.partKeys.erase(std::remove_if(clip.partKeys.begin(), clip.partKeys.end(), [id](const auto& key) { return key.part == id; }), clip.partKeys.end());
    const auto source = std::find_if(asset_.parts.begin(), asset_.parts.end(),
        [id](const auto& part) { return part.id == id; });
    const std::string removedName = source == asset_.parts.end() ? std::string{} : source->name;
    for (auto& variant : replacement.variants)
        variant.overrides.erase(std::remove_if(variant.overrides.begin(), variant.overrides.end(),
            [&](const auto& overrideValue) { return !removedName.empty() && overrideValue.partName == removedName; }),
            variant.overrides.end());
    return commit("Remove rig part", std::move(replacement), error);
}

bool SpriteRig2DAuthoringSession::add_clip(SpriteRigClip2D clip, std::string* error) {
    auto replacement = asset_; replacement.clips.push_back(std::move(clip));
    return commit("Add rig clip", std::move(replacement), error);
}

bool SpriteRig2DAuthoringSession::upsert_bone_key(std::string_view clipName, SpriteRigBoneKey2D key, std::string* error) {
    auto replacement = asset_; auto* clip = find_clip(replacement, clipName);
    if (!clip) return fail(error, "sprite rig clip was not found");
    const auto it = std::find_if(clip->boneKeys.begin(), clip->boneKeys.end(), [&](const auto& candidate) {
        return candidate.bone == key.bone && std::abs(candidate.timeSeconds - key.timeSeconds) < 0.00001F;
    });
    if (it == clip->boneKeys.end()) clip->boneKeys.push_back(key); else *it = key;
    return commit("Set rig bone key", std::move(replacement), error);
}

bool SpriteRig2DAuthoringSession::upsert_part_key(std::string_view clipName, SpriteRigPartKey2D key, std::string* error) {
    auto replacement = asset_; auto* clip = find_clip(replacement, clipName);
    if (!clip) return fail(error, "sprite rig clip was not found");
    const auto it = std::find_if(clip->partKeys.begin(), clip->partKeys.end(), [&](const auto& candidate) {
        return candidate.part == key.part && std::abs(candidate.timeSeconds - key.timeSeconds) < 0.00001F;
    });
    if (it == clip->partKeys.end()) clip->partKeys.push_back(std::move(key)); else *it = std::move(key);
    return commit("Set rig part key", std::move(replacement), error);
}

bool SpriteRig2DAuthoringSession::add_two_bone_ik(SpriteRigTwoBoneIk2D constraint,
                                                  SpriteRigConstraintId* id,
                                                  std::string* error) {
    auto replacement = asset_; constraint.id = next_constraint_id(replacement); replacement.constraints.push_back(constraint);
    if (!commit("Add rig IK", std::move(replacement), error)) return false;
    if (id) *id = constraint.id;
    return true;
}

bool SpriteRig2DAuthoringSession::set_ik_target(SpriteRigConstraintId id, SpriteVec2 target,
                                                float weight, std::string* error) {
    auto replacement = asset_;
    const auto it = std::find_if(replacement.constraints.begin(), replacement.constraints.end(), [id](const auto& constraint) { return constraint.id == id; });
    if (it == replacement.constraints.end()) return fail(error, "sprite rig IK constraint was not found");
    it->targetPixels = target; it->weight = weight;
    return commit("Move rig IK target", std::move(replacement), error);
}

bool SpriteRig2DAuthoringSession::add_variant(SpriteRigVariant2D variant, std::string* error) {
    auto replacement = asset_; replacement.variants.push_back(std::move(variant));
    return commit("Add rig variant", std::move(replacement), error);
}

bool SpriteRig2DAuthoringSession::duplicate_variant(std::string_view source, std::string name, std::string* error) {
    auto replacement = asset_;
    const auto it = std::find_if(replacement.variants.begin(), replacement.variants.end(), [source](const auto& variant) { return variant.name == source; });
    if (it == replacement.variants.end()) return fail(error, "sprite rig source variant was not found");
    auto duplicate = *it; duplicate.name = std::move(name); replacement.variants.push_back(std::move(duplicate));
    return commit("Duplicate rig variant", std::move(replacement), error);
}

bool SpriteRig2DAuthoringSession::undo() noexcept {
    if (undo_.empty()) return false;
    redo_.push_back({"Redo", asset_}); asset_ = std::move(undo_.back().asset); undo_.pop_back(); return true;
}

bool SpriteRig2DAuthoringSession::redo() noexcept {
    if (redo_.empty()) return false;
    undo_.push_back({"Undo", asset_}); asset_ = std::move(redo_.back().asset); redo_.pop_back(); return true;
}

} // namespace editor
} // namespace dve
