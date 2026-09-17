#include "dve/sprite_animation.hpp"
#include "dve/sprite_palette.hpp"
#include "dve/sprite2d.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace dve;

SpritePaletteBank bank(std::string name, SpriteColor8 primary, SpriteColor8 secondary,
                       SpriteColor8 accentA, SpriteColor8 accentB) {
    SpritePaletteBank result;
    result.name = std::move(name);
    result.colors = {
        {0U, 0U, 0U, 0U},
        {24U, 28U, 44U, 255U},
        primary,
        secondary,
        {245U, 118U, 74U, 255U},
        accentA,
        accentB,
        {255U, 255U, 255U, 255U},
    };
    return result;
}

SpriteFrame frame(std::uint32_t index, std::string name, float duration = 0.1F,
                  std::string event = {}) {
    SpriteFrame result;
    result.name = std::move(name);
    result.atlasRect = {index * 16U, 0U, 16U, 16U};
    result.sourceWidth = 16U;
    result.sourceHeight = 16U;
    result.pivotPixels = {8.0F, 0.0F};
    result.durationSeconds = duration;
    result.event = std::move(event);
    return result;
}

SpriteCombatWindow combat(std::size_t first, std::size_t last, std::string name,
                          SpriteCombatRole role, SpriteVec2 center, SpriteVec2 size) {
    SpriteCombatWindow result;
    result.firstSequenceIndex = first;
    result.lastSequenceIndex = last;
    result.volume.name = std::move(name);
    result.volume.role = role;
    result.volume.shape = SpriteCombatShape::Box;
    result.volume.centerPixels = center;
    result.volume.sizePixels = size;
    return result;
}

SpriteAnimationParameterDefinition boolean_parameter(std::string name, bool value) {
    SpriteAnimationParameterDefinition result;
    result.name = std::move(name);
    result.defaultValue.type = SpriteAnimationParameterType::Boolean;
    result.defaultValue.booleanValue = value;
    return result;
}

SpriteAnimationCondition boolean_condition(std::string name, bool value) {
    SpriteAnimationCondition result;
    result.parameter = std::move(name);
    result.operation = value ? SpriteAnimationCompareOp::IsTrue : SpriteAnimationCompareOp::IsFalse;
    result.comparison.type = SpriteAnimationParameterType::Boolean;
    result.comparison.booleanValue = value;
    return result;
}

SpriteAnimationTransition transition(std::string from, std::string to,
                                     SpriteAnimationCondition condition,
                                     std::int32_t priority = 0) {
    SpriteAnimationTransition result;
    result.fromState = std::move(from);
    result.toState = std::move(to);
    result.priority = priority;
    result.blendDurationSeconds = 0.08F;
    result.blendCurve = SpriteAnimationBlendCurve::SmoothStep;
    result.interruptionSource = SpriteAnimationInterruptionSource::CurrentThenPrevious;
    result.trackPolicy = SpriteAnimationBlendTrackPolicy::HighestWeight;
    result.eventPolicy = SpriteAnimationBlendEventPolicy::Both;
    result.rootMotionPolicy = SpriteAnimationBlendRootMotionPolicy::Weighted;
    result.conditions.push_back(std::move(condition));
    return result;
}

} // namespace

int main(int argc, char** argv) {
    using namespace dve;
    const std::filesystem::path root = argc > 1 ? std::filesystem::path(argv[1])
                                                 : std::filesystem::current_path();
    const std::filesystem::path directory = root / "assets/sprites";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        std::cerr << "failed to create sprite asset directory: " << ec.message() << '\n';
        return 1;
    }

    SpritePaletteAsset palette;
    palette.name = "Sun Route Cast";
    palette.transparentIndex = 0U;
    palette.banks = {
        bank("Player", {48U, 96U, 160U, 255U}, {92U, 176U, 255U, 255U},
             {245U, 210U, 76U, 255U}, {85U, 231U, 128U, 255U}),
        bank("Patrol Bot", {126U, 63U, 152U, 255U}, {204U, 115U, 232U, 255U},
             {245U, 118U, 74U, 255U}, {255U, 184U, 108U, 255U}),
        bank("Sun Token", {191U, 108U, 20U, 255U}, {255U, 182U, 42U, 255U},
             {255U, 226U, 92U, 255U}, {255U, 148U, 36U, 255U}),
        bank("Checkpoint", {43U, 133U, 94U, 255U}, {85U, 231U, 128U, 255U},
             {92U, 176U, 255U, 255U}, {255U, 255U, 255U, 255U}),
    };
    palette.cycles.push_back({"Sun pulse", 5U, 6U, 8U, 0,
                              SpritePaletteCycleDirection::PingPong});
    palette.recompute_hash();
    std::string error;
    if (!palette.validate(&error) ||
        !write_dvepalette(directory / "v218_sun_route_palette.dvepalette", palette, &error)) {
        std::cerr << "palette generation failed: " << error << '\n';
        return 1;
    }

    SpriteAsset asset;
    asset.name = "Sun Route Cast";
    asset.textureAsset = "assets/sprites/v218_sun_route_cast_index.png";
    asset.textureWidth = 256U;
    asset.textureHeight = 16U;
    asset.pixelsPerWorldUnit = 16.0F;
    asset.sampling = SpriteSampling::Nearest;
    asset.paletteAsset = "assets/sprites/v218_sun_route_palette.dvepalette";
    asset.frames = {
        frame(0U, "player_idle"), frame(1U, "player_run_a", 0.09F, "footstep"),
        frame(2U, "player_run_b", 0.09F, "footstep"), frame(3U, "player_jump", 0.12F),
        frame(4U, "enemy_walk_a", 0.13F), frame(5U, "enemy_walk_b", 0.13F),
        frame(6U, "projectile", 0.05F), frame(7U, "token_a", 0.08F),
        frame(8U, "token_b", 0.08F), frame(9U, "token_c", 0.08F),
        frame(10U, "token_d", 0.08F), frame(11U, "checkpoint_off", 0.2F),
        frame(12U, "checkpoint_on", 0.12F, "checkpoint_pulse"),
        frame(13U, "hazard_a", 0.14F), frame(14U, "hazard_b", 0.14F),
        frame(15U, "effect_marker", 0.08F),
    };

    SpriteClip idle{"player_idle", SpriteLoopMode::Loop, 1.0F, {0U}};
    idle.combatWindows.push_back(combat(0U, 0U, "player_body", SpriteCombatRole::Hurtbox,
                                        {0.0F, 8.0F}, {10.0F, 15.0F}));
    idle.socketKeys.push_back({"weapon", 0U, {7.0F, 8.0F}, 0.0F, {1.0F, 1.0F},
                               SpriteTrackInterpolation::Step});

    SpriteClip run{"player_run", SpriteLoopMode::Loop, 1.0F, {1U, 2U}};
    run.combatWindows.push_back(combat(0U, 1U, "player_body", SpriteCombatRole::Hurtbox,
                                       {0.0F, 8.0F}, {10.0F, 15.0F}));
    run.socketKeys.push_back({"weapon", 0U, {7.0F, 8.0F}, 0.0F, {1.0F, 1.0F},
                              SpriteTrackInterpolation::Linear});
    run.socketKeys.push_back({"weapon", 1U, {8.0F, 7.0F}, -5.0F, {1.0F, 1.0F},
                              SpriteTrackInterpolation::Linear});
    run.rootMotionKeys.push_back({0U, {1.0F, 0.0F}, 0.0F});
    run.rootMotionKeys.push_back({1U, {1.0F, 0.0F}, 0.0F});

    SpriteClip jump{"player_jump", SpriteLoopMode::Loop, 1.0F, {3U}};
    jump.combatWindows.push_back(combat(0U, 0U, "player_body", SpriteCombatRole::Hurtbox,
                                        {0.0F, 8.0F}, {10.0F, 14.0F}));
    jump.combatWindows.push_back(combat(0U, 0U, "projectile_spawn", SpriteCombatRole::Trigger,
                                        {9.0F, 8.0F}, {4.0F, 4.0F}));
    jump.socketKeys.push_back({"weapon", 0U, {8.0F, 8.0F}, -12.0F, {1.0F, 1.0F},
                               SpriteTrackInterpolation::Step});

    SpriteClip enemy{"enemy_walk", SpriteLoopMode::Loop, 1.0F, {4U, 5U}};
    enemy.combatWindows.push_back(combat(0U, 1U, "enemy_body", SpriteCombatRole::Hurtbox,
                                         {0.0F, 8.0F}, {12.0F, 14.0F}));
    SpriteCombatWindow enemyTouch = combat(0U, 1U, "enemy_touch", SpriteCombatRole::Hitbox,
                                            {0.0F, 8.0F}, {13.0F, 14.0F});
    enemyTouch.volume.attackId = 2U;
    enemyTouch.volume.damage = 1.0F;
    enemyTouch.volume.knockbackPixelsPerSecond = {70.0F, 38.0F};
    enemyTouch.volume.hitStopTicks = 2U;
    enemy.combatWindows.push_back(std::move(enemyTouch));

    SpriteClip projectile{"projectile", SpriteLoopMode::Loop, 1.0F, {6U}};
    SpriteCombatWindow projectileHit = combat(0U, 0U, "projectile_hit", SpriteCombatRole::Hitbox,
                                               {0.0F, 7.0F}, {10.0F, 5.0F});
    projectileHit.volume.attackId = 3U;
    projectileHit.volume.damage = 1.0F;
    projectileHit.volume.knockbackPixelsPerSecond = {120.0F, 20.0F};
    projectile.combatWindows.push_back(std::move(projectileHit));
    projectile.socketKeys.push_back({"trail", 0U, {-5.0F, 7.0F}, 0.0F, {1.0F, 1.0F},
                                     SpriteTrackInterpolation::Step});

    SpriteClip token{"token_spin", SpriteLoopMode::Loop, 1.0F, {7U, 8U, 9U, 10U}};
    SpritePropertyKey collectible;
    collectible.name = "collectible";
    collectible.sequenceIndex = 0U;
    collectible.value.type = SpritePropertyType::Boolean;
    collectible.value.booleanValue = true;
    token.propertyKeys.push_back(std::move(collectible));

    SpriteClip checkpointOff{"checkpoint_off", SpriteLoopMode::Loop, 1.0F, {11U}};
    SpriteClip checkpointOn{"checkpoint_on", SpriteLoopMode::Loop, 1.0F, {12U}};
    SpriteClip hazard{"hazard", SpriteLoopMode::Loop, 1.0F, {13U, 14U}};
    hazard.combatWindows.push_back(combat(0U, 1U, "hazard", SpriteCombatRole::Trigger,
                                          {0.0F, 4.0F}, {16.0F, 8.0F}));

    asset.clips = {std::move(idle), std::move(run), std::move(jump), std::move(enemy),
                   std::move(projectile), std::move(token), std::move(checkpointOff),
                   std::move(checkpointOn), std::move(hazard)};
    assign_sprite_track_ids(asset);
    asset.recompute_hash();
    if (!asset.validate(&error) ||
        !write_dvesprite(directory / "v218_sun_route_cast.dvesprite", asset, &error)) {
        std::cerr << "sprite generation failed: " << error << '\n';
        return 1;
    }

    SpriteAnimationStateMachineAsset machine;
    machine.name = "Sun Route Player";
    machine.compatibleSpriteAssetHash = asset.contentHash;
    machine.initialState = "Idle";
    machine.parameters = {boolean_parameter("moving", false), boolean_parameter("airborne", false)};
    machine.states = {
        {"Idle", "player_idle", 1.0F, {80.0F, 120.0F}},
        {"Run", "player_run", 1.0F, {340.0F, 120.0F}},
        {"Jump", "player_jump", 1.0F, {210.0F, 300.0F}},
    };
    machine.transitions.push_back(transition("Idle", "Run", boolean_condition("moving", true), 10));
    machine.transitions.push_back(transition("Run", "Idle", boolean_condition("moving", false), 10));
    machine.transitions.push_back(transition("*", "Jump", boolean_condition("airborne", true), 100));
    machine.transitions.push_back(transition("Jump", "Idle", boolean_condition("airborne", false), 90));
    machine.recompute_hash();
    if (!machine.validate(asset, &error) ||
        !write_dvesprite_machine(directory / "v218_sun_route_player.dvesprite_machine", machine, &error)) {
        std::cerr << "state-machine generation failed: " << error << '\n';
        return 1;
    }

    std::cout << "generated v2.18 sprite assets\n";
    return 0;
}
