#include "dve/control_rig.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <type_traits>

namespace dve {
namespace {

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

template <typename Integer>
void hash_integer(std::uint64_t& hash, Integer value) noexcept {
    using Unsigned = std::make_unsigned_t<Integer>;
    const Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t i = 0U; i < sizeof(Unsigned); ++i)
        hash_byte(hash, static_cast<std::uint8_t>((bits >> (i * 8U)) & 0xFFU));
}

void hash_float(std::uint64_t& hash, float value) noexcept {
    hash_integer(hash, std::bit_cast<std::uint32_t>(value));
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_integer(hash, static_cast<std::uint64_t>(value.size()));
    for (unsigned char byte : value) hash_byte(hash, byte);
}

void hash_float3(std::uint64_t& hash, Float3 value) noexcept {
    hash_float(hash, value.x); hash_float(hash, value.y); hash_float(hash, value.z);
}

void hash_transform(std::uint64_t& hash, const RigidTransform& value) noexcept {
    hash_float3(hash, value.position);
    const Quaternion rotation = normalize(value.rotation);
    hash_float(hash, rotation.x); hash_float(hash, rotation.y);
    hash_float(hash, rotation.z); hash_float(hash, rotation.w);
}

bool write_atomic(const std::filesystem::path& path, std::string_view bytes, std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return fail(error, "could not create control rig asset directory: " + ec.message());
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return fail(error, "could not open temporary control rig asset");
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, ec);
            return fail(error, "could not write complete control rig asset");
        }
    }
    std::filesystem::path backup = path;
    backup += ".bak";
    const bool replacing = std::filesystem::exists(path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return fail(error, "could not inspect existing control rig asset");
    }
    if (replacing) {
        std::filesystem::remove(backup, ec);
        ec.clear();
        std::filesystem::rename(path, backup, ec);
        if (ec) {
            std::filesystem::remove(temporary, ec);
            return fail(error, "could not stage existing control rig asset");
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        if (replacing) {
            ec.clear();
            std::filesystem::rename(backup, path, ec);
        }
        return fail(error, "could not publish control rig asset transactionally");
    }
    if (replacing) std::filesystem::remove(backup, ec);
    return true;
}

bool valid_name(std::string_view name) noexcept {
    return !name.empty() && name.size() <= 255U &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 32U && c != 127U; });
}

bool finite(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(Quaternion value) noexcept {
    const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           std::isfinite(value.w) && lengthSquared > 0.0F;
}

bool finite(const RigidTransform& value) noexcept {
    return finite(value.position) && finite(value.rotation);
}

Float3 cross(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Quaternion from_to(Float3 from, Float3 to) noexcept {
    from = normalize(from);
    to = normalize(to);
    const float cosine = std::clamp(dot(from, to), -1.0F, 1.0F);
    if (cosine > 0.99999F) return {};
    if (cosine < -0.99999F) {
        Float3 axis = cross(from, {0.0F, 0.0F, 1.0F});
        if (!(length_squared(axis) > 1.0e-8F)) axis = cross(from, {0.0F, 1.0F, 0.0F});
        return quaternion_from_axis_angle(normalize(axis), 3.14159265359F);
    }
    return quaternion_from_axis_angle(normalize(cross(from, to)), std::acos(cosine));
}

Float3 clamp_vector(Float3 value, Float3 minimum, Float3 maximum) noexcept {
    return {std::clamp(value.x, minimum.x, maximum.x),
            std::clamp(value.y, minimum.y, maximum.y),
            std::clamp(value.z, minimum.z, maximum.z)};
}

RigidTransform apply_control_policy(
    const ControlRigControl& control, RigidTransform value, const RigidTransform& previous) noexcept {
    if (control.kind == ControlRigControlKind::Translation) value.rotation = previous.rotation;
    if (control.kind == ControlRigControlKind::Rotation) value.position = previous.position;
    if (control.limits.translationEnabled)
        value.position = clamp_vector(
            value.position, control.limits.minimumTranslation, control.limits.maximumTranslation);
    if (control.limits.rotationEnabled) {
        const Float3 euler = quaternion_to_euler_xyz(value.rotation);
        value.rotation = quaternion_from_euler_xyz(clamp_vector(
            euler, control.limits.minimumRotationRadians, control.limits.maximumRotationRadians));
    }
    value.rotation = normalize(value.rotation);
    return value;
}

const ControlRigControl* find_control(
    const ControlRigAsset& rig, ControlRigControlId id) noexcept {
    const auto found = std::find_if(rig.controls.begin(), rig.controls.end(),
        [id](const ControlRigControl& control) { return control.id == id; });
    return found == rig.controls.end() ? nullptr : &*found;
}

bool apply_model_transform(
    const SkeletonAsset& skeleton, LocalPose& pose, BoneIndex bone,
    const RigidTransform& desiredModel, bool translation, bool rotation,
    std::string* error) {
    const auto model = compute_model_pose(skeleton, pose, error);
    if (model.empty()) return false;
    const std::int32_t parent = skeleton.bones[bone].parent;
    const RigidTransform desiredLocal = parent < 0 ? desiredModel : relative_rigid_transform(
        model[static_cast<std::size_t>(parent)], desiredModel);
    if (translation) pose[bone].position = desiredLocal.position;
    if (rotation) pose[bone].rotation = desiredLocal.rotation;
    return true;
}

bool solve_fabrik(
    const SkeletonAsset& skeleton, LocalPose& pose, std::span<const BoneIndex> chain,
    Float3 target, std::uint32_t maximumIterations, float tolerance, std::string* error) {
    auto model = compute_model_pose(skeleton, pose, error);
    if (model.empty()) return false;
    std::vector<Float3> positions;
    positions.reserve(chain.size());
    for (BoneIndex bone : chain) positions.push_back(model[bone].position);
    std::vector<float> lengths(chain.size() - 1U);
    float totalLength{};
    for (std::size_t i = 0; i + 1U < positions.size(); ++i) {
        lengths[i] = length(subtract(positions[i + 1U], positions[i]));
        if (!(lengths[i] > 1.0e-6F)) return fail(error, "FABRIK chain contains a zero-length segment");
        totalLength += lengths[i];
    }
    const Float3 root = positions.front();
    if (length(subtract(target, root)) >= totalLength) {
        const Float3 direction = normalize(subtract(target, root));
        for (std::size_t i = 1U; i < positions.size(); ++i)
            positions[i] = add(positions[i - 1U], multiply(direction, lengths[i - 1U]));
    } else {
        for (std::uint32_t iteration = 0U; iteration < maximumIterations; ++iteration) {
            positions.back() = target;
            for (std::size_t i = positions.size() - 1U; i > 0U; --i) {
                const Float3 direction = normalize(subtract(positions[i - 1U], positions[i]));
                positions[i - 1U] = add(positions[i], multiply(direction, lengths[i - 1U]));
            }
            positions.front() = root;
            for (std::size_t i = 1U; i < positions.size(); ++i) {
                const Float3 direction = normalize(subtract(positions[i], positions[i - 1U]));
                positions[i] = add(positions[i - 1U], multiply(direction, lengths[i - 1U]));
            }
            if (length(subtract(positions.back(), target)) <= tolerance) break;
        }
    }
    for (std::size_t i = 0U; i + 1U < chain.size(); ++i) {
        model = compute_model_pose(skeleton, pose, error);
        if (model.empty()) return false;
        const BoneIndex bone = chain[i];
        const BoneIndex child = chain[i + 1U];
        const Float3 currentDirection = subtract(model[child].position, model[bone].position);
        const Float3 desiredDirection = subtract(positions[i + 1U], positions[i]);
        const Quaternion delta = from_to(currentDirection, desiredDirection);
        const Quaternion desiredModelRotation = normalize(multiply(delta, model[bone].rotation));
        const std::int32_t parent = skeleton.bones[bone].parent;
        pose[bone].rotation = parent < 0 ? desiredModelRotation : normalize(multiply(
            conjugate(model[static_cast<std::size_t>(parent)].rotation), desiredModelRotation));
    }
    return true;
}

} // namespace

AnimationValidationResult validate_control_rig(
    const SkeletonAsset& skeleton, const ControlRigAsset& rig) noexcept {
    const auto invalid = [](std::string message) { return AnimationValidationResult{false, std::move(message)}; };
    if (!validate_skeleton(skeleton) || !valid_name(rig.name))
        return invalid("control rig name or skeleton is invalid");
    if (rig.controls.size() > 4096U || rig.nodes.empty() || rig.nodes.size() > 16384U)
        return invalid("control rig control or node count is out of range");
    std::map<ControlRigControlId, const ControlRigControl*> controls;
    std::set<std::string, std::less<>> controlNames;
    for (const ControlRigControl& control : rig.controls) {
        const ControlRigLimits& limits = control.limits;
        if (control.id == kInvalidControlRigControlId || !valid_name(control.name) ||
            !controls.emplace(control.id, &control).second || !controlNames.insert(control.name).second ||
            !finite(control.defaultLocal) || !finite(limits.minimumTranslation) ||
            !finite(limits.maximumTranslation) || !finite(limits.minimumRotationRadians) ||
            !finite(limits.maximumRotationRadians) ||
            limits.minimumTranslation.x > limits.maximumTranslation.x ||
            limits.minimumTranslation.y > limits.maximumTranslation.y ||
            limits.minimumTranslation.z > limits.maximumTranslation.z ||
            limits.minimumRotationRadians.x > limits.maximumRotationRadians.x ||
            limits.minimumRotationRadians.y > limits.maximumRotationRadians.y ||
            limits.minimumRotationRadians.z > limits.maximumRotationRadians.z)
            return invalid("control rig control identity, default, or limits are invalid");
        if (control.space == ControlRigSpace::Bone) {
            if (control.spaceBone >= skeleton.bones.size() ||
                control.parentControl != kInvalidControlRigControlId)
                return invalid("bone-space control reference is invalid");
        } else if (control.space == ControlRigSpace::Control) {
            if (control.parentControl == kInvalidControlRigControlId ||
                control.parentControl == control.id)
                return invalid("control-space parent is invalid");
        } else if (control.parentControl != kInvalidControlRigControlId ||
                   control.spaceBone != kInvalidBoneIndex) {
            return invalid("model-space control contains an unrelated space reference");
        }
    }
    for (const ControlRigControl& control : rig.controls)
        if (control.space == ControlRigSpace::Control && !controls.contains(control.parentControl))
            return invalid("control-space parent does not exist");
    std::map<ControlRigControlId, std::uint8_t> marks;
    std::function<bool(ControlRigControlId)> visit = [&](ControlRigControlId id) {
        std::uint8_t& mark = marks[id];
        if (mark == 1U) return false;
        if (mark == 2U) return true;
        mark = 1U;
        const ControlRigControl& control = *controls.at(id);
        if (control.space == ControlRigSpace::Control && !visit(control.parentControl)) return false;
        mark = 2U;
        return true;
    };
    for (const auto& [id, control] : controls) {
        (void)control;
        if (!visit(id)) return invalid("control rig spaces contain a cycle");
    }
    std::set<ControlRigNodeId> nodeIds;
    std::set<std::string, std::less<>> nodeNames;
    auto validBone = [&](BoneIndex bone) { return bone < skeleton.bones.size(); };
    auto validControl = [&](ControlRigControlId id) { return controls.contains(id); };
    for (const ControlRigNode& node : rig.nodes) {
        if (node.id == kInvalidControlRigNodeId || !nodeIds.insert(node.id).second ||
            !valid_name(node.name) || !nodeNames.insert(node.name).second ||
            !std::isfinite(node.weight) || node.weight < 0.0F || node.weight > 1.0F ||
            !finite(node.offset)) return invalid("control rig node identity, weight, or offset is invalid");
        switch (node.kind) {
            case ControlRigNodeKind::SetBoneTransform:
            case ControlRigNodeKind::ParentConstraint:
                if (!validBone(node.bone) || !validControl(node.targetControl) ||
                    (!node.affectTranslation && !node.affectRotation))
                    return invalid("control-driven bone node is invalid");
                break;
            case ControlRigNodeKind::CopyBoneTransform:
                if (!validBone(node.bone) || !validBone(node.sourceBone) || node.bone == node.sourceBone ||
                    (!node.affectTranslation && !node.affectRotation))
                    return invalid("copy-bone node is invalid");
                break;
            case ControlRigNodeKind::AimConstraint:
                if (!validBone(node.bone) || !validControl(node.targetControl) ||
                    !finite(node.localAimAxis) || !(length_squared(node.localAimAxis) > 1.0e-8F))
                    return invalid("aim-constraint node is invalid");
                break;
            case ControlRigNodeKind::TwoBoneIk:
                if (!validBone(node.bone) || !validBone(node.middleBone) || !validBone(node.endBone) ||
                    skeleton.bones[node.middleBone].parent != static_cast<std::int32_t>(node.bone) ||
                    skeleton.bones[node.endBone].parent != static_cast<std::int32_t>(node.middleBone) ||
                    !validControl(node.targetControl) || !validControl(node.poleControl) ||
                    !std::isfinite(node.maximumStretch) || node.maximumStretch < 1.0F ||
                    node.maximumStretch > 4.0F)
                    return invalid("two-bone IK node is invalid");
                break;
            case ControlRigNodeKind::Fabrik:
                if (node.chain.size() < 2U || node.chain.size() > 64U ||
                    !validControl(node.targetControl) || node.maximumIterations == 0U ||
                    node.maximumIterations > 128U || !std::isfinite(node.toleranceMeters) ||
                    node.toleranceMeters <= 0.0F) return invalid("FABRIK node is invalid");
                for (std::size_t i = 0U; i < node.chain.size(); ++i) {
                    if (!validBone(node.chain[i]) || (i > 0U &&
                        skeleton.bones[node.chain[i]].parent != static_cast<std::int32_t>(node.chain[i - 1U])))
                        return invalid("FABRIK node chain is not a direct hierarchy");
                }
                break;
        }
    }
    return {true, {}};
}

std::uint64_t control_rig_content_hash(const ControlRigAsset& rig) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, rig.name);
    hash_integer(hash, static_cast<std::uint64_t>(rig.controls.size()));
    for (const ControlRigControl& control : rig.controls) {
        hash_integer(hash, control.id);
        hash_string(hash, control.name);
        hash_byte(hash, static_cast<std::uint8_t>(control.kind));
        hash_byte(hash, static_cast<std::uint8_t>(control.space));
        hash_integer(hash, control.spaceBone);
        hash_integer(hash, control.parentControl);
        hash_transform(hash, control.defaultLocal);
        hash_byte(hash, control.limits.translationEnabled ? 1U : 0U);
        hash_float3(hash, control.limits.minimumTranslation);
        hash_float3(hash, control.limits.maximumTranslation);
        hash_byte(hash, control.limits.rotationEnabled ? 1U : 0U);
        hash_float3(hash, control.limits.minimumRotationRadians);
        hash_float3(hash, control.limits.maximumRotationRadians);
    }
    hash_integer(hash, static_cast<std::uint64_t>(rig.nodes.size()));
    for (const ControlRigNode& node : rig.nodes) {
        hash_integer(hash, node.id);
        hash_string(hash, node.name);
        hash_byte(hash, static_cast<std::uint8_t>(node.kind));
        hash_byte(hash, static_cast<std::uint8_t>(node.phase));
        hash_byte(hash, node.enabled ? 1U : 0U);
        hash_float(hash, node.weight);
        hash_integer(hash, node.bone); hash_integer(hash, node.sourceBone);
        hash_integer(hash, node.middleBone); hash_integer(hash, node.endBone);
        hash_integer(hash, static_cast<std::uint64_t>(node.chain.size()));
        for (BoneIndex bone : node.chain) hash_integer(hash, bone);
        hash_integer(hash, node.targetControl); hash_integer(hash, node.poleControl);
        hash_transform(hash, node.offset);
        hash_float3(hash, node.localAimAxis);
        hash_byte(hash, node.affectTranslation ? 1U : 0U);
        hash_byte(hash, node.affectRotation ? 1U : 0U);
        hash_byte(hash, node.allowStretch ? 1U : 0U);
        hash_float(hash, node.maximumStretch);
        hash_integer(hash, node.maximumIterations);
        hash_float(hash, node.toleranceMeters);
    }
    return hash;
}

bool write_dvecontrolrig(
    const std::filesystem::path& path, const SkeletonAsset& skeleton,
    const ControlRigAsset& rig, std::string* error) {
    const AnimationValidationResult validation = validate_control_rig(skeleton, rig);
    if (!validation) return fail(error, validation.message);
    const std::uint64_t hash = control_rig_content_hash(rig);
    if (rig.contentHash != 0U && rig.contentHash != hash)
        return fail(error, "control rig content hash is stale");
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "DVE_CONTROL_RIG 1\n";
    out << "name " << std::quoted(rig.name) << '\n';
    out << "hash " << hash << '\n';
    out << "controls " << rig.controls.size() << '\n';
    for (const ControlRigControl& control : rig.controls) {
        out << "control " << control.id << ' ' << std::quoted(control.name) << ' '
            << static_cast<unsigned>(control.kind) << ' ' << static_cast<unsigned>(control.space) << ' '
            << control.spaceBone << ' ' << control.parentControl << '\n';
        out << "default " << control.defaultLocal.position.x << ' ' << control.defaultLocal.position.y << ' '
            << control.defaultLocal.position.z << ' ' << control.defaultLocal.rotation.x << ' '
            << control.defaultLocal.rotation.y << ' ' << control.defaultLocal.rotation.z << ' '
            << control.defaultLocal.rotation.w << '\n';
        out << "limits " << control.limits.translationEnabled << ' '
            << control.limits.minimumTranslation.x << ' ' << control.limits.minimumTranslation.y << ' '
            << control.limits.minimumTranslation.z << ' ' << control.limits.maximumTranslation.x << ' '
            << control.limits.maximumTranslation.y << ' ' << control.limits.maximumTranslation.z << ' '
            << control.limits.rotationEnabled << ' ' << control.limits.minimumRotationRadians.x << ' '
            << control.limits.minimumRotationRadians.y << ' ' << control.limits.minimumRotationRadians.z << ' '
            << control.limits.maximumRotationRadians.x << ' ' << control.limits.maximumRotationRadians.y << ' '
            << control.limits.maximumRotationRadians.z << '\n';
    }
    out << "nodes " << rig.nodes.size() << '\n';
    for (const ControlRigNode& node : rig.nodes) {
        out << "node " << node.id << ' ' << std::quoted(node.name) << ' '
            << static_cast<unsigned>(node.kind) << ' ' << static_cast<unsigned>(node.phase) << ' '
            << node.enabled << ' ' << node.weight << ' ' << node.bone << ' ' << node.sourceBone << ' '
            << node.middleBone << ' ' << node.endBone << ' ' << node.targetControl << ' '
            << node.poleControl << '\n';
        out << "offset " << node.offset.position.x << ' ' << node.offset.position.y << ' '
            << node.offset.position.z << ' ' << node.offset.rotation.x << ' ' << node.offset.rotation.y << ' '
            << node.offset.rotation.z << ' ' << node.offset.rotation.w << '\n';
        out << "settings " << node.localAimAxis.x << ' ' << node.localAimAxis.y << ' '
            << node.localAimAxis.z << ' ' << node.affectTranslation << ' ' << node.affectRotation << ' '
            << node.allowStretch << ' ' << node.maximumStretch << ' ' << node.maximumIterations << ' '
            << node.toleranceMeters << ' ' << node.chain.size();
        for (BoneIndex bone : node.chain) out << ' ' << bone;
        out << '\n';
    }
    out << "end\n";
    return write_atomic(path, out.str(), error);
}

ControlRigReadResult read_dvecontrolrig(
    const std::filesystem::path& path, std::uint64_t maximumBytes) {
    ControlRigReadResult result;
    auto readFail = [&](std::string message) {
        result.asset = {};
        result.error = std::move(message);
        return result;
    };
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    if (ec) return readFail("could not inspect control rig asset: " + ec.message());
    if (size == 0U || size > maximumBytes || size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return readFail("control rig asset size is out of range");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::ifstream input(path, std::ios::binary);
    if (!input || !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        return readFail("could not read complete control rig asset");
    std::istringstream stream(bytes);
    std::string label;
    unsigned version{};
    if (!(stream >> label >> version) || label != "DVE_CONTROL_RIG" || version != 1U)
        return readFail("control rig header is invalid");
    if (!(stream >> label >> std::quoted(result.asset.name)) || label != "name")
        return readFail("control rig name record is invalid");
    std::uint64_t storedHash{};
    if (!(stream >> label >> storedHash) || label != "hash")
        return readFail("control rig hash record is invalid");
    std::size_t controlCount{};
    if (!(stream >> label >> controlCount) || label != "controls" || controlCount > 4096U)
        return readFail("control rig control count is invalid");
    result.asset.controls.resize(controlCount);
    for (ControlRigControl& control : result.asset.controls) {
        unsigned kind{}, space{};
        if (!(stream >> label >> control.id >> std::quoted(control.name) >> kind >> space >>
              control.spaceBone >> control.parentControl) || label != "control" ||
            kind > static_cast<unsigned>(ControlRigControlKind::Rotation) ||
            space > static_cast<unsigned>(ControlRigSpace::Control))
            return readFail("control rig control record is invalid");
        control.kind = static_cast<ControlRigControlKind>(kind);
        control.space = static_cast<ControlRigSpace>(space);
        if (!(stream >> label >> control.defaultLocal.position.x >> control.defaultLocal.position.y >>
              control.defaultLocal.position.z >> control.defaultLocal.rotation.x >>
              control.defaultLocal.rotation.y >> control.defaultLocal.rotation.z >>
              control.defaultLocal.rotation.w) || label != "default")
            return readFail("control rig default record is invalid");
        if (!(stream >> label >> control.limits.translationEnabled >>
              control.limits.minimumTranslation.x >> control.limits.minimumTranslation.y >>
              control.limits.minimumTranslation.z >> control.limits.maximumTranslation.x >>
              control.limits.maximumTranslation.y >> control.limits.maximumTranslation.z >>
              control.limits.rotationEnabled >> control.limits.minimumRotationRadians.x >>
              control.limits.minimumRotationRadians.y >> control.limits.minimumRotationRadians.z >>
              control.limits.maximumRotationRadians.x >> control.limits.maximumRotationRadians.y >>
              control.limits.maximumRotationRadians.z) || label != "limits")
            return readFail("control rig limits record is invalid");
    }
    std::size_t nodeCount{};
    if (!(stream >> label >> nodeCount) || label != "nodes" || nodeCount == 0U || nodeCount > 16384U)
        return readFail("control rig node count is invalid");
    result.asset.nodes.resize(nodeCount);
    for (ControlRigNode& node : result.asset.nodes) {
        unsigned kind{}, phase{};
        if (!(stream >> label >> node.id >> std::quoted(node.name) >> kind >> phase >> node.enabled >>
              node.weight >> node.bone >> node.sourceBone >> node.middleBone >> node.endBone >>
              node.targetControl >> node.poleControl) || label != "node" ||
            kind > static_cast<unsigned>(ControlRigNodeKind::Fabrik) ||
            phase > static_cast<unsigned>(ControlRigSolvePhase::PostSolve))
            return readFail("control rig node record is invalid");
        node.kind = static_cast<ControlRigNodeKind>(kind);
        node.phase = static_cast<ControlRigSolvePhase>(phase);
        if (!(stream >> label >> node.offset.position.x >> node.offset.position.y >> node.offset.position.z >>
              node.offset.rotation.x >> node.offset.rotation.y >> node.offset.rotation.z >>
              node.offset.rotation.w) || label != "offset")
            return readFail("control rig offset record is invalid");
        std::size_t chainCount{};
        if (!(stream >> label >> node.localAimAxis.x >> node.localAimAxis.y >> node.localAimAxis.z >>
              node.affectTranslation >> node.affectRotation >> node.allowStretch >> node.maximumStretch >>
              node.maximumIterations >> node.toleranceMeters >> chainCount) || label != "settings" ||
            chainCount > 64U) return readFail("control rig settings record is invalid");
        node.chain.resize(chainCount);
        for (BoneIndex& bone : node.chain)
            if (!(stream >> bone)) return readFail("control rig chain record is truncated");
    }
    if (!(stream >> label) || label != "end") return readFail("control rig end record is missing");
    stream >> std::ws;
    if (!stream.eof()) return readFail("control rig asset contains trailing data");
    const std::uint64_t calculatedHash = control_rig_content_hash(result.asset);
    if (storedHash != calculatedHash) return readFail("control rig content hash does not match payload");
    result.asset.contentHash = calculatedHash;
    return result;
}

static bool evaluate_control_rig_impl(
    const SkeletonAsset& skeleton, const ControlRigAsset& rig,
    std::span<const RigidTransform> inputPose,
    const std::map<ControlRigControlId, RigidTransform>& controlLocals,
    LocalPose& outputPose,
    ControlRigEvaluationTrace* trace,
    std::map<ControlRigControlId, RigidTransform>* outControlModels,
    std::string* error) {
    if (trace) *trace = {};
    const AnimationValidationResult validation = validate_control_rig(skeleton, rig);
    auto evaluationFail = [&](std::string message) {
        if (trace) trace->error = message;
        return fail(error, std::move(message));
    };
    if (!validation) return evaluationFail(validation.message);
    if (inputPose.size() != skeleton.bones.size())
        return evaluationFail("control rig input pose bone count does not match skeleton");
    std::string poseError;
    const auto baseModel = compute_model_pose(skeleton, inputPose, &poseError);
    if (baseModel.empty()) return evaluationFail(poseError);
    outputPose.assign(inputPose.begin(), inputPose.end());

    std::map<ControlRigControlId, RigidTransform> resolved;
    std::function<bool(const ControlRigControl&)> resolve = [&](const ControlRigControl& control) {
        if (resolved.contains(control.id)) return true;
        RigidTransform local = control.defaultLocal;
        if (const auto authored = controlLocals.find(control.id); authored != controlLocals.end())
            local = apply_control_policy(control, authored->second, control.defaultLocal);
        RigidTransform model = local;
        if (control.space == ControlRigSpace::Bone)
            model = compose_rigid_transforms(baseModel[control.spaceBone], local);
        else if (control.space == ControlRigSpace::Control) {
            const ControlRigControl* parent = find_control(rig, control.parentControl);
            if (!parent || !resolve(*parent)) return false;
            model = compose_rigid_transforms(resolved.at(parent->id), local);
        }
        resolved.emplace(control.id, model);
        return true;
    };
    for (const ControlRigControl& control : rig.controls)
        if (!resolve(control)) return evaluationFail("control rig space resolution failed");
    if (trace) trace->controlModels = resolved;

    std::vector<const ControlRigNode*> execution;
    execution.reserve(rig.nodes.size());
    for (const ControlRigNode& node : rig.nodes) execution.push_back(&node);
    std::stable_sort(execution.begin(), execution.end(), [](const ControlRigNode* a, const ControlRigNode* b) {
        return static_cast<std::uint8_t>(a->phase) < static_cast<std::uint8_t>(b->phase);
    });
    for (const ControlRigNode* node : execution) {
        if (!node->enabled || node->weight == 0.0F) continue;
        const LocalPose before = outputPose;
        bool executed = false;
        std::string nodeError;
        if (node->kind == ControlRigNodeKind::SetBoneTransform ||
            node->kind == ControlRigNodeKind::ParentConstraint) {
            const RigidTransform desired = compose_rigid_transforms(
                resolved.at(node->targetControl), node->offset);
            executed = apply_model_transform(skeleton, outputPose, node->bone, desired,
                node->affectTranslation, node->affectRotation, &nodeError);
        } else if (node->kind == ControlRigNodeKind::CopyBoneTransform) {
            const auto model = compute_model_pose(skeleton, outputPose, &nodeError);
            if (!model.empty()) {
                const RigidTransform desired = compose_rigid_transforms(model[node->sourceBone], node->offset);
                executed = apply_model_transform(skeleton, outputPose, node->bone, desired,
                    node->affectTranslation, node->affectRotation, &nodeError);
            }
        } else if (node->kind == ControlRigNodeKind::AimConstraint) {
            const auto model = compute_model_pose(skeleton, outputPose, &nodeError);
            if (!model.empty()) {
                const Float3 current = rotate(model[node->bone].rotation, normalize(node->localAimAxis));
                const Float3 target = subtract(resolved.at(node->targetControl).position, model[node->bone].position);
                if (!(length_squared(target) > 1.0e-8F)) nodeError = "aim control coincides with constrained bone";
                else {
                    RigidTransform desired = model[node->bone];
                    desired.rotation = normalize(multiply(from_to(current, target), desired.rotation));
                    executed = apply_model_transform(
                        skeleton, outputPose, node->bone, desired, false, true, &nodeError);
                }
            }
        } else if (node->kind == ControlRigNodeKind::TwoBoneIk) {
            TwoBoneIkRequest request;
            request.root = node->bone;
            request.middle = node->middleBone;
            request.end = node->endBone;
            request.targetModel = resolved.at(node->targetControl).position;
            request.poleModel = resolved.at(node->poleControl).position;
            request.allowStretch = node->allowStretch;
            request.maximumStretch = node->maximumStretch;
            executed = solve_two_bone_ik(skeleton, outputPose, request, &nodeError);
        } else if (node->kind == ControlRigNodeKind::Fabrik) {
            executed = solve_fabrik(skeleton, outputPose, node->chain,
                resolved.at(node->targetControl).position, node->maximumIterations,
                node->toleranceMeters, &nodeError);
        }
        ControlRigTraceEntry entry{node->id, node->phase, node->weight, 0.0F, executed};
        if (!executed) {
            if (nodeError.empty()) nodeError = "control rig node evaluation failed";
            if (trace) {
                trace->nodes.push_back(entry);
                trace->error = nodeError;
            }
            return fail(error, nodeError);
        }
        if (node->weight < 1.0F)
            outputPose = blend_local_poses(before, outputPose, node->weight);
        for (std::size_t bone = 0U; bone < outputPose.size(); ++bone)
            entry.maximumLocalTranslationDelta = std::max(
                entry.maximumLocalTranslationDelta,
                length(subtract(outputPose[bone].position, before[bone].position)));
        if (trace) trace->nodes.push_back(entry);
    }
    if (outControlModels) *outControlModels = resolved;
    return true;
}

bool evaluate_control_rig(
    const SkeletonAsset& skeleton, const ControlRigAsset& rig,
    std::span<const RigidTransform> inputPose,
    const std::map<ControlRigControlId, RigidTransform>& controlLocals,
    LocalPose& outputPose,
    std::map<ControlRigControlId, RigidTransform>* outControlModels,
    std::string* error) {
    return evaluate_control_rig_impl(
        skeleton, rig, inputPose, controlLocals, outputPose, nullptr, outControlModels, error);
}

bool evaluate_control_rig_traced(
    const SkeletonAsset& skeleton, const ControlRigAsset& rig,
    std::span<const RigidTransform> inputPose,
    const std::map<ControlRigControlId, RigidTransform>& controlLocals,
    LocalPose& outputPose, ControlRigEvaluationTrace& trace,
    std::map<ControlRigControlId, RigidTransform>* outControlModels,
    std::string* error) {
    return evaluate_control_rig_impl(
        skeleton, rig, inputPose, controlLocals, outputPose, &trace, outControlModels, error);
}

bool bake_control_rig_clip(
    const SkeletonAsset& skeleton, const AnimationClipAsset& source,
    const ControlRigAsset& rig,
    const std::map<ControlRigControlId, RigidTransform>& controlLocals,
    float samplesPerSecond, std::string outputName,
    AnimationClipAsset& output, std::string* error) {
    if (!validate_animation_clip(source, &skeleton)) return fail(error, "source animation clip is invalid");
    if (!validate_control_rig(skeleton, rig)) return fail(error, "control rig is invalid");
    if (!std::isfinite(samplesPerSecond) || samplesPerSecond < 1.0F || samplesPerSecond > 240.0F ||
        !valid_name(outputName)) return fail(error, "control rig bake settings are invalid");
    const std::uint32_t intervals = std::max<std::uint32_t>(
        1U, static_cast<std::uint32_t>(std::ceil(source.durationSeconds * samplesPerSecond)));
    output = {};
    output.name = std::move(outputName);
    output.durationSeconds = source.durationSeconds;
    output.looping = source.looping;
    output.events = source.events;
    output.tracks.resize(skeleton.bones.size());
    for (std::size_t bone = 0U; bone < skeleton.bones.size(); ++bone)
        output.tracks[bone].bone = static_cast<BoneIndex>(bone);
    for (std::uint32_t sample = 0U; sample <= intervals; ++sample) {
        const float time = source.durationSeconds * static_cast<float>(sample) / static_cast<float>(intervals);
        const LocalPose input = sample_animation_clip(skeleton, source, time);
        LocalPose baked;
        std::string bakeError;
        if (!evaluate_control_rig(skeleton, rig, input, controlLocals, baked, nullptr, &bakeError))
            return fail(error, "control rig bake failed: " + bakeError);
        for (std::size_t bone = 0U; bone < baked.size(); ++bone) {
            output.tracks[bone].translations.push_back({time, baked[bone].position});
            output.tracks[bone].rotations.push_back({time, baked[bone].rotation});
        }
    }
    const AnimationValidationResult validation = validate_animation_clip(output, &skeleton);
    if (!validation) return fail(error, validation.message);
    output.contentHash = animation_clip_content_hash(output);
    return true;
}

ControlRigRuntime::ControlRigRuntime(SkeletalAnimationRuntime& animation) noexcept
    : animation_(&animation) {}

bool ControlRigRuntime::bind(
    std::uint64_t objectId, ControlRigAsset rig, std::string* error) {
    const SkeletonAsset* skeleton = animation_->skeleton(objectId);
    if (!skeleton) return fail(error, "control rig object has no skeleton");
    const AnimationValidationResult validation = validate_control_rig(*skeleton, rig);
    if (!validation) return fail(error, validation.message);
    const std::uint64_t hash = control_rig_content_hash(rig);
    if (rig.contentHash != 0U && rig.contentHash != hash)
        return fail(error, "control rig content hash is stale");
    rig.contentHash = hash;
    Instance instance;
    for (const ControlRigControl& control : rig.controls)
        instance.controlLocals.emplace(control.id, control.defaultLocal);
    instance.rig = std::move(rig);
    instances_.insert_or_assign(objectId, std::move(instance));
    return true;
}

bool ControlRigRuntime::unbind(std::uint64_t objectId) noexcept {
    return instances_.erase(objectId) != 0U;
}

bool ControlRigRuntime::has_instance(std::uint64_t objectId) const noexcept {
    return instances_.contains(objectId);
}

bool ControlRigRuntime::set_enabled(std::uint64_t objectId, bool enabled) noexcept {
    const auto instance = instances_.find(objectId);
    if (instance == instances_.end()) return false;
    instance->second.enabled = enabled;
    return true;
}

bool ControlRigRuntime::set_control_local(
    std::uint64_t objectId, ControlRigControlId controlId, RigidTransform value,
    std::string* error) {
    const auto instance = instances_.find(objectId);
    if (instance == instances_.end()) return fail(error, "control rig is not bound");
    const ControlRigControl* control = find_control(instance->second.rig, controlId);
    if (!control) return fail(error, "control rig control does not exist");
    if (!finite(value)) return fail(error, "control rig value is not finite");
    const RigidTransform previous = instance->second.controlLocals.at(controlId);
    instance->second.controlLocals[controlId] = apply_control_policy(*control, value, previous);
    return true;
}

bool ControlRigRuntime::set_control_local(
    std::uint64_t objectId, std::string_view controlName, RigidTransform value,
    std::string* error) {
    const auto instance = instances_.find(objectId);
    if (instance == instances_.end()) return fail(error, "control rig is not bound");
    const auto control = std::find_if(instance->second.rig.controls.begin(), instance->second.rig.controls.end(),
        [controlName](const ControlRigControl& candidate) { return candidate.name == controlName; });
    if (control == instance->second.rig.controls.end()) return fail(error, "control rig control does not exist");
    return set_control_local(objectId, control->id, value, error);
}

bool ControlRigRuntime::reset_control(
    std::uint64_t objectId, ControlRigControlId controlId, std::string* error) {
    const auto instance = instances_.find(objectId);
    if (instance == instances_.end()) return fail(error, "control rig is not bound");
    const ControlRigControl* control = find_control(instance->second.rig, controlId);
    if (!control) return fail(error, "control rig control does not exist");
    instance->second.controlLocals[controlId] = control->defaultLocal;
    return true;
}

const RigidTransform* ControlRigRuntime::control_local(
    std::uint64_t objectId, ControlRigControlId control) const noexcept {
    const auto instance = instances_.find(objectId);
    if (instance == instances_.end()) return nullptr;
    const auto value = instance->second.controlLocals.find(control);
    return value == instance->second.controlLocals.end() ? nullptr : &value->second;
}

const RigidTransform* ControlRigRuntime::control_model(
    std::uint64_t objectId, ControlRigControlId control) const noexcept {
    const auto instance = instances_.find(objectId);
    if (instance == instances_.end()) return nullptr;
    const auto value = instance->second.controlModels.find(control);
    return value == instance->second.controlModels.end() ? nullptr : &value->second;
}

std::string_view ControlRigRuntime::last_error(std::uint64_t objectId) const noexcept {
    const auto instance = instances_.find(objectId);
    return instance == instances_.end() ? std::string_view{} : std::string_view(instance->second.lastError);
}

bool ControlRigRuntime::set_debug_tracing(std::uint64_t objectId, bool enabled) noexcept {
    const auto instance = instances_.find(objectId);
    if (instance == instances_.end()) return false;
    instance->second.debugTracing = enabled;
    if (!enabled) instance->second.trace = {};
    return true;
}

const ControlRigEvaluationTrace* ControlRigRuntime::evaluation_trace(
    std::uint64_t objectId) const noexcept {
    const auto instance = instances_.find(objectId);
    if (instance == instances_.end() || !instance->second.debugTracing) return nullptr;
    return &instance->second.trace;
}

bool ControlRigRuntime::evaluate(std::uint64_t objectId, std::string* error) {
    const auto instance = instances_.find(objectId);
    if (instance == instances_.end()) return fail(error, "control rig is not bound");
    if (!instance->second.enabled) return true;
    const SkeletonAsset* skeleton = animation_->skeleton(objectId);
    const LocalPose* input = animation_->local_pose(objectId);
    if (!skeleton || !input) return fail(error, "control rig animation instance is missing");
    LocalPose output;
    std::string evaluationError;
    const bool evaluated = instance->second.debugTracing
        ? evaluate_control_rig_traced(*skeleton, instance->second.rig, *input,
            instance->second.controlLocals, output, instance->second.trace,
            &instance->second.controlModels, &evaluationError)
        : evaluate_control_rig(*skeleton, instance->second.rig, *input,
            instance->second.controlLocals, output, &instance->second.controlModels,
            &evaluationError);
    if (!evaluated) {
        instance->second.lastError = evaluationError;
        return fail(error, evaluationError);
    }
    if (!animation_->publish_local_pose(objectId, output, &evaluationError)) {
        instance->second.lastError = evaluationError;
        return fail(error, evaluationError);
    }
    instance->second.lastError.clear();
    return true;
}

void ControlRigRuntime::evaluate_all() {
    for (auto& [objectId, instance] : instances_) {
        (void)instance;
        std::string ignored;
        (void)evaluate(objectId, &ignored);
    }
}

} // namespace dve
