#include "dve/sprite_authoring.hpp"

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

bool close(float left, float right, float epsilon = 0.0001F) {
    return std::abs(left - right) <= epsilon;
}

SpriteAsset tracked_asset() {
    SpriteAsset asset;
    asset.name = "Tracked Hero";
    asset.textureAsset = "textures/tracked_hero.png";
    asset.textureWidth = 48U;
    asset.textureHeight = 16U;
    asset.pixelsPerWorldUnit = 16.0F;
    asset.frames = {
        {"windup", {0U, 0U, 16U, 16U}, 16U, 16U, 0, 0, {8.0F, 0.0F}, 0.1F, ""},
        {"active", {16U, 0U, 16U, 16U}, 16U, 16U, 0, 0, {8.0F, 0.0F}, 0.1F, "swing"},
        {"recover", {32U, 0U, 16U, 16U}, 16U, 16U, 0, 0, {8.0F, 0.0F}, 0.1F, ""},
    };
    SpriteClip clip{"attack", SpriteLoopMode::Once, 1.0F, {0U, 1U, 2U}};
    SpriteCombatVolume hit;
    hit.name = "blade";
    hit.role = SpriteCombatRole::Hitbox;
    hit.shape = SpriteCombatShape::Box;
    hit.centerPixels = {10.0F, 6.0F};
    hit.sizePixels = {12.0F, 5.0F};
    hit.radiusPixels = 1.0F;
    hit.attackId = 7U;
    hit.damage = 12.5F;
    hit.knockbackPixelsPerSecond = {80.0F, 30.0F};
    hit.hitStopTicks = 6U;
    clip.combatWindows.push_back({1U, 2U, hit});
    clip.socketKeys.push_back({"weapon", 0U, {0.0F, 4.0F}, 0.0F, {1.0F, 1.0F},
                               SpriteTrackInterpolation::Linear});
    clip.socketKeys.push_back({"weapon", 2U, {20.0F, 8.0F}, 90.0F, {2.0F, 1.0F},
                               SpriteTrackInterpolation::Step});
    SpritePropertyValue speed0;
    speed0.type = SpritePropertyType::Float;
    speed0.floatValue = 0.0F;
    SpritePropertyValue speed2 = speed0;
    speed2.floatValue = 10.0F;
    clip.propertyKeys.push_back({"speed", 0U, SpriteTrackInterpolation::Linear, speed0});
    clip.propertyKeys.push_back({"speed", 2U, SpriteTrackInterpolation::Step, speed2});
    SpritePropertyValue cancel;
    cancel.type = SpritePropertyType::Boolean;
    cancel.booleanValue = true;
    clip.propertyKeys.push_back({"can_cancel", 1U, SpriteTrackInterpolation::Step, cancel});
    clip.rootMotionKeys.push_back({1U, {3.0F, 1.0F}, 5.0F});
    asset.clips.push_back(std::move(clip));
    asset.recompute_hash();
    return asset;
}

void test_validation_codec_and_hash() {
    SpriteAsset asset = tracked_asset();
    std::string error;
    require(asset.validate(&error), error);
    const std::uint64_t originalHash = asset.contentHash;
    asset.clips[0U].combatWindows[0U].volume.damage = 13.0F;
    require(sprite_asset_content_hash(asset) != originalHash,
            "sprite content hash ignored combat metadata");
    asset = tracked_asset();

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "dve_sprite_tracks_v208.dvesprite";
    require(write_dvesprite(path, asset, &error), error);
    const SpriteAssetReadResult read = read_dvesprite(path);
    require(static_cast<bool>(read), read.error);
    require(read.asset.contentHash == asset.contentHash &&
            read.asset.clips[0U].combatWindows.size() == 1U &&
            read.asset.clips[0U].socketKeys.size() == 2U &&
            read.asset.clips[0U].propertyKeys.size() == 3U &&
            read.asset.clips[0U].rootMotionKeys.size() == 1U,
            "DVE_SPRITE 3 metadata round trip changed authored tracks");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    SpriteAsset invalid = tracked_asset();
    invalid.clips[0U].socketKeys.push_back(invalid.clips[0U].socketKeys.front());
    require(!invalid.validate(), "duplicate socket coordinate was accepted");
    invalid = tracked_asset();
    invalid.clips[0U].propertyKeys.back().interpolation = SpriteTrackInterpolation::Linear;
    require(!invalid.validate(), "linearly interpolated boolean property was accepted");
    invalid = tracked_asset();
    invalid.clips[0U].combatWindows[0U].volume.shape = SpriteCombatShape::Circle;
    require(!invalid.validate(), "circle with non-diameter size was accepted");
}

void test_deterministic_track_sampling() {
    const SpriteAsset asset = tracked_asset();
    const auto sample = sample_sprite_clip_tracks(asset, "attack", 0.15F);
    require(sample && sample->animation.frame == 1U &&
            sample->animation.authoredSequenceIndex == 1U,
            "track sampling selected the wrong authored timeline position");
    require(sample->combatVolumes.size() == 1U &&
            sample->combatVolumes[0U].volume.attackId == 7U,
            "active combat window was not sampled");
    require(sample->sockets.size() == 1U && close(sample->sockets[0U].positionPixels.x, 15.0F) &&
            close(sample->sockets[0U].positionPixels.y, 7.0F) &&
            close(sample->sockets[0U].rotationDegrees, 67.5F),
            "linear socket interpolation is incorrect");
    const auto speed = std::find_if(sample->properties.begin(), sample->properties.end(),
        [](const SpritePropertySample& property) { return property.name == "speed"; });
    const auto cancel = std::find_if(sample->properties.begin(), sample->properties.end(),
        [](const SpritePropertySample& property) { return property.name == "can_cancel"; });
    require(speed != sample->properties.end() && close(speed->value.floatValue, 7.5F),
            "linear float property interpolation is incorrect");
    require(cancel != sample->properties.end() && cancel->value.booleanValue,
            "stepped boolean property was not sampled");
    require(close(sample->rootMotionDeltaPixels.x, 3.0F) &&
            close(sample->rootMotionRotationDegrees, 5.0F),
            "root-motion key was not sampled at its authored frame");

    const auto before = sample_sprite_clip_tracks(asset, "attack", 0.05F);
    require(before && before->combatVolumes.empty() && before->properties.size() == 1U,
            "inactive combat/property tracks leaked before their first key");
}

void test_runtime_world_transform_and_flips() {
    SpriteRuntime runtime;
    std::string error;
    require(runtime.register_asset(1U, tracked_asset(), &error), error);
    SpriteInstanceDesc desc;
    desc.asset = 1U;
    desc.clip = "attack";
    desc.transform = make_rigid_transform({10.0F, 20.0F, 0.0F}, {});
    desc.flipX = true;
    require(runtime.bind(9U, desc, &error), error);
    require(runtime.seek(9U, 0.15F), "could not seek tracked sprite instance");
    const auto sampled = runtime.sample_tracks(9U);
    require(sampled && sampled->sockets.size() == 1U && sampled->combatVolumes.size() == 1U,
            "runtime did not expose sampled sprite gameplay tracks");
    require(close(sampled->sockets[0U].positionPixels.x, -15.0F) &&
            close(sampled->sockets[0U].worldTransform.position.x, 10.0F - 15.0F / 16.0F) &&
            close(sampled->sockets[0U].worldTransform.position.y, 20.0F + 7.0F / 16.0F),
            "flip-aware socket world transform is incorrect");
    require(close(sampled->combatVolumes[0U].volume.centerPixels.x, -10.0F) &&
            close(sampled->combatVolumes[0U].volume.knockbackPixelsPerSecond.x, -80.0F),
            "flip-aware combat metadata is incorrect");
    require(close(sampled->rootMotionDeltaPixels.x, -3.0F),
            "flip-aware root motion is incorrect");
}

void test_authoring_transactions_and_reorder() {
    SpriteAuthoringSession session(tracked_asset());
    std::string error;
    SpriteSocketKey effect{"effect", 1U, {2.0F, 3.0F}, 0.0F, {1.0F, 1.0F},
                           SpriteTrackInterpolation::Step};
    require(session.upsert_socket_key("attack", effect, &error), error);
    require(session.can_undo(), "socket edit did not enter the sprite undo stack");
    require(session.undo(), "socket edit could not be undone");
    require(session.redo(), "socket edit could not be redone");

    SpritePropertyValue enabled;
    enabled.type = SpritePropertyType::Boolean;
    enabled.booleanValue = true;
    require(session.upsert_property_key(
        "attack", {"invulnerable", 0U, SpriteTrackInterpolation::Step, enabled}, &error), error);
    require(session.set_root_motion_key("attack", {2U, {1.0F, 0.0F}, 0.0F}, &error), error);
    require(session.move_clip_frame("attack", 0U, 2U, &error), error);
    const SpriteClip& reordered = session.asset().clips[0U];
    require(reordered.frames == std::vector<SpriteFrameIndex>({1U, 2U, 0U}),
            "timeline reorder produced the wrong frame permutation");
    require(reordered.combatWindows.size() == 1U &&
            reordered.combatWindows[0U].firstSequenceIndex == 0U &&
            reordered.combatWindows[0U].lastSequenceIndex == 1U,
            "combat window membership was not preserved across timeline reorder");
    const auto weapon0 = std::find_if(reordered.socketKeys.begin(), reordered.socketKeys.end(),
        [](const SpriteSocketKey& key) { return key.name == "weapon" && key.sequenceIndex == 2U; });
    const auto root = std::find_if(reordered.rootMotionKeys.begin(), reordered.rootMotionKeys.end(),
        [](const SpriteRootMotionKey& key) { return key.sequenceIndex == 1U; });
    require(weapon0 != reordered.socketKeys.end() && root != reordered.rootMotionKeys.end(),
            "socket or root-motion keys lost frame identity during timeline reorder");
}

} // namespace

int main() {
    try {
        test_validation_codec_and_hash();
        test_deterministic_track_sampling();
        test_runtime_world_transform_and_flips();
        test_authoring_transactions_and_reorder();
        std::cout << "dve_v208_sprite_tracks_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v208_sprite_tracks_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
