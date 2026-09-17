#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "dve/animation.hpp"
#include "dve/component.hpp"
#include "dve/game_world.hpp"
#include "dve/rigid_body_adapter.hpp"

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float a, float b, float tolerance = 1.0e-4F) {
    return std::abs(a - b) <= tolerance;
}

SkeletonAsset make_skeleton() {
    SkeletonAsset skeleton;
    skeleton.name = "Test Rig";
    skeleton.bones = {
        {"root", -1, make_rigid_transform({}, {})},
        {"arm", 0, make_rigid_transform({0.0F, 1.0F, 0.0F}, {})},
        {"hand", 1, make_rigid_transform({1.0F, 0.0F, 0.0F}, {})},
    };
    skeleton.sockets = {
        {"weapon", 2U, make_rigid_transform({0.25F, 0.0F, 0.0F}, {})},
    };
    return skeleton;
}

AnimationClipAsset make_idle() {
    AnimationClipAsset clip;
    clip.name = "Idle";
    clip.durationSeconds = 2.0F;
    clip.looping = true;
    return clip;
}

AnimationClipAsset make_wave() {
    AnimationClipAsset clip;
    clip.name = "Wave";
    clip.durationSeconds = 2.0F;
    clip.looping = false;
    BoneAnimationTrack arm;
    arm.bone = 1U;
    arm.rotations = {
        {0.0F, {}},
        {2.0F, quaternion_from_axis_angle({0.0F, 0.0F, 1.0F}, 1.57079632679F)},
    };
    clip.tracks.push_back(std::move(arm));
    clip.events = {{1.0F, "halfway"}, {2.0F, "complete"}};
    return clip;
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void test_validation_and_deterministic_assets() {
    SkeletonAsset skeleton = make_skeleton();
    require(static_cast<bool>(validate_skeleton(skeleton)), "valid skeleton was rejected");
    require(static_cast<bool>(validate_animation_clip(make_wave(), &skeleton)), "valid animation clip was rejected");

    SkeletonAsset invalid = skeleton;
    invalid.bones[1].parent = 2;
    require(!validate_skeleton(invalid), "forward/cyclic skeleton parent was accepted");
    invalid = skeleton;
    invalid.sockets.push_back(invalid.sockets.front());
    require(!validate_skeleton(invalid), "duplicate skeletal socket was accepted");

    AnimationClipAsset badClip = make_wave();
    badClip.tracks.front().rotations[1].timeSeconds = 0.0F;
    require(!validate_animation_clip(badClip, &skeleton), "unordered animation keys were accepted");

    const auto root = std::filesystem::temp_directory_path() / "dve_animation_v180_assets";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::string error;
    const auto skeletonA = root / "test_a.dveskeleton";
    const auto skeletonB = root / "test_b.dveskeleton";
    const auto clipA = root / "wave_a.dveanim";
    const auto clipB = root / "wave_b.dveanim";
    require(write_dveskeleton(skeletonA, skeleton, &error), error);
    const SkeletonReadResult loadedSkeleton = read_dveskeleton(skeletonA);
    require(static_cast<bool>(loadedSkeleton), loadedSkeleton.error);
    require(loadedSkeleton.asset.contentHash == skeleton_content_hash(skeleton), "skeleton hash changed on read");
    require(write_dveskeleton(skeletonB, loadedSkeleton.asset, &error), error);
    require(read_bytes(skeletonA) == read_bytes(skeletonB), "skeleton write is not byte deterministic");
    const std::string skeletonBytes = read_bytes(skeletonA);
    require(write_dveskeleton(skeletonA, loadedSkeleton.asset, &error), error);
    require(read_bytes(skeletonA) == skeletonBytes, "transactional skeleton replacement changed bytes");

    const AnimationClipAsset wave = make_wave();
    require(write_dveanim(clipA, wave, &error), error);
    const AnimationClipReadResult loadedClip = read_dveanim(clipA);
    require(static_cast<bool>(loadedClip), loadedClip.error);
    require(loadedClip.asset.contentHash == animation_clip_content_hash(wave), "clip hash changed on read");
    require(write_dveanim(clipB, loadedClip.asset, &error), error);
    require(read_bytes(clipA) == read_bytes(clipB), "animation clip write is not byte deterministic");
    const std::string clipBytes = read_bytes(clipA);
    require(write_dveanim(clipA, loadedClip.asset, &error), error);
    require(read_bytes(clipA) == clipBytes, "transactional clip replacement changed bytes");

    {
        std::ofstream stream(clipA, std::ios::binary | std::ios::app);
        stream << "junk";
    }
    require(!read_dveanim(clipA), "trailing animation asset data was accepted");
    std::filesystem::remove_all(root, ec);
}

void test_sampling_blending_and_model_pose() {
    const SkeletonAsset skeleton = make_skeleton();
    const AnimationClipAsset wave = make_wave();
    const LocalPose bind = make_bind_pose(skeleton);
    const LocalPose half = sample_animation_clip(skeleton, wave, 1.0F);
    const Float3 rotated = rotate(half[1].rotation, {1.0F, 0.0F, 0.0F});
    require(close(rotated.x, 0.7071067F) && close(rotated.y, 0.7071067F),
            "clip rotation did not interpolate by shortest arc");

    const LocalPose quarter = blend_local_poses(bind, half, 0.5F);
    const Float3 quarterDirection = rotate(quarter[1].rotation, {1.0F, 0.0F, 0.0F});
    require(close(quarterDirection.x, 0.9238795F) && close(quarterDirection.y, 0.3826834F),
            "two-pose blend is incorrect");

    std::string error;
    const std::array<LocalPose, 2> poses{bind, half};
    const std::array<float, 2> weights{1.0F, 3.0F};
    const LocalPose stack = blend_local_pose_stack(poses, weights, &error);
    require(stack.size() == skeleton.bones.size(), error);
    const auto model = compute_model_pose(skeleton, half, &error);
    require(model.size() == skeleton.bones.size(), error);
    require(close(model[2].position.x, 0.7071067F) && close(model[2].position.y, 1.7071067F),
            "hierarchical model pose is incorrect");

    const auto socket = resolve_skeletal_socket(
        skeleton, half, "weapon", make_rigid_transform({10.0F, 0.0F, 0.0F}, {}), &error);
    require(socket.has_value(), error);
    require(close(socket->position.x, 10.8838835F) && close(socket->position.y, 1.8838835F),
            "skeletal socket did not follow model pose");
}

void test_cpu_skinning_reference() {
    const SkeletonAsset skeleton = make_skeleton();
    LocalPose pose = make_bind_pose(skeleton);
    pose[1].rotation = quaternion_from_axis_angle({0.0F, 0.0F, 1.0F}, 1.57079632679F);
    SkinVertex vertex;
    vertex.position = {1.0F, 1.0F, 0.0F};
    vertex.normal = {1.0F, 0.0F, 0.0F};
    vertex.influences.bones[0] = 1U;
    vertex.influences.weights[0] = 2.0F; // reference path normalizes authored weights
    std::vector<CpuSkinnedVertex> output;
    std::string error;
    require(skin_vertices_cpu(skeleton, pose, std::span<const SkinVertex>(&vertex, 1U), output, &error), error);
    require(output.size() == 1U && close(output[0].position.x, 0.0F) && close(output[0].position.y, 2.0F),
            "CPU skinning position is incorrect");
    require(close(output[0].normal.x, 0.0F) && close(output[0].normal.y, 1.0F),
            "CPU skinning normal is incorrect");

    vertex.influences.bones[0] = 99U;
    require(!skin_vertices_cpu(skeleton, pose, std::span<const SkinVertex>(&vertex, 1U), output, &error),
            "out-of-range skin influence was accepted");
}

void test_runtime_crossfade_and_game_world_socket_attachment() {
    const SkeletonAsset skeleton = make_skeleton();
    const AnimationClipAsset idle = make_idle();
    const AnimationClipAsset wave = make_wave();
    SkeletalAnimationRuntime runtime;
    std::string error;
    require(runtime.bind_skeleton(7U, skeleton, &error), error);
    require(runtime.add_clip(7U, idle, &error), error);
    require(runtime.add_clip(7U, wave, &error), error);
    require(runtime.play(7U, "Idle", true, &error), error);
    require(runtime.crossfade(7U, "Wave", 0.5F, &error), error);
    runtime.tick(0.6F);
    require(runtime.active_clip(7U) == "Wave", "crossfade did not promote target clip");

    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc parentDesc;
    parentDesc.name = "Character";
    parentDesc.transform.position = {5.0F, 0.0F, 0.0F};
    const GameObjectId parent = world.create_object(std::move(parentDesc), &error);
    require(parent != kInvalidGameObjectId, error);
    GameObjectDesc childDesc;
    childDesc.name = "Sword";
    const GameObjectId child = world.create_object(std::move(childDesc), &error);
    require(child != kInvalidGameObjectId, error);
    require(world.animation().bind_skeleton(parent, skeleton, &error), error);
    require(world.animation().add_clip(parent, wave, &error), error);
    require(world.animation().play(parent, "Wave", true, &error), error);
    require(!world.attach_object(child, parent, false, "missing_socket", true, true, &error),
            "unknown skeletal socket was accepted");
    require(world.attach_object(child, parent, false, "weapon", true, true, &error), error);
    const auto before = world.transform(child);
    require(before && close(before->position.x, 6.25F) && close(before->position.y, 1.0F),
            "initial socket attachment transform is incorrect");
    world.tick(1.0F);
    const auto after = world.transform(child);
    require(after && close(after->position.x, 5.8838835F) && close(after->position.y, 1.8838835F),
            "GameWorld socket attachment did not follow sampled animation");
    require(world.destroy_object(parent), "animated parent destruction failed");
    require(!world.animation().has_instance(parent), "destroyed object retained its animation instance");
    require(!world.parent_of(child).has_value(), "child did not detach when animated parent was destroyed");
}

void test_animator_component_schema() {
    const ComponentTypeRegistry registry = ComponentTypeRegistry::make_default();
    const ComponentTypeSchema* schema = registry.find("dve.skeletal_animator");
    require(schema && !schema->allowMultiple, "skeletal animator component schema is missing");
    require(schema->properties.size() == 4U && schema->properties[0].assetReference &&
            schema->properties[1].assetReference, "animator asset-reference properties are incomplete");
}

} // namespace

int main() {
    try {
        test_validation_and_deterministic_assets();
        test_sampling_blending_and_model_pose();
        test_cpu_skinning_reference();
        test_runtime_crossfade_and_game_world_socket_attachment();
        test_animator_component_schema();
        std::cout << "DVE v1.80 skeletal animation tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE v1.80 skeletal animation tests failed: " << exception.what() << '\n';
        return 1;
    }
}
