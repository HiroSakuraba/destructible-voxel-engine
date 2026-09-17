#include "dve/rigid_body_adapter.hpp"
#include "dve/sprite_animation.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float left, float right, float epsilon = 0.0003F) {
    return std::abs(left - right) <= epsilon;
}

SpriteAsset make_sprite_asset() {
    SpriteAsset asset;
    asset.name = "v210 actor";
    asset.textureAsset = "textures/v210.png";
    asset.textureWidth = 40U;
    asset.textureHeight = 10U;
    asset.pixelsPerWorldUnit = 10.0F;
    asset.frames = {
        {"idle", {0U, 0U, 10U, 10U}, 10U, 10U, 0, 0, {5.0F, 0.0F}, 0.1F, "idle_enter"},
        {"step", {10U, 0U, 10U, 10U}, 10U, 10U, 0, 0, {5.0F, 0.0F}, 0.1F, "step"},
        {"land", {20U, 0U, 10U, 10U}, 10U, 10U, 0, 0, {5.0F, 0.0F}, 0.1F, "land"},
        {"attack", {30U, 0U, 10U, 10U}, 10U, 10U, 0, 0, {5.0F, 0.0F}, 0.1F, "strike"},
    };
    asset.clips.push_back({"idle", SpriteLoopMode::Loop, 1.0F, {0U}});
    asset.clips.push_back({"run", SpriteLoopMode::Loop, 1.0F, {0U, 1U, 2U}});
    asset.clips.push_back({"attack", SpriteLoopMode::Once, 1.0F, {3U, 1U, 2U}});

    SpriteClip& run = asset.clips[1];
    SpriteCombatWindow combat;
    combat.firstSequenceIndex = 1U;
    combat.lastSequenceIndex = 1U;
    combat.volume.name = "foot";
    combat.volume.role = SpriteCombatRole::Trigger;
    run.combatWindows.push_back(combat);
    SpriteSocketKey socket;
    socket.name = "weapon";
    socket.sequenceIndex = 0U;
    socket.positionPixels = {2.0F, 1.0F};
    run.socketKeys.push_back(socket);
    SpritePropertyKey property;
    property.name = "grounded";
    property.sequenceIndex = 2U;
    property.value.type = SpritePropertyType::Boolean;
    property.value.booleanValue = true;
    run.propertyKeys.push_back(property);
    SpriteRootMotionKey root;
    root.sequenceIndex = 1U;
    root.deltaPixels = {3.0F, 0.0F};
    run.rootMotionKeys.push_back(root);

    SpriteClip& attack = asset.clips[2];
    SpriteSocketKey attackSocket = socket;
    attackSocket.sequenceIndex = 0U;
    attack.socketKeys.push_back(attackSocket);
    assign_sprite_track_ids(asset);
    asset.recompute_hash();
    return asset;
}

SpriteAnimationStateMachineAsset make_machine(const SpriteAsset& sprite) {
    SpriteAnimationStateMachineAsset machine;
    machine.name = "locomotion";
    machine.compatibleSpriteAssetHash = sprite.contentHash;
    machine.initialState = "idle";

    SpriteAnimationParameterDefinition speed;
    speed.name = "speed";
    speed.defaultValue.type = SpriteAnimationParameterType::Float;
    machine.parameters.push_back(speed);
    SpriteAnimationParameterDefinition attack;
    attack.name = "attack";
    attack.defaultValue.type = SpriteAnimationParameterType::Trigger;
    machine.parameters.push_back(attack);

    machine.states = {
        {"idle", "idle", 1.0F},
        {"run", "run", 1.0F},
        {"attack", "attack", 1.0F},
    };

    SpriteAnimationCondition attackCondition;
    attackCondition.parameter = "attack";
    attackCondition.operation = SpriteAnimationCompareOp::Triggered;
    attackCondition.comparison.type = SpriteAnimationParameterType::Trigger;
    SpriteAnimationTransition attackTransition;
    attackTransition.fromState = "*";
    attackTransition.toState = "attack";
    attackTransition.priority = 100;
    attackTransition.conditions.push_back(attackCondition);
    machine.transitions.push_back(attackTransition);

    SpriteAnimationCondition moving;
    moving.parameter = "speed";
    moving.operation = SpriteAnimationCompareOp::Greater;
    moving.comparison.type = SpriteAnimationParameterType::Float;
    moving.comparison.floatValue = 0.5F;
    SpriteAnimationTransition beginRun;
    beginRun.fromState = "idle";
    beginRun.toState = "run";
    beginRun.priority = 10;
    beginRun.conditions.push_back(moving);
    machine.transitions.push_back(beginRun);

    SpriteAnimationCondition stopped = moving;
    stopped.operation = SpriteAnimationCompareOp::LessEqual;
    SpriteAnimationTransition endRun;
    endRun.fromState = "run";
    endRun.toState = "idle";
    endRun.priority = 10;
    endRun.conditions.push_back(stopped);
    machine.transitions.push_back(endRun);

    SpriteAnimationTransition finishAttack;
    finishAttack.fromState = "attack";
    finishAttack.toState = "idle";
    finishAttack.hasExitTime = true;
    finishAttack.exitNormalizedTime = 0.99F;
    machine.transitions.push_back(finishAttack);
    machine.recompute_hash();
    return machine;
}

void test_interval_queries_and_runtime_delivery() {
    const SpriteAsset asset = make_sprite_asset();
    SpriteIntervalQueryResult forward;
    SpriteIntervalQueryOptions options;
    std::string error;
    require(query_sprite_clip_interval_events(asset, "run", 0.0F, 0.35F, options,
                                              forward, &error), error);
    require(forward.crossedFrameBoundaries == 3U && forward.crossedLoopBoundary,
            "forward interval did not report every skipped frame and loop boundary");
    const auto frameCount = std::count_if(forward.events.begin(), forward.events.end(),
        [](const SpriteIntervalEvent& event) {
            return event.kind == SpriteIntervalEventKind::FrameEvent;
        });
    require(frameCount == 3 && forward.events.front().timeSeconds <= forward.events.back().timeSeconds,
            "forward interval frame events were incomplete or unordered");
    require(std::any_of(forward.events.begin(), forward.events.end(),
        [](const SpriteIntervalEvent& event) {
            return event.kind == SpriteIntervalEventKind::CombatActivated && event.name == "foot";
        }), "forward interval omitted combat activation");
    require(std::any_of(forward.events.begin(), forward.events.end(),
        [](const SpriteIntervalEvent& event) {
            return event.kind == SpriteIntervalEventKind::PropertyKey && event.name == "grounded";
        }), "forward interval omitted property-key crossing");

    SpriteIntervalQueryResult reverse;
    require(query_sprite_clip_interval_events(asset, "run", 0.35F, 0.05F, options,
                                              reverse, &error), error);
    require(reverse.reverse && reverse.crossedFrameBoundaries == 3U &&
            reverse.events.front().timeSeconds >= reverse.events.back().timeSeconds,
            "reverse/rollback interval ordering is incorrect");
    require(std::any_of(reverse.events.begin(), reverse.events.end(),
        [](const SpriteIntervalEvent& event) {
            return event.kind == SpriteIntervalEventKind::CombatDeactivated;
        }), "rollback interval did not invert a combat boundary");

    SpriteRuntime runtime;
    require(runtime.register_asset(1U, asset, &error), error);
    SpriteInstanceDesc desc;
    desc.asset = 1U;
    desc.clip = "run";
    require(runtime.bind(9U, desc, &error), error);
    runtime.tick(0.35F);
    require(runtime.events().size() == 3U && runtime.interval_events().size() >= 3U,
            "SpriteRuntime collapsed skipped frame events into one event");
    require(std::all_of(runtime.interval_events().begin(), runtime.interval_events().end(),
        [](const SpriteIntervalEvent& event) { return event.owner == 9U; }),
        "runtime interval events omitted owner identity");
}

void test_state_machine_codec_transitions_and_rollback() {
    const SpriteAsset asset = make_sprite_asset();
    SpriteAnimationStateMachineAsset machine = make_machine(asset);
    std::string error;
    require(machine.validate(asset, &error), error);
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "dve_v210_machine.dvesm";
    require(write_dvesprite_machine(path, machine, &error), error);
    const SpriteAnimationMachineReadResult read = read_dvesprite_machine(path);
    require(read && read.asset.contentHash == machine.contentHash &&
            read.asset.validate(asset, &error), read.error.empty() ? error : read.error);

    SpriteRuntime sprites;
    require(sprites.register_asset(1U, asset, &error), error);
    SpriteInstanceDesc desc;
    desc.asset = 1U;
    require(sprites.bind(17U, desc, &error), error);
    SpriteAnimationStateRuntime states(sprites);
    require(states.register_machine(4U, 1U, machine, &error), error);
    require(states.bind(17U, 4U, &error), error);
    require(states.current_state(17U) == "idle", "state machine did not enter initial state");

    require(states.set_float(17U, "speed", 1.0F, &error), error);
    states.tick(0.0F);
    require(states.current_state(17U) == "run" && states.transition_events().size() == 1U,
            "parameter transition did not enter run");
    states.tick(0.15F);
    const auto snapshot = states.capture_snapshot(17U);
    require(snapshot && snapshot->state == "run" && snapshot->spriteTimeSeconds > 0.1F,
            "state-machine rollback snapshot omitted timing");

    require(states.fire_trigger(17U, "attack", &error), error);
    states.tick(0.0F);
    require(states.current_state(17U) == "attack", "any-state trigger did not win by priority");
    const auto trigger = states.parameter(17U, "attack");
    require(trigger && !trigger->booleanValue, "successful transition did not consume trigger");
    states.tick(0.31F);
    require(states.current_state(17U) == "idle", "exit-time transition did not leave attack");

    require(states.restore_snapshot(*snapshot, &error), error);
    require(states.current_state(17U) == "run" &&
            close(sprites.time_seconds(17U), snapshot->spriteTimeSeconds),
            "rollback restore did not reproduce state and clip time");
    std::filesystem::remove(path);
}

void test_root_motion_adapters_and_scene_attachments() {
    std::string error;
    Physics2DWorldSettings settings;
    settings.backend = Physics2DBackend::NativeTile;
    std::unique_ptr<Physics2DWorld> physics2d = create_physics2d_world(settings, &error);
    require(physics2d != nullptr, error);
    Physics2DBodyDef bodyDef;
    bodyDef.type = Physics2DBodyType::Kinematic;
    const Physics2DBodyHandle body = physics2d->create_body(bodyDef, &error);
    require(static_cast<bool>(body), error);

    SpriteRootMotionDelta delta;
    delta.deltaPixels = {10.0F, 20.0F};
    delta.worldTranslation = {1.0F, 2.0F, 0.0F};
    delta.rotationDegrees = 90.0F;
    SpriteRootMotionApplyOptions options;
    options.pixelsPerWorldUnit = 10.0F;
    require(apply_sprite_root_motion_to_physics2d(delta, *physics2d, body, options, &error), error);
    Physics2DBodyState bodyState;
    require(physics2d->body_state(body, bodyState) && close(bodyState.positionPixels.x, 10.0F) &&
            close(bodyState.positionPixels.y, 20.0F) &&
            close(bodyState.angleRadians, 1.57079632679F),
            "2D transform root-motion adapter applied the wrong transform");
    options.mode = SpriteRootMotionApplyMode::Velocity;
    options.additiveVelocity = false;
    options.deltaSeconds = 0.5F;
    require(apply_sprite_root_motion_to_physics2d(delta, *physics2d, body, options, &error), error);
    require(physics2d->body_state(body, bodyState) &&
            close(bodyState.linearVelocityPixelsPerSecond.x, 20.0F) &&
            close(bodyState.linearVelocityPixelsPerSecond.y, 40.0F),
            "2D velocity root-motion adapter applied the wrong velocity");

    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc actorDesc;
    actorDesc.name = "actor";
    const GameObjectId actor = world.create_object(std::move(actorDesc), &error);
    GameObjectDesc weaponDesc;
    weaponDesc.name = "weapon";
    const GameObjectId weapon = world.create_object(std::move(weaponDesc), &error);
    require(actor != kInvalidGameObjectId && weapon != kInvalidGameObjectId, error);
    options.mode = SpriteRootMotionApplyMode::Transform;
    options.translationScale = 1.0F;
    require(apply_sprite_root_motion_to_game_object(delta, world, actor, options, &error), error);
    const auto actorPosition = world.position(actor);
    require(actorPosition && close(actorPosition->x, 1.0F) && close(actorPosition->y, 2.0F),
            "GameWorld root-motion adapter applied the wrong position");

    const SpriteAsset asset = make_sprite_asset();
    SpriteRuntime sprites;
    require(sprites.register_asset(1U, asset, &error), error);
    SpriteInstanceDesc spriteDesc;
    spriteDesc.asset = 1U;
    spriteDesc.clip = "run";
    spriteDesc.transform = make_rigid_transform({3.0F, 4.0F, 0.0F}, {});
    require(sprites.bind(22U, spriteDesc, &error), error);
    SpriteSocketAttachmentDesc attachment;
    attachment.id = 77U;
    attachment.parent = 22U;
    attachment.socket = "weapon";
    require(sprites.bind_socket_attachment(attachment, &error), error);
    SpriteGameObjectAttachmentBridge bridge;
    require(bridge.bind({77U, weapon, true, true}, &error), error);
    const SpriteAttachmentSyncReport report = bridge.sync(sprites, world);
    require(report.updated == 1U && report.failed == 0U, "scene attachment bridge did not update");
    const auto weaponPosition = world.position(weapon);
    require(weaponPosition && close(weaponPosition->x, 3.2F) && close(weaponPosition->y, 4.1F),
            "scene attachment bridge composed the wrong socket transform");
}

} // namespace

int main() {
    try {
        test_interval_queries_and_runtime_delivery();
        test_state_machine_codec_transitions_and_rollback();
        test_root_motion_adapters_and_scene_attachments();
        std::cout << "dve_v210_sprite_animation_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v210_sprite_animation_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
