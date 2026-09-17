#pragma once

#include "dve/character_controller2d.hpp"
#include "dve/game_world.hpp"
#include "dve/physics2d.hpp"
#include "dve/sprite2d.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dve {

using SpriteAnimationMachineId = std::uint64_t;
inline constexpr SpriteAnimationMachineId kInvalidSpriteAnimationMachineId = 0U;

enum class SpriteAnimationParameterType : std::uint8_t { Boolean, Integer, Float, Trigger };
enum class SpriteAnimationCompareOp : std::uint8_t {
    IsTrue,
    IsFalse,
    Equal,
    NotEqual,
    Greater,
    GreaterEqual,
    Less,
    LessEqual,
    Triggered,
    NotTriggered,
};

struct SpriteAnimationParameterValue {
    SpriteAnimationParameterType type{SpriteAnimationParameterType::Boolean};
    bool booleanValue{};
    std::int64_t integerValue{};
    float floatValue{};
};

struct SpriteAnimationParameterDefinition {
    std::string name;
    SpriteAnimationParameterValue defaultValue;
};

struct SpriteAnimationCondition {
    std::string parameter;
    SpriteAnimationCompareOp operation{SpriteAnimationCompareOp::IsTrue};
    SpriteAnimationParameterValue comparison;
};

struct SpriteAnimationState {
    std::string name;
    std::string clip;
    float playbackSpeed{1.0F};
    // Native graph-authoring position. Runtime semantics do not depend on this value.
    SpriteVec2 graphPosition{};
};

enum class SpriteAnimationInterruptionSource : std::uint8_t {
    NoInterruption,
    CurrentState,
    PreviousState,
    CurrentThenPrevious,
    PreviousThenCurrent,
};

enum class SpriteAnimationBlendCurve : std::uint8_t {
    Linear,
    SmoothStep,
    EaseIn,
    EaseOut,
};

enum class SpriteAnimationBlendTrackPolicy : std::uint8_t {
    DestinationOnly,
    SourceOnly,
    HighestWeight,
    BothWeighted,
};

enum class SpriteAnimationBlendEventPolicy : std::uint8_t {
    DestinationOnly,
    SourceOnly,
    Both,
};

enum class SpriteAnimationBlendRootMotionPolicy : std::uint8_t {
    DestinationOnly,
    SourceOnly,
    Weighted,
};

struct SpriteAnimationTransition {
    // "*" is an any-state transition. Declaration order breaks equal-priority ties.
    std::string fromState;
    std::string toState;
    std::int32_t priority{};
    bool hasExitTime{};
    float exitNormalizedTime{1.0F};
    float minimumStateTimeSeconds{};
    bool resetTime{true};
    bool consumeTriggers{true};
    float blendDurationSeconds{};
    bool synchronizeNormalizedTime{};
    SpriteAnimationInterruptionSource interruptionSource{
        SpriteAnimationInterruptionSource::CurrentState};
    SpriteAnimationBlendCurve blendCurve{SpriteAnimationBlendCurve::Linear};
    SpriteAnimationBlendTrackPolicy trackPolicy{
        SpriteAnimationBlendTrackPolicy::HighestWeight};
    SpriteAnimationBlendEventPolicy eventPolicy{
        SpriteAnimationBlendEventPolicy::DestinationOnly};
    SpriteAnimationBlendRootMotionPolicy rootMotionPolicy{
        SpriteAnimationBlendRootMotionPolicy::Weighted};
    std::vector<SpriteAnimationCondition> conditions;
};

struct SpriteAnimationStateMachineAsset {
    std::string name;
    std::uint64_t compatibleSpriteAssetHash{}; // zero permits any asset with the named clips
    std::string initialState;
    std::vector<SpriteAnimationParameterDefinition> parameters;
    std::vector<SpriteAnimationState> states;
    std::vector<SpriteAnimationTransition> transitions;
    std::uint64_t contentHash{};

    [[nodiscard]] bool validate(const SpriteAsset& sprite, std::string* error = nullptr) const;
    void recompute_hash() noexcept;
};

[[nodiscard]] std::uint64_t sprite_animation_state_machine_content_hash(
    const SpriteAnimationStateMachineAsset& asset) noexcept;
[[nodiscard]] bool write_dvesprite_machine(
    const std::filesystem::path& path, const SpriteAnimationStateMachineAsset& asset,
    std::string* error = nullptr);
struct SpriteAnimationMachineReadResult {
    SpriteAnimationStateMachineAsset asset;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
[[nodiscard]] SpriteAnimationMachineReadResult read_dvesprite_machine(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes = 4ULL * 1024ULL * 1024ULL);

struct SpriteAnimationTransitionEvent {
    SpriteOwnerId owner{kInvalidSpriteOwnerId};
    SpriteAnimationMachineId machine{kInvalidSpriteAnimationMachineId};
    std::string fromState;
    std::string toState;
    std::size_t transitionIndex{};
    float spriteTimeSeconds{};
    std::uint64_t transitionSerial{};
};

struct SpriteAnimationBlendSnapshot {
    bool active{};
    std::string sourceState;
    std::string destinationState;
    float sourceTimeSeconds{};
    float elapsedSeconds{};
    float durationSeconds{};
    SpriteAnimationInterruptionSource interruptionSource{
        SpriteAnimationInterruptionSource::CurrentState};
    SpriteAnimationBlendCurve curve{SpriteAnimationBlendCurve::Linear};
    SpriteAnimationBlendTrackPolicy trackPolicy{
        SpriteAnimationBlendTrackPolicy::HighestWeight};
    SpriteAnimationBlendEventPolicy eventPolicy{
        SpriteAnimationBlendEventPolicy::DestinationOnly};
    SpriteAnimationBlendRootMotionPolicy rootMotionPolicy{
        SpriteAnimationBlendRootMotionPolicy::Weighted};
    [[nodiscard]] float destination_weight() const noexcept;
};

struct SpriteAnimationBlendSample {
    SpriteOwnerId owner{kInvalidSpriteOwnerId};
    bool active{};
    std::string sourceState;
    std::string destinationState;
    std::optional<SpriteSample> source;
    std::optional<SpriteSample> destination;
    float sourceWeight{};
    float destinationWeight{1.0F};
};


struct SpriteAnimationWeightedCombatSample {
    SpriteCombatVolumeSample sample;
    float weight{1.0F};
    bool source{};
};

struct SpriteAnimationWeightedSocketSample {
    SpriteSocketSample sample;
    float weight{1.0F};
    bool source{};
};

struct SpriteAnimationWeightedPropertySample {
    SpritePropertySample sample;
    float weight{1.0F};
    bool source{};
};

struct SpriteAnimationBlendGameplaySample {
    SpriteOwnerId owner{kInvalidSpriteOwnerId};
    bool active{};
    float sourceWeight{};
    float destinationWeight{1.0F};
    std::vector<SpriteAnimationWeightedCombatSample> combatVolumes;
    std::vector<SpriteAnimationWeightedSocketSample> sockets;
    std::vector<SpriteAnimationWeightedPropertySample> properties;
    SpriteRootMotionDelta rootMotion;
};

struct SpriteAnimationControllerSnapshot {
    SpriteOwnerId owner{kInvalidSpriteOwnerId};
    SpriteAnimationMachineId machine{kInvalidSpriteAnimationMachineId};
    std::string state;
    float stateElapsedSeconds{};
    float spriteTimeSeconds{};
    std::uint64_t transitionSerial{};
    SpriteAnimationBlendSnapshot blend;
    std::map<std::string, SpriteAnimationParameterValue, std::less<>> parameters;
};

// Owns state-machine evaluation for every registered sprite owner and advances the supplied
// SpriteRuntime exactly once per tick. Do not also call SpriteRuntime::tick for the same frame.
class SpriteAnimationStateRuntime {
public:
    explicit SpriteAnimationStateRuntime(SpriteRuntime& sprites) noexcept : sprites_(&sprites) {}

    [[nodiscard]] bool register_machine(
        SpriteAnimationMachineId id, SpriteAssetId spriteAsset,
        SpriteAnimationStateMachineAsset machine, std::string* error = nullptr);
    [[nodiscard]] bool unregister_machine(SpriteAnimationMachineId id) noexcept;
    [[nodiscard]] const SpriteAnimationStateMachineAsset* machine(
        SpriteAnimationMachineId id) const noexcept;

    [[nodiscard]] bool bind(
        SpriteOwnerId owner, SpriteAnimationMachineId machine,
        std::string* error = nullptr);
    [[nodiscard]] bool unbind(SpriteOwnerId owner) noexcept;
    [[nodiscard]] bool contains(SpriteOwnerId owner) const noexcept;
    [[nodiscard]] std::string_view current_state(SpriteOwnerId owner) const noexcept;
    [[nodiscard]] float state_elapsed_seconds(SpriteOwnerId owner) const noexcept;
    [[nodiscard]] bool is_blending(SpriteOwnerId owner) const noexcept;
    [[nodiscard]] std::optional<SpriteAnimationBlendSample> blend_sample(
        SpriteOwnerId owner) const noexcept;
    [[nodiscard]] std::optional<SpriteAnimationBlendGameplaySample> blend_gameplay_sample(
        SpriteOwnerId owner) const noexcept;
    [[nodiscard]] SpriteRenderList build_render_list(Float3 cameraOrigin = {}) const;

    [[nodiscard]] bool set_boolean(
        SpriteOwnerId owner, std::string_view parameter, bool value,
        std::string* error = nullptr);
    [[nodiscard]] bool set_integer(
        SpriteOwnerId owner, std::string_view parameter, std::int64_t value,
        std::string* error = nullptr);
    [[nodiscard]] bool set_float(
        SpriteOwnerId owner, std::string_view parameter, float value,
        std::string* error = nullptr);
    [[nodiscard]] bool fire_trigger(
        SpriteOwnerId owner, std::string_view parameter, std::string* error = nullptr);
    [[nodiscard]] bool reset_trigger(
        SpriteOwnerId owner, std::string_view parameter, std::string* error = nullptr);
    [[nodiscard]] std::optional<SpriteAnimationParameterValue> parameter(
        SpriteOwnerId owner, std::string_view name) const noexcept;

    [[nodiscard]] bool force_state(
        SpriteOwnerId owner, std::string_view state, bool restart = true,
        std::string* error = nullptr);
    [[nodiscard]] std::optional<SpriteAnimationControllerSnapshot> capture_snapshot(
        SpriteOwnerId owner) const;
    [[nodiscard]] bool restore_snapshot(
        const SpriteAnimationControllerSnapshot& snapshot, std::string* error = nullptr);

    void tick(float deltaSeconds);
    [[nodiscard]] std::span<const SpriteAnimationTransitionEvent> transition_events() const noexcept {
        return transitionEvents_;
    }
    [[nodiscard]] std::span<const SpriteIntervalEvent> gameplay_events() const noexcept {
        return gameplayEvents_;
    }

private:
    struct RegisteredMachine {
        SpriteAssetId spriteAsset{kInvalidSpriteAssetId};
        SpriteAnimationStateMachineAsset asset;
    };
    struct Controller {
        SpriteAnimationMachineId machine{kInvalidSpriteAnimationMachineId};
        std::string state;
        float stateElapsedSeconds{};
        std::uint64_t transitionSerial{};
        SpriteAnimationBlendSnapshot blend;
        std::map<std::string, SpriteAnimationParameterValue, std::less<>> parameters;
    };

    [[nodiscard]] bool evaluate_transition(
        SpriteOwnerId owner, Controller& controller, bool exitTimePass);
    [[nodiscard]] bool transition_conditions_met(
        const SpriteAnimationTransition& transition, const Controller& controller) const noexcept;
    [[nodiscard]] bool apply_state(
        SpriteOwnerId owner, Controller& controller, std::string_view state,
        bool restart, std::string* error = nullptr);

    SpriteRuntime* sprites_{};
    std::map<SpriteAnimationMachineId, RegisteredMachine> machines_;
    std::map<SpriteOwnerId, Controller> controllers_;
    std::vector<SpriteAnimationTransitionEvent> transitionEvents_;
    std::vector<SpriteIntervalEvent> gameplayEvents_;
};

using SpriteSideEffectCommandId = std::uint64_t;
inline constexpr SpriteSideEffectCommandId kInvalidSpriteSideEffectCommandId = 0U;

enum class SpriteSideEffectSource : std::uint8_t { IntervalEvent, StateTransition };

struct SpriteSideEffectCommand {
    SpriteSideEffectCommandId id{kInvalidSpriteSideEffectCommandId};
    std::uint64_t simulationTick{};
    SpriteSideEffectSource source{SpriteSideEffectSource::IntervalEvent};
    SpriteIntervalEvent intervalEvent;
    SpriteAnimationTransitionEvent transitionEvent;
};

// Idempotency ledger for audio, effects, projectiles, camera impulses, and other commands that
// must not execute twice during rollback resimulation. Commands after a rollback tick are removed
// and may be issued again when the simulation replays those ticks.
class SpriteSideEffectLedger {
public:
    [[nodiscard]] std::optional<SpriteSideEffectCommand> issue(
        std::uint64_t simulationTick, const SpriteIntervalEvent& event);
    [[nodiscard]] std::optional<SpriteSideEffectCommand> issue(
        std::uint64_t simulationTick, const SpriteAnimationTransitionEvent& event);
    [[nodiscard]] bool contains(SpriteSideEffectCommandId id) const noexcept;
    [[nodiscard]] std::size_t committed_count() const noexcept { return committed_.size(); }
    void rollback_after(std::uint64_t simulationTick) noexcept;
    void clear() noexcept { committed_.clear(); }

private:
    std::map<SpriteSideEffectCommandId, std::uint64_t> committed_;
};

enum class SpriteRootMotionApplyMode : std::uint8_t { Transform, Velocity };
struct SpriteRootMotionApplyOptions {
    SpriteRootMotionApplyMode mode{SpriteRootMotionApplyMode::Transform};
    GameplayPlane2D plane{GameplayPlane2D::XY};
    float pixelsPerWorldUnit{16.0F};
    float deltaSeconds{1.0F / 60.0F};
    float translationScale{1.0F};
    bool applyRotation{true};
    bool additiveVelocity{true};
};

[[nodiscard]] bool apply_sprite_root_motion_to_physics2d(
    const SpriteRootMotionDelta& delta, Physics2DWorld& world, Physics2DBodyHandle body,
    const SpriteRootMotionApplyOptions& options = {}, std::string* error = nullptr);
[[nodiscard]] bool apply_pending_sprite_root_motion_to_physics2d(
    SpriteRuntime& sprites, SpriteOwnerId owner, Physics2DWorld& world,
    Physics2DBodyHandle body, const SpriteRootMotionApplyOptions& options = {},
    std::string* error = nullptr);
[[nodiscard]] bool apply_sprite_root_motion_to_game_object(
    const SpriteRootMotionDelta& delta, GameWorld& world, GameObjectId object,
    const SpriteRootMotionApplyOptions& options = {}, std::string* error = nullptr);
[[nodiscard]] bool apply_pending_sprite_root_motion_to_game_object(
    SpriteRuntime& sprites, SpriteOwnerId owner, GameWorld& world, GameObjectId object,
    const SpriteRootMotionApplyOptions& options = {}, std::string* error = nullptr);

struct SpriteRootMotionCollisionOptions {
    SpriteRootMotionApplyOptions application{};
    // Geometry is authored in body-local pixel coordinates. The adapter moves it to the current
    // body transform before each shape cast.
    Physics2DQueryShape localShape{};
    Physics2DQueryFilter filter{};
    float skinPixels{0.05F};
    std::uint32_t maximumSlideIterations{2U};
    bool slideAlongSurfaces{true};
    bool allowStepUp{};
    float maximumStepHeightPixels{4.0F};
    bool snapToGround{};
    float groundSnapDistancePixels{2.0F};
    float maximumGroundSlopeDegrees{55.0F};
    TileVec2 movingPlatformDeltaPixels{};
    bool requeueResidual{};
};

struct SpriteRootMotionCollisionResult {
    TileVec2 requestedPixels{};
    TileVec2 appliedPixels{};
    TileVec2 residualPixels{};
    std::uint32_t collisionCount{};
    bool startedOverlapping{};
    bool blocked{};
    bool steppedUp{};
    bool snappedToGround{};
    bool movingPlatformApplied{};
    TileVec2 supportNormal{};
};

[[nodiscard]] bool apply_sprite_root_motion_collision_aware(
    const SpriteRootMotionDelta& delta, Physics2DWorld& world, Physics2DBodyHandle body,
    const SpriteRootMotionCollisionOptions& options, SpriteRootMotionCollisionResult& result,
    std::string* error = nullptr);
[[nodiscard]] bool apply_pending_sprite_root_motion_collision_aware(
    SpriteRuntime& sprites, SpriteOwnerId owner, Physics2DWorld& world, Physics2DBodyHandle body,
    const SpriteRootMotionCollisionOptions& options, SpriteRootMotionCollisionResult& result,
    std::string* error = nullptr);
[[nodiscard]] bool apply_pending_sprite_root_motion_to_character_controller(
    SpriteRuntime& sprites, SpriteOwnerId owner, Physics2DWorld& world, Physics2DBodyHandle body,
    CharacterController2D& controller, const SpriteRootMotionCollisionOptions& options,
    SpriteRootMotionCollisionResult& result, std::string* error = nullptr);

struct SpriteGameObjectAttachmentBinding {
    SpriteAttachmentId attachment{kInvalidSpriteAttachmentId};
    GameObjectId object{kInvalidGameObjectId};
    bool inheritVisibility{true};
    bool hideWhenSocketMissing{true};
};

struct SpriteAttachmentSyncReport {
    std::size_t updated{};
    std::size_t hidden{};
    std::size_t missing{};
    std::size_t failed{};
    std::vector<std::string> diagnostics;
};

// Applies renderer-neutral socket attachment samples to GameWorld marker/dynamic objects.
class SpriteGameObjectAttachmentBridge {
public:
    [[nodiscard]] bool bind(SpriteGameObjectAttachmentBinding binding,
                            std::string* error = nullptr);
    [[nodiscard]] bool unbind(SpriteAttachmentId attachment) noexcept;
    void clear() noexcept { bindings_.clear(); }
    [[nodiscard]] SpriteAttachmentSyncReport sync(
        const SpriteRuntime& sprites, GameWorld& world) const;

private:
    std::map<SpriteAttachmentId, SpriteGameObjectAttachmentBinding> bindings_;
};

} // namespace dve
