#include "dve/sprite_rig2d.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void run() {
    SpriteRig2DAuthoringSession session;
    std::string error;
    SpriteRigBoneId upper{}, lower{};
    require(session.add_bone("upper_arm", 1U, {{4.0F, 8.0F}, 0.0F, {1.0F,1.0F}}, 10.0F,
                             &upper, &error), error);
    require(session.add_bone("lower_arm", upper, {{10.0F, 0.0F}, 0.0F, {1.0F,1.0F}}, 9.0F,
                             &lower, &error), error);
    SpriteRigPart2D torso;
    torso.name = "torso"; torso.bone = 1U;
    torso.spriteAsset = "assets/sprites/v218_sun_route_cast.dvesprite";
    torso.clip = "player_idle"; torso.drawOrder = 0;
    SpriteRigPartId torsoId{};
    require(session.add_part(torso, &torsoId, &error), error);
    SpriteRigPart2D hand;
    hand.name = "hand"; hand.bone = lower;
    hand.spriteAsset = "assets/sprites/v218_sun_route_cast.dvesprite";
    hand.clip = "projectile"; hand.local.translation = {9.0F, 0.0F}; hand.drawOrder = 2;
    SpriteRigPartId handId{};
    require(session.add_part(hand, &handId, &error), error);
    require(session.add_clip({"wave", 1.0F, true, {}, {}}, &error), error);
    require(session.upsert_bone_key("wave", {upper, 0.0F, {{4.0F,8.0F}, -20.0F, {1,1}}}, &error), error);
    require(session.upsert_bone_key("wave", {upper, 0.5F, {{4.0F,8.0F}, 35.0F, {1,1}}}, &error), error);
    require(session.upsert_bone_key("wave", {upper, 1.0F, {{4.0F,8.0F}, -20.0F, {1,1}}}, &error), error);
    require(session.upsert_part_key("wave", {handId, 0.5F, std::nullopt,
                                               std::optional<std::string>{"player_run"},
                                               std::nullopt, std::optional<std::int32_t>{3}}, &error), error);
    SpriteRigConstraintId ik{};
    require(session.add_two_bone_ik({0U, "hand_aim", upper, lower, {18.0F, 18.0F}, 1.0F, 0.65F},
                                    &ik, &error), error);
    SpriteRigVariant2D armed;
    armed.name = "armed";
    armed.overrides.push_back({"hand", std::optional<std::string>{"weapons/blaster.dvesprite"},
                               std::optional<std::string>{"idle"}, std::optional<bool>{true},
                               std::optional<std::uint32_t>{2U}});
    require(session.add_variant(armed, &error), error);
    require(session.duplicate_variant("armed", "armed_blue", &error), error);

    SpriteRigPose2D pose;
    require(sample_sprite_rig2d(session.asset(), "wave", 0.5F, "armed", pose, &error), error);
    require(pose.bones.size() == 3U && pose.parts.size() == 2U,
            "sprite-rig sample omitted bones or parts");
    require(pose.parts.back().spriteAsset == "weapons/blaster.dvesprite" &&
            pose.parts.back().paletteBank == 2U,
            "sprite-rig equipment variant was not applied");
    require(pose.poseHash != 0U && std::isfinite(pose.bones[1U].world.rotationDegrees),
            "sprite-rig pose or IK output is invalid");
    std::vector<SpriteRigBakeFrame2D> bake;
    require(build_sprite_rig2d_bake_plan(session.asset(), "wave", "armed", 12.0F, bake, &error), error);
    require(bake.size() == 12U && bake.front().poseHash != bake[6U].poseHash,
            "sprite-rig bake plan did not sample animation");

    const auto root = std::filesystem::temp_directory_path() / "dve_v222_rig_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto path = root / "pilot.dvespriterig";
    require(session.save(path, &error), error);
    const auto reopened = read_dvespriterig(path);
    require(static_cast<bool>(reopened), reopened.error);
    require(reopened.asset.contentHash == session.asset().contentHash,
            "sprite-rig round trip changed the content hash");
    require(session.set_ik_target(ik, {12.0F, 22.0F}, 1.0F, &error), error);
    const auto changedHash = session.asset().contentHash;
    require(session.undo(), "sprite-rig undo failed");
    require(session.redo(), "sprite-rig redo failed");
    require(session.asset().contentHash == changedHash, "sprite-rig redo did not restore the change");
    std::filesystem::remove_all(root);
}
}

int main() {
    try {
        run();
        std::cout << "dve_v222_sprite_rig2d_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
