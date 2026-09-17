#include "dve/animation_controller.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <type_traits>

namespace dve {
namespace {

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool valid_name(std::string_view name) noexcept {
    return !name.empty() && name.size() <= 255U &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 32U && c != 127U; });
}

bool finite_transform(const RigidTransform& value) noexcept {
    const float normSquared = value.rotation.x * value.rotation.x + value.rotation.y * value.rotation.y +
        value.rotation.z * value.rotation.z + value.rotation.w * value.rotation.w;
    return std::isfinite(value.position.x) && std::isfinite(value.position.y) &&
        std::isfinite(value.position.z) && std::isfinite(value.rotation.x) &&
        std::isfinite(value.rotation.y) && std::isfinite(value.rotation.z) &&
        std::isfinite(value.rotation.w) && normSquared > 0.0F;
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

RigidTransform inverse_transform(const RigidTransform& value) noexcept {
    const Quaternion rotation = conjugate(normalize(value.rotation));
    return make_rigid_transform(rotate(rotation, multiply(value.position, -1.0F)), rotation);
}

bool condition_matches(
    const AnimationCondition& condition,
    const std::map<std::string, AnimationParameterValue, std::less<>>& parameters,
    const std::map<std::string, bool, std::less<>>& triggers) {
    if (condition.operation == AnimationConditionOperator::Triggered) {
        const auto found = triggers.find(condition.parameter);
        return found != triggers.end() && found->second;
    }
    const auto found = parameters.find(condition.parameter);
    if (found == parameters.end() || found->second.index() != condition.value.index()) return false;
    return std::visit([&](const auto& left) {
        using T = std::decay_t<decltype(left)>;
        const T right = std::get<T>(condition.value);
        switch (condition.operation) {
            case AnimationConditionOperator::Equal: return left == right;
            case AnimationConditionOperator::NotEqual: return left != right;
            case AnimationConditionOperator::Greater:
                if constexpr (!std::is_same_v<T, bool>) return left > right; else return false;
            case AnimationConditionOperator::GreaterOrEqual:
                if constexpr (!std::is_same_v<T, bool>) return left >= right; else return false;
            case AnimationConditionOperator::Less:
                if constexpr (!std::is_same_v<T, bool>) return left < right; else return false;
            case AnimationConditionOperator::LessOrEqual:
                if constexpr (!std::is_same_v<T, bool>) return left <= right; else return false;
            case AnimationConditionOperator::Triggered: return false;
        }
        return false;
    }, found->second);
}

} // namespace

AnimationValidationResult validate_animation_controller(
    const AnimationControllerAsset& controller) noexcept {
    const auto invalid = [](std::string message) { return AnimationValidationResult{false, std::move(message)}; };
    if (!valid_name(controller.name) || !valid_name(controller.initialState))
        return invalid("animation controller name or initial state is invalid");
    if (controller.states.empty() || controller.states.size() > 4096U)
        return invalid("animation controller state count is out of range");
    std::set<std::string, std::less<>> states;
    for (const AnimationControllerState& state : controller.states) {
        if (!valid_name(state.name) || !valid_name(state.clip) || !states.insert(state.name).second)
            return invalid("animation controller state names or clips are invalid");
        if (!std::isfinite(state.playbackSpeed) || state.playbackSpeed < 0.0F || state.playbackSpeed > 16.0F)
            return invalid("animation controller playback speed is invalid");
    }
    if (!states.contains(controller.initialState)) return invalid("animation controller initial state is missing");
    for (const auto& [name, value] : controller.parameters) {
        if (!valid_name(name)) return invalid("animation controller parameter name is invalid");
        if (const double* number = std::get_if<double>(&value); number && !std::isfinite(*number))
            return invalid("animation controller parameter is not finite");
    }
    if (controller.transitions.size() > 16384U) return invalid("animation transition count is out of range");
    for (const AnimationControllerTransition& transition : controller.transitions) {
        if (!states.contains(transition.from) || !states.contains(transition.to) || transition.from == transition.to)
            return invalid("animation transition endpoints are invalid");
        if (!std::isfinite(transition.blendSeconds) || transition.blendSeconds < 0.0F ||
            !std::isfinite(transition.minimumStateTimeSeconds) || transition.minimumStateTimeSeconds < 0.0F)
            return invalid("animation transition timing is invalid");
        for (const AnimationCondition& condition : transition.conditions) {
            if (!valid_name(condition.parameter)) return invalid("animation condition parameter is invalid");
            if (condition.operation != AnimationConditionOperator::Triggered) {
                const auto parameter = controller.parameters.find(condition.parameter);
                if (parameter == controller.parameters.end() || parameter->second.index() != condition.value.index())
                    return invalid("animation condition parameter is missing or has the wrong type");
            }
        }
    }
    return {true, {}};
}

AnimationControllerRuntime::AnimationControllerRuntime(SkeletalAnimationRuntime& animation) noexcept
    : animation_(&animation) {}

const AnimationControllerState* AnimationControllerRuntime::find_state(
    const Instance& instance, std::string_view name) const noexcept {
    const auto found = std::find_if(instance.asset.states.begin(), instance.asset.states.end(),
        [name](const AnimationControllerState& state) { return state.name == name; });
    return found == instance.asset.states.end() ? nullptr : &*found;
}

bool AnimationControllerRuntime::enter_state(
    std::uint64_t objectId, Instance& instance, std::string_view stateName,
    float blendSeconds, std::string* error) {
    const AnimationControllerState* next = find_state(instance, stateName);
    if (!next) return fail(error, "animation controller state does not exist");
    const bool started = instance.state.empty()
        ? animation_->play(objectId, next->clip, true, error)
        : animation_->crossfade(objectId, next->clip, blendSeconds, error);
    if (!started || !animation_->set_playback_speed(objectId, next->playbackSpeed, error)) return false;
    (void)animation_->set_root_motion_enabled(objectId, next->applyRootMotion);
    instance.state = next->name;
    instance.stateTime = 0.0F;
    return true;
}

bool AnimationControllerRuntime::bind(
    std::uint64_t objectId, AnimationControllerAsset controller, std::string* error) {
    if (!animation_->has_instance(objectId)) return fail(error, "animation controller object has no skeleton");
    const AnimationValidationResult validation = validate_animation_controller(controller);
    if (!validation) return fail(error, validation.message);
    Instance instance;
    instance.parameters = controller.parameters;
    for (const AnimationControllerTransition& transition : controller.transitions)
        for (const AnimationCondition& condition : transition.conditions)
            if (condition.operation == AnimationConditionOperator::Triggered)
                instance.triggers.try_emplace(condition.parameter, false);
    instance.asset = std::move(controller);
    const std::string initial = instance.asset.initialState;
    auto [position, inserted] = instances_.insert_or_assign(objectId, std::move(instance));
    (void)inserted;
    if (!enter_state(objectId, position->second, initial, 0.0F, error)) {
        instances_.erase(position);
        return false;
    }
    return true;
}

bool AnimationControllerRuntime::unbind(std::uint64_t objectId) noexcept {
    return instances_.erase(objectId) != 0U;
}

bool AnimationControllerRuntime::set_parameter(
    std::uint64_t objectId, std::string_view name, AnimationParameterValue value,
    std::string* error) {
    const auto instance = instances_.find(objectId);
    if (instance == instances_.end()) return fail(error, "animation controller is not bound");
    const auto parameter = instance->second.parameters.find(name);
    if (parameter == instance->second.parameters.end()) return fail(error, "animation parameter does not exist");
    if (parameter->second.index() != value.index()) return fail(error, "animation parameter type mismatch");
    if (const double* number = std::get_if<double>(&value); number && !std::isfinite(*number))
        return fail(error, "animation parameter is not finite");
    parameter->second = value;
    return true;
}

bool AnimationControllerRuntime::trigger(
    std::uint64_t objectId, std::string_view name, std::string* error) {
    const auto instance = instances_.find(objectId);
    if (instance == instances_.end()) return fail(error, "animation controller is not bound");
    const auto trigger = instance->second.triggers.find(name);
    if (trigger == instance->second.triggers.end()) return fail(error, "animation trigger does not exist");
    trigger->second = true;
    return true;
}

std::string_view AnimationControllerRuntime::state(std::uint64_t objectId) const noexcept {
    const auto instance = instances_.find(objectId);
    return instance == instances_.end() ? std::string_view{} : std::string_view(instance->second.state);
}

float AnimationControllerRuntime::state_time(std::uint64_t objectId) const noexcept {
    const auto instance = instances_.find(objectId);
    return instance == instances_.end() ? 0.0F : instance->second.stateTime;
}

void AnimationControllerRuntime::tick(float deltaSeconds) {
    if (!(deltaSeconds > 0.0F) || !std::isfinite(deltaSeconds)) return;
    for (auto& [objectId, instance] : instances_) {
        const AnimationControllerState* current = find_state(instance, instance.state);
        if (!current) continue;
        instance.stateTime += deltaSeconds * current->playbackSpeed;
        std::vector<const AnimationControllerTransition*> candidates;
        for (const AnimationControllerTransition& transition : instance.asset.transitions)
            if (transition.from == instance.state && instance.stateTime >= transition.minimumStateTimeSeconds)
                candidates.push_back(&transition);
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto* a, const auto* b) {
            return a->priority > b->priority;
        });
        for (const AnimationControllerTransition* transition : candidates) {
            const bool matches = std::all_of(transition->conditions.begin(), transition->conditions.end(),
                [&](const AnimationCondition& condition) {
                    return condition_matches(condition, instance.parameters, instance.triggers);
                });
            if (!matches) continue;
            std::string ignored;
            if (enter_state(objectId, instance, transition->to, transition->blendSeconds, &ignored)) {
                for (const AnimationCondition& condition : transition->conditions)
                    if (condition.operation == AnimationConditionOperator::Triggered)
                        instance.triggers[condition.parameter] = false;
            }
            break;
        }
    }
}

bool solve_two_bone_ik(
    const SkeletonAsset& skeleton, LocalPose& pose, const TwoBoneIkRequest& request,
    std::string* error) {
    if (request.root >= skeleton.bones.size() || request.middle >= skeleton.bones.size() ||
        request.end >= skeleton.bones.size() || skeleton.bones[request.middle].parent != request.root ||
        skeleton.bones[request.end].parent != request.middle)
        return fail(error, "two-bone IK requires a valid direct bone chain");
    if (!std::isfinite(request.weight) || request.weight < 0.0F || request.weight > 1.0F ||
        !std::isfinite(request.maximumStretch) || request.maximumStretch < 1.0F || request.maximumStretch > 4.0F)
        return fail(error, "two-bone IK weight or stretch limit is invalid");
    std::string poseError;
    auto model = compute_model_pose(skeleton, pose, &poseError);
    if (model.empty()) return fail(error, poseError);
    const Float3 rootPosition = model[request.root].position;
    float upperLength = length(subtract(model[request.middle].position, rootPosition));
    float lowerLength = length(subtract(model[request.end].position, model[request.middle].position));
    if (!(upperLength > 1.0e-6F) || !(lowerLength > 1.0e-6F))
        return fail(error, "two-bone IK chain has zero-length segments");
    Float3 toTarget = subtract(request.targetModel, rootPosition);
    float distance = length(toTarget);
    if (!(distance > 1.0e-6F)) return fail(error, "two-bone IK target coincides with chain root");
    const LocalPose original = pose;
    if (request.allowStretch && distance > upperLength + lowerLength) {
        const float scale = std::min(request.maximumStretch, distance / (upperLength + lowerLength));
        pose[request.middle].position = multiply(pose[request.middle].position, scale);
        pose[request.end].position = multiply(pose[request.end].position, scale);
        model = compute_model_pose(skeleton, pose, &poseError);
        upperLength *= scale;
        lowerLength *= scale;
    }
    const Float3 direction = normalize(toTarget);
    distance = std::clamp(distance, std::abs(upperLength - lowerLength) + 1.0e-5F,
                          upperLength + lowerLength - 1.0e-5F);
    Float3 planeNormal = cross(direction, subtract(request.poleModel, rootPosition));
    if (!(length_squared(planeNormal) > 1.0e-8F)) planeNormal = cross(direction, {0.0F, 0.0F, 1.0F});
    if (!(length_squared(planeNormal) > 1.0e-8F)) planeNormal = cross(direction, {0.0F, 1.0F, 0.0F});
    planeNormal = normalize(planeNormal);
    const Float3 bendDirection = normalize(cross(planeNormal, direction));
    const float along = (distance * distance + upperLength * upperLength - lowerLength * lowerLength) /
                        (2.0F * distance);
    const float perpendicular = std::sqrt(std::max(0.0F, upperLength * upperLength - along * along));
    const Float3 elbow = add(rootPosition,
        add(multiply(direction, along), multiply(bendDirection, perpendicular)));

    const Float3 currentUpper = subtract(model[request.middle].position, rootPosition);
    const Quaternion rootDelta = from_to(currentUpper, subtract(elbow, rootPosition));
    const Quaternion desiredRootModel = normalize(multiply(rootDelta, model[request.root].rotation));
    const std::int32_t rootParent = skeleton.bones[request.root].parent;
    pose[request.root].rotation = rootParent < 0 ? desiredRootModel : normalize(multiply(
        conjugate(model[static_cast<std::size_t>(rootParent)].rotation), desiredRootModel));
    model = compute_model_pose(skeleton, pose, &poseError);

    const Float3 currentLower = subtract(model[request.end].position, model[request.middle].position);
    const Quaternion middleDelta = from_to(currentLower, subtract(request.targetModel, elbow));
    const Quaternion desiredMiddleModel = normalize(multiply(middleDelta, model[request.middle].rotation));
    pose[request.middle].rotation = normalize(multiply(
        conjugate(model[request.root].rotation), desiredMiddleModel));
    if (request.weight < 1.0F) pose = blend_local_poses(original, pose, request.weight);
    return true;
}

AnimationValidationResult validate_ragdoll(
    const SkeletonAsset& skeleton, const RagdollDefinition& ragdoll) noexcept {
    constexpr float kPi = 3.14159265358979323846F;
    const auto invalid = [](std::string message) { return AnimationValidationResult{false, std::move(message)}; };
    if (!validate_skeleton(skeleton) || !valid_name(ragdoll.name) || ragdoll.bodies.empty() ||
        ragdoll.bodies.size() > skeleton.bones.size()) return invalid("ragdoll name, skeleton, or body count is invalid");
    std::set<BoneIndex> bones;
    for (const RagdollBodyDefinition& body : ragdoll.bodies) {
        if (body.bone >= skeleton.bones.size() || !bones.insert(body.bone).second ||
            !std::isfinite(body.massKilograms) || body.massKilograms <= 0.0F ||
            !std::isfinite(body.halfExtents.x) || !std::isfinite(body.halfExtents.y) ||
            !std::isfinite(body.halfExtents.z) || body.halfExtents.x <= 0.0F ||
            body.halfExtents.y <= 0.0F || body.halfExtents.z <= 0.0F ||
            !finite_transform(body.bodyFromBone) || !std::isfinite(body.simulationWeight) ||
            body.simulationWeight < 0.0F || body.simulationWeight > 1.0F)
            return invalid("ragdoll body is invalid");
    }
    for (const RagdollJointDefinition& joint : ragdoll.joints) {
        if (joint.parentBody >= ragdoll.bodies.size() || joint.childBody >= ragdoll.bodies.size() ||
            joint.parentBody == joint.childBody || !(length_squared(joint.axis) > 0.0F) ||
            !std::isfinite(joint.minimumRadians) || !std::isfinite(joint.maximumRadians) ||
            joint.minimumRadians > joint.maximumRadians || joint.minimumRadians < -kPi ||
            joint.maximumRadians > kPi || !std::isfinite(joint.swingLimitRadians) ||
            joint.swingLimitRadians < 0.0F || joint.swingLimitRadians > kPi)
            return invalid("ragdoll joint is invalid");
    }
    std::vector<std::int32_t> parent(ragdoll.bodies.size(), -1);
    std::set<std::pair<std::uint16_t, std::uint16_t>> pairs;
    for (const RagdollJointDefinition& joint : ragdoll.joints) {
        const auto pair = std::minmax(joint.parentBody, joint.childBody);
        if (!pairs.emplace(pair.first, pair.second).second || parent[joint.childBody] != -1)
            return invalid("ragdoll joints are duplicated or give one body multiple parents");
        parent[joint.childBody] = static_cast<std::int32_t>(joint.parentBody);
    }
    for (std::size_t body = 0U; body < parent.size(); ++body) {
        std::set<std::size_t> visited;
        std::int32_t current = static_cast<std::int32_t>(body);
        while (current >= 0) {
            const std::size_t index = static_cast<std::size_t>(current);
            if (!visited.insert(index).second) return invalid("ragdoll joints contain a cycle");
            current = parent[index];
        }
    }
    return {true, {}};
}

LocalPose ragdoll_pose_from_body_world(
    const SkeletonAsset& skeleton, const RagdollDefinition& ragdoll,
    const RigidTransform& objectWorld, std::span<const RigidTransform> bodyWorld,
    std::span<const RigidTransform> animationPose, float blendWeight, std::string* error) {
    std::vector<float> weights(ragdoll.bodies.size(), 1.0F);
    return ragdoll_pose_from_body_world_weighted(
        skeleton, ragdoll, objectWorld, bodyWorld, animationPose, blendWeight, weights, error);
}

LocalPose ragdoll_pose_from_body_world_weighted(
    const SkeletonAsset& skeleton, const RagdollDefinition& ragdoll,
    const RigidTransform& objectWorld, std::span<const RigidTransform> bodyWorld,
    std::span<const RigidTransform> animationPose, float globalBlendWeight,
    std::span<const float> bodyBlendWeights, std::string* error) {
    const AnimationValidationResult validation = validate_ragdoll(skeleton, ragdoll);
    if (!validation) { fail(error, validation.message); return {}; }
    if (bodyWorld.size() != ragdoll.bodies.size() || animationPose.size() != skeleton.bones.size() ||
        bodyBlendWeights.size() != ragdoll.bodies.size() || !std::isfinite(globalBlendWeight) ||
        globalBlendWeight < 0.0F || globalBlendWeight > 1.0F ||
        std::any_of(bodyBlendWeights.begin(), bodyBlendWeights.end(), [](float weight) {
            return !std::isfinite(weight) || weight < 0.0F || weight > 1.0F;
        })) {
        fail(error, "ragdoll pose input sizes or blend weight are invalid"); return {};
    }
    LocalPose result(animationPose.begin(), animationPose.end());
    std::map<BoneIndex, RigidTransform> desiredModel;
    for (std::size_t i = 0; i < ragdoll.bodies.size(); ++i) {
        const RigidTransform bodyModel = relative_rigid_transform(objectWorld, bodyWorld[i]);
        desiredModel[ragdoll.bodies[i].bone] = compose_rigid_transforms(
            bodyModel, inverse_transform(ragdoll.bodies[i].bodyFromBone));
    }
    std::vector<RigidTransform> model(skeleton.bones.size());
    for (std::size_t bone = 0; bone < skeleton.bones.size(); ++bone) {
        const std::int32_t parent = skeleton.bones[bone].parent;
        if (const auto desired = desiredModel.find(static_cast<BoneIndex>(bone)); desired != desiredModel.end())
            result[bone] = parent < 0 ? desired->second : relative_rigid_transform(
                model[static_cast<std::size_t>(parent)], desired->second);
        model[bone] = parent < 0 ? result[bone] : compose_rigid_transforms(
            model[static_cast<std::size_t>(parent)], result[bone]);
    }
    for (std::size_t body = 0U; body < ragdoll.bodies.size(); ++body) {
        const BoneIndex bone = ragdoll.bodies[body].bone;
        result[bone] = interpolate_rigid_transform(
            animationPose[bone], result[bone], globalBlendWeight * bodyBlendWeights[body]);
    }
    return result;
}

void RagdollBlendState::tick(float deltaSeconds) noexcept {
    if (!(deltaSeconds > 0.0F) || !std::isfinite(deltaSeconds) ||
        !std::isfinite(blendRatePerSecond) || blendRatePerSecond < 0.0F) return;
    const float step = blendRatePerSecond * deltaSeconds;
    if (weight < targetWeight) weight = std::min(targetWeight, weight + step);
    else weight = std::max(targetWeight, weight - step);
    weight = std::clamp(weight, 0.0F, 1.0F);
}

} // namespace dve
