#include "dve/sprite_animation_authoring.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float left, float right, float epsilon = 0.01F) {
    return std::fabs(left - right) <= epsilon;
}

SpriteAsset make_sprite() {
    SpriteAsset asset;
    asset.name = "v212 actor";
    asset.textureAsset = "v212.png";
    asset.textureWidth = 40U;
    asset.textureHeight = 10U;
    asset.pixelsPerWorldUnit = 10.0F;
    asset.frames = {
        {"idle0", {0U,0U,10U,10U}, 10U,10U,0,0,{5.0F,0.0F},0.1F,"idle0"},
        {"idle1", {10U,0U,10U,10U},10U,10U,0,0,{5.0F,0.0F},0.1F,"idle1"},
        {"run0", {20U,0U,10U,10U}, 10U,10U,0,0,{5.0F,0.0F},0.1F,"run0"},
        {"run1", {30U,0U,10U,10U}, 10U,10U,0,0,{5.0F,0.0F},0.1F,"run1"},
    };
    SpriteClip idle{"idle", SpriteLoopMode::Loop, 1.0F, {0U,1U}};
    SpriteCombatWindow idleHurt;
    idleHurt.firstSequenceIndex = 0U;
    idleHurt.lastSequenceIndex = 1U;
    idleHurt.volume.name = "idle_hurt";
    idleHurt.volume.role = SpriteCombatRole::Hurtbox;
    idle.combatWindows.push_back(idleHurt);
    SpriteRootMotionKey idleRoot;
    idleRoot.sequenceIndex = 1U;
    idleRoot.deltaPixels = {2.0F, 0.0F};
    idle.rootMotionKeys.push_back(idleRoot);

    SpriteClip run{"run", SpriteLoopMode::Loop, 1.0F, {2U,3U}};
    SpriteCombatWindow runHit;
    runHit.firstSequenceIndex = 0U;
    runHit.lastSequenceIndex = 1U;
    runHit.volume.name = "run_hit";
    runHit.volume.role = SpriteCombatRole::Hitbox;
    run.combatWindows.push_back(runHit);
    SpriteSocketKey socket;
    socket.name = "weapon";
    socket.sequenceIndex = 0U;
    socket.positionPixels = {3.0F, 4.0F};
    run.socketKeys.push_back(socket);
    SpriteRootMotionKey runRoot;
    runRoot.sequenceIndex = 1U;
    runRoot.deltaPixels = {6.0F, 0.0F};
    run.rootMotionKeys.push_back(runRoot);

    asset.clips = {idle, run};
    assign_sprite_track_ids(asset);
    asset.recompute_hash();
    return asset;
}

SpriteAnimationStateMachineAsset make_machine(const SpriteAsset& sprite) {
    SpriteAnimationStateMachineAsset machine;
    machine.name = "v212 machine";
    machine.compatibleSpriteAssetHash = sprite.contentHash;
    machine.initialState = "idle";
    SpriteAnimationParameterDefinition moving;
    moving.name = "moving";
    moving.defaultValue.type = SpriteAnimationParameterType::Boolean;
    machine.parameters.push_back(moving);
    machine.states = {
        {"idle", "idle", 1.0F, {-100.0F, 0.0F}},
        {"run", "run", 1.0F, {100.0F, 0.0F}},
    };
    SpriteAnimationCondition condition;
    condition.parameter = "moving";
    condition.operation = SpriteAnimationCompareOp::IsTrue;
    condition.comparison.type = SpriteAnimationParameterType::Boolean;
    SpriteAnimationTransition transition;
    transition.fromState = "idle";
    transition.toState = "run";
    transition.blendDurationSeconds = 1.0F;
    transition.blendCurve = SpriteAnimationBlendCurve::SmoothStep;
    transition.trackPolicy = SpriteAnimationBlendTrackPolicy::BothWeighted;
    transition.eventPolicy = SpriteAnimationBlendEventPolicy::Both;
    transition.rootMotionPolicy = SpriteAnimationBlendRootMotionPolicy::Weighted;
    transition.conditions.push_back(condition);
    machine.transitions.push_back(transition);
    machine.recompute_hash();
    return machine;
}

class ScriptedWorld final : public Physics2DWorld {
public:
    Physics2DBackend backend() const noexcept override { return Physics2DBackend::Box2D; }
    Physics2DCapabilities capabilities() const noexcept override {
        Physics2DCapabilities value;
        value.shapeCasts = true;
        return value;
    }
    const Physics2DWorldSettings& settings() const noexcept override { return settings_; }
    bool set_tile_map(const TileMap&, std::string*) override { return true; }
    void clear_tile_map() override {}
    Physics2DBodyHandle create_body(const Physics2DBodyDef& def, std::string*) override {
        Physics2DBodyState state;
        state.positionPixels = def.positionPixels;
        states_.emplace(1U, state);
        return {1U};
    }
    bool destroy_body(Physics2DBodyHandle body) override { return states_.erase(body.value) != 0U; }
    Physics2DColliderHandle add_collider(Physics2DBodyHandle, const Physics2DColliderDef&, std::string*) override { return {2U}; }
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
    bool set_body_linear_velocity(Physics2DBodyHandle, TileVec2) override { return true; }
    bool set_body_gravity_scale(Physics2DBodyHandle, float) override { return true; }
    bool apply_linear_impulse(Physics2DBodyHandle, TileVec2) override { return true; }
    bool set_body_enabled(Physics2DBodyHandle, bool) override { return true; }
    bool drop_through_one_way(Physics2DBodyHandle, float) override { return true; }
    Physics2DRayCastHit ray_cast(TileVec2, TileVec2, std::uint64_t, std::uint64_t) const override { return {}; }
    Physics2DShapeCastHit cast_shape(const Physics2DQueryShape&, TileVec2, const Physics2DQueryFilter&) const override {
        if (cursor_ >= hits_.size()) return {};
        return hits_[cursor_++];
    }
    std::span<const Physics2DEvent> events() const noexcept override { return events_; }
    void set_hits(std::vector<Physics2DShapeCastHit> hits) { hits_ = std::move(hits); cursor_ = 0U; }
private:
    Physics2DWorldSettings settings_{};
    std::unordered_map<std::uint64_t, Physics2DBodyState> states_;
    mutable std::vector<Physics2DShapeCastHit> hits_;
    mutable std::size_t cursor_{};
    std::vector<Physics2DEvent> events_;
};

void test_v3_codec_runtime_and_dual_rendering() {
    const SpriteAsset sprite = make_sprite();
    SpriteAnimationStateMachineAsset machine = make_machine(sprite);
    std::string error;
    require(machine.validate(sprite, &error), error);
    const auto path = std::filesystem::temp_directory_path() / "dve_v212_machine.dvesm";
    require(write_dvesprite_machine(path, machine, &error), error);
    const auto read = read_dvesprite_machine(path);
    require(static_cast<bool>(read), read.error);
    require(read.asset.transitions[0].blendCurve == SpriteAnimationBlendCurve::SmoothStep &&
            read.asset.transitions[0].trackPolicy == SpriteAnimationBlendTrackPolicy::BothWeighted,
            "v3 machine codec omitted gameplay blend policy");

    SpriteRuntime sprites;
    require(sprites.register_asset(1U, sprite, &error), error);
    SpriteInstanceDesc instance;
    instance.asset = 1U;
    instance.clip = "idle";
    require(sprites.bind(7U, instance, &error), error);
    SpriteAnimationStateRuntime runtime(sprites);
    require(runtime.register_machine(2U, 1U, machine, &error), error);
    require(runtime.bind(7U, 2U, &error), error);
    require(runtime.set_boolean(7U, "moving", true, &error), error);
    runtime.tick(0.0F);
    runtime.tick(0.5F);
    const auto blend = runtime.blend_sample(7U);
    require(blend && close(blend->destinationWeight, 0.5F), "smooth-step blend weight is incorrect");
    const auto gameplay = runtime.blend_gameplay_sample(7U);
    require(gameplay && gameplay->combatVolumes.size() == 2U &&
            close(gameplay->combatVolumes[0].weight + gameplay->combatVolumes[1].weight, 1.0F),
            "blend gameplay policy did not retain weighted source and destination tracks");
    const SpriteRenderList render = runtime.build_render_list();
    require(render.items.size() == 2U && render.items[0].blendMode == SpriteBlendMode::Alpha &&
            close(render.items[0].vertices[0].color[3], 0.5F) &&
            close(render.items[1].vertices[0].color[3], 0.5F),
            "active blend did not produce dual alpha-weighted sprite submissions");
    require(!runtime.gameplay_events().empty(), "blend event policy produced no interval evidence");
    const auto pending = sprites.pending_root_motion(7U);
    require(pending && pending->worldTranslation.x > 0.0F,
            "weighted blend root motion did not reach the sprite runtime");
    std::filesystem::remove(path);
}

void test_visual_graph_workspace() {
    const SpriteAsset sprite = make_sprite();
    SpriteAnimationMachineGraphWorkspace graph;
    std::string error;
    require(graph.create(sprite, "graph", "idle", &error), error);
    require(graph.session().add_state("run", "run", {220.0F, 80.0F}, &error), error);
    const SpriteAnimationGraphRect viewport{0.0F,0.0F,800.0F,500.0F};
    SpriteAnimationGraphFrame frame = graph.frame(viewport);
    require(frame.nodes.size() == 2U, "graph frame omitted state nodes");
    const auto entry = frame.nodes[0];
    const auto run = frame.nodes[1];
    const SpriteVec2 entryPort{entry.outputPort.x + entry.outputPort.width * 0.5F,
                               entry.outputPort.y + entry.outputPort.height * 0.5F};
    const SpriteVec2 runCenter{run.rect.x + run.rect.width * 0.5F,
                               run.rect.y + run.rect.height * 0.5F};
    require(graph.pointer_down(1, entryPort, viewport), "graph transition drag did not start");
    require(graph.pointer_move(runCenter, viewport), "graph transition preview did not update");
    require(graph.pointer_up(1, runCenter, viewport, &error), error);
    require(graph.session().asset().transitions.size() == 1U,
            "graph transition drag did not author a transition");

    frame = graph.frame(viewport);
    const SpriteVec2 runPoint{frame.nodes[1].rect.x + 20.0F, frame.nodes[1].rect.y + 20.0F};
    require(graph.pointer_down(1, runPoint, viewport), "graph node drag did not start");
    require(graph.pointer_move({runPoint.x + 50.0F, runPoint.y + 25.0F}, viewport),
            "graph node drag did not update");
    require(graph.pointer_up(1, {runPoint.x + 50.0F, runPoint.y + 25.0F}, viewport, &error), error);
    require(graph.session().asset().states[1].graphPosition.x > 220.0F,
            "graph node drag did not commit through the authoring session");

    graph.set_search_query("run");
    require(graph.select_search_result(), "graph search did not select a matching node");
    require(graph.key_down("c", true, false, &error), "graph copy failed");
    require(graph.key_down("v", true, false, &error), error);
    require(graph.session().asset().states.size() == 3U,
            "graph copy/paste did not duplicate the selected state");
    const float oldZoom = graph.view().zoom;
    require(graph.wheel(2.0F, {400.0F,250.0F}, viewport) && graph.view().zoom > oldZoom,
            "graph cursor-centered zoom failed");
    graph.set_live_state("run", "Entry");
    frame = graph.frame(viewport);
    require(std::count_if(frame.nodes.begin(), frame.nodes.end(),
            [](const SpriteAnimationGraphNodeFrame& node) { return node.live; }) >= 2,
            "graph live-state highlighting omitted active states");
}

void test_step_snap_platform_root_motion() {
    ScriptedWorld world;
    Physics2DBodyDef bodyDef;
    const Physics2DBodyHandle body = world.create_body(bodyDef, nullptr);
    Physics2DShapeCastHit wall;
    wall.hit = true;
    wall.fraction = 0.25F;
    wall.normal = {-1.0F, 0.0F};
    Physics2DShapeCastHit ground;
    ground.hit = true;
    ground.fraction = 0.5F;
    ground.normal = {0.0F, 1.0F};
    world.set_hits({wall, {}, {}, ground});

    SpriteRootMotionDelta delta;
    delta.worldTranslation = {10.0F, 0.0F, 0.0F};
    SpriteRootMotionCollisionOptions options;
    options.application.pixelsPerWorldUnit = 1.0F;
    options.application.applyRotation = false;
    options.localShape.shape = Physics2DShapeType::Box;
    options.localShape.halfExtentsPixels = {2.0F, 4.0F};
    options.skinPixels = 0.0F;
    options.allowStepUp = true;
    options.maximumStepHeightPixels = 4.0F;
    options.snapToGround = true;
    options.groundSnapDistancePixels = 2.0F;
    options.movingPlatformDeltaPixels = {2.0F, 0.0F};
    SpriteRootMotionCollisionResult result;
    std::string error;
    require(apply_sprite_root_motion_collision_aware(delta, world, body, options, result, &error), error);
    Physics2DBodyState state;
    require(world.body_state(body, state), "root-motion body disappeared");
    require(result.steppedUp && result.snappedToGround && result.movingPlatformApplied &&
            close(result.residualPixels.x, 0.0F) && close(state.positionPixels.x, 12.0F) &&
            close(state.positionPixels.y, 3.0F),
            "step, ground snap, or moving-platform decomposition is incorrect");
}

} // namespace

int main() {
    try {
        test_v3_codec_runtime_and_dual_rendering();
        test_visual_graph_workspace();
        test_step_snap_platform_root_motion();
        std::cout << "v2.12 sprite animation tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "v2.12 sprite animation tests failed: " << exception.what() << '\n';
        return 1;
    }
}
