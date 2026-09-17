#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dve/game_world.hpp"
#include "dve/query.hpp"

namespace dve {

using GamePlayerId = std::uint32_t;
inline constexpr GamePlayerId kInvalidGamePlayerId = 0;
using GameTriggerId = std::uint64_t;
inline constexpr GameTriggerId kInvalidGameTriggerId = 0;

enum class CharacterStance : std::uint8_t { Standing, Crouched };

struct CharacterControllerConfig {
    float standingHeightMeters{1.8F};
    float crouchedHeightMeters{1.15F};
    float radiusMeters{0.35F};
    float skinMeters{0.015F};
    float maximumGroundSpeedMetersPerSecond{5.5F};
    float groundAccelerationMetersPerSecondSquared{38.0F};
    float airAccelerationMetersPerSecondSquared{11.0F};
    float groundBrakingMetersPerSecondSquared{42.0F};
    float gravityMetersPerSecondSquared{22.0F};
    float jumpSpeedMetersPerSecond{7.0F};
    float maximumFallSpeedMetersPerSecond{32.0F};
    float maximumSlopeDegrees{50.0F};
    float stepHeightMeters{0.35F};
    float groundProbeMeters{0.12F};
    float coyoteTimeSeconds{0.10F};
    float jumpBufferSeconds{0.12F};
    std::uint32_t maximumSlideIterations{5U};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct CharacterInput {
    Float3 move{}; // x/y are the horizontal plane; z is ignored.
    bool jumpPressed{};
    bool crouchHeld{};
};

struct CharacterControllerState {
    GameObjectId pawn{kInvalidGameObjectId};
    Float3 velocity{};
    bool grounded{};
    GameObjectId supportObject{kInvalidGameObjectId};
    Float3 groundNormal{0.0F, 0.0F, 1.0F};
    CharacterStance stance{CharacterStance::Standing};
    float coyoteRemainingSeconds{};
    float jumpBufferRemainingSeconds{};
    std::uint64_t fixedTick{};
};

struct CharacterMoveTelemetry {
    std::uint32_t sweeps{};
    std::uint32_t contacts{};
    std::uint32_t stepAttempts{};
    std::uint32_t successfulSteps{};
    std::uint32_t depenetrationIterations{};
    bool jumped{};
    bool ceilingHit{};
    bool floorRemoved{};
    bool inheritedPlatformVelocity{};
};

struct CharacterReplayFrame {
    std::uint64_t tick{};
    CharacterInput input{};
};

struct CharacterReplay {
    std::vector<CharacterReplayFrame> frames;
    [[nodiscard]] std::uint64_t stable_hash() const noexcept;
};

enum class TriggerShape : std::uint8_t { Box, Sphere };
enum class TriggerEventKind : std::uint8_t { Enter, Stay, Exit };

struct TriggerVolumeDesc {
    std::string name;
    TriggerShape shape{TriggerShape::Box};
    RigidTransform transform{};
    Float3 halfExtents{0.5F, 0.5F, 0.5F};
    float radiusMeters{0.5F};
    bool enabled{true};
    bool oneShot{};
    bool charactersOnly{true};
    std::string requiredTag;

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct TriggerEvent {
    GameTriggerId trigger{kInvalidGameTriggerId};
    TriggerEventKind kind{TriggerEventKind::Enter};
    GameObjectId object{kInvalidGameObjectId};
    std::uint64_t fixedTick{};
};

struct GamePlayerState {
    GamePlayerId id{kInvalidGamePlayerId};
    std::string name;
    bool local{};
    GameObjectId pawn{kInvalidGameObjectId};
    CharacterInput input{};
};

class GameplayRuntime {
public:
    explicit GameplayRuntime(GameWorld& world);

    [[nodiscard]] bool add_character(
        GameObjectId pawn, CharacterControllerConfig config = {}, std::string* error = nullptr);
    bool remove_character(GameObjectId pawn);
    [[nodiscard]] bool has_character(GameObjectId pawn) const noexcept;
    [[nodiscard]] CharacterControllerState* character(GameObjectId pawn) noexcept;
    [[nodiscard]] const CharacterControllerState* character(GameObjectId pawn) const noexcept;
    [[nodiscard]] const CharacterMoveTelemetry* telemetry(GameObjectId pawn) const noexcept;
    [[nodiscard]] std::vector<GameObjectId> character_ids() const;
    bool set_character_input(GameObjectId pawn, CharacterInput input);

    [[nodiscard]] GamePlayerId create_player(std::string name, bool local = true);
    bool destroy_player(GamePlayerId player);
    bool possess(GamePlayerId player, GameObjectId pawn, std::string* error = nullptr);
    bool unpossess(GamePlayerId player);
    [[nodiscard]] GamePlayerState* player(GamePlayerId player) noexcept;
    [[nodiscard]] const GamePlayerState* player(GamePlayerId player) const noexcept;
    [[nodiscard]] std::optional<GamePlayerId> controller_of(GameObjectId pawn) const noexcept;
    bool set_player_input(GamePlayerId player, CharacterInput input);

    // Prediction/reconciliation entry points. They update one character without advancing
    // unrelated players, timers, or trigger callbacks.
    bool simulate_character_input(
        GameObjectId pawn, CharacterInput input, float fixedDeltaSeconds,
        std::uint64_t simulationTick, std::string* error = nullptr);
    bool apply_authoritative_character_state(
        GameObjectId pawn, Float3 worldPosition, Float3 velocity, bool grounded,
        GameObjectId supportObject, CharacterStance stance, std::uint64_t authoritativeTick,
        std::string* error = nullptr);

    [[nodiscard]] GameTriggerId create_trigger(TriggerVolumeDesc desc, std::string* error = nullptr);
    bool destroy_trigger(GameTriggerId trigger);
    bool set_trigger_enabled(GameTriggerId trigger, bool enabled);
    [[nodiscard]] const TriggerVolumeDesc* trigger(GameTriggerId trigger) const noexcept;
    [[nodiscard]] bool trigger_fired(GameTriggerId trigger) const noexcept;
    [[nodiscard]] std::vector<GameTriggerId> trigger_ids() const;

    using TriggerListener = std::function<void(const TriggerEvent&)>;
    void on_trigger(TriggerListener listener);

    bool begin_recording(GamePlayerId player);
    [[nodiscard]] std::optional<CharacterReplay> end_recording(GamePlayerId player);
    bool begin_playback(GamePlayerId player, CharacterReplay replay, bool loop = false);
    bool stop_playback(GamePlayerId player);

    void fixed_update(float fixedDeltaSeconds);
    [[nodiscard]] std::uint64_t fixed_tick() const noexcept { return fixedTick_; }

    [[nodiscard]] std::string serialize_trigger_state() const;
    bool restore_trigger_state(std::string_view text, std::string* error = nullptr);

private:
    struct CharacterRecord {
        CharacterControllerConfig config{};
        CharacterControllerState state{};
        CharacterInput input{};
        CharacterMoveTelemetry telemetry{};
    };
    struct TriggerRecord {
        TriggerVolumeDesc desc{};
        std::unordered_set<GameObjectId> occupants;
        bool fired{};
    };
    struct RecordingState { CharacterReplay replay; };
    struct PlaybackState {
        CharacterReplay replay;
        std::size_t nextFrame{};
        bool loop{};
        std::uint64_t playbackStartTick{};
        std::uint64_t replayFirstTick{};
    };

    [[nodiscard]] Capsule capsule_for(GameObjectId pawn, const CharacterRecord& record) const;
    [[nodiscard]] bool try_set_stance(GameObjectId pawn, CharacterRecord& record, CharacterStance stance);
    void update_character(GameObjectId pawn, CharacterRecord& record, float dt, std::uint64_t simulationTick);
    void update_triggers();

    GameWorld* world_{};
    std::unordered_map<GameObjectId, CharacterRecord> characters_;
    std::unordered_map<GamePlayerId, GamePlayerState> players_;
    std::unordered_map<GameObjectId, GamePlayerId> pawnControllers_;
    std::unordered_map<GameTriggerId, TriggerRecord> triggers_;
    std::vector<TriggerListener> triggerListeners_;
    std::unordered_map<GamePlayerId, RecordingState> recordings_;
    std::unordered_map<GamePlayerId, PlaybackState> playbacks_;
    GamePlayerId nextPlayerId_{1U};
    GameTriggerId nextTriggerId_{1U};
    std::uint64_t fixedTick_{};
};

} // namespace dve
