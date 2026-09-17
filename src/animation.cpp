#include "dve/animation.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <system_error>
#include <type_traits>

namespace dve {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

bool finite(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(Quaternion value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           std::isfinite(value.w) &&
           value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w > 0.0F;
}

bool finite(const RigidTransform& value) noexcept {
    return finite(value.position) && finite(value.rotation);
}

bool valid_name(std::string_view value) noexcept {
    if (value.empty() || value.size() > 255U) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32U && c != 127U; });
}

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

template <typename Integer>
void hash_integer(std::uint64_t& hash, Integer value) noexcept {
    using Unsigned = std::make_unsigned_t<Integer>;
    const Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t i = 0; i < sizeof(Unsigned); ++i)
        hash_byte(hash, static_cast<std::uint8_t>((bits >> (i * 8U)) & 0xFFU));
}

void hash_float(std::uint64_t& hash, float value) noexcept {
    hash_integer(hash, std::bit_cast<std::uint32_t>(value));
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_integer(hash, static_cast<std::uint64_t>(value.size()));
    for (unsigned char c : value) hash_byte(hash, c);
}

void hash_transform(std::uint64_t& hash, const RigidTransform& value) noexcept {
    hash_float(hash, value.position.x);
    hash_float(hash, value.position.y);
    hash_float(hash, value.position.z);
    const Quaternion rotation = normalize(value.rotation);
    hash_float(hash, rotation.x);
    hash_float(hash, rotation.y);
    hash_float(hash, rotation.z);
    hash_float(hash, rotation.w);
}

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool write_atomic(const std::filesystem::path& path, std::string_view bytes, std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return fail(error, "could not create animation asset directory: " + ec.message());
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return fail(error, "could not open temporary animation asset");
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, ec);
            return fail(error, "could not write complete animation asset");
        }
    }
    std::filesystem::path backup = path;
    backup += ".bak";
    const bool replacing = std::filesystem::exists(path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return fail(error, "could not inspect existing animation asset");
    }
    if (replacing) {
        std::filesystem::remove(backup, ec);
        ec.clear();
        std::filesystem::rename(path, backup, ec);
        if (ec) {
            std::filesystem::remove(temporary, ec);
            return fail(error, "could not stage existing animation asset for replacement");
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        if (replacing) {
            ec.clear();
            std::filesystem::rename(backup, path, ec);
        }
        return fail(error, "could not publish animation asset transactionally");
    }
    if (replacing) std::filesystem::remove(backup, ec);
    return true;
}

template <typename Key, typename Value, typename Interpolator>
Value sample_keys(std::span<const Key> keys, float time, Value fallback, Interpolator interpolate) {
    if (keys.empty()) return fallback;
    if (time <= keys.front().timeSeconds) return keys.front().value;
    if (time >= keys.back().timeSeconds) return keys.back().value;
    const auto upper = std::upper_bound(keys.begin(), keys.end(), time,
        [](float value, const Key& key) { return value < key.timeSeconds; });
    const Key& b = *upper;
    const Key& a = *(upper - 1);
    const float span = b.timeSeconds - a.timeSeconds;
    const float alpha = span > 0.0F ? (time - a.timeSeconds) / span : 0.0F;
    return interpolate(a.value, b.value, alpha);
}

Float3 interpolate_position(Float3 a, Float3 b, float alpha) noexcept {
    return add(a, multiply(subtract(b, a), alpha));
}

Quaternion interpolate_rotation(Quaternion a, Quaternion b, float alpha) noexcept {
    RigidTransform from;
    from.rotation = a;
    RigidTransform to;
    to.rotation = b;
    return interpolate_rigid_transform(from, to, alpha).rotation;
}

RigidTransform animation_root_delta(
    const SkeletonAsset& skeleton, const AnimationClipAsset& clip, float fromTime, float toTime) {
    if (skeleton.bones.empty() || !std::isfinite(fromTime) || !std::isfinite(toTime) ||
        !(toTime > fromTime)) return {};
    auto root_at = [&](float time, bool looping) {
        RigidTransform root = skeleton.bones.front().bindLocal;
        float sampledTime = std::clamp(time, 0.0F, clip.durationSeconds);
        if (looping) {
            sampledTime = std::fmod(time, clip.durationSeconds);
            if (sampledTime < 0.0F) sampledTime += clip.durationSeconds;
        }
        const auto track = std::find_if(clip.tracks.begin(), clip.tracks.end(),
            [](const BoneAnimationTrack& value) { return value.bone == 0U; });
        if (track == clip.tracks.end()) return root;
        root.position = sample_keys<TranslationKey, Float3>(
            track->translations, sampledTime, root.position, interpolate_position);
        root.rotation = sample_keys<RotationKey, Quaternion>(
            track->rotations, sampledTime, root.rotation, interpolate_rotation);
        return root;
    };
    if (!clip.looping) return relative_rigid_transform(root_at(fromTime, false), root_at(toTime, false));
    const float duration = clip.durationSeconds;
    const double firstCycleDouble = std::floor(static_cast<double>(fromTime) / duration);
    const double lastCycleDouble = std::floor(static_cast<double>(toTime) / duration);
    if (firstCycleDouble > static_cast<double>(std::numeric_limits<std::int64_t>::max()) ||
        lastCycleDouble > static_cast<double>(std::numeric_limits<std::int64_t>::max())) return {};
    const auto firstCycle = static_cast<std::int64_t>(firstCycleDouble);
    const auto lastCycle = static_cast<std::int64_t>(lastCycleDouble);
    const float fromLocal = fromTime - static_cast<float>(firstCycle) * duration;
    const float toLocal = toTime - static_cast<float>(lastCycle) * duration;
    if (firstCycle == lastCycle)
        return relative_rigid_transform(root_at(fromLocal, false), root_at(toLocal, false));
    RigidTransform result = relative_rigid_transform(root_at(fromLocal, false), root_at(duration, false));
    const RigidTransform fullCycle = relative_rigid_transform(root_at(0.0F, false), root_at(duration, false));
    std::uint64_t middleCycles = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, lastCycle - firstCycle - 1));
    RigidTransform cyclePower = fullCycle;
    while (middleCycles != 0U) {
        if ((middleCycles & 1U) != 0U) result = compose_rigid_transforms(result, cyclePower);
        middleCycles >>= 1U;
        if (middleCycles != 0U) cyclePower = compose_rigid_transforms(cyclePower, cyclePower);
    }
    return compose_rigid_transforms(
        result, relative_rigid_transform(root_at(0.0F, false), root_at(toLocal, false)));
}

float clip_time(const AnimationClipAsset& clip, float time) noexcept {
    if (!std::isfinite(time)) return 0.0F;
    if (!clip.looping) return std::clamp(time, 0.0F, clip.durationSeconds);
    float wrapped = std::fmod(time, clip.durationSeconds);
    if (wrapped < 0.0F) wrapped += clip.durationSeconds;
    return wrapped;
}

RigidTransform inverse_rigid_transform(const RigidTransform& value) noexcept {
    const Quaternion inverseRotation = conjugate(normalize(value.rotation));
    return make_rigid_transform(rotate(inverseRotation, multiply(value.position, -1.0F)), inverseRotation);
}

} // namespace

AnimationValidationResult validate_skeleton(const SkeletonAsset& asset) noexcept {
    const auto invalid = [](std::string message) { return AnimationValidationResult{false, std::move(message)}; };
    if (!valid_name(asset.name)) return invalid("skeleton name is invalid");
    if (asset.bones.empty() || asset.bones.size() > kMaximumSkeletonBones)
        return invalid("skeleton bone count is out of range");
    std::size_t roots = 0U;
    for (std::size_t i = 0; i < asset.bones.size(); ++i) {
        const SkeletonBone& bone = asset.bones[i];
        if (!valid_name(bone.name)) return invalid("skeleton bone name is invalid");
        for (std::size_t j = 0; j < i; ++j)
            if (asset.bones[j].name == bone.name) return invalid("skeleton bone names are not unique");
        if (bone.parent == -1) ++roots;
        else if (bone.parent < 0 || static_cast<std::size_t>(bone.parent) >= i)
            return invalid("skeleton parents must precede their children");
        if (!finite(bone.bindLocal)) return invalid("skeleton bind transform is not finite");
    }
    if (roots == 0U) return invalid("skeleton has no root bone");
    if (asset.sockets.size() > 4096U) return invalid("skeleton socket count is out of range");
    for (std::size_t i = 0; i < asset.sockets.size(); ++i) {
        const SkeletalSocket& socket = asset.sockets[i];
        if (!valid_name(socket.name)) return invalid("skeletal socket name is invalid");
        if (socket.bone >= asset.bones.size()) return invalid("skeletal socket bone is invalid");
        if (!finite(socket.localTransform)) return invalid("skeletal socket transform is not finite");
        for (std::size_t j = 0; j < i; ++j)
            if (asset.sockets[j].name == socket.name) return invalid("skeletal socket names are not unique");
    }
    return {true, {}};
}

AnimationValidationResult validate_animation_clip(
    const AnimationClipAsset& clip, const SkeletonAsset* skeleton) noexcept {
    const auto invalid = [](std::string message) { return AnimationValidationResult{false, std::move(message)}; };
    if (!valid_name(clip.name)) return invalid("animation clip name is invalid");
    if (!(clip.durationSeconds > 0.0F) || !std::isfinite(clip.durationSeconds))
        return invalid("animation clip duration is invalid");
    if (clip.tracks.size() > kMaximumSkeletonBones) return invalid("animation track count is out of range");
    for (std::size_t i = 0; i < clip.tracks.size(); ++i) {
        const BoneAnimationTrack& track = clip.tracks[i];
        if (track.bone == kInvalidBoneIndex || (skeleton && track.bone >= skeleton->bones.size()))
            return invalid("animation track bone is invalid");
        if (!skeleton && track.bone >= kMaximumSkeletonBones)
            return invalid("animation track bone exceeds the supported skeleton limit");
        if (track.translations.empty() && track.rotations.empty())
            return invalid("animation track has no keys");
        for (std::size_t j = 0; j < i; ++j)
            if (clip.tracks[j].bone == track.bone) return invalid("animation clip has duplicate bone tracks");
        if (track.translations.size() > 1000000U || track.rotations.size() > 1000000U)
            return invalid("animation key count is out of range");
        float previous = -1.0F;
        for (const TranslationKey& key : track.translations) {
            if (!std::isfinite(key.timeSeconds) || key.timeSeconds < 0.0F ||
                key.timeSeconds > clip.durationSeconds || key.timeSeconds <= previous || !finite(key.value))
                return invalid("animation translation keys are invalid or unordered");
            previous = key.timeSeconds;
        }
        previous = -1.0F;
        for (const RotationKey& key : track.rotations) {
            if (!std::isfinite(key.timeSeconds) || key.timeSeconds < 0.0F ||
                key.timeSeconds > clip.durationSeconds || key.timeSeconds <= previous || !finite(key.value))
                return invalid("animation rotation keys are invalid or unordered");
            previous = key.timeSeconds;
        }
    }
    float previousEvent = -1.0F;
    for (const AnimationEventKey& event : clip.events) {
        if (!std::isfinite(event.timeSeconds) || event.timeSeconds < 0.0F ||
            event.timeSeconds > clip.durationSeconds || event.timeSeconds < previousEvent || !valid_name(event.name))
            return invalid("animation events are invalid or unordered");
        previousEvent = event.timeSeconds;
    }
    return {true, {}};
}

std::uint64_t skeleton_content_hash(const SkeletonAsset& asset) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, asset.name);
    hash_integer(hash, static_cast<std::uint64_t>(asset.bones.size()));
    for (const SkeletonBone& bone : asset.bones) {
        hash_string(hash, bone.name);
        hash_integer(hash, bone.parent);
        hash_transform(hash, bone.bindLocal);
    }
    hash_integer(hash, static_cast<std::uint64_t>(asset.sockets.size()));
    for (const SkeletalSocket& socket : asset.sockets) {
        hash_string(hash, socket.name);
        hash_integer(hash, socket.bone);
        hash_transform(hash, socket.localTransform);
    }
    return hash;
}

std::uint64_t animation_clip_content_hash(const AnimationClipAsset& clip) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, clip.name);
    hash_float(hash, clip.durationSeconds);
    hash_byte(hash, clip.looping ? 1U : 0U);
    hash_integer(hash, static_cast<std::uint64_t>(clip.tracks.size()));
    for (const BoneAnimationTrack& track : clip.tracks) {
        hash_integer(hash, track.bone);
        hash_integer(hash, static_cast<std::uint64_t>(track.translations.size()));
        for (const TranslationKey& key : track.translations) {
            hash_float(hash, key.timeSeconds);
            hash_float(hash, key.value.x); hash_float(hash, key.value.y); hash_float(hash, key.value.z);
        }
        hash_integer(hash, static_cast<std::uint64_t>(track.rotations.size()));
        for (const RotationKey& key : track.rotations) {
            hash_float(hash, key.timeSeconds);
            const Quaternion value = normalize(key.value);
            hash_float(hash, value.x); hash_float(hash, value.y); hash_float(hash, value.z); hash_float(hash, value.w);
        }
    }
    hash_integer(hash, static_cast<std::uint64_t>(clip.events.size()));
    for (const AnimationEventKey& event : clip.events) {
        hash_float(hash, event.timeSeconds);
        hash_string(hash, event.name);
    }
    return hash;
}

bool write_dveskeleton(const std::filesystem::path& path, const SkeletonAsset& asset, std::string* error) {
    const AnimationValidationResult validation = validate_skeleton(asset);
    if (!validation) return fail(error, validation.message);
    const std::uint64_t hash = skeleton_content_hash(asset);
    if (asset.contentHash != 0U && asset.contentHash != hash) return fail(error, "skeleton content hash is stale");
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<float>::max_digits10);
    stream << "DVE_SKELETON 1\nname " << std::quoted(asset.name) << "\n";
    stream << "bones " << asset.bones.size() << "\n";
    for (std::size_t i = 0; i < asset.bones.size(); ++i) {
        const SkeletonBone& bone = asset.bones[i];
        const Quaternion q = normalize(bone.bindLocal.rotation);
        stream << "bone " << i << ' ' << bone.parent << ' ' << std::quoted(bone.name) << ' '
               << bone.bindLocal.position.x << ' ' << bone.bindLocal.position.y << ' ' << bone.bindLocal.position.z << ' '
               << q.x << ' ' << q.y << ' ' << q.z << ' ' << q.w << "\n";
    }
    stream << "sockets " << asset.sockets.size() << "\n";
    for (const SkeletalSocket& socket : asset.sockets) {
        const Quaternion q = normalize(socket.localTransform.rotation);
        stream << "socket " << std::quoted(socket.name) << ' ' << socket.bone << ' '
               << socket.localTransform.position.x << ' ' << socket.localTransform.position.y << ' '
               << socket.localTransform.position.z << ' ' << q.x << ' ' << q.y << ' ' << q.z << ' ' << q.w << "\n";
    }
    stream << "hash " << hash << "\n";
    return write_atomic(path, stream.str(), error);
}

SkeletonReadResult read_dveskeleton(const std::filesystem::path& path, std::uint64_t maximumBytes) {
    SkeletonReadResult result;
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size > maximumBytes) { result.error = ec ? "could not stat skeleton asset" : "skeleton asset exceeds byte limit"; return result; }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { result.error = "could not open skeleton asset"; return result; }
    std::string magic, token;
    int version{};
    if (!(stream >> magic >> version) || magic != "DVE_SKELETON" || version != 1) {
        result.error = "unsupported skeleton asset header"; return result;
    }
    if (!(stream >> token) || token != "name" || !(stream >> std::quoted(result.asset.name))) {
        result.error = "invalid skeleton name record"; return result;
    }
    std::size_t boneCount{};
    if (!(stream >> token >> boneCount) || token != "bones" || boneCount == 0U || boneCount > kMaximumSkeletonBones) {
        result.error = "invalid skeleton bone count"; return result;
    }
    result.asset.bones.resize(boneCount);
    for (std::size_t i = 0; i < boneCount; ++i) {
        std::size_t index{};
        SkeletonBone& bone = result.asset.bones[i];
        if (!(stream >> token >> index >> bone.parent >> std::quoted(bone.name) >>
              bone.bindLocal.position.x >> bone.bindLocal.position.y >> bone.bindLocal.position.z >>
              bone.bindLocal.rotation.x >> bone.bindLocal.rotation.y >> bone.bindLocal.rotation.z >> bone.bindLocal.rotation.w) ||
            token != "bone" || index != i) {
            result.error = "invalid skeleton bone record"; return result;
        }
        bone.bindLocal.rotation = normalize(bone.bindLocal.rotation);
    }
    std::size_t socketCount{};
    if (!(stream >> token >> socketCount) || token != "sockets" || socketCount > 4096U) {
        result.error = "invalid skeleton socket count"; return result;
    }
    result.asset.sockets.resize(socketCount);
    for (SkeletalSocket& socket : result.asset.sockets) {
        unsigned int bone{};
        if (!(stream >> token >> std::quoted(socket.name) >> bone >>
              socket.localTransform.position.x >> socket.localTransform.position.y >> socket.localTransform.position.z >>
              socket.localTransform.rotation.x >> socket.localTransform.rotation.y >>
              socket.localTransform.rotation.z >> socket.localTransform.rotation.w) || token != "socket" ||
            bone > std::numeric_limits<BoneIndex>::max()) {
            result.error = "invalid skeletal socket record"; return result;
        }
        socket.bone = static_cast<BoneIndex>(bone);
        socket.localTransform.rotation = normalize(socket.localTransform.rotation);
    }
    if (!(stream >> token >> result.asset.contentHash) || token != "hash") {
        result.error = "missing skeleton content hash"; return result;
    }
    stream >> std::ws;
    if (!stream.eof()) { result.error = "trailing skeleton asset data"; return result; }
    const AnimationValidationResult validation = validate_skeleton(result.asset);
    if (!validation) { result.error = validation.message; return result; }
    if (result.asset.contentHash != skeleton_content_hash(result.asset)) {
        result.error = "skeleton content hash mismatch"; return result;
    }
    return result;
}

bool write_dveanim(const std::filesystem::path& path, const AnimationClipAsset& clip, std::string* error) {
    const AnimationValidationResult validation = validate_animation_clip(clip);
    if (!validation) return fail(error, validation.message);
    const std::uint64_t hash = animation_clip_content_hash(clip);
    if (clip.contentHash != 0U && clip.contentHash != hash) return fail(error, "animation clip content hash is stale");
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<float>::max_digits10);
    stream << "DVE_ANIMATION_CLIP 1\nname " << std::quoted(clip.name) << "\n";
    stream << "duration " << clip.durationSeconds << "\nlooping " << (clip.looping ? 1 : 0) << "\n";
    stream << "tracks " << clip.tracks.size() << "\n";
    for (const BoneAnimationTrack& track : clip.tracks) {
        stream << "track " << track.bone << ' ' << track.translations.size() << ' ' << track.rotations.size() << "\n";
        for (const TranslationKey& key : track.translations)
            stream << "translation " << key.timeSeconds << ' ' << key.value.x << ' ' << key.value.y << ' ' << key.value.z << "\n";
        for (const RotationKey& key : track.rotations) {
            const Quaternion q = normalize(key.value);
            stream << "rotation " << key.timeSeconds << ' ' << q.x << ' ' << q.y << ' ' << q.z << ' ' << q.w << "\n";
        }
    }
    stream << "events " << clip.events.size() << "\n";
    for (const AnimationEventKey& event : clip.events)
        stream << "event " << event.timeSeconds << ' ' << std::quoted(event.name) << "\n";
    stream << "hash " << hash << "\n";
    return write_atomic(path, stream.str(), error);
}

AnimationClipReadResult read_dveanim(const std::filesystem::path& path, std::uint64_t maximumBytes) {
    AnimationClipReadResult result;
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size > maximumBytes) { result.error = ec ? "could not stat animation clip" : "animation clip exceeds byte limit"; return result; }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { result.error = "could not open animation clip"; return result; }
    std::string magic, token;
    int version{}, looping{};
    if (!(stream >> magic >> version) || magic != "DVE_ANIMATION_CLIP" || version != 1) {
        result.error = "unsupported animation clip header"; return result;
    }
    if (!(stream >> token) || token != "name" || !(stream >> std::quoted(result.asset.name)) ||
        !(stream >> token >> result.asset.durationSeconds) || token != "duration" ||
        !(stream >> token >> looping) || token != "looping" || (looping != 0 && looping != 1)) {
        result.error = "invalid animation clip metadata"; return result;
    }
    result.asset.looping = looping != 0;
    std::size_t trackCount{};
    if (!(stream >> token >> trackCount) || token != "tracks" || trackCount > kMaximumSkeletonBones) {
        result.error = "invalid animation track count"; return result;
    }
    result.asset.tracks.resize(trackCount);
    for (BoneAnimationTrack& track : result.asset.tracks) {
        unsigned int bone{};
        std::size_t translationCount{}, rotationCount{};
        if (!(stream >> token >> bone >> translationCount >> rotationCount) || token != "track" ||
            bone > std::numeric_limits<BoneIndex>::max() || translationCount > 1000000U || rotationCount > 1000000U) {
            result.error = "invalid animation track record"; return result;
        }
        track.bone = static_cast<BoneIndex>(bone);
        track.translations.resize(translationCount);
        for (TranslationKey& key : track.translations)
            if (!(stream >> token >> key.timeSeconds >> key.value.x >> key.value.y >> key.value.z) || token != "translation") {
                result.error = "invalid animation translation key"; return result;
            }
        track.rotations.resize(rotationCount);
        for (RotationKey& key : track.rotations) {
            if (!(stream >> token >> key.timeSeconds >> key.value.x >> key.value.y >> key.value.z >> key.value.w) || token != "rotation") {
                result.error = "invalid animation rotation key"; return result;
            }
            key.value = normalize(key.value);
        }
    }
    std::size_t eventCount{};
    if (!(stream >> token >> eventCount) || token != "events" || eventCount > 1000000U) {
        result.error = "invalid animation event count"; return result;
    }
    result.asset.events.resize(eventCount);
    for (AnimationEventKey& event : result.asset.events)
        if (!(stream >> token >> event.timeSeconds >> std::quoted(event.name)) || token != "event") {
            result.error = "invalid animation event record"; return result;
        }
    if (!(stream >> token >> result.asset.contentHash) || token != "hash") {
        result.error = "missing animation clip content hash"; return result;
    }
    stream >> std::ws;
    if (!stream.eof()) { result.error = "trailing animation clip data"; return result; }
    const AnimationValidationResult validation = validate_animation_clip(result.asset);
    if (!validation) { result.error = validation.message; return result; }
    if (result.asset.contentHash != animation_clip_content_hash(result.asset)) {
        result.error = "animation clip content hash mismatch"; return result;
    }
    return result;
}

LocalPose make_bind_pose(const SkeletonAsset& skeleton) {
    LocalPose pose;
    pose.reserve(skeleton.bones.size());
    for (const SkeletonBone& bone : skeleton.bones) pose.push_back(bone.bindLocal);
    return pose;
}

LocalPose sample_animation_clip(
    const SkeletonAsset& skeleton, const AnimationClipAsset& clip, float timeSeconds) {
    LocalPose pose = make_bind_pose(skeleton);
    if (!validate_skeleton(skeleton) || !validate_animation_clip(clip, &skeleton)) return pose;
    const float time = clip_time(clip, timeSeconds);
    for (const BoneAnimationTrack& track : clip.tracks) {
        RigidTransform& value = pose[track.bone];
        value.position = sample_keys<TranslationKey, Float3>(
            track.translations, time, value.position, interpolate_position);
        value.rotation = sample_keys<RotationKey, Quaternion>(
            track.rotations, time, value.rotation, interpolate_rotation);
    }
    return pose;
}

LocalPose blend_local_poses(
    std::span<const RigidTransform> from, std::span<const RigidTransform> to, float weight) {
    if (from.size() != to.size()) return {};
    weight = std::clamp(std::isfinite(weight) ? weight : 0.0F, 0.0F, 1.0F);
    LocalPose result(from.size());
    for (std::size_t i = 0; i < from.size(); ++i)
        result[i] = interpolate_rigid_transform(from[i], to[i], weight);
    return result;
}

LocalPose blend_local_pose_stack(
    std::span<const LocalPose> poses, std::span<const float> weights, std::string* error) {
    if (poses.empty() || poses.size() != weights.size()) {
        fail(error, "pose stack and weight counts do not match"); return {};
    }
    const std::size_t boneCount = poses.front().size();
    LocalPose result;
    float accumulated{};
    for (std::size_t i = 0; i < poses.size(); ++i) {
        if (poses[i].size() != boneCount || !std::isfinite(weights[i]) || weights[i] < 0.0F) {
            fail(error, "pose stack contains mismatched poses or invalid weights"); return {};
        }
        if (!(weights[i] > 0.0F)) continue;
        if (!(accumulated > 0.0F)) result = poses[i];
        else result = blend_local_poses(result, poses[i], weights[i] / (accumulated + weights[i]));
        accumulated += weights[i];
    }
    if (!(accumulated > 0.0F)) { fail(error, "pose stack has zero total weight"); return {}; }
    return result;
}

std::vector<RigidTransform> compute_model_pose(
    const SkeletonAsset& skeleton, std::span<const RigidTransform> localPose, std::string* error) {
    const AnimationValidationResult validation = validate_skeleton(skeleton);
    if (!validation) { fail(error, validation.message); return {}; }
    if (localPose.size() != skeleton.bones.size()) { fail(error, "local pose bone count does not match skeleton"); return {}; }
    std::vector<RigidTransform> model(localPose.size());
    for (std::size_t i = 0; i < localPose.size(); ++i) {
        if (!finite(localPose[i])) { fail(error, "local pose contains a nonfinite transform"); return {}; }
        const std::int32_t parent = skeleton.bones[i].parent;
        model[i] = parent < 0 ? localPose[i] : compose_rigid_transforms(model[static_cast<std::size_t>(parent)], localPose[i]);
    }
    return model;
}

bool skin_vertices_cpu(
    const SkeletonAsset& skeleton, std::span<const RigidTransform> localPose,
    std::span<const SkinVertex> vertices, std::vector<CpuSkinnedVertex>& output,
    std::string* error) {
    output.clear();
    const LocalPose bindLocal = make_bind_pose(skeleton);
    const auto bindModel = compute_model_pose(skeleton, bindLocal, error);
    if (bindModel.empty()) return false;
    const auto animatedModel = compute_model_pose(skeleton, localPose, error);
    if (animatedModel.empty()) return false;
    std::vector<RigidTransform> skinTransforms(bindModel.size());
    for (std::size_t i = 0; i < bindModel.size(); ++i)
        skinTransforms[i] = compose_rigid_transforms(animatedModel[i], inverse_rigid_transform(bindModel[i]));
    output.reserve(vertices.size());
    for (const SkinVertex& vertex : vertices) {
        if (!finite(vertex.position) || !finite(vertex.normal) || !(length_squared(vertex.normal) > 0.0F))
            return fail(error, "skin vertex position or normal is invalid");
        float totalWeight{};
        for (std::size_t influence = 0; influence < kMaximumSkinInfluences; ++influence) {
            const float weight = vertex.influences.weights[influence];
            if (!std::isfinite(weight) || weight < 0.0F) return fail(error, "skin weight is invalid");
            if (weight > 0.0F && vertex.influences.bones[influence] >= skeleton.bones.size())
                return fail(error, "skin influence bone is invalid");
            totalWeight += weight;
        }
        if (!(totalWeight > 0.0F)) return fail(error, "skin vertex has zero total weight");
        Float3 position{}, normal{};
        for (std::size_t influence = 0; influence < kMaximumSkinInfluences; ++influence) {
            const float normalizedWeight = vertex.influences.weights[influence] / totalWeight;
            if (!(normalizedWeight > 0.0F)) continue;
            const RigidTransform& transform = skinTransforms[vertex.influences.bones[influence]];
            position = add(position, multiply(transform_point(transform, vertex.position), normalizedWeight));
            normal = add(normal, multiply(transform_vector(transform, vertex.normal), normalizedWeight));
        }
        output.push_back({position, normalize(normal)});
    }
    return true;
}

std::optional<RigidTransform> resolve_skeletal_socket(
    const SkeletonAsset& skeleton, std::span<const RigidTransform> localPose,
    std::string_view socketName, const RigidTransform& objectWorldTransform, std::string* error) {
    const auto socket = std::find_if(skeleton.sockets.begin(), skeleton.sockets.end(),
        [socketName](const SkeletalSocket& value) { return value.name == socketName; });
    if (socket == skeleton.sockets.end()) return std::nullopt;
    const auto model = compute_model_pose(skeleton, localPose, error);
    if (model.empty()) return std::nullopt;
    return compose_rigid_transforms(
        compose_rigid_transforms(objectWorldTransform, model[socket->bone]), socket->localTransform);
}

struct SkeletalAnimationRuntime::Impl {
    struct Instance {
        SkeletonAsset skeleton;
        std::map<std::string, AnimationClipAsset, std::less<>> clips;
        LocalPose pose;
        std::string active;
        float time{};
        std::string target;
        float targetTime{};
        float fadeElapsed{};
        float fadeDuration{};
        float playbackSpeed{1.0F};
        bool rootMotionEnabled{};
        bool rootMotionPending{};
        RigidTransform rootMotionAccum{};
    };
    std::map<std::uint64_t, Instance> instances;
};

SkeletalAnimationRuntime::SkeletalAnimationRuntime() : impl_(std::make_unique<Impl>()) {}
SkeletalAnimationRuntime::~SkeletalAnimationRuntime() = default;

bool SkeletalAnimationRuntime::bind_skeleton(
    std::uint64_t objectId, SkeletonAsset skeletonAsset, std::string* error) {
    if (objectId == 0U) return fail(error, "animation object id is invalid");
    const AnimationValidationResult validation = validate_skeleton(skeletonAsset);
    if (!validation) return fail(error, validation.message);
    const std::uint64_t hash = skeleton_content_hash(skeletonAsset);
    if (skeletonAsset.contentHash != 0U && skeletonAsset.contentHash != hash)
        return fail(error, "skeleton content hash is stale");
    skeletonAsset.contentHash = hash;
    Impl::Instance instance;
    instance.pose = make_bind_pose(skeletonAsset);
    instance.skeleton = std::move(skeletonAsset);
    impl_->instances.insert_or_assign(objectId, std::move(instance));
    return true;
}

bool SkeletalAnimationRuntime::unbind(std::uint64_t objectId) noexcept {
    return impl_->instances.erase(objectId) != 0U;
}

bool SkeletalAnimationRuntime::add_clip(
    std::uint64_t objectId, AnimationClipAsset clip, std::string* error) {
    const auto instance = impl_->instances.find(objectId);
    if (instance == impl_->instances.end()) return fail(error, "animation instance is not bound");
    const AnimationValidationResult validation = validate_animation_clip(clip, &instance->second.skeleton);
    if (!validation) return fail(error, validation.message);
    const std::uint64_t hash = animation_clip_content_hash(clip);
    if (clip.contentHash != 0U && clip.contentHash != hash) return fail(error, "animation clip content hash is stale");
    clip.contentHash = hash;
    instance->second.clips.insert_or_assign(clip.name, std::move(clip));
    return true;
}

bool SkeletalAnimationRuntime::play(
    std::uint64_t objectId, std::string_view clipName, bool restart, std::string* error) {
    const auto instance = impl_->instances.find(objectId);
    if (instance == impl_->instances.end()) return fail(error, "animation instance is not bound");
    const auto clip = instance->second.clips.find(clipName);
    if (clip == instance->second.clips.end()) return fail(error, "animation clip is not registered");
    if (restart || instance->second.active != clipName) instance->second.time = 0.0F;
    instance->second.active = clip->first;
    instance->second.target.clear();
    instance->second.fadeElapsed = instance->second.fadeDuration = 0.0F;
    instance->second.pose = sample_animation_clip(instance->second.skeleton, clip->second, instance->second.time);
    return true;
}

bool SkeletalAnimationRuntime::crossfade(
    std::uint64_t objectId, std::string_view clipName, float durationSeconds, std::string* error) {
    const auto instance = impl_->instances.find(objectId);
    if (instance == impl_->instances.end()) return fail(error, "animation instance is not bound");
    if (!std::isfinite(durationSeconds) || durationSeconds < 0.0F) return fail(error, "crossfade duration is invalid");
    if (!instance->second.clips.contains(clipName)) return fail(error, "animation clip is not registered");
    if (instance->second.active.empty() || durationSeconds == 0.0F)
        return play(objectId, clipName, true, error);
    instance->second.target = std::string(clipName);
    instance->second.targetTime = 0.0F;
    instance->second.fadeElapsed = 0.0F;
    instance->second.fadeDuration = durationSeconds;
    return true;
}

bool SkeletalAnimationRuntime::set_playback_speed(
    std::uint64_t objectId, float speed, std::string* error) {
    const auto instance = impl_->instances.find(objectId);
    if (instance == impl_->instances.end()) return fail(error, "animation instance is not bound");
    if (!std::isfinite(speed) || speed < 0.0F || speed > 16.0F)
        return fail(error, "animation playback speed is invalid");
    instance->second.playbackSpeed = speed;
    return true;
}

bool SkeletalAnimationRuntime::set_root_motion_enabled(
    std::uint64_t objectId, bool enabled) noexcept {
    const auto instance = impl_->instances.find(objectId);
    if (instance == impl_->instances.end()) return false;
    instance->second.rootMotionEnabled = enabled;
    instance->second.rootMotionPending = false;
    instance->second.rootMotionAccum = {};
    return true;
}

std::optional<RootMotionDelta> SkeletalAnimationRuntime::consume_root_motion(
    std::uint64_t objectId) noexcept {
    const auto instance = impl_->instances.find(objectId);
    if (instance == impl_->instances.end() || !instance->second.rootMotionPending) return std::nullopt;
    const RigidTransform delta = instance->second.rootMotionAccum;
    instance->second.rootMotionAccum = {};
    instance->second.rootMotionPending = false;
    return RootMotionDelta{delta.position, delta.rotation};
}

std::vector<std::uint64_t> SkeletalAnimationRuntime::root_motion_objects() const {
    std::vector<std::uint64_t> result;
    for (const auto& [objectId, instance] : impl_->instances)
        if (instance.rootMotionPending) result.push_back(objectId);
    return result;
}

bool SkeletalAnimationRuntime::publish_local_pose(
    std::uint64_t objectId, std::span<const RigidTransform> pose, std::string* error) {
    const auto instance = impl_->instances.find(objectId);
    if (instance == impl_->instances.end()) return fail(error, "animation instance is not bound");
    if (pose.size() != instance->second.skeleton.bones.size())
        return fail(error, "published local pose bone count does not match skeleton");
    std::string poseError;
    if (compute_model_pose(instance->second.skeleton, pose, &poseError).empty())
        return fail(error, poseError);
    instance->second.pose.assign(pose.begin(), pose.end());
    return true;
}

void SkeletalAnimationRuntime::tick(float deltaSeconds) {
    if (!(deltaSeconds > 0.0F) || !std::isfinite(deltaSeconds)) return;
    for (auto& [objectId, instance] : impl_->instances) {
        (void)objectId;
        if (instance.active.empty()) continue;
        const float scaledDelta = deltaSeconds * instance.playbackSpeed;
        if (!std::isfinite(scaledDelta)) continue;
        const float previousTime = instance.time;
        instance.time += scaledDelta;
        const auto active = instance.clips.find(instance.active);
        if (active == instance.clips.end()) continue;
        RigidTransform motion = animation_root_delta(
            instance.skeleton, active->second, previousTime, instance.time);
        LocalPose activePose = sample_animation_clip(instance.skeleton, active->second, instance.time);
        if (instance.target.empty()) {
            instance.pose = std::move(activePose);
            if (instance.rootMotionEnabled) {
                instance.rootMotionAccum = compose_rigid_transforms(instance.rootMotionAccum, motion);
                instance.rootMotionPending = true;
                if (!instance.pose.empty()) instance.pose.front() = instance.skeleton.bones.front().bindLocal;
            }
            continue;
        }
        const auto target = instance.clips.find(instance.target);
        if (target == instance.clips.end()) { instance.target.clear(); instance.pose = std::move(activePose); continue; }
        const float previousTargetTime = instance.targetTime;
        instance.targetTime += scaledDelta;
        instance.fadeElapsed += deltaSeconds;
        LocalPose targetPose = sample_animation_clip(instance.skeleton, target->second, instance.targetTime);
        const float weight = std::clamp(instance.fadeElapsed / instance.fadeDuration, 0.0F, 1.0F);
        instance.pose = blend_local_poses(activePose, targetPose, weight);
        if (instance.rootMotionEnabled) {
            const RigidTransform targetMotion = animation_root_delta(
                instance.skeleton, target->second, previousTargetTime, instance.targetTime);
            RigidTransform blendedMotion = interpolate_rigid_transform(motion, targetMotion, weight);
            instance.rootMotionAccum = compose_rigid_transforms(instance.rootMotionAccum, blendedMotion);
            instance.rootMotionPending = true;
            if (!instance.pose.empty()) instance.pose.front() = instance.skeleton.bones.front().bindLocal;
        }
        if (weight >= 1.0F) {
            instance.active = instance.target;
            instance.time = instance.targetTime;
            instance.target.clear();
            instance.fadeElapsed = instance.fadeDuration = 0.0F;
        }
    }
}

bool SkeletalAnimationRuntime::has_instance(std::uint64_t objectId) const noexcept {
    return impl_->instances.contains(objectId);
}

const SkeletonAsset* SkeletalAnimationRuntime::skeleton(std::uint64_t objectId) const noexcept {
    const auto instance = impl_->instances.find(objectId);
    return instance == impl_->instances.end() ? nullptr : &instance->second.skeleton;
}

const LocalPose* SkeletalAnimationRuntime::local_pose(std::uint64_t objectId) const noexcept {
    const auto instance = impl_->instances.find(objectId);
    return instance == impl_->instances.end() ? nullptr : &instance->second.pose;
}

std::string_view SkeletalAnimationRuntime::active_clip(std::uint64_t objectId) const noexcept {
    const auto instance = impl_->instances.find(objectId);
    return instance == impl_->instances.end() ? std::string_view{} : std::string_view(instance->second.active);
}

std::optional<float> SkeletalAnimationRuntime::playback_speed(std::uint64_t objectId) const noexcept {
    const auto instance = impl_->instances.find(objectId);
    return instance == impl_->instances.end() ? std::nullopt :
        std::optional<float>(instance->second.playbackSpeed);
}

std::optional<RigidTransform> SkeletalAnimationRuntime::socket_world_transform(
    std::uint64_t objectId, std::string_view socketName,
    const RigidTransform& objectWorldTransform) const {
    const auto instance = impl_->instances.find(objectId);
    if (instance == impl_->instances.end()) return std::nullopt;
    return resolve_skeletal_socket(
        instance->second.skeleton, instance->second.pose, socketName, objectWorldTransform);
}

} // namespace dve
