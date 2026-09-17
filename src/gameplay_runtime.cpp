#include "dve/gameplay_runtime.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <sstream>

namespace dve {
namespace {

constexpr float kPi = 3.14159265358979323846F;

[[nodiscard]] bool finite(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] float clamp_axis(float current, float target, float maximumDelta) noexcept {
    if (current < target) return std::min(current + maximumDelta, target);
    if (current > target) return std::max(current - maximumDelta, target);
    return current;
}

[[nodiscard]] Float3 horizontal(Float3 value) noexcept { return {value.x, value.y, 0.0F}; }

[[nodiscard]] Float3 capsule_base(const Capsule& capsule) noexcept {
    return {capsule.pointA.x, capsule.pointA.y, capsule.pointA.z - capsule.radius};
}

void translate(Capsule& capsule, Float3 displacement) noexcept {
    capsule.pointA = add(capsule.pointA, displacement);
    capsule.pointB = add(capsule.pointB, displacement);
}

[[nodiscard]] float segment_distance_squared(Float3 point, Float3 a, Float3 b) noexcept {
    const Float3 ab = subtract(b, a);
    const float denominator = length_squared(ab);
    const float t = denominator > 1.0e-8F
        ? std::clamp(dot(subtract(point, a), ab) / denominator, 0.0F, 1.0F) : 0.0F;
    return length_squared(subtract(point, add(a, multiply(ab, t))));
}

[[nodiscard]] bool capsule_overlaps_trigger(
    const Capsule& worldCapsule, const TriggerVolumeDesc& trigger) noexcept {
    const Capsule local{
        inverse_transform_point(trigger.transform, worldCapsule.pointA),
        inverse_transform_point(trigger.transform, worldCapsule.pointB),
        worldCapsule.radius};
    if (trigger.shape == TriggerShape::Sphere) {
        const float combined = trigger.radiusMeters + local.radius;
        return segment_distance_squared({}, local.pointA, local.pointB) <= combined * combined;
    }
    const Float3 minimum = subtract(
        {std::min(local.pointA.x, local.pointB.x), std::min(local.pointA.y, local.pointB.y),
         std::min(local.pointA.z, local.pointB.z)},
        {local.radius, local.radius, local.radius});
    const Float3 maximum = add(
        {std::max(local.pointA.x, local.pointB.x), std::max(local.pointA.y, local.pointB.y),
         std::max(local.pointA.z, local.pointB.z)},
        {local.radius, local.radius, local.radius});
    return maximum.x >= -trigger.halfExtents.x && minimum.x <= trigger.halfExtents.x &&
           maximum.y >= -trigger.halfExtents.y && minimum.y <= trigger.halfExtents.y &&
           maximum.z >= -trigger.halfExtents.z && minimum.z <= trigger.halfExtents.z;
}

[[nodiscard]] bool point_inside_trigger(Float3 worldPoint, const TriggerVolumeDesc& trigger) noexcept {
    const Float3 local = inverse_transform_point(trigger.transform, worldPoint);
    if (trigger.shape == TriggerShape::Sphere) return length_squared(local) <= trigger.radiusMeters * trigger.radiusMeters;
    return std::abs(local.x) <= trigger.halfExtents.x && std::abs(local.y) <= trigger.halfExtents.y &&
           std::abs(local.z) <= trigger.halfExtents.z;
}

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
}

template <typename T>
void hash_value(std::uint64_t& hash, const T& value) noexcept { hash_bytes(hash, &value, sizeof(value)); }

} // namespace

bool CharacterControllerConfig::validate(std::string* error) const noexcept {
    const auto reject = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (!(radiusMeters > 0.0F) || !std::isfinite(radiusMeters)) return reject("character radius must be positive and finite");
    if (!(standingHeightMeters >= radiusMeters * 2.0F) || !std::isfinite(standingHeightMeters))
        return reject("standing height must fit the capsule diameter");
    if (!(crouchedHeightMeters >= radiusMeters * 2.0F) || crouchedHeightMeters > standingHeightMeters)
        return reject("crouched height must be between the capsule diameter and standing height");
    if (!(skinMeters >= 0.0F) || skinMeters >= radiusMeters) return reject("character skin must be non-negative and smaller than radius");
    if (!(maximumGroundSpeedMetersPerSecond >= 0.0F) || !std::isfinite(maximumGroundSpeedMetersPerSecond))
        return reject("maximum ground speed must be finite and non-negative");
    if (!(groundAccelerationMetersPerSecondSquared >= 0.0F) || !(airAccelerationMetersPerSecondSquared >= 0.0F) ||
        !(groundBrakingMetersPerSecondSquared >= 0.0F)) return reject("character acceleration values must be non-negative");
    if (!(gravityMetersPerSecondSquared >= 0.0F) || !(jumpSpeedMetersPerSecond >= 0.0F) ||
        !(maximumFallSpeedMetersPerSecond > 0.0F)) return reject("gravity, jump speed, and maximum fall speed are invalid");
    if (!(maximumSlopeDegrees >= 0.0F && maximumSlopeDegrees < 89.0F)) return reject("maximum slope must be in [0,89)");
    if (!(stepHeightMeters >= 0.0F) || !(groundProbeMeters >= 0.0F)) return reject("step height and ground probe must be non-negative");
    if (!(coyoteTimeSeconds >= 0.0F) || !(jumpBufferSeconds >= 0.0F)) return reject("jump timing windows must be non-negative");
    if (maximumSlideIterations == 0U || maximumSlideIterations > 16U) return reject("slide iterations must be between 1 and 16");
    return true;
}

bool TriggerVolumeDesc::validate(std::string* error) const noexcept {
    const auto reject = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (!finite(transform.position)) return reject("trigger transform must be finite");
    if (shape == TriggerShape::Sphere) {
        if (!(radiusMeters > 0.0F) || !std::isfinite(radiusMeters)) return reject("trigger radius must be positive and finite");
    } else if (!(halfExtents.x > 0.0F && halfExtents.y > 0.0F && halfExtents.z > 0.0F) || !finite(halfExtents)) {
        return reject("trigger half extents must be positive and finite");
    }
    return true;
}

std::uint64_t CharacterReplay::stable_hash() const noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& frame : frames) {
        hash_value(hash, frame.tick);
        hash_value(hash, frame.input.move.x);
        hash_value(hash, frame.input.move.y);
        hash_value(hash, frame.input.jumpPressed);
        hash_value(hash, frame.input.crouchHeld);
    }
    return hash;
}

GameplayRuntime::GameplayRuntime(GameWorld& world) : world_(&world) {}

bool GameplayRuntime::add_character(GameObjectId pawn, CharacterControllerConfig config, std::string* error) {
    if (!world_->has_object(pawn)) {
        if (error) *error = "character pawn does not exist";
        return false;
    }
    if (!config.validate(error)) return false;
    if (characters_.contains(pawn)) {
        if (error) *error = "character pawn is already registered";
        return false;
    }
    CharacterRecord record;
    record.config = config;
    record.state.pawn = pawn;
    characters_.emplace(pawn, std::move(record));
    return true;
}

bool GameplayRuntime::remove_character(GameObjectId pawn) {
    pawnControllers_.erase(pawn);
    for (auto& [id, playerState] : players_) {
        (void)id;
        if (playerState.pawn == pawn) playerState.pawn = kInvalidGameObjectId;
    }
    return characters_.erase(pawn) > 0U;
}

bool GameplayRuntime::has_character(GameObjectId pawn) const noexcept { return characters_.contains(pawn); }

CharacterControllerState* GameplayRuntime::character(GameObjectId pawn) noexcept {
    const auto it = characters_.find(pawn);
    return it == characters_.end() ? nullptr : &it->second.state;
}

const CharacterControllerState* GameplayRuntime::character(GameObjectId pawn) const noexcept {
    const auto it = characters_.find(pawn);
    return it == characters_.end() ? nullptr : &it->second.state;
}

const CharacterMoveTelemetry* GameplayRuntime::telemetry(GameObjectId pawn) const noexcept {
    const auto it = characters_.find(pawn);
    return it == characters_.end() ? nullptr : &it->second.telemetry;
}

std::vector<GameObjectId> GameplayRuntime::character_ids() const {
    std::vector<GameObjectId> ids;
    ids.reserve(characters_.size());
    for (const auto& [id, record] : characters_) { (void)record; ids.push_back(id); }
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool GameplayRuntime::set_character_input(GameObjectId pawn, CharacterInput input) {
    const auto it = characters_.find(pawn);
    if (it == characters_.end() || !finite(input.move)) return false;
    input.move.z = 0.0F;
    const float magnitudeSquared = length_squared(input.move);
    if (magnitudeSquared > 1.0F) input.move = normalize(input.move);
    it->second.input = input;
    return true;
}

GamePlayerId GameplayRuntime::create_player(std::string name, bool local) {
    const GamePlayerId id = nextPlayerId_++;
    players_.emplace(id, GamePlayerState{id, std::move(name), local, kInvalidGameObjectId, {}});
    return id;
}

bool GameplayRuntime::destroy_player(GamePlayerId playerId) {
    const auto it = players_.find(playerId);
    if (it == players_.end()) return false;
    if (it->second.pawn != kInvalidGameObjectId) pawnControllers_.erase(it->second.pawn);
    recordings_.erase(playerId);
    playbacks_.erase(playerId);
    players_.erase(it);
    return true;
}

bool GameplayRuntime::possess(GamePlayerId playerId, GameObjectId pawn, std::string* error) {
    auto it = players_.find(playerId);
    if (it == players_.end()) {
        if (error) *error = "player does not exist";
        return false;
    }
    if (!characters_.contains(pawn)) {
        if (error) *error = "pawn is not a registered character";
        return false;
    }
    const auto occupied = pawnControllers_.find(pawn);
    if (occupied != pawnControllers_.end() && occupied->second != playerId) {
        if (error) *error = "pawn is already possessed";
        return false;
    }
    if (it->second.pawn != kInvalidGameObjectId) pawnControllers_.erase(it->second.pawn);
    it->second.pawn = pawn;
    pawnControllers_[pawn] = playerId;
    return true;
}

bool GameplayRuntime::unpossess(GamePlayerId playerId) {
    auto it = players_.find(playerId);
    if (it == players_.end() || it->second.pawn == kInvalidGameObjectId) return false;
    pawnControllers_.erase(it->second.pawn);
    it->second.pawn = kInvalidGameObjectId;
    return true;
}

GamePlayerState* GameplayRuntime::player(GamePlayerId playerId) noexcept {
    const auto it = players_.find(playerId);
    return it == players_.end() ? nullptr : &it->second;
}

const GamePlayerState* GameplayRuntime::player(GamePlayerId playerId) const noexcept {
    const auto it = players_.find(playerId);
    return it == players_.end() ? nullptr : &it->second;
}

std::optional<GamePlayerId> GameplayRuntime::controller_of(GameObjectId pawn) const noexcept {
    const auto it = pawnControllers_.find(pawn);
    return it == pawnControllers_.end() ? std::nullopt : std::optional<GamePlayerId>{it->second};
}

bool GameplayRuntime::set_player_input(GamePlayerId playerId, CharacterInput input) {
    auto it = players_.find(playerId);
    if (it == players_.end() || !finite(input.move)) return false;
    input.move.z = 0.0F;
    if (length_squared(input.move) > 1.0F) input.move = normalize(input.move);
    it->second.input = input;
    return true;
}

bool GameplayRuntime::simulate_character_input(
    GameObjectId pawn, CharacterInput input, float fixedDeltaSeconds,
    std::uint64_t simulationTick, std::string* error) {
    auto it = characters_.find(pawn);
    if (it == characters_.end() || !world_->has_object(pawn)) {
        if (error != nullptr) *error = "prediction target is not a live character";
        return false;
    }
    if (!(fixedDeltaSeconds > 0.0F) || !std::isfinite(fixedDeltaSeconds) ||
        !finite(input.move) || simulationTick == 0U) {
        if (error != nullptr) *error = "prediction input, tick, or fixed delta is invalid";
        return false;
    }
    input.move.z = 0.0F;
    if (length_squared(input.move) > 1.0F) input.move = normalize(input.move);
    it->second.input = input;
    update_character(pawn, it->second, fixedDeltaSeconds, simulationTick);
    return true;
}

bool GameplayRuntime::apply_authoritative_character_state(
    GameObjectId pawn, Float3 worldPosition, Float3 velocity, bool grounded,
    GameObjectId supportObject, CharacterStance stance, std::uint64_t authoritativeTick,
    std::string* error) {
    auto it = characters_.find(pawn);
    if (it == characters_.end() || !world_->has_object(pawn)) {
        if (error != nullptr) *error = "authoritative target is not a live character";
        return false;
    }
    if (!finite(worldPosition) || !finite(velocity) || authoritativeTick == 0U) {
        if (error != nullptr) *error = "authoritative character state is not finite or has an invalid tick";
        return false;
    }
    if (supportObject != kInvalidGameObjectId && !world_->has_object(supportObject)) {
        grounded = false;
        supportObject = kInvalidGameObjectId;
    }
    CharacterRecord& record = it->second;
    record.state.stance = stance;
    if (!world_->set_position(pawn, worldPosition)) {
        if (error != nullptr) *error = "could not place character at the authoritative position";
        return false;
    }
    record.state.velocity = velocity;
    record.state.grounded = grounded;
    record.state.supportObject = grounded ? supportObject : kInvalidGameObjectId;
    record.state.groundNormal = {0.0F, 0.0F, 1.0F};
    record.state.coyoteRemainingSeconds = grounded ? record.config.coyoteTimeSeconds : 0.0F;
    record.state.jumpBufferRemainingSeconds = 0.0F;
    record.state.fixedTick = authoritativeTick;
    record.input = {};
    record.telemetry = {};
    return true;
}

GameTriggerId GameplayRuntime::create_trigger(TriggerVolumeDesc desc, std::string* error) {
    if (!desc.validate(error)) return kInvalidGameTriggerId;
    const GameTriggerId id = nextTriggerId_++;
    triggers_.emplace(id, TriggerRecord{std::move(desc), {}, false});
    return id;
}

bool GameplayRuntime::destroy_trigger(GameTriggerId triggerId) { return triggers_.erase(triggerId) > 0U; }

bool GameplayRuntime::set_trigger_enabled(GameTriggerId triggerId, bool enabled) {
    const auto it = triggers_.find(triggerId);
    if (it == triggers_.end()) return false;
    it->second.desc.enabled = enabled;
    if (!enabled) it->second.occupants.clear();
    return true;
}

const TriggerVolumeDesc* GameplayRuntime::trigger(GameTriggerId triggerId) const noexcept {
    const auto it = triggers_.find(triggerId);
    return it == triggers_.end() ? nullptr : &it->second.desc;
}

bool GameplayRuntime::trigger_fired(GameTriggerId triggerId) const noexcept {
    const auto it = triggers_.find(triggerId);
    return it != triggers_.end() && it->second.fired;
}

std::vector<GameTriggerId> GameplayRuntime::trigger_ids() const {
    std::vector<GameTriggerId> ids;
    ids.reserve(triggers_.size());
    for (const auto& [id, record] : triggers_) { (void)record; ids.push_back(id); }
    std::sort(ids.begin(), ids.end());
    return ids;
}

void GameplayRuntime::on_trigger(TriggerListener listener) { triggerListeners_.push_back(std::move(listener)); }

bool GameplayRuntime::begin_recording(GamePlayerId playerId) {
    if (!players_.contains(playerId)) return false;
    recordings_[playerId] = {};
    return true;
}

std::optional<CharacterReplay> GameplayRuntime::end_recording(GamePlayerId playerId) {
    const auto it = recordings_.find(playerId);
    if (it == recordings_.end()) return std::nullopt;
    CharacterReplay replay = std::move(it->second.replay);
    recordings_.erase(it);
    return replay;
}

bool GameplayRuntime::begin_playback(GamePlayerId playerId, CharacterReplay replay, bool loop) {
    if (!players_.contains(playerId) || replay.frames.empty()) return false;
    const std::uint64_t firstTick = replay.frames.front().tick;
    playbacks_[playerId] = PlaybackState{std::move(replay), 0U, loop, fixedTick_ + 1U, firstTick};
    return true;
}

bool GameplayRuntime::stop_playback(GamePlayerId playerId) { return playbacks_.erase(playerId) > 0U; }

Capsule GameplayRuntime::capsule_for(GameObjectId pawn, const CharacterRecord& record) const {
    const Float3 base = world_->position(pawn).value_or(Float3{});
    const float height = record.state.stance == CharacterStance::Standing
        ? record.config.standingHeightMeters : record.config.crouchedHeightMeters;
    const float radius = record.config.radiusMeters;
    return {{base.x, base.y, base.z + radius}, {base.x, base.y, base.z + height - radius}, radius};
}

bool GameplayRuntime::try_set_stance(GameObjectId pawn, CharacterRecord& record, CharacterStance stance) {
    if (record.state.stance == stance) return true;
    const CharacterStance old = record.state.stance;
    record.state.stance = stance;
    const Capsule candidate = capsule_for(pawn, record);
    if (stance == CharacterStance::Standing && world_->capsule_overlaps(candidate, pawn)) {
        record.state.stance = old;
        return false;
    }
    return true;
}

void GameplayRuntime::update_character(GameObjectId pawn, CharacterRecord& record, float dt, std::uint64_t simulationTick) {
    record.telemetry = {};
    if (!world_->has_object(pawn)) return;
    record.state.fixedTick = simulationTick;
    const bool wasGrounded = record.state.grounded;

    (void)try_set_stance(pawn, record,
        record.input.crouchHeld ? CharacterStance::Crouched : CharacterStance::Standing);

    Capsule capsule = capsule_for(pawn, record);
    const GameCapsuleDepenetration depenetration = world_->depenetrate_capsule(
        capsule, pawn, 8U, record.config.skinMeters);
    record.telemetry.depenetrationIterations = static_cast<std::uint32_t>(depenetration.iterations);
    if (depenetration.iterations > 0U) world_->set_position(pawn, capsule_base(capsule));

    Float3 desired = horizontal(record.input.move);
    if (length_squared(desired) > 1.0F) desired = normalize(desired);
    desired = multiply(desired, record.config.maximumGroundSpeedMetersPerSecond);
    const float acceleration = record.state.grounded
        ? (length_squared(desired) > 1.0e-6F ? record.config.groundAccelerationMetersPerSecondSquared
                                            : record.config.groundBrakingMetersPerSecondSquared)
        : record.config.airAccelerationMetersPerSecondSquared;
    record.state.velocity.x = clamp_axis(record.state.velocity.x, desired.x, acceleration * dt);
    record.state.velocity.y = clamp_axis(record.state.velocity.y, desired.y, acceleration * dt);

    if (record.state.grounded) record.state.coyoteRemainingSeconds = record.config.coyoteTimeSeconds;
    else record.state.coyoteRemainingSeconds = std::max(0.0F, record.state.coyoteRemainingSeconds - dt);
    if (record.input.jumpPressed) record.state.jumpBufferRemainingSeconds = record.config.jumpBufferSeconds;
    else record.state.jumpBufferRemainingSeconds = std::max(0.0F, record.state.jumpBufferRemainingSeconds - dt);

    Float3 supportVelocity{};
    if (record.state.grounded && record.state.supportObject != kInvalidGameObjectId) {
        if (const auto velocity = world_->velocity_at_point(record.state.supportObject, capsule.pointA)) {
            supportVelocity = *velocity;
            record.telemetry.inheritedPlatformVelocity = length_squared(supportVelocity) > 1.0e-8F;
        }
    }

    if (record.state.jumpBufferRemainingSeconds > 0.0F && record.state.coyoteRemainingSeconds > 0.0F) {
        record.state.velocity = add(record.state.velocity, supportVelocity);
        record.state.velocity.z = std::max(record.state.velocity.z, record.config.jumpSpeedMetersPerSecond);
        record.state.grounded = false;
        record.state.supportObject = kInvalidGameObjectId;
        record.state.coyoteRemainingSeconds = 0.0F;
        record.state.jumpBufferRemainingSeconds = 0.0F;
        supportVelocity = {};
        record.telemetry.jumped = true;
    }

    if (!record.state.grounded) {
        record.state.velocity.z = std::max(
            record.state.velocity.z - record.config.gravityMetersPerSecondSquared * dt,
            -record.config.maximumFallSpeedMetersPerSecond);
    } else if (record.state.velocity.z < 0.0F) {
        record.state.velocity.z = 0.0F;
    }

    Float3 remaining = add(multiply(record.state.velocity, dt), multiply(supportVelocity, dt));
    const float walkableZ = std::cos(record.config.maximumSlopeDegrees * kPi / 180.0F);
    bool groundedFromMove = false;
    GameObjectId moveSupport = kInvalidGameObjectId;
    Float3 moveGroundNormal{0.0F, 0.0F, 1.0F};

    for (std::uint32_t iteration = 0U;
         iteration < record.config.maximumSlideIterations && length_squared(remaining) > 1.0e-10F;
         ++iteration) {
        ++record.telemetry.sweeps;
        const auto hit = world_->capsule_sweep(capsule, remaining, pawn);
        if (!hit) {
            translate(capsule, remaining);
            remaining = {};
            break;
        }
        ++record.telemetry.contacts;
        const float distance = length(remaining);
        const float skinFraction = distance > 1.0e-6F ? record.config.skinMeters / distance : 0.0F;
        const float travel = std::clamp(hit->time - skinFraction, 0.0F, 1.0F);
        translate(capsule, multiply(remaining, travel));
        Float3 residual = multiply(remaining, 1.0F - travel);
        const bool walkable = hit->worldNormal.z >= walkableZ;

        const bool horizontalImpact = std::abs(hit->worldNormal.z) < walkableZ &&
            length_squared(horizontal(residual)) > 1.0e-8F && record.config.stepHeightMeters > 0.0F;
        if (horizontalImpact) {
            ++record.telemetry.stepAttempts;
            const Float3 up{0.0F, 0.0F, record.config.stepHeightMeters};
            if (!world_->capsule_sweep(capsule, up, pawn)) {
                Capsule stepped = capsule;
                translate(stepped, up);
                const Float3 horizontalResidual = horizontal(residual);
                if (!world_->capsule_sweep(stepped, horizontalResidual, pawn)) {
                    translate(stepped, horizontalResidual);
                    const Float3 down{0.0F, 0.0F, -(record.config.stepHeightMeters + record.config.groundProbeMeters)};
                    const auto floor = world_->capsule_sweep(stepped, down, pawn);
                    if (floor && floor->worldNormal.z >= walkableZ) {
                        const float downDistance = length(down);
                        const float downSkin = downDistance > 1.0e-6F ? record.config.skinMeters / downDistance : 0.0F;
                        translate(stepped, multiply(down, std::clamp(floor->time - downSkin, 0.0F, 1.0F)));
                        capsule = stepped;
                        groundedFromMove = true;
                        moveSupport = floor->objectId;
                        moveGroundNormal = floor->worldNormal;
                        record.state.velocity.z = 0.0F;
                        remaining = {};
                        ++record.telemetry.successfulSteps;
                        break;
                    }
                }
            }
        }

        if (walkable && residual.z <= 0.0F) {
            groundedFromMove = true;
            moveSupport = hit->objectId;
            moveGroundNormal = hit->worldNormal;
            if (record.state.velocity.z < 0.0F) record.state.velocity.z = 0.0F;
        }
        if (hit->worldNormal.z < -0.5F && record.state.velocity.z > 0.0F) {
            record.state.velocity.z = 0.0F;
            record.telemetry.ceilingHit = true;
        }
        const float intoSurface = dot(residual, hit->worldNormal);
        if (intoSurface < 0.0F) residual = subtract(residual, multiply(hit->worldNormal, intoSurface));
        const float velocityIntoSurface = dot(record.state.velocity, hit->worldNormal);
        if (velocityIntoSurface < 0.0F)
            record.state.velocity = subtract(record.state.velocity, multiply(hit->worldNormal, velocityIntoSurface));
        remaining = residual;
    }

    record.state.grounded = groundedFromMove;
    record.state.supportObject = moveSupport;
    record.state.groundNormal = moveGroundNormal;

    if (!record.telemetry.jumped && record.state.velocity.z <= 0.0F) {
        const Float3 probe{0.0F, 0.0F, -record.config.groundProbeMeters};
        ++record.telemetry.sweeps;
        const auto ground = world_->capsule_sweep(capsule, probe, pawn);
        if (ground && ground->worldNormal.z >= walkableZ) {
            const float distance = length(probe);
            const float skinFraction = distance > 1.0e-6F ? record.config.skinMeters / distance : 0.0F;
            translate(capsule, multiply(probe, std::clamp(ground->time - skinFraction, 0.0F, 1.0F)));
            record.state.grounded = true;
            record.state.supportObject = ground->objectId;
            record.state.groundNormal = ground->worldNormal;
            if (record.state.velocity.z < 0.0F) record.state.velocity.z = 0.0F;
        }
    }

    record.telemetry.floorRemoved = wasGrounded && !record.state.grounded;
    world_->set_position(pawn, capsule_base(capsule));
    record.input.jumpPressed = false;
}

void GameplayRuntime::update_triggers() {
    std::vector<GameTriggerId> triggerIds;
    triggerIds.reserve(triggers_.size());
    for (const auto& [id, record] : triggers_) { (void)record; triggerIds.push_back(id); }
    std::sort(triggerIds.begin(), triggerIds.end());

    for (const GameTriggerId triggerId : triggerIds) {
        TriggerRecord& record = triggers_.at(triggerId);
        if (!record.desc.enabled) continue;
        std::unordered_set<GameObjectId> current;
        if (record.desc.charactersOnly) {
            for (const auto& [pawn, characterRecord] : characters_) {
                if (!world_->has_object(pawn)) continue;
                if (!record.desc.requiredTag.empty() && !world_->has_tag(pawn, record.desc.requiredTag)) continue;
                if (capsule_overlaps_trigger(capsule_for(pawn, characterRecord), record.desc)) current.insert(pawn);
            }
        } else {
            for (const GameObjectId object : world_->object_ids()) {
                if (!record.desc.requiredTag.empty() && !world_->has_tag(object, record.desc.requiredTag)) continue;
                const auto position = world_->position(object);
                if (position && point_inside_trigger(*position, record.desc)) current.insert(object);
            }
        }

        std::vector<TriggerEvent> events;
        for (const GameObjectId object : current) {
            events.push_back({triggerId, record.occupants.contains(object) ? TriggerEventKind::Stay : TriggerEventKind::Enter,
                              object, fixedTick_});
        }
        for (const GameObjectId object : record.occupants) {
            if (!current.contains(object)) events.push_back({triggerId, TriggerEventKind::Exit, object, fixedTick_});
        }
        std::sort(events.begin(), events.end(), [](const TriggerEvent& a, const TriggerEvent& b) {
            if (a.object != b.object) return a.object < b.object;
            return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
        });
        bool entered = false;
        for (const TriggerEvent& event : events) {
            entered = entered || event.kind == TriggerEventKind::Enter;
            for (const TriggerListener& listener : triggerListeners_) listener(event);
        }
        record.occupants = std::move(current);
        if (entered) record.fired = true;
        if (entered && record.desc.oneShot) {
            record.desc.enabled = false;
            record.occupants.clear();
        }
    }
}

void GameplayRuntime::fixed_update(float fixedDeltaSeconds) {
    if (!(fixedDeltaSeconds > 0.0F) || !std::isfinite(fixedDeltaSeconds)) return;
    ++fixedTick_;

    for (auto& [playerId, playback] : playbacks_) {
        auto playerIt = players_.find(playerId);
        if (playerIt == players_.end() || playback.replay.frames.empty()) continue;
        while (playback.nextFrame < playback.replay.frames.size()) {
            const std::uint64_t sourceTick = playback.replay.frames[playback.nextFrame].tick;
            const std::uint64_t scheduledTick = playback.playbackStartTick + (sourceTick - playback.replayFirstTick);
            if (scheduledTick > fixedTick_) break;
            playerIt->second.input = playback.replay.frames[playback.nextFrame].input;
            ++playback.nextFrame;
        }
        if (playback.nextFrame >= playback.replay.frames.size() && playback.loop) {
            playback.nextFrame = 0U;
            playback.playbackStartTick = fixedTick_ + 1U;
        }
    }

    for (auto& [playerId, playerState] : players_) {
        const auto recording = recordings_.find(playerId);
        if (recording != recordings_.end()) recording->second.replay.frames.push_back({fixedTick_, playerState.input});
        if (playerState.pawn != kInvalidGameObjectId) set_character_input(playerState.pawn, playerState.input);
    }

    std::vector<GameObjectId> pawns;
    pawns.reserve(characters_.size());
    for (const auto& [pawn, record] : characters_) { (void)record; pawns.push_back(pawn); }
    std::sort(pawns.begin(), pawns.end());
    for (const GameObjectId pawn : pawns) update_character(pawn, characters_.at(pawn), fixedDeltaSeconds, fixedTick_);
    for (auto& [id, playerState] : players_) { (void)id; playerState.input.jumpPressed = false; }
    update_triggers();
}

std::string GameplayRuntime::serialize_trigger_state() const {
    std::ostringstream output;
    output << "DVE_TRIGGER_STATE 1\n";
    std::vector<GameTriggerId> ids;
    for (const auto& [id, record] : triggers_) { (void)record; ids.push_back(id); }
    std::sort(ids.begin(), ids.end());
    for (const GameTriggerId id : ids) {
        const TriggerRecord& record = triggers_.at(id);
        output << id << ' ' << (record.desc.enabled ? 1 : 0) << ' ' << (record.fired ? 1 : 0) << '\n';
    }
    return output.str();
}

bool GameplayRuntime::restore_trigger_state(std::string_view text, std::string* error) {
    std::istringstream input{std::string(text)};
    std::string magic;
    int version{};
    if (!(input >> magic >> version) || magic != "DVE_TRIGGER_STATE" || version != 1) {
        if (error) *error = "invalid trigger-state header";
        return false;
    }
    GameTriggerId id{};
    int enabled{};
    int fired{};
    while (input >> id >> enabled >> fired) {
        auto it = triggers_.find(id);
        if (it == triggers_.end() || (enabled != 0 && enabled != 1) || (fired != 0 && fired != 1)) {
            if (error) *error = "trigger-state entry does not match the live trigger set";
            return false;
        }
        it->second.desc.enabled = enabled != 0;
        it->second.fired = fired != 0;
        it->second.occupants.clear();
    }
    if (!input.eof()) {
        if (error) *error = "malformed trigger-state entry";
        return false;
    }
    return true;
}

} // namespace dve
