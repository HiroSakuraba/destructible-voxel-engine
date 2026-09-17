#include "dve/sprite_animation_authoring.hpp"

#include <bit>
#include <cmath>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float left, float right, float epsilon = 0.001F) {
    return std::fabs(left - right) <= epsilon;
}

constexpr std::uint64_t kTestFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kTestFnvPrime = 1099511628211ULL;
void test_hash_byte(std::uint64_t& hash, std::uint8_t value) {
    hash ^= value;
    hash *= kTestFnvPrime;
}
template<class T> void test_hash_integer(std::uint64_t& hash, T value) {
    using Unsigned = std::make_unsigned_t<T>;
    const Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0U; index < sizeof(T); ++index)
        test_hash_byte(hash, static_cast<std::uint8_t>(
            (bits >> (index * 8U)) & static_cast<Unsigned>(0xFFU)));
}
void test_hash_float(std::uint64_t& hash, float value) {
    test_hash_integer(hash, std::bit_cast<std::uint32_t>(value));
}
void test_hash_string(std::uint64_t& hash, std::string_view value) {
    test_hash_integer(hash, static_cast<std::uint64_t>(value.size()));
    for (char raw : value)
        test_hash_byte(hash, static_cast<std::uint8_t>(static_cast<unsigned char>(raw)));
}
std::uint64_t legacy_single_state_hash(const SpriteAsset& sprite) {
    std::uint64_t hash = kTestFnvOffset;
    test_hash_string(hash, "legacy");
    test_hash_integer(hash, sprite.contentHash);
    test_hash_string(hash, "idle");
    test_hash_integer(hash, std::uint64_t{0U});
    test_hash_integer(hash, std::uint64_t{1U});
    test_hash_string(hash, "idle");
    test_hash_string(hash, "idle");
    test_hash_float(hash, 1.0F);
    test_hash_integer(hash, std::uint64_t{0U});
    return hash;
}

SpriteAsset make_sprite() {
    SpriteAsset asset;
    asset.name = "v211 actor";
    asset.textureAsset = "v211.png";
    asset.textureWidth = 30U;
    asset.textureHeight = 10U;
    asset.pixelsPerWorldUnit = 10.0F;
    asset.frames = {
        {"idle", {0U, 0U, 10U, 10U}, 10U, 10U, 0, 0, {5.0F, 0.0F}, 0.2F, "idle"},
        {"run0", {10U, 0U, 10U, 10U}, 10U, 10U, 0, 0, {5.0F, 0.0F}, 0.1F, "step"},
        {"run1", {20U, 0U, 10U, 10U}, 10U, 10U, 0, 0, {5.0F, 0.0F}, 0.1F, "step"},
    };
    asset.clips.push_back({"idle", SpriteLoopMode::Loop, 1.0F, {0U}});
    asset.clips.push_back({"run", SpriteLoopMode::Loop, 1.0F, {1U, 2U}});
    asset.clips.push_back({"attack", SpriteLoopMode::Once, 1.0F, {2U, 1U}});
    SpriteRootMotionKey root;
    root.sequenceIndex = 1U;
    root.deltaPixels = {4.0F, 0.0F};
    asset.clips[1].rootMotionKeys.push_back(root);
    assign_sprite_track_ids(asset);
    asset.recompute_hash();
    return asset;
}

SpriteAnimationStateMachineAsset make_machine(const SpriteAsset& sprite) {
    SpriteAnimationStateMachineAsset machine;
    machine.name = "v211 machine";
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
        {"idle", "idle", 1.0F, {10.0F, 20.0F}},
        {"run", "run", 1.0F, {240.0F, 20.0F}},
        {"attack", "attack", 1.0F, {470.0F, 20.0F}},
    };
    SpriteAnimationCondition moving;
    moving.parameter = "speed";
    moving.operation = SpriteAnimationCompareOp::Greater;
    moving.comparison.type = SpriteAnimationParameterType::Float;
    moving.comparison.floatValue = 0.5F;
    SpriteAnimationTransition begin;
    begin.fromState = "idle";
    begin.toState = "run";
    begin.priority = 10;
    begin.blendDurationSeconds = 0.2F;
    begin.synchronizeNormalizedTime = true;
    begin.interruptionSource = SpriteAnimationInterruptionSource::NoInterruption;
    begin.conditions.push_back(moving);
    machine.transitions.push_back(begin);
    SpriteAnimationCondition attackCondition;
    attackCondition.parameter = "attack";
    attackCondition.operation = SpriteAnimationCompareOp::Triggered;
    attackCondition.comparison.type = SpriteAnimationParameterType::Trigger;
    SpriteAnimationTransition strike;
    strike.fromState = "*";
    strike.toState = "attack";
    strike.priority = 100;
    strike.blendDurationSeconds = 0.1F;
    strike.conditions.push_back(attackCondition);
    machine.transitions.push_back(strike);
    machine.recompute_hash();
    return machine;
}

class CastWorld final : public Physics2DWorld {
public:
    Physics2DBackend backend() const noexcept override { return Physics2DBackend::Box2D; }
    Physics2DCapabilities capabilities() const noexcept override {
        Physics2DCapabilities result;
        result.shapeCasts = true;
        return result;
    }
    const Physics2DWorldSettings& settings() const noexcept override { return settings_; }
    bool set_tile_map(const TileMap&, std::string*) override { return true; }
    void clear_tile_map() override {}
    Physics2DBodyHandle create_body(const Physics2DBodyDef& def, std::string*) override {
        Physics2DBodyHandle handle{next_++};
        Physics2DBodyState state;
        state.positionPixels = def.positionPixels;
        state.angleRadians = def.angleRadians;
        state.enabled = def.enabled;
        states_.emplace(handle.value, state);
        return handle;
    }
    bool destroy_body(Physics2DBodyHandle body) override { return states_.erase(body.value) != 0U; }
    Physics2DColliderHandle add_collider(Physics2DBodyHandle, const Physics2DColliderDef&,
                                         std::string*) override { return {next_++}; }
    bool destroy_collider(Physics2DColliderHandle) override { return true; }
    void step(float) override {}
    bool body_state(Physics2DBodyHandle body, Physics2DBodyState& out) const override {
        const auto found = states_.find(body.value);
        if (found == states_.end()) return false;
        out = found->second;
        return true;
    }
    bool set_body_transform(Physics2DBodyHandle body, TileVec2 position, float angle) override {
        auto found = states_.find(body.value);
        if (found == states_.end()) return false;
        found->second.positionPixels = position;
        found->second.angleRadians = angle;
        return true;
    }
    bool set_body_linear_velocity(Physics2DBodyHandle body, TileVec2 velocity) override {
        auto found = states_.find(body.value);
        if (found == states_.end()) return false;
        found->second.linearVelocityPixelsPerSecond = velocity;
        return true;
    }
    bool set_body_gravity_scale(Physics2DBodyHandle body, float) override {
        return states_.contains(body.value);
    }
    bool apply_linear_impulse(Physics2DBodyHandle body, TileVec2 impulse) override {
        auto found = states_.find(body.value);
        if (found == states_.end()) return false;
        found->second.linearVelocityPixelsPerSecond.x += impulse.x;
        found->second.linearVelocityPixelsPerSecond.y += impulse.y;
        return true;
    }
    bool set_body_enabled(Physics2DBodyHandle body, bool enabled) override {
        auto found = states_.find(body.value);
        if (found == states_.end()) return false;
        found->second.enabled = enabled;
        return true;
    }
    bool drop_through_one_way(Physics2DBodyHandle body, float) override {
        return states_.contains(body.value);
    }
    Physics2DRayCastHit ray_cast(TileVec2, TileVec2, std::uint64_t,
                                 std::uint64_t) const override { return {}; }
    Physics2DShapeCastHit cast_shape(const Physics2DQueryShape&, TileVec2,
                                     const Physics2DQueryFilter&) const override {
        if (castCount_++ == 0U) {
            Physics2DShapeCastHit hit;
            hit.hit = true;
            hit.fraction = 0.5F;
            hit.normal = {-1.0F, 0.0F};
            return hit;
        }
        return {};
    }
    std::span<const Physics2DEvent> events() const noexcept override { return events_; }

private:
    Physics2DWorldSettings settings_{};
    std::unordered_map<std::uint64_t, Physics2DBodyState> states_;
    mutable std::uint32_t castCount_{};
    std::uint64_t next_{1U};
    std::vector<Physics2DEvent> events_;
};

void test_v1_machine_migration() {
    const SpriteAsset sprite = make_sprite();
    const auto path = std::filesystem::temp_directory_path() / "dve_v211_legacy_machine.dvesm";
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << std::setprecision(9)
           << "DVE_SPRITE_MACHINE 1\n"
           << "name " << std::quoted(std::string("legacy")) << '\n'
           << "sprite_hash " << sprite.contentHash << '\n'
           << "initial " << std::quoted(std::string("idle")) << '\n'
           << "parameters 0\n"
           << "states 1\n"
           << "state " << std::quoted(std::string("idle")) << ' '
           << std::quoted(std::string("idle")) << " 1\n"
           << "transitions 0\n"
           << "hash " << legacy_single_state_hash(sprite) << '\n';
    stream.close();
    const SpriteAnimationMachineReadResult read = read_dvesprite_machine(path);
    std::string error;
    require(read && read.asset.validate(sprite, &error), read ? error : read.error);
    require(read.asset.states[0].graphPosition.x == 0.0F &&
            read.asset.contentHash == sprite_animation_state_machine_content_hash(read.asset),
            "v1 machine did not migrate to the v2 semantic hash");
    std::filesystem::remove(path);
}

void test_v2_codec_blending_interruption_and_snapshot() {
    const SpriteAsset sprite = make_sprite();
    SpriteAnimationStateMachineAsset machine = make_machine(sprite);
    std::string error;
    require(machine.validate(sprite, &error), error);
    const auto path = std::filesystem::temp_directory_path() / "dve_v211_machine.dvesm";
    require(write_dvesprite_machine(path, machine, &error), error);
    const auto read = read_dvesprite_machine(path);
    require(read && read.asset.contentHash == machine.contentHash, read.error);
    require(close(read.asset.states[1].graphPosition.x, 240.0F) &&
            close(read.asset.transitions[0].blendDurationSeconds, 0.2F),
            "v2 codec omitted graph or blend data");

    SpriteRuntime sprites;
    require(sprites.register_asset(1U, sprite, &error), error);
    SpriteInstanceDesc desc;
    desc.asset = 1U;
    require(sprites.bind(9U, desc, &error), error);
    SpriteAnimationStateRuntime runtime(sprites);
    require(runtime.register_machine(2U, 1U, machine, &error), error);
    require(runtime.bind(9U, 2U, &error), error);
    require(runtime.set_float(9U, "speed", 1.0F, &error), error);
    runtime.tick(0.0F);
    require(runtime.current_state(9U) == "run" && runtime.is_blending(9U),
            "blend transition did not begin");
    auto blend = runtime.blend_sample(9U);
    require(blend && blend->source && blend->destination && close(blend->sourceWeight, 1.0F),
            "blend sample omitted its source or destination");
    runtime.tick(0.1F);
    blend = runtime.blend_sample(9U);
    require(blend && close(blend->destinationWeight, 0.5F, 0.01F),
            "blend weight did not advance deterministically");
    const auto snapshot = runtime.capture_snapshot(9U);
    require(snapshot && snapshot->blend.active, "snapshot omitted active blend state");
    require(runtime.fire_trigger(9U, "attack", &error), error);
    runtime.tick(0.0F);
    require(runtime.current_state(9U) == "run",
            "non-interruptible blend accepted an any-state transition");
    runtime.tick(0.11F);
    runtime.tick(0.0F);
    require(runtime.current_state(9U) == "attack" &&
            runtime.transition_events().front().transitionSerial == 2U,
            "transition did not execute after blend completion");
    require(runtime.restore_snapshot(*snapshot, &error), error);
    require(runtime.current_state(9U) == "run" && runtime.is_blending(9U),
            "rollback snapshot did not restore blend state");
    std::filesystem::remove(path);
}

void test_authoring_session_and_side_effect_ledger() {
    const SpriteAsset sprite = make_sprite();
    SpriteAnimationMachineAuthoringSession authoring;
    std::string error;
    require(authoring.create(sprite, "authored", "idle", &error), error);
    require(authoring.add_state("run", "run", {200.0F, 40.0F}, &error), error);
    require(authoring.move_state(1U, {220.0F, 60.0F}, &error), error);
    SpriteAnimationParameterDefinition speed;
    speed.name = "speed";
    speed.defaultValue.type = SpriteAnimationParameterType::Float;
    require(authoring.add_parameter(speed, &error), error);
    SpriteAnimationTransition transition;
    transition.fromState = "Entry";
    transition.toState = "run";
    transition.blendDurationSeconds = 0.15F;
    SpriteAnimationCondition condition;
    condition.parameter = "speed";
    condition.operation = SpriteAnimationCompareOp::Greater;
    condition.comparison.type = SpriteAnimationParameterType::Float;
    condition.comparison.floatValue = 0.5F;
    transition.conditions.push_back(condition);
    require(authoring.add_transition(transition, &error), error);
    require(authoring.rename_state(1U, "locomotion", &error), error);
    require(authoring.asset().transitions[0].toState == "locomotion",
            "state rename did not repair transition references");
    require(authoring.undo(&error) && authoring.asset().states[1].name == "run", error);
    require(authoring.redo(&error) && authoring.asset().states[1].name == "locomotion", error);
    const auto path = std::filesystem::temp_directory_path() / "dve_v211_authored.dvesm";
    require(authoring.save(path, &error), error);
    SpriteAnimationMachineAuthoringSession reopened;
    require(reopened.open(path, sprite, &error), error);
    require(reopened.asset().states.size() == 2U && !reopened.dirty(),
            "authoring document did not reopen cleanly");

    SpriteSideEffectLedger ledger;
    SpriteIntervalEvent interval;
    interval.owner = 4U;
    interval.kind = SpriteIntervalEventKind::FrameEvent;
    interval.cycle = 2;
    interval.playbackSequenceIndex = 1U;
    interval.frame = 3U;
    interval.name = "footstep";
    const auto first = ledger.issue(10U, interval);
    require(first && !ledger.issue(10U, interval),
            "side-effect ledger accepted a duplicate interval command");
    ledger.rollback_after(9U);
    const auto replay = ledger.issue(10U, interval);
    require(replay && replay->id == first->id,
            "rollback did not permit the same deterministic command identity to replay");
    SpriteAnimationTransitionEvent stateEvent;
    stateEvent.owner = 4U;
    stateEvent.machine = 7U;
    stateEvent.fromState = "idle";
    stateEvent.toState = "run";
    stateEvent.transitionIndex = 1U;
    stateEvent.transitionSerial = 3U;
    require(ledger.issue(11U, stateEvent).has_value(),
            "side-effect ledger rejected a distinct state transition");
    std::filesystem::remove(path);
}

void test_collision_aware_root_motion() {
    CastWorld world;
    Physics2DBodyDef def;
    const Physics2DBodyHandle body = world.create_body(def, nullptr);
    SpriteRootMotionDelta delta;
    delta.worldTranslation = {10.0F, 10.0F, 0.0F};
    SpriteRootMotionCollisionOptions options;
    options.application.pixelsPerWorldUnit = 1.0F;
    options.application.translationScale = 1.0F;
    options.application.applyRotation = false;
    options.skinPixels = 0.0F;
    options.maximumSlideIterations = 2U;
    options.localShape.shape = Physics2DShapeType::Box;
    options.localShape.halfExtentsPixels = {2.0F, 4.0F};
    SpriteRootMotionCollisionResult result;
    std::string error;
    require(apply_sprite_root_motion_collision_aware(
                delta, world, body, options, result, &error), error);
    Physics2DBodyState state;
    require(world.body_state(body, state), "collision test body disappeared");
    require(result.collisionCount == 1U && close(result.appliedPixels.x, 5.0F) &&
            close(result.appliedPixels.y, 10.0F) && close(state.positionPixels.x, 5.0F) &&
            close(state.positionPixels.y, 10.0F),
            "collision-aware root motion did not stop and slide along the cast normal");
}

} // namespace

int main() {
    try {
        test_v1_machine_migration();
        test_v2_codec_blending_interruption_and_snapshot();
        test_authoring_session_and_side_effect_ledger();
        test_collision_aware_root_motion();
        std::cout << "dve_v211_sprite_animation_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v211_sprite_animation_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
